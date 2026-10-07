#pragma once
#include <Arduino.h>

enum LogLevel : uint8_t { LL_DEBUG, LL_INFO, LL_WARN, LL_ERROR };

void log_init();
void app_log(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_set_debug(bool on);
bool log_debug_enabled();
void log_clear();
String log_to_json();
