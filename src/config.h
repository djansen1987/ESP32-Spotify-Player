#pragma once
#include <Arduino.h>
#include <vector>

constexpr size_t MAX_PINS = 20;
constexpr size_t MAX_NETWORKS = 8;

struct Pin {
    String id;
    String name;
};

struct WifiNet {
    String ssid;
    String pass;
};

struct AppConfig {
    std::vector<WifiNet> networks;
    String clientId;
    int artSize = 300;
    int ledBrightness = 40;
    String webPassHash;
    String webPassSalt;
    String refreshToken;
    std::vector<Pin> pins;
};

extern AppConfig g_config;
extern volatile bool g_internetOk;

void config_load();
bool config_save();

std::vector<WifiNet> config_get_networks();
bool config_has_networks();
void config_add_network(const String &ssid, const String &pass);
void config_promote_network(const String &ssid);
void config_remove_network(const String &ssid);
void config_clear_networks();

bool config_web_password_set();
void config_set_web_password(const String &password);
void config_clear_web_password();
bool config_check_web_password(const String &password);

std::vector<Pin> config_get_pins();
bool config_add_pin(const String &id, const String &name);
void config_remove_pin(const String &id);
void config_move_pin_up(const String &id);
void config_rename_pin(const String &id, const String &name);
