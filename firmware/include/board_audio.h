#pragma once
#include <Arduino.h>

namespace board_audio {
constexpr uint32_t sample_rate = 16000;
constexpr size_t frame_samples = 320;
bool begin();
size_t capture(int16_t* mono, size_t samples, uint8_t channel, float gain);
bool play(const int16_t* mono, size_t samples, uint8_t volume);
void silence();
}
