#pragma once
#include <Arduino.h>
#include <vector>

#define SPOTIFY_REDIRECT_URI "http://127.0.0.1:8080/callback"
#define SPOTIFY_ART_PATH "/art.jpg"
#define SPOTIFY_SCOPES "user-read-playback-state user-modify-playback-state playlist-read-private"

enum SpotifyCmd : uint8_t {
    CMD_PLAY_PAUSE = 1,
    CMD_NEXT,
    CMD_PREV,
    CMD_VOL_UP,
    CMD_VOL_DOWN,
    CMD_LIST_DEVICES,
    CMD_LIST_PLAYLISTS,
    CMD_LIST_WEB_PLAYLISTS,
    CMD_LOOKUP_PLAYLIST,
    CMD_SEARCH_PLAYLISTS,
    CMD_SEEK,
    CMD_TRANSFER,
    CMD_PLAY_CONTEXT,
};

enum ListKind : uint8_t { LIST_DEVICES, LIST_PLAYLISTS, LIST_WEB_PLAYLISTS, LIST_WEB_LOOKUP, LIST_WEB_SEARCH };
enum ListStatus : uint8_t { LIST_IDLE, LIST_LOADING, LIST_READY, LIST_ERROR };

struct ListItem {
    String id;
    String name;
    bool active = false;
    bool pinned = false;
    String owner;
};

struct ListData {
    ListStatus status = LIST_IDLE;
    std::vector<ListItem> items;
    String error;
    String key;
    uint32_t version = 0;
    uint32_t offset = 0;
    uint32_t next = 0;
    uint32_t total = 0;
};

struct PlayerState {
    bool active = false;
    bool playing = false;
    bool isExplicit = false;
    String trackId;
    String title;
    String artist;
    String album;
    String albumType;
    String releaseDate;
    String isrc;
    String deviceName;
    String deviceType;
    String artUrl;
    String artUrlSmall;
    String message;
    uint16_t trackNumber = 0;
    uint16_t discNumber = 0;
    uint16_t totalTracks = 0;
    uint32_t progressMs = 0;
    uint32_t durationMs = 0;
    int volume = -1;
    uint32_t stamp = 0;
};

void spotify_init();
void spotify_start();
bool spotify_exchange_code(const String &code, const String &verifier, String &error);
void spotify_send(SpotifyCmd cmd);
void spotify_seek(uint32_t positionMs);
void spotify_request_list(ListKind kind, uint32_t offset = 0, const String &query = "");
void spotify_select(ListKind kind, const String &id);
void spotify_get_list(ListKind kind, ListData &out);
void spotify_get_state(PlayerState &out);
bool spotify_art_begin_read();
void spotify_art_end_read();
bool spotify_auth_lost();
uint32_t spotify_last_poll_ms();
uint32_t spotify_list_version(ListKind kind);
bool spotify_token_valid();
