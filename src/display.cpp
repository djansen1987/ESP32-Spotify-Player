#include "display.h"
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>

namespace {

constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr int DRAW_BUF_LINES = 10;

constexpr int TS_CS = 33;
constexpr int TS_IRQ = 36;
constexpr int TS_CLK = 25;
constexpr int TS_MOSI = 32;
constexpr int TS_MISO = 39;

TFT_eSPI tft;
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(TS_CS, TS_IRQ);

lv_disp_draw_buf_t drawBuf;
lv_color_t drawPixels[SCREEN_W * DRAW_BUF_LINES];
lv_disp_drv_t dispDrv;
lv_indev_drv_t indevDrv;

void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&pixels->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(drv);
}

void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
    if (touch.tirqTouched() && touch.touched()) {
        TS_Point p = touch.getPoint();
        data->point.x = constrain(map(p.x, 200, 3700, 0, SCREEN_W - 1), 0, SCREEN_W - 1);
        data->point.y = constrain(map(p.y, 240, 3800, 0, SCREEN_H - 1), 0, SCREEN_H - 1);
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

} // namespace

void display_init() {
    lv_init();

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    touchSPI.begin(TS_CLK, TS_MISO, TS_MOSI, TS_CS);
    touch.begin(touchSPI);
    touch.setRotation(1);

    lv_disp_draw_buf_init(&drawBuf, drawPixels, nullptr, SCREEN_W * DRAW_BUF_LINES);
    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res = SCREEN_W;
    dispDrv.ver_res = SCREEN_H;
    dispDrv.flush_cb = flushCb;
    dispDrv.draw_buf = &drawBuf;
    lv_disp_drv_register(&dispDrv);

    lv_indev_drv_init(&indevDrv);
    indevDrv.type = LV_INDEV_TYPE_POINTER;
    indevDrv.read_cb = touchReadCb;
    indevDrv.long_press_time = 700;
    lv_indev_drv_register(&indevDrv);
}
