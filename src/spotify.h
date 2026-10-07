#pragma once
#include <Arduino.h>

#define SPOTIFY_REDIRECT_URI "http://127.0.0.1:8080/callback"
#define SPOTIFY_ART_PATH "/art.jpg"

enum SpotifyCmd : uint8_t {
    CMD_PLAY_PAUSE = 1,
    CMD_NEXT,
    CMD_PREV,
    CMD_VOL_UP,
    CMD_VOL_DOWN,
};

struct PlayerState {
    bool active = false;
    bool playing = false;
    String trackId;
    String title;
    String artist;
    String artUrl;
    String artUrlSmall;
    String message;
    uint32_t progressMs = 0;
    uint32_t durationMs = 0;
    int volume = -1;
    uint32_t stamp = 0;
};

void spotify_init();
void spotify_start();
bool spotify_exchange_code(const String &code, const String &verifier, String &error);
void spotify_send(SpotifyCmd cmd);
void spotify_get_state(PlayerState &out);
bool spotify_art_begin_read();
void spotify_art_end_read();
bool spotify_auth_lost();
