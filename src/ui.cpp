#include "ui.h"
#include "app_log.h"
#include <TJpg_Decoder.h>
#include <LittleFS.h>
#include <lvgl.h>

LV_FONT_DECLARE(font_ui_14)
LV_FONT_DECLARE(font_ui_20)

namespace {

constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr int BG_MAX_W = 120;
constexpr int BG_MAX_H = (BG_MAX_W * SCREEN_H + SCREEN_W - 1) / SCREEN_W + 1;
constexpr uint32_t OPTIMISTIC_MS = 2500;

const lv_color_t DEFAULT_ACCENT = lv_color_hex(0x1DB954);
const lv_color_t DEFAULT_TINT = lv_color_hex(0x1B1B3A);
const lv_color_t BG_BOTTOM = lv_color_hex(0x07070D);
const lv_color_t TEXT_MAIN = lv_color_hex(0xFFFFFF);
const lv_color_t TEXT_DIM = lv_color_hex(0xE0E0EC);

lv_obj_t *screenSetup = nullptr;
lv_obj_t *lblSetupInstructions;
lv_obj_t *lblSetupIp;

lv_obj_t *screenPlayer = nullptr;
lv_obj_t *bgImg;
lv_obj_t *dimLayer;
lv_obj_t *lblTitle;
lv_obj_t *lblArtist;
lv_obj_t *lblMessage;
lv_obj_t *lblVolume;
lv_obj_t *lblBrand;
lv_obj_t *barProgress;
lv_obj_t *lblElapsed;
lv_obj_t *lblTotal;
lv_obj_t *btnPlay;
lv_obj_t *lblPlayPause;

lv_color_t *bgPixels = nullptr;
lv_img_dsc_t bgDsc;
int bgW = BG_MAX_W;
int bgH = BG_MAX_H;
int cropX = 0;
int cropY = 0;
uint32_t sumR, sumG, sumB, pixelCount;

String shownTrackId;
String shownTitle;
String shownArtist;
String shownMessage;
bool shownPlaying = false;
int shownVolume = -2;
int shownElapsedSec = -1;
int shownTotalSec = -1;
uint32_t optimisticUntil = 0;

bool optimisticActive() { return (int32_t)(millis() - optimisticUntil) < 0; }

void applyGlass(lv_obj_t *o, int radius, lv_opa_t fill) {
    lv_obj_set_style_bg_color(o, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(o, fill, 0);
    lv_obj_set_style_border_color(o, lv_color_white(), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_40, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *makeGlass(lv_obj_t *parent, int x, int y, int w, int h, int radius, lv_opa_t fill = LV_OPA_10) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    applyGlass(o, radius, fill);
    return o;
}

void styleScreen(lv_obj_t *scr, lv_color_t top) {
    lv_obj_set_style_bg_color(scr, top, 0);
    lv_obj_set_style_bg_grad_color(scr, BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *makeLabel(lv_obj_t *parent, int x, int y, int w, lv_color_t color, const lv_font_t *font) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    if (w > 0) lv_obj_set_width(lbl, w);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    return lbl;
}

lv_obj_t *makeTextBlock(int y, int h, lv_color_t color, const lv_font_t *font) {
    lv_obj_t *lbl = makeLabel(screenPlayer, 16, y, SCREEN_W - 32, color, font);
    lv_obj_set_height(lbl, h);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl, "");
    return lbl;
}

void applyTheme(lv_color_t accent, lv_color_t tint) {
    styleScreen(screenPlayer, tint);
    lv_obj_set_style_bg_color(barProgress, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(btnPlay, accent, 0);
    lv_obj_set_style_text_color(lblVolume, accent, 0);
    lv_obj_set_style_text_color(lblBrand, accent, 0);
}

void setBackgroundVisible(bool visible) {
    if (visible) {
        lv_obj_clear_flag(bgImg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(bgImg, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_bg_opa(dimLayer, visible ? LV_OPA_60 : LV_OPA_TRANSP, 0);
}

void buildSetupUi() {
    screenSetup = lv_obj_create(nullptr);
    styleScreen(screenSetup, DEFAULT_TINT);

    lv_obj_t *title = makeLabel(screenSetup, 0, 0, 0, DEFAULT_ACCENT, &font_ui_20);
    lv_label_set_text(title, "Device Setup");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *card = makeGlass(screenSetup, 16, 56, 288, 168, 22);

    lblSetupInstructions = makeLabel(card, 0, 0, 256, TEXT_MAIN, &font_ui_14);
    lv_obj_set_style_text_align(lblSetupInstructions, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lblSetupInstructions, LV_ALIGN_TOP_MID, 0, 18);

    lblSetupIp = makeLabel(card, 0, 0, 256, DEFAULT_ACCENT, &font_ui_20);
    lv_obj_set_style_text_align(lblSetupIp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lblSetupIp, LV_ALIGN_BOTTOM_MID, 0, -18);
}

void setPlayIcon() {
    lv_label_set_text(lblPlayPause, shownPlaying ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void setVolumeLabel() {
    char buf[24];
    if (shownVolume < 0) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MID " --");
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MID " %d%%", shownVolume);
    }
    lv_label_set_text(lblVolume, buf);
}

void onButton(lv_event_t *e) {
    SpotifyCmd cmd = (SpotifyCmd)(uintptr_t)lv_event_get_user_data(e);
    if (cmd == CMD_PLAY_PAUSE) {
        shownPlaying = !shownPlaying;
        setPlayIcon();
        optimisticUntil = millis() + OPTIMISTIC_MS;
    } else if ((cmd == CMD_VOL_UP || cmd == CMD_VOL_DOWN) && shownVolume >= 0) {
        shownVolume = constrain(shownVolume + (cmd == CMD_VOL_UP ? 10 : -10), 0, 100);
        setVolumeLabel();
        optimisticUntil = millis() + OPTIMISTIC_MS;
    }
    spotify_send(cmd);
}

lv_obj_t *makeButton(lv_obj_t *parent, int x, int w, SpotifyCmd cmd, const char *text) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, 26);
    lv_obj_set_size(btn, w, 34);
    applyGlass(btn, LV_RADIUS_CIRCLE, LV_OPA_20);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, onButton, LV_EVENT_CLICKED, (void *)(uintptr_t)cmd);

    lv_obj_t *lbl = makeLabel(btn, 0, 0, 0, TEXT_MAIN, &font_ui_14);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

void buildPlayerUi() {
    screenPlayer = lv_obj_create(nullptr);
    styleScreen(screenPlayer, DEFAULT_TINT);

    bgDsc.header.always_zero = 0;
    bgDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    bgDsc.header.w = bgW;
    bgDsc.header.h = bgH;
    bgDsc.data_size = bgW * bgH * sizeof(lv_color_t);
    bgDsc.data = (const uint8_t *)bgPixels;

    bgImg = lv_img_create(screenPlayer);
    lv_img_set_src(bgImg, &bgDsc);
    lv_obj_add_flag(bgImg, LV_OBJ_FLAG_HIDDEN);

    dimLayer = lv_obj_create(screenPlayer);
    lv_obj_set_size(dimLayer, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(dimLayer, 0, 0);
    lv_obj_set_style_bg_color(dimLayer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dimLayer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dimLayer, 0, 0);
    lv_obj_set_style_radius(dimLayer, 0, 0);
    lv_obj_clear_flag(dimLayer, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lblVolume = makeLabel(screenPlayer, 14, 8, 100, DEFAULT_ACCENT, &font_ui_14);
    lblBrand = makeLabel(screenPlayer, SCREEN_W - 114, 8, 100, DEFAULT_ACCENT, &font_ui_14);
    lv_obj_set_style_text_align(lblBrand, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lblBrand, "Spotify");

    lblTitle = makeTextBlock(34, 52, TEXT_MAIN, &font_ui_20);
    lblArtist = makeTextBlock(90, 20, TEXT_DIM, &font_ui_14);
    lblMessage = makeTextBlock(116, 36, lv_color_hex(0xFFB347), &font_ui_14);

    lv_obj_t *panel = makeGlass(screenPlayer, 10, 170, 300, 64, 24, LV_OPA_20);

    lblElapsed = makeLabel(panel, 12, 3, 36, TEXT_DIM, &font_ui_14);
    lv_label_set_text(lblElapsed, "0:00");

    barProgress = lv_bar_create(panel);
    lv_obj_set_pos(barProgress, 52, 11);
    lv_obj_set_size(barProgress, 196, 6);
    lv_bar_set_range(barProgress, 0, 1000);
    lv_obj_set_style_bg_color(barProgress, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(barProgress, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(barProgress, DEFAULT_ACCENT, LV_PART_INDICATOR);

    lblTotal = makeLabel(panel, 252, 3, 36, TEXT_DIM, &font_ui_14);
    lv_obj_set_style_text_align(lblTotal, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lblTotal, "0:00");

    makeButton(panel, 9, 46, CMD_VOL_DOWN, LV_SYMBOL_VOLUME_MID);
    makeButton(panel, 64, 46, CMD_PREV, LV_SYMBOL_PREV);
    btnPlay = makeButton(panel, 119, 62, CMD_PLAY_PAUSE, LV_SYMBOL_PLAY);
    lv_obj_set_style_bg_opa(btnPlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btnPlay, 0, 0);
    lblPlayPause = lv_obj_get_child(btnPlay, 0);
    lv_obj_set_style_text_color(lblPlayPause, BG_BOTTOM, 0);
    makeButton(panel, 190, 46, CMD_NEXT, LV_SYMBOL_NEXT);
    makeButton(panel, 245, 46, CMD_VOL_UP, LV_SYMBOL_VOLUME_MAX);

    setVolumeLabel();
    applyTheme(DEFAULT_ACCENT, DEFAULT_TINT);
}

void formatTime(char *out, size_t size, int seconds) {
    snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

bool artCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    for (int row = 0; row < h; row++) {
        int by = y + row - cropY;
        if (by < 0 || by >= bgH) continue;
        for (int col = 0; col < w; col++) {
            int bx = x + col - cropX;
            if (bx < 0 || bx >= bgW) continue;
            uint16_t p = bitmap[row * w + col];
            bgPixels[by * bgW + bx].full = p;
            sumR += (p >> 11) & 0x1F;
            sumG += (p >> 5) & 0x3F;
            sumB += p & 0x1F;
            pixelCount++;
        }
    }
    return true;
}

void themeFromArt() {
    if (!pixelCount) return;
    lv_color_t avg = lv_color_make(((sumR / pixelCount) * 255) / 31, ((sumG / pixelCount) * 255) / 63, ((sumB / pixelCount) * 255) / 31);
    lv_color_hsv_t hsv = lv_color_to_hsv(avg);
    uint8_t accentSat = hsv.s < 20 ? hsv.s : min<int>(100, hsv.s + 25);
    applyTheme(lv_color_hsv_to_rgb(hsv.h, accentSat, 95), lv_color_hsv_to_rgb(hsv.h, min<int>(100, hsv.s + 10), 30));
}

} // namespace

void ui_init() {
    bgPixels = (lv_color_t *)malloc(BG_MAX_W * BG_MAX_H * sizeof(lv_color_t));
    TJpgDec.setCallback(artCallback);
    buildSetupUi();
    buildPlayerUi();
}

void show_setup_screen() {
    lv_scr_load(screenSetup);
}

void update_setup_screen(const char *ip_address, bool is_ap_mode) {
    if (is_ap_mode) {
        lv_label_set_text(lblSetupInstructions, "1. Connect to Wi-Fi:\n'Spotify-Player-Setup'\n\n2. Open browser to:");
    } else {
        lv_label_set_text(lblSetupInstructions, "Wi-Fi Connected!\n\nTo configure Spotify,\nopen browser to:");
    }
    lv_label_set_text(lblSetupIp, ip_address);
}

void ui_set_setup_status(const char *text) {
    lv_label_set_text(lblSetupInstructions, text);
    lv_label_set_text(lblSetupIp, "");
}

void show_player_screen() {
    lv_scr_load(screenPlayer);
}

void ui_update_player(const PlayerState &s) {
    String title = s.active ? s.title : "Nothing playing";
    String artist = s.active ? s.artist : "";
    if (title != shownTitle) {
        shownTitle = title;
        lv_label_set_text(lblTitle, title.c_str());
    }
    if (artist != shownArtist) {
        shownArtist = artist;
        lv_label_set_text(lblArtist, artist.c_str());
    }
    if (s.message != shownMessage) {
        shownMessage = s.message;
        lv_label_set_text(lblMessage, s.message.c_str());
    }
    if (!optimisticActive()) {
        if (s.playing != shownPlaying) {
            shownPlaying = s.playing;
            setPlayIcon();
        }
        if (s.volume != shownVolume) {
            shownVolume = s.volume;
            setVolumeLabel();
        }
    }
    if (s.trackId != shownTrackId) {
        shownTrackId = s.trackId;
        if (s.artUrl.isEmpty()) {
            setBackgroundVisible(false);
            applyTheme(DEFAULT_ACCENT, DEFAULT_TINT);
        }
    }

    uint32_t progress = s.progressMs;
    if (s.playing) progress += millis() - s.stamp;
    if (s.durationMs && progress > s.durationMs) progress = s.durationMs;

    lv_bar_set_value(barProgress, s.durationMs ? (int32_t)((uint64_t)progress * 1000 / s.durationMs) : 0, LV_ANIM_OFF);

    int elapsed = progress / 1000;
    if (elapsed != shownElapsedSec) {
        shownElapsedSec = elapsed;
        char buf[12];
        formatTime(buf, sizeof(buf), elapsed);
        lv_label_set_text(lblElapsed, buf);
    }
    int total = s.durationMs / 1000;
    if (total != shownTotalSec) {
        shownTotalSec = total;
        char buf[12];
        formatTime(buf, sizeof(buf), total);
        lv_label_set_text(lblTotal, buf);
    }
}

void ui_show_art() {
    if (!bgPixels) {
        app_log(LL_ERROR, "Art: no background buffer");
        return;
    }
    uint16_t w = 0, h = 0;
    JRESULT res = TJpgDec.getFsJpgSize(&w, &h, SPOTIFY_ART_PATH, LittleFS);
    if (res != JDR_OK) {
        app_log(LL_ERROR, "Art: JPEG header error %d", res);
        return;
    }

    uint8_t scale = 1;
    while (scale < 8 && w / (scale * 2) >= BG_MAX_W) scale *= 2;
    TJpgDec.setJpgScale(scale);

    int decodedW = w / scale;
    int decodedH = h / scale;
    bgW = min(decodedW, BG_MAX_W);
    bgH = min(decodedH, (bgW * SCREEN_H + SCREEN_W - 1) / SCREEN_W + 1);
    cropX = (decodedW - bgW) / 2;
    cropY = (decodedH - bgH) / 2;
    sumR = sumG = sumB = pixelCount = 0;

    res = TJpgDec.drawFsJpg(0, 0, SPOTIFY_ART_PATH, LittleFS);
    if (res != JDR_OK) {
        app_log(LL_ERROR, "Art: decode error %d", res);
        return;
    }

    bgDsc.header.w = bgW;
    bgDsc.header.h = bgH;
    bgDsc.data_size = bgW * bgH * sizeof(lv_color_t);
    lv_img_set_src(bgImg, &bgDsc);
    lv_img_set_antialias(bgImg, true);
    lv_img_set_zoom(bgImg, (SCREEN_W * 256) / bgW);
    lv_obj_set_pos(bgImg, (SCREEN_W - bgW) / 2, (SCREEN_H - bgH) / 2);
    setBackgroundVisible(true);
    lv_obj_invalidate(screenPlayer);
    themeFromArt();
}
