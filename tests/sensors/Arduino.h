#pragma once
#include <cstdint>
extern uint32_t test_clock;
inline uint32_t millis() { return test_clock; }
