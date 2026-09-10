#pragma once
#include <Arduino.h>

namespace board_controls {
// Wire must already be initialized by board_audio. Only amplifier pin 8 and
// key pins 9..11 belong to this driver; camera/display expansion pins are left alone.
bool begin();
bool amplifier(bool enabled);
// Bit 0: volume down, bit 1: mute, bit 2: volume up. Debouncing belongs to the caller.
uint8_t keys();
}
