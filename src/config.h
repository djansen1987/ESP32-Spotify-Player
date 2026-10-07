#pragma once
#include <Arduino.h>
#include <vector>

constexpr size_t MAX_PINS = 20;

struct Pin {
    String id;
    String name;
};

struct AppConfig {
    String ssid;
    String pass;
    String clientId;
    int artSize = 300;
    String refreshToken;
    std::vector<Pin> pins;
};

extern AppConfig g_config;

void config_load();
bool config_save();

std::vector<Pin> config_get_pins();
bool config_add_pin(const String &id, const String &name);
void config_remove_pin(const String &id);
void config_move_pin_up(const String &id);
void config_rename_pin(const String &id, const String &name);
