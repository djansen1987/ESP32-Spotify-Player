#include "config.h"
#include "app_log.h"
#include "display.h"
#include "led.h"
#include "spotify.h"
#include "ui.h"
#include "web_portal.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <lvgl.h>

#if CONFIG_BT_ENABLED
#include <esp_bt.h>
#endif

enum AppState {
    WIFI_SETUP_MODE,
    WIFI_CONNECTED_NO_SPOTIFY,
    READY,
};

static const char *AP_NAME = "Spotify-Player-Setup";
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 12000;
static const uint32_t NET_RECHECK_MS = 60000;
static const size_t MAX_NETWORK_ATTEMPTS = 4;
static const uint32_t AP_RETRY_STA_MS = 5UL * 60 * 1000;
static const uint32_t UI_TICK_MS = 250;
static const uint32_t HEAP_CHECK_MS = 5000;
static const uint32_t LOW_HEAP_BYTES = 25000;

static AppState appState = WIFI_SETUP_MODE;
volatile bool g_internetOk = true;
static uint32_t lastNetCheck = 0;
static uint32_t apStartedAt = 0;
static uint32_t lastTick = 0;
static uint32_t lastHeapCheck = 0;
static volatile bool resetRequested = false;
static volatile bool passwordResetRequested = false;
static bool rescueAp = false;
static uint32_t offlineSince = 0;
static uint32_t bootHeldSince = 0;
static const uint32_t OFFLINE_WINDOW_MS = 60000;
static const uint32_t RESCUE_AFTER_MS = 120000;
static const uint32_t HINT_AFTER_MS = 20000;
static const uint32_t BOOT_HOLD_MS = 3000;

static void logResetReason() {
    esp_reset_reason_t reason = esp_reset_reason();
    switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        app_log(LL_WARN, "Unexpected restart, reset reason code %d (4=panic, 5-7=watchdog, 9=brownout)", (int)reason);
        break;
    default:
        break;
    }
}

static void checkHeap() {
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxBlock = ESP.getMaxAllocHeap();
    app_log(LL_DEBUG, "Heap free=%u max=%u min=%u 8bit=%u", (unsigned)freeHeap, (unsigned)maxBlock, (unsigned)ESP.getMinFreeHeap(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    app_log(LL_DEBUG, "LVGL used=%u%% free=%u biggest=%u", (unsigned)mon.used_pct, (unsigned)mon.free_size, (unsigned)mon.free_biggest_size);
    if (freeHeap < LOW_HEAP_BYTES) app_log(LL_WARN, "Low heap: free=%u max=%u", (unsigned)freeHeap, (unsigned)maxBlock);
}

static const char *resetReasonName() {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "power";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_BROWNOUT:
        return "brownout";
    default:
        return "other";
    }
}

static String deviceInfo() {
    String t;
    if (WiFi.status() == WL_CONNECTED) {
        int rssi = WiFi.RSSI();
        const char *quality = rssi >= -55 ? "excellent" : rssi >= -65 ? "good" : rssi >= -75 ? "fair" : "weak";
        t += "IP: " + WiFi.localIP().toString() + "\n";
        t += "SSID: " + WiFi.SSID() + " (ch " + String(WiFi.channel()) + ")\n";
        t += "Signal: " + String(rssi) + " dBm (" + quality + ")\n";
    } else {
        t += "Wi-Fi not connected\n";
        t += "Setup IP: " + WiFi.softAPIP().toString() + "\n";
        t += "Known networks: " + String(config_get_networks().size()) + "\n";
    }
    t += "MAC: " + WiFi.macAddress() + "\n";
    uint32_t up = millis() / 1000;
    char buf[64];
    snprintf(buf, sizeof(buf), "Up %uh %02um, reset: %s\n", (unsigned)(up / 3600), (unsigned)((up / 60) % 60), resetReasonName());
    t += buf;
    snprintf(buf, sizeof(buf), "Heap: %uK free, %uK block\n", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024));
    if (rescueAp) snprintf(buf, sizeof(buf), "Setup AP: %s (192.168.4.1)\n", AP_NAME);
    t += buf;
    snprintf(buf, sizeof(buf), "Spotify: %s, poll %u ms", spotify_token_valid() ? "token ok" : "no token", (unsigned)spotify_last_poll_ms());
    t += buf;
    return t;
}

// A captive portal answers with a redirect instead of 204.
static bool checkInternet() {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    if (!http.begin(client, "http://connectivitycheck.gstatic.com/generate_204")) return false;
    int code = http.GET();
    http.end();
    return code == 204;
}

static bool tryNetwork(const WifiNet &net) {
    String status = "Connecting to\n" + net.ssid + "...";
    ui_set_setup_status(status.c_str());
    WiFi.disconnect();
    delay(100);
    WiFi.begin(net.ssid.c_str(), net.pass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        lv_timer_handler();
        delay(50);
    }
    return WiFi.status() == WL_CONNECTED;
}

static bool connectStation() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);

    std::vector<WifiNet> known = config_get_networks();
    if (known.empty()) return false;

    ui_set_setup_status("Looking for known\nWi-Fi networks...");
    lv_timer_handler();
    int found = WiFi.scanNetworks();
    std::vector<WifiNet> candidates;
    for (const WifiNet &net : known) {
        for (int i = 0; i < found; i++) {
            if (WiFi.SSID(i) == net.ssid) {
                candidates.push_back(net);
                break;
            }
        }
    }
    WiFi.scanDelete();
    // Hidden networks do not show up in a scan, so fall back to trying every saved network.
    if (candidates.empty()) candidates = known;
    if (candidates.size() > MAX_NETWORK_ATTEMPTS) candidates.resize(MAX_NETWORK_ATTEMPTS);

    int noInternet = -1;
    for (size_t i = 0; i < candidates.size(); i++) {
        if (!tryNetwork(candidates[i])) continue;
        ui_set_setup_status("Checking internet...");
        lv_timer_handler();
        if (checkInternet()) {
            config_promote_network(candidates[i].ssid);
            g_internetOk = true;
            return true;
        }
        app_log(LL_WARN, "Wi-Fi '%s' has no internet access (captive portal?)", candidates[i].ssid.c_str());
        if (noInternet < 0) noInternet = i;
    }

    // Stay reachable on a network without internet so the Wi-Fi settings can still be changed.
    if (noInternet >= 0 && tryNetwork(candidates[noInternet])) {
        g_internetOk = false;
        return true;
    }
    return false;
}

static void startAccessPoint() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_NAME);
    String ip = WiFi.softAPIP().toString();
    update_setup_screen(ip.c_str(), true);
    apStartedAt = millis();
    appState = WIFI_SETUP_MODE;
}

static void enterNoSpotify() {
    String ip = WiFi.localIP().toString();
    update_setup_screen(ip.c_str(), false);
    show_setup_screen();
    appState = WIFI_CONNECTED_NO_SPOTIFY;
}

static void enterReady() {
    show_player_screen();
    spotify_start();
    appState = READY;
}

static void handleSpotifyCode() {
    String code, verifier;
    if (!web_portal_take_code(code, verifier)) return;

    String error;
    bool ok = spotify_exchange_code(code, verifier, error);
    web_portal_set_result(ok, error);
    if (ok) enterReady();
}

static void updatePlayerUi() {
    PlayerState state;
    spotify_get_state(state);
    ui_update_player(state);

    if (spotify_art_begin_read()) {
        ui_show_art();
        spotify_art_end_read();
    }
}

static void requestWifiReset() { resetRequested = true; }

static void handleWifiReset() {
    if (!resetRequested) return;
    bool clearPassword = passwordResetRequested;
    if (clearPassword) config_clear_web_password();
    String current = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
    if (current.length()) {
        config_remove_network(current);
    } else {
        config_clear_networks();
    }
    show_setup_screen();
    ui_set_setup_status(clearPassword ? "Network forgotten and\nweb password removed.\nRestarting..."
                                      : current.length() ? "Network forgotten.\nRestarting..." : "Wi-Fi settings cleared.\nRestarting in setup mode...");
    for (int i = 0; i < 30; i++) {
        lv_timer_handler();
        delay(30);
    }
    ESP.restart();
}

static void handleBootButton() {
    if (digitalRead(0) != LOW) {
        bootHeldSince = 0;
        return;
    }
    if (!bootHeldSince) bootHeldSince = millis();
    if (millis() - bootHeldSince > BOOT_HOLD_MS) {
        passwordResetRequested = true;
        resetRequested = true;
    }
}

// A guest network with a captive portal connects but has no internet, so keep a setup AP reachable.
static void handleRescueAp() {
    if (appState == WIFI_SETUP_MODE) return;
    bool wifiDown = WiFi.status() != WL_CONNECTED;
    if (appState == READY && millis() > OFFLINE_WINDOW_MS) {
        g_internetOk = spotify_recently_ok(OFFLINE_WINDOW_MS);
    } else if (!wifiDown && !g_internetOk && millis() - lastNetCheck > NET_RECHECK_MS) {
        lastNetCheck = millis();
        if (checkInternet()) g_internetOk = true;
    }
    bool offline = wifiDown || !g_internetOk;
    if (!offline) {
        ui_set_hint("");
        offlineSince = 0;
        if (rescueAp) {
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            rescueAp = false;
        }
        return;
    }
    if (!offlineSince) offlineSince = millis();
    if (millis() - offlineSince > HINT_AFTER_MS) ui_set_hint("No internet. Long-press the screen to forget this Wi-Fi.");
    if (!rescueAp && millis() - offlineSince > RESCUE_AFTER_MS) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(AP_NAME);
        rescueAp = true;
    }
}

void setup() {
    Serial.begin(115200);
    log_init();
    logResetReason();
#if CONFIG_BT_ENABLED
    esp_bt_mem_release(ESP_BT_MODE_BTDM);
#endif

    if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
    config_load();
    led_init();

    display_init();
    ui_init();
    ui_set_info_provider(deviceInfo);
    ui_set_reset_handler(requestWifiReset);
    pinMode(0, INPUT_PULLUP);
    show_setup_screen();
    spotify_init();

    if (config_has_networks() && connectStation()) {
        if (g_config.refreshToken.length()) {
            enterReady();
        } else {
            enterNoSpotify();
        }
    } else {
        startAccessPoint();
    }
    web_portal_begin();
    Serial.printf("Boot done: heap free=%u max=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void loop() {
    lv_timer_handler();
    led_tick();
    web_portal_loop();
    handleSpotifyCode();
    handleBootButton();
    handleWifiReset();
    handleRescueAp();

    if (appState == READY && spotify_auth_lost()) enterNoSpotify();

    if (appState == WIFI_SETUP_MODE && config_has_networks() && WiFi.softAPgetStationNum() == 0 &&
        millis() - apStartedAt > AP_RETRY_STA_MS) {
        ESP.restart();
    }

    if (appState == READY && millis() - lastTick >= UI_TICK_MS) {
        lastTick = millis();
        updatePlayerUi();
    }
    if (millis() - lastHeapCheck >= HEAP_CHECK_MS) {
        lastHeapCheck = millis();
        checkHeap();
    }
    delay(5);
}
