#include "board_audio.h"
#include <Wire.h>
#include <driver/i2s.h>
#include <algorithm>

namespace board_audio {
namespace {
constexpr i2s_port_t port = I2S_NUM_0;
bool write_register(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(0x10); Wire.write(reg); Wire.write(value);
    return Wire.endTransmission() == 0;
}
}

bool begin() {
    // Muse Luxe routing: ES8388 codec, shared I2S clocks, GPIO21 amplifier enable.
    // Register meanings and board pins are documented in docs/hardware.md.
    pinMode(21, OUTPUT); digitalWrite(21, LOW);
    Wire.begin(18, 23, 100000);
    if (!write_register(0, 0x80)) return false;
    delay(10);
    const uint8_t configuration[][2] = {
        {0,0x00}, {25,0x04}, {1,0x50}, {2,0x00}, {8,0x00}, {4,0xC0},
        {0,0x12}, {1,0x00}, {23,0x18}, {24,0x02}, {39,0x90}, {42,0x90},
        {43,0x80}, {45,0x00}, {26,0x00}, {27,0x00}, {2,0xF0}, {2,0x00},
        {29,0x1C}, {4,0x30}, {0,0x05}, {1,0x40}, {3,0x00}, {9,0x77},
        {10,0x00}, {12,0x4C}, {13,0x02}, {16,0x00}, {17,0x00},
        {18,0x00}, {22,0x00}, {46,0x18}, {47,0x18}, {25,0x00}
    };
    for (const auto& item : configuration) if (!write_register(item[0], item[1])) return false;
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    config.sample_rate = sample_rate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = 8; config.dma_buf_len = frame_samples;
    config.use_apll = true; config.tx_desc_auto_clear = true;
    config.fixed_mclk = sample_rate * 256;
    if (i2s_driver_install(port, &config, 0, nullptr) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.mck_io_num = 0; pins.bck_io_num = 5; pins.ws_io_num = 25;
    pins.data_out_num = 26; pins.data_in_num = 35;
    if (i2s_set_pin(port, &pins) != ESP_OK) { i2s_driver_uninstall(port); return false; }
    i2s_zero_dma_buffer(port);
    digitalWrite(21, HIGH);
    return true;
}

size_t capture(int16_t* mono, size_t samples, uint8_t channel, float gain) {
    int16_t stereo[frame_samples * 2];
    const size_t count = std::min(samples, frame_samples);
    size_t bytes = 0;
    if (i2s_read(port, stereo, count * 4, &bytes, pdMS_TO_TICKS(100)) != ESP_OK) return 0;
    const size_t frames = bytes / 4;
    for (size_t i = 0; i < frames; ++i) {
        const int32_t scaled = static_cast<int32_t>(stereo[i * 2 + (channel & 1)] * gain);
        mono[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, scaled)));
    }
    return frames;
}

bool play(const int16_t* mono, size_t samples, uint8_t volume) {
    if (samples > frame_samples) return false;
    int16_t stereo[frame_samples * 2];
    for (size_t i = 0; i < samples; ++i) {
        const int16_t value = static_cast<int32_t>(mono[i]) * volume / 100;
        stereo[i * 2] = value; stereo[i * 2 + 1] = value;
    }
    size_t written = 0;
    return i2s_write(port, stereo, samples * 4, &written, pdMS_TO_TICKS(1000)) == ESP_OK && written == samples * 4;
}
void silence() { i2s_zero_dma_buffer(port); }
}
