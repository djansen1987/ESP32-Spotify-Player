#pragma once
#include <stdint.h>

void led_init();
// 0xRRGGBB, 0 turns the LED off; the LED fades to the new colour.
void led_set_color(uint32_t rgb);
void led_set_brightness(int percent);
void led_tick();
