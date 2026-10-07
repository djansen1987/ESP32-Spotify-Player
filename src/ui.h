#pragma once
#include "spotify.h"
#include <Arduino.h>

void ui_init();
void ui_set_info_provider(String (*provider)());
void show_setup_screen();
void update_setup_screen(const char *ip_address, bool is_ap_mode);
void ui_set_setup_status(const char *text);
void show_player_screen();
void ui_update_player(const PlayerState &state);
void ui_show_art();
