#include "spotify.h"
#include "app_log.h"
#include "config.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace {

const char *TOKEN_URL = "https://accounts.spotify.com/api/token";
const char *API_BASE = "https://api.spotify.com/v1";
constexpr size_t MAX_ART_BYTES = 400 * 1024;
constexpr uint32_t POLL_MS = 3000;
constexpr uint32_t COMMAND_REPOLL_MS = 800;

WiFiClientSecure sClient;
String sClientHost;

SemaphoreHandle_t netLock;
SemaphoreHandle_t stateLock;
QueueHandle_t cmdQueue;
bool taskStarted = false;

PlayerState shared;
SemaphoreHandle_t artLock;
volatile bool artReady = false;

String accessToken;
uint32_t tokenExpiresAt = 0;
volatile bool authLost = false;
uint32_t backoffUntil = 0;
uint8_t backoffStep = 0;
String lastArtUrl;
uint8_t artFailures = 0;

struct HttpResult {
    int code = -1;
    String body;
    uint32_t retryAfter = 0;
};

bool reached(uint32_t t) { return (int32_t)(millis() - t) >= 0; }

String urlEncode(const String &s) {
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            out += buf;
        }
    }
    return out;
}

String hostOf(const String &url) {
    int start = url.indexOf("://") + 3;
    int end = url.indexOf('/', start);
    return end < 0 ? url.substring(start) : url.substring(start, end);
}

void useHost(const String &host) {
    if (host != sClientHost) {
        sClient.stop();
        sClientHost = host;
    }
}

String describeError(const String &body, int code) {
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
        if (doc["error"].is<JsonObject>()) {
            const char *msg = doc["error"]["message"] | "";
            if (msg[0]) return msg;
        } else {
            const char *desc = doc["error_description"] | "";
            if (desc[0]) return desc;
            const char *err = doc["error"] | "";
            if (err[0]) return err;
        }
    }
    return code > 0 ? "HTTP " + String(code) : "Network error";
}

HttpResult httpOnce(const char *method, const String &url, const char *contentType, const String &body, const String &bearer) {
    HttpResult r;
    useHost(hostOf(url));

    HTTPClient http;
    http.setReuse(true);
    http.setConnectTimeout(6000);
    http.setTimeout(6000);
    const char *keys[] = {"Retry-After"};
    http.collectHeaders(keys, 1);

    if (!http.begin(sClient, url)) return r;
    if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);
    if (contentType) http.addHeader("Content-Type", contentType);
    if (body.isEmpty() && strcmp(method, "GET") != 0) http.addHeader("Content-Length", "0");

    uint32_t t0 = millis();
    r.code = http.sendRequest(method, (uint8_t *)body.c_str(), body.length());
    // Without this guard getString() blocks until the server closes a 204 response.
    if (r.code > 0 && r.code != 204 && http.getSize() != 0) r.body = http.getString();
    if (r.code > 0 && http.hasHeader("Retry-After")) r.retryAfter = http.header("Retry-After").toInt();
    http.end();

    String path = url.substring(url.indexOf('/', 8));
    if (r.code < 0) {
        app_log(LL_WARN, "%s %s failed: %s", method, path.c_str(), HTTPClient::errorToString(r.code).c_str());
    } else if (r.code >= 400) {
        app_log(LL_WARN, "%s %s -> %d: %.160s", method, path.c_str(), r.code, r.body.c_str());
    } else {
        app_log(LL_DEBUG, "%s %s -> %d (%u ms, %u B)", method, path.c_str(), r.code, (unsigned)(millis() - t0), (unsigned)r.body.length());
    }
    return r;
}

HttpResult httpDo(const char *method, const String &url, const char *contentType, const String &body, const String &bearer) {
    bool reused = hostOf(url) == sClientHost && sClient.connected();
    HttpResult r = httpOnce(method, url, contentType, body, bearer);
    // A kept-alive connection the server already closed shows up as a timeout.
    if (r.code < 0 && reused) {
        sClient.stop();
        r = httpOnce(method, url, contentType, body, bearer);
    }
    return r;
}

void setMessage(const String &msg) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    shared.message = msg;
    xSemaphoreGive(stateLock);
}

void noteRateLimit(const HttpResult &r) {
    uint32_t wait = r.retryAfter;
    if (wait == 0) {
        wait = min<uint32_t>(60, 2u << backoffStep);
        if (backoffStep < 5) backoffStep++;
    }
    backoffUntil = millis() + wait * 1000;
    setMessage("Rate limited, retrying in " + String(wait) + "s");
}

bool backoffActive() { return !reached(backoffUntil); }

bool storeTokens(const String &body) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        app_log(LL_ERROR, "Token response is not valid JSON");
        return false;
    }
    const char *token = doc["access_token"] | "";
    if (!token[0]) {
        app_log(LL_ERROR, "Token response has no access_token");
        return false;
    }

    accessToken = token;
    uint32_t expires = doc["expires_in"] | 3600;
    tokenExpiresAt = millis() + (expires > 120 ? expires - 60 : expires) * 1000UL;

    const char *refresh = doc["refresh_token"] | "";
    if (refresh[0] && g_config.refreshToken != refresh) {
        g_config.refreshToken = refresh;
        config_save();
    }
    return true;
}

bool refreshAccessToken() {
    String body = "grant_type=refresh_token&refresh_token=" + urlEncode(g_config.refreshToken) +
                  "&client_id=" + urlEncode(g_config.clientId);
    HttpResult r = httpDo("POST", TOKEN_URL, "application/x-www-form-urlencoded", body, "");

    if (r.code == 200 && storeTokens(r.body)) {
        app_log(LL_INFO, "Access token refreshed");
        backoffStep = 0;
        return true;
    }
    if (r.code == 400 || r.code == 401) {
        app_log(LL_ERROR, "Refresh token rejected, Spotify login required again");
        g_config.refreshToken = "";
        config_save();
        accessToken = "";
        authLost = true;
    } else if (r.code == 429) {
        noteRateLimit(r);
    } else {
        setMessage(describeError(r.body, r.code));
    }
    return false;
}

HttpResult apiCall(const char *method, const String &path) {
    HttpResult r;
    if (accessToken.isEmpty() || reached(tokenExpiresAt)) {
        if (!refreshAccessToken()) return r;
    }

    String url = String(API_BASE) + path;
    r = httpDo(method, url, nullptr, "", accessToken);
    if (r.code == 401) {
        if (!refreshAccessToken()) return r;
        r = httpDo(method, url, nullptr, "", accessToken);
    }
    if (r.code == 429) noteRateLimit(r);
    return r;
}

String pickImage(JsonArray images, int target, String *smallest = nullptr) {
    String url;
    int best = INT_MAX;
    int smallestWidth = INT_MAX;
    for (JsonObject img : images) {
        const char *u = img["url"] | "";
        if (!u[0]) continue;
        int width = img["width"] | 0;
        int diff = abs(width - target);
        if (diff < best) {
            best = diff;
            url = u;
        }
        if (smallest && width < smallestWidth) {
            smallestWidth = width;
            *smallest = u;
        }
    }
    return url;
}

bool parsePlayer(const String &body, PlayerState &out) {
    JsonDocument filter;
    filter["is_playing"] = true;
    filter["progress_ms"] = true;
    filter["device"]["volume_percent"] = true;
    JsonObject item = filter["item"].to<JsonObject>();
    item["id"] = true;
    item["name"] = true;
    item["type"] = true;
    item["duration_ms"] = true;
    item["artists"][0]["name"] = true;
    item["album"]["images"][0]["url"] = true;
    item["album"]["images"][0]["width"] = true;
    item["show"]["name"] = true;
    item["images"][0]["url"] = true;
    item["images"][0]["width"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) {
        app_log(LL_ERROR, "Player JSON error: %s (%u B): %.150s", err.c_str(), (unsigned)body.length(), body.c_str());
        return false;
    }

    out.playing = doc["is_playing"] | false;
    out.progressMs = doc["progress_ms"] | 0;
    out.volume = doc["device"]["volume_percent"] | -1;

    JsonObject it = doc["item"];
    if (it.isNull()) return true;

    out.active = true;
    out.trackId = (const char *)(it["id"] | "");
    out.title = (const char *)(it["name"] | "");
    out.durationMs = it["duration_ms"] | 0;

    if (strcmp(it["type"] | "", "episode") == 0) {
        out.artist = (const char *)(it["show"]["name"] | "");
        out.artUrl = pickImage(it["images"], g_config.artSize, &out.artUrlSmall);
    } else {
        for (JsonObject a : it["artists"].as<JsonArray>()) {
            if (out.artist.length()) out.artist += ", ";
            out.artist += (const char *)(a["name"] | "");
        }
        out.artUrl = pickImage(it["album"]["images"], g_config.artSize, &out.artUrlSmall);
    }
    return true;
}

bool downloadArt(const String &url) {
    // Drop the API TLS session first: two sessions do not fit in heap.
    sClient.stop();
    sClientHost = "";
    app_log(LL_DEBUG, "Art: %s (free=%u, max=%u)", url.c_str(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    xSemaphoreTake(artLock, portMAX_DELAY);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    bool ok = false;
    if (http.begin(sClient, url)) {
        int code = http.GET();
        int size = http.getSize();
        if (code != 200) {
            app_log(LL_WARN, "Art: HTTP %d", code);
        } else if (size > (int)MAX_ART_BYTES) {
            app_log(LL_WARN, "Art: image too large (%d bytes)", size);
        } else {
            File file = LittleFS.open(SPOTIFY_ART_PATH, "w");
            if (!file) {
                app_log(LL_WARN, "Art: cannot open temp file");
            } else {
                int written = http.writeToStream(&file);
                file.close();
                ok = written > 0;
                if (!ok) {
                    app_log(LL_WARN, "Art: download failed (%d)", written);
                    LittleFS.remove(SPOTIFY_ART_PATH);
                }
            }
        }
        http.end();
    } else {
        app_log(LL_WARN, "Art: begin failed");
    }
    sClient.stop();
    if (ok) artReady = true;
    xSemaphoreGive(artLock);
    return ok;
}

void pollPlayer() {
    HttpResult r = apiCall("GET", "/me/player?additional_types=track,episode");

    if (r.code == 200 && r.body.length()) {
        PlayerState s;
        if (!parsePlayer(r.body, s)) {
            setMessage("Unexpected response from Spotify");
            return;
        }
        s.stamp = millis();
        xSemaphoreTake(stateLock, portMAX_DELAY);
        shared = s;
        xSemaphoreGive(stateLock);
        backoffStep = 0;
        app_log(LL_DEBUG, "Now: %s - %s (%s)", s.artist.c_str(), s.title.c_str(), s.playing ? "playing" : "paused");

        if (s.artUrl.length() && s.artUrl != lastArtUrl) {
            r.body = String();
            bool got = downloadArt(s.artUrl);
            if (!got && s.artUrlSmall.length() && s.artUrlSmall != s.artUrl) got = downloadArt(s.artUrlSmall);
            if (got || ++artFailures >= 3) {
                lastArtUrl = s.artUrl;
                artFailures = 0;
            }
        }
    } else if (r.code == 204 || r.code == 200) {
        xSemaphoreTake(stateLock, portMAX_DELAY);
        shared = PlayerState();
        shared.message = "No active device. Start playback in Spotify.";
        xSemaphoreGive(stateLock);
    } else if (r.code != 429 && !authLost) {
        setMessage(describeError(r.body, r.code));
    }
}

void runCommand(uint8_t cmd) {
    PlayerState snap;
    spotify_get_state(snap);

    HttpResult r;
    int newVolume = snap.volume;
    switch (cmd) {
    case CMD_PLAY_PAUSE:
        r = apiCall("PUT", snap.playing ? "/me/player/pause" : "/me/player/play");
        break;
    case CMD_NEXT:
        r = apiCall("POST", "/me/player/next");
        break;
    case CMD_PREV:
        r = apiCall("POST", "/me/player/previous");
        break;
    case CMD_VOL_UP:
    case CMD_VOL_DOWN:
        if (snap.volume < 0) return;
        newVolume = constrain(snap.volume + (cmd == CMD_VOL_UP ? 10 : -10), 0, 100);
        r = apiCall("PUT", "/me/player/volume?volume_percent=" + String(newVolume));
        break;
    default:
        return;
    }

    if (r.code >= 200 && r.code < 300) {
        xSemaphoreTake(stateLock, portMAX_DELAY);
        if (cmd == CMD_PLAY_PAUSE) shared.playing = !snap.playing;
        shared.volume = newVolume;
        shared.message = "";
        xSemaphoreGive(stateLock);
    } else if (r.code != 429 && !authLost) {
        setMessage(describeError(r.body, r.code));
    }
}

void spotifyTask(void *) {
    uint32_t nextPoll = millis();
    for (;;) {
        if (authLost) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int32_t wait = (int32_t)(nextPoll - millis());
        uint8_t cmd = 0;
        bool gotCmd = xQueueReceive(cmdQueue, &cmd, pdMS_TO_TICKS(wait > 0 ? wait : 0)) == pdTRUE;

        xSemaphoreTake(netLock, portMAX_DELAY);
        bool online = WiFi.status() == WL_CONNECTED;
        if (gotCmd && online && !backoffActive()) {
            runCommand(cmd);
            nextPoll = millis() + COMMAND_REPOLL_MS;
        }
        if (reached(nextPoll)) {
            if (!online) {
                setMessage("Wi-Fi disconnected");
            } else if (!backoffActive()) {
                pollPlayer();
            }
            nextPoll = millis() + POLL_MS;
            if (backoffActive() && (int32_t)(backoffUntil - nextPoll) > 0) nextPoll = backoffUntil;
        }
        xSemaphoreGive(netLock);
    }
}

} // namespace

void spotify_init() {
    netLock = xSemaphoreCreateMutex();
    stateLock = xSemaphoreCreateMutex();
    artLock = xSemaphoreCreateMutex();
    cmdQueue = xQueueCreate(4, sizeof(uint8_t));
    sClient.setInsecure();
}

void spotify_start() {
    if (taskStarted) return;
    taskStarted = true;
    xTaskCreatePinnedToCore(spotifyTask, "spotify", 12288, nullptr, 1, nullptr, 0);
}

bool spotify_exchange_code(const String &code, const String &verifier, String &error) {
    xSemaphoreTake(netLock, portMAX_DELAY);
    String body = "grant_type=authorization_code&code=" + urlEncode(code) +
                  "&redirect_uri=" + urlEncode(SPOTIFY_REDIRECT_URI) +
                  "&client_id=" + urlEncode(g_config.clientId) +
                  "&code_verifier=" + urlEncode(verifier);
    HttpResult r = httpDo("POST", TOKEN_URL, "application/x-www-form-urlencoded", body, "");

    bool ok = r.code == 200 && storeTokens(r.body);
    if (ok) {
        authLost = false;
        backoffStep = 0;
        backoffUntil = millis();
        lastArtUrl = "";
    } else {
        error = describeError(r.body, r.code);
    }
    xSemaphoreGive(netLock);
    return ok;
}

void spotify_send(SpotifyCmd cmd) {
    uint8_t c = cmd;
    xQueueSend(cmdQueue, &c, 0);
}

void spotify_get_state(PlayerState &out) {
    xSemaphoreTake(stateLock, portMAX_DELAY);
    out = shared;
    xSemaphoreGive(stateLock);
}

bool spotify_art_begin_read() {
    if (!artReady || xSemaphoreTake(artLock, 0) != pdTRUE) return false;
    if (!artReady) {
        xSemaphoreGive(artLock);
        return false;
    }
    return true;
}

void spotify_art_end_read() {
    artReady = false;
    LittleFS.remove(SPOTIFY_ART_PATH);
    xSemaphoreGive(artLock);
}

bool spotify_auth_lost() { return authLost; }
