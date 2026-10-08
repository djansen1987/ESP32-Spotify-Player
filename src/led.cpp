#include "led.h"
#include "config.h"
#include <Arduino.h>
#include <math.h>

namespace {

// The CYD LED is common-anode, so a low duty cycle means bright.
constexpr int PINS[3] = {4, 16, 17};
constexpr int CHANNELS[3] = {2, 3, 4};
constexpr int PWM_BITS = 12;
constexpr int PWM_MAX = (1 << PWM_BITS) - 1;
constexpr uint32_t TICK_MS = 10;
constexpr float FADE_PER_TICK = 0.025f;

float current[3] = {0, 0, 0};
float target[3] = {0, 0, 0};
float currentBrightness = 0;
volatile float targetBrightness = 0;
int lastDuty[3] = {-1, -1, -1};
uint32_t lastTick = 0;

float approach(float value, float goal, float alpha) {
    float diff = goal - value;
    return fabsf(diff) < 0.4f ? goal : value + diff * alpha;
}

} // namespace

void led_init() {
    for (int i = 0; i < 3; i++) {
        ledcSetup(CHANNELS[i], 5000, PWM_BITS);
        ledcAttachPin(PINS[i], CHANNELS[i]);
        ledcWrite(CHANNELS[i], PWM_MAX);
    }
    targetBrightness = g_config.ledBrightness;
}

void led_set_color(uint32_t rgb) {
    target[0] = (rgb >> 16) & 0xFF;
    target[1] = (rgb >> 8) & 0xFF;
    target[2] = rgb & 0xFF;
}

void led_set_brightness(int percent) {
    targetBrightness = constrain(percent, 0, 100);
}

void led_tick() {
    uint32_t now = millis();
    if (now - lastTick < TICK_MS) return;
    float ticks = min<float>((now - lastTick) / (float)TICK_MS, 10.0f);
    lastTick = now;

    float alpha = 1.0f - powf(1.0f - FADE_PER_TICK, ticks);
    for (int i = 0; i < 3; i++) current[i] = approach(current[i], target[i], alpha);
    currentBrightness = approach(currentBrightness, targetBrightness, alpha);

    for (int i = 0; i < 3; i++) {
        // Gamma keeps the fade perceptually even.
        float linear = powf(current[i] / 255.0f, 2.2f) * (currentBrightness / 100.0f);
        int duty = PWM_MAX - (int)lroundf(linear * PWM_MAX);
        if (duty != lastDuty[i]) {
            lastDuty[i] = duty;
            ledcWrite(CHANNELS[i], duty);
        }
    }
}
