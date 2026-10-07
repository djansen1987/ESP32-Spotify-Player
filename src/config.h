#pragma once
#include <Arduino.h>

struct AppConfig {
    String ssid;
    String pass;
    String clientId;
    int artSize = 300;
    String refreshToken;
};

extern AppConfig g_config;

void config_load();
bool config_save();
