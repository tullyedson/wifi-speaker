#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace board_display {
enum class Action { none, mute, quieter, louder, gain_down, gain_up };
void begin();
void wake();
void sleep();
void configure(uint8_t brightness, uint32_t timeout_ms, const String& screen);
String cycle();
bool available();
bool valid_expression(const String& expression);
void expression(const String& name, uint32_t duration_ms, bool fixed_gaze, float x, float y);
void blink();
void select(const String& screen);
void status(JsonObject object);
Action poll();
void update(const String& state, uint8_t volume, float mic_gain, uint16_t level, const String& address);
}
