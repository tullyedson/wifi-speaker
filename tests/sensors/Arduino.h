#pragma once
#include <cstdint>
#include <cassert>
extern uint32_t test_clock;
inline uint32_t millis() { return test_clock; }
constexpr int INPUT_PULLUP = 2, HIGH = 1;
extern int radar_level;
extern unsigned radar_pin_reads, radar_pin_setups;
inline void pinMode(int pin, int mode) { assert(pin == 21 && mode == INPUT_PULLUP); ++radar_pin_setups; }
inline int digitalRead(int pin) { assert(pin == 21); ++radar_pin_reads; return radar_level; }
