#include "config.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

static const char *CONFIG_PATH = "/config.json";

AppConfig g_config;

void config_load() {
    File file = LittleFS.open(CONFIG_PATH, "r");
    if (!file) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    if (err) {
        Serial.printf("Config parse error: %s\n", err.c_str());
        return;
    }

    g_config.ssid = doc["ssid"] | "";
    g_config.pass = doc["pass"] | "";
    g_config.clientId = doc["client_id"] | "";
    g_config.artSize = doc["art_size"] | 300;
    g_config.refreshToken = doc["refresh_token"] | "";
}

bool config_save() {
    JsonDocument doc;
    doc["ssid"] = g_config.ssid;
    doc["pass"] = g_config.pass;
    doc["client_id"] = g_config.clientId;
    doc["art_size"] = g_config.artSize;
    doc["refresh_token"] = g_config.refreshToken;

    File file = LittleFS.open(CONFIG_PATH, "w");
    if (!file) return false;
    serializeJson(doc, file);
    file.close();
    return true;
}
