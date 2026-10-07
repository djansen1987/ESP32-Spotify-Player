#include "config.h"
#include "app_log.h"
#include "display.h"
#include "spotify.h"
#include "ui.h"
#include "web_portal.h"
#include <Arduino.h>
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
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
static const uint32_t AP_RETRY_STA_MS = 5UL * 60 * 1000;
static const uint32_t UI_TICK_MS = 250;
static const uint32_t HEAP_CHECK_MS = 5000;
static const uint32_t LOW_HEAP_BYTES = 25000;

static AppState appState = WIFI_SETUP_MODE;
static uint32_t apStartedAt = 0;
static uint32_t lastTick = 0;
static uint32_t lastHeapCheck = 0;

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
        t += "Gateway: " + WiFi.gatewayIP().toString() + "\n";
        t += "SSID: " + WiFi.SSID() + " (ch " + String(WiFi.channel()) + ")\n";
        t += "Signal: " + String(rssi) + " dBm (" + quality + ")\n";
    } else {
        t += "Wi-Fi not connected\n";
        t += "Setup IP: " + WiFi.softAPIP().toString() + "\n";
        t += "Stored SSID: " + g_config.ssid + "\n";
        t += "Clients on setup AP: " + String(WiFi.softAPgetStationNum()) + "\n";
    }
    t += "MAC: " + WiFi.macAddress() + "\n";
    uint32_t up = millis() / 1000;
    char buf[64];
    snprintf(buf, sizeof(buf), "Up %uh %02um, reset: %s\n", (unsigned)(up / 3600), (unsigned)((up / 60) % 60), resetReasonName());
    t += buf;
    snprintf(buf, sizeof(buf), "Heap: %uK free, %uK block\n", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024));
    t += buf;
    snprintf(buf, sizeof(buf), "Spotify: %s, poll %u ms", spotify_token_valid() ? "token ok" : "no token", (unsigned)spotify_last_poll_ms());
    t += buf;
    return t;
}

static bool connectStation() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(g_config.ssid.c_str(), g_config.pass.c_str());
    ui_set_setup_status("Connecting to Wi-Fi...");

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        lv_timer_handler();
        delay(50);
    }
    return WiFi.status() == WL_CONNECTED;
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

void setup() {
    Serial.begin(115200);
    log_init();
    logResetReason();
#if CONFIG_BT_ENABLED
    esp_bt_mem_release(ESP_BT_MODE_BTDM);
#endif

    if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
    config_load();

    display_init();
    ui_init();
    ui_set_info_provider(deviceInfo);
    show_setup_screen();
    spotify_init();

    if (g_config.ssid.length() && connectStation()) {
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
    web_portal_loop();
    handleSpotifyCode();

    if (appState == READY && spotify_auth_lost()) enterNoSpotify();

    if (appState == WIFI_SETUP_MODE && g_config.ssid.length() && WiFi.softAPgetStationNum() == 0 &&
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
