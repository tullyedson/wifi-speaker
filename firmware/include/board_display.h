#pragma once
#include <Arduino.h>

namespace board_display {
enum class Action { none, mute, quieter, louder };
void begin();
void wake();
Action poll();
void update(const String& state, uint8_t volume, uint16_t level, const String& address);
}
