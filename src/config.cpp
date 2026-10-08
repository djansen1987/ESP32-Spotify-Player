#include "config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <algorithm>
#include <mbedtls/sha256.h>

static const char *CONFIG_PATH = "/config.json";

AppConfig g_config;

static SemaphoreHandle_t configLock() {
    static SemaphoreHandle_t lock = xSemaphoreCreateRecursiveMutex();
    return lock;
}

struct ScopedLock {
    ScopedLock() { xSemaphoreTakeRecursive(configLock(), portMAX_DELAY); }
    ~ScopedLock() { xSemaphoreGiveRecursive(configLock()); }
};

void config_load() {
    ScopedLock lock;
    File file = LittleFS.open(CONFIG_PATH, "r");
    if (!file) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
        Serial.printf("Config parse error: %s\n", err.c_str());
        return;
    }

    g_config.networks.clear();
    for (JsonObject o : doc["wifi"].as<JsonArray>()) {
        if (g_config.networks.size() >= MAX_NETWORKS) break;
        WifiNet net;
        net.ssid = (const char *)(o["ssid"] | "");
        net.pass = (const char *)(o["pass"] | "");
        if (net.ssid.length()) g_config.networks.push_back(std::move(net));
    }
    // Older firmware stored a single network.
    const char *legacySsid = doc["ssid"] | "";
    if (g_config.networks.empty() && legacySsid[0]) {
        g_config.networks.push_back({legacySsid, (const char *)(doc["pass"] | "")});
    }
    g_config.clientId = doc["client_id"] | "";
    g_config.artSize = doc["art_size"] | 300;
    g_config.ledBrightness = constrain((int)(doc["led_brightness"] | 40), 0, 100);
    g_config.refreshToken = doc["refresh_token"] | "";
    g_config.webPassHash = doc["web_pass_hash"] | "";
    g_config.webPassSalt = doc["web_pass_salt"] | "";

    g_config.pins.clear();
    for (JsonObject o : doc["pins"].as<JsonArray>()) {
        if (g_config.pins.size() >= MAX_PINS) break;
        Pin pin;
        pin.id = (const char *)(o["id"] | "");
        pin.name = (const char *)(o["name"] | "");
        if (pin.id.length()) g_config.pins.push_back(std::move(pin));
    }
}

bool config_save() {
    ScopedLock lock;
    JsonDocument doc;
    JsonArray wifi = doc["wifi"].to<JsonArray>();
    for (const WifiNet &net : g_config.networks) {
        JsonObject o = wifi.add<JsonObject>();
        o["ssid"] = net.ssid;
        o["pass"] = net.pass;
    }
    doc["client_id"] = g_config.clientId;
    doc["art_size"] = g_config.artSize;
    doc["led_brightness"] = g_config.ledBrightness;
    doc["refresh_token"] = g_config.refreshToken;
    doc["web_pass_hash"] = g_config.webPassHash;
    doc["web_pass_salt"] = g_config.webPassSalt;
    JsonArray pins = doc["pins"].to<JsonArray>();
    for (const Pin &pin : g_config.pins) {
        JsonObject o = pins.add<JsonObject>();
        o["id"] = pin.id;
        o["name"] = pin.name;
    }

    File file = LittleFS.open(CONFIG_PATH, "w");
    if (!file) return false;
    serializeJson(doc, file);
    file.close();
    return true;
}

static String hashPassword(const String &salt, const String &password) {
    String input = salt + password;
    uint8_t digest[32];
    mbedtls_sha256((const unsigned char *)input.c_str(), input.length(), digest, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return String(hex);
}

bool config_web_password_set() {
    ScopedLock lock;
    return g_config.webPassHash.length() > 0;
}

void config_set_web_password(const String &password) {
    ScopedLock lock;
    char salt[17];
    snprintf(salt, sizeof(salt), "%08x%08x", (unsigned)esp_random(), (unsigned)esp_random());
    g_config.webPassSalt = salt;
    g_config.webPassHash = hashPassword(g_config.webPassSalt, password);
    config_save();
}

void config_clear_web_password() {
    ScopedLock lock;
    g_config.webPassHash = "";
    g_config.webPassSalt = "";
    config_save();
}

bool config_check_web_password(const String &password) {
    ScopedLock lock;
    if (g_config.webPassHash.isEmpty()) return true;
    String candidate = hashPassword(g_config.webPassSalt, password);
    if (candidate.length() != g_config.webPassHash.length()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < candidate.length(); i++) diff |= candidate[i] ^ g_config.webPassHash[i];
    return diff == 0;
}

std::vector<WifiNet> config_get_networks() {
    ScopedLock lock;
    return g_config.networks;
}

bool config_has_networks() {
    ScopedLock lock;
    return !g_config.networks.empty();
}

void config_add_network(const String &ssid, const String &pass) {
    ScopedLock lock;
    for (size_t i = 0; i < g_config.networks.size(); i++) {
        if (g_config.networks[i].ssid == ssid) {
            g_config.networks.erase(g_config.networks.begin() + i);
            break;
        }
    }
    g_config.networks.insert(g_config.networks.begin(), {ssid, pass});
    if (g_config.networks.size() > MAX_NETWORKS) g_config.networks.pop_back();
    config_save();
}

void config_promote_network(const String &ssid) {
    ScopedLock lock;
    for (size_t i = 1; i < g_config.networks.size(); i++) {
        if (g_config.networks[i].ssid == ssid) {
            std::rotate(g_config.networks.begin(), g_config.networks.begin() + i, g_config.networks.begin() + i + 1);
            config_save();
            return;
        }
    }
}

void config_remove_network(const String &ssid) {
    ScopedLock lock;
    for (size_t i = 0; i < g_config.networks.size(); i++) {
        if (g_config.networks[i].ssid == ssid) {
            g_config.networks.erase(g_config.networks.begin() + i);
            config_save();
            return;
        }
    }
}

void config_clear_networks() {
    ScopedLock lock;
    g_config.networks.clear();
    config_save();
}

std::vector<Pin> config_get_pins() {
    ScopedLock lock;
    return g_config.pins;
}

bool config_add_pin(const String &id, const String &name) {
    ScopedLock lock;
    for (const Pin &pin : g_config.pins) {
        if (pin.id == id) return true;
    }
    if (g_config.pins.size() >= MAX_PINS) return false;
    g_config.pins.push_back({id, name});
    return config_save();
}

void config_remove_pin(const String &id) {
    ScopedLock lock;
    for (size_t i = 0; i < g_config.pins.size(); i++) {
        if (g_config.pins[i].id == id) {
            g_config.pins.erase(g_config.pins.begin() + i);
            config_save();
            return;
        }
    }
}

void config_move_pin_up(const String &id) {
    ScopedLock lock;
    for (size_t i = 1; i < g_config.pins.size(); i++) {
        if (g_config.pins[i].id == id) {
            std::swap(g_config.pins[i], g_config.pins[i - 1]);
            config_save();
            return;
        }
    }
}

void config_rename_pin(const String &id, const String &name) {
    ScopedLock lock;
    for (Pin &pin : g_config.pins) {
        if (pin.id == id) {
            pin.name = name;
            config_save();
            return;
        }
    }
}
