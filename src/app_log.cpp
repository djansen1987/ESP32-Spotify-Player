#include "app_log.h"
#include <ArduinoJson.h>
#include <stdarg.h>

namespace {

constexpr size_t MAX_ENTRIES = 30;
constexpr size_t TEXT_LEN = 160;

struct Entry {
    uint32_t ms;
    uint8_t level;
    char text[TEXT_LEN];
};

Entry entries[MAX_ENTRIES];
size_t nextSlot = 0;
size_t count = 0;
bool debugOn = false;
SemaphoreHandle_t logLock = nullptr;

SemaphoreHandle_t lockHandle() { return logLock; }

const char LEVEL_CHARS[] = {'D', 'I', 'W', 'E'};

} // namespace

void log_init() { logLock = xSemaphoreCreateMutex(); }

void app_log(LogLevel level, const char *fmt, ...) {
    if (level < LL_WARN && !debugOn) return;
    if (!logLock) return;

    char text[TEXT_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    Serial.printf("[%c] %s\n", LEVEL_CHARS[level], text);

    xSemaphoreTake(lockHandle(), portMAX_DELAY);
    Entry &e = entries[nextSlot];
    e.ms = millis();
    e.level = level;
    memcpy(e.text, text, sizeof(text));
    nextSlot = (nextSlot + 1) % MAX_ENTRIES;
    if (count < MAX_ENTRIES) count++;
    xSemaphoreGive(lockHandle());
}

void log_set_debug(bool on) { debugOn = on; }

bool log_debug_enabled() { return debugOn; }

void log_clear() {
    xSemaphoreTake(lockHandle(), portMAX_DELAY);
    nextSlot = 0;
    count = 0;
    xSemaphoreGive(lockHandle());
}

String log_to_json() {
    JsonDocument doc;
    doc["debug"] = debugOn;
    JsonArray list = doc["entries"].to<JsonArray>();

    xSemaphoreTake(lockHandle(), portMAX_DELAY);
    for (size_t i = 0; i < count; i++) {
        size_t idx = (nextSlot + MAX_ENTRIES - 1 - i) % MAX_ENTRIES;
        const Entry &e = entries[idx];
        JsonObject o = list.add<JsonObject>();
        o["t"] = e.ms / 1000;
        o["l"] = String(LEVEL_CHARS[e.level]);
        o["m"] = e.text;
    }
    xSemaphoreGive(lockHandle());

    String out;
    serializeJson(doc, out);
    return out;
}
