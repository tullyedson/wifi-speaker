#pragma once
#include <Arduino.h>

namespace status_colors {
// One palette for LCD eyes and physical RGB status lights.
inline uint16_t rgb565(const String& state) {
    if (state == "Recognizing") return 0x45BF; // Sky blue.
    if (state == "Thinking") return 0xB3FF; // Violet.
    if (state == "Preparing") return 0xFDEA; // Gold.
    if (state == "Speaking") return 0x47EE; // Green.
    if (state == "Muted" || state == "Paused") return 0xFD08; // Amber.
    if (state == "Alarm" || state == "Audio error") return 0xFA8A; // Coral red.
    if (state == "Connecting" || state == "Wi-Fi setup") return 0x6476; // Slate blue.
    return 0x6F9B; // Mint while listening.
}

inline uint32_t rgb888(const String& state) {
    const uint16_t color = rgb565(state);
    const uint32_t red = ((color >> 11) & 31) * 255 / 31;
    const uint32_t green = ((color >> 5) & 63) * 255 / 63;
    const uint32_t blue = (color & 31) * 255 / 31;
    return (red << 16) | (green << 8) | blue;
}
}
