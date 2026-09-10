#include "board_controls.h"
#include "board_config.h"
#include <Wire.h>

namespace board_controls {
namespace {
#if defined(BOARD_WAVESHARE_S3_AUDIO)
bool read_register(uint8_t reg, uint8_t& value) {
    Wire.beginTransmission(0x20); Wire.write(reg);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(0x20, 1) != 1) return false;
    value = Wire.read(); return true;
}
bool write_register(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(0x20); Wire.write(reg); Wire.write(value);
    return Wire.endTransmission() == 0;
}
#endif
}
bool begin() {
#if defined(BOARD_WAVESHARE_S3_AUDIO)
    uint8_t direction;
    // Set the output latch low before enabling the amplifier pin as an output.
    if (!amplifier(false) || !read_register(7, direction)) return false;
    return write_register(7, (direction | 0x0E) & 0xFE);
#else
    pinMode(board_config::amplifier, OUTPUT);
    return amplifier(false);
#endif
}
bool amplifier(bool enabled) {
#if defined(BOARD_WAVESHARE_S3_AUDIO)
    uint8_t output;
    return read_register(3, output) && write_register(3, enabled ? output | 1 : output & 0xFE);
#else
    digitalWrite(board_config::amplifier, enabled ? HIGH : LOW); return true;
#endif
}
uint8_t keys() {
#if defined(BOARD_WAVESHARE_S3_AUDIO)
    uint8_t input;
    return read_register(1, input) ? ((~input >> 1) & 7) : 0;
#else
    return 0;
#endif
}
}
