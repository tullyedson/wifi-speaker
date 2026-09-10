#include "board_audio.h"
#include "board_config.h"
#include <Wire.h>
#include <driver/i2s.h>
#include <algorithm>

namespace board_audio {
namespace {
constexpr i2s_port_t port = I2S_NUM_0;
bool write_register(uint8_t reg, uint8_t value) {
#if defined(BOARD_SPOTPEAR_BALL_V2)
    Wire.beginTransmission(0x18); Wire.write(reg); Wire.write(value);
#else
    Wire.beginTransmission(0x10); Wire.write(reg); Wire.write(value);
#endif
    return Wire.endTransmission() == 0;
}
}

bool begin() {
    pinMode(board_config::amplifier, OUTPUT); digitalWrite(board_config::amplifier, LOW);
    if (!Wire.begin(board_config::sda, board_config::scl, 100000)) return false;
    Wire.setTimeOut(50);
#if defined(BOARD_MUSE_LUXE)
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
#endif
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    config.sample_rate = sample_rate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = 8; config.dma_buf_len = frame_samples;
    config.use_apll = board_config::use_apll; config.tx_desc_auto_clear = true;
    config.fixed_mclk = sample_rate * 256;
    if (i2s_driver_install(port, &config, 0, nullptr) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.mck_io_num = board_config::mclk; pins.bck_io_num = board_config::bclk; pins.ws_io_num = board_config::lrclk;
    pins.data_out_num = board_config::audio_out; pins.data_in_num = board_config::audio_in;
    if (i2s_set_pin(port, &pins) != ESP_OK) { i2s_driver_uninstall(port); return false; }
    i2s_zero_dma_buffer(port);
#if defined(BOARD_SPOTPEAR_BALL_V2)
    // ES8311 slave, analog microphone, 16-bit I2S, MCLK=4.096 MHz / 16 kHz.
    // Clock coefficients and power sequence follow Espressif's Apache-2.0
    // esp_codec_dev ES8311 driver. See docs/third-party/es8311.md.
    write_register(0x44, 0x08); // The first access after power-up may fail.
    const uint8_t configuration[][2] = {
        {0x44,0x08}, {0x01,0x30}, {0x02,0x00}, {0x03,0x10}, {0x16,0x24},
        {0x04,0x10}, {0x05,0x00}, {0x0B,0x00}, {0x0C,0x00}, {0x10,0x1F},
        {0x11,0x7F}, {0x00,0x80}, {0x01,0x3F}, {0x06,0x03}, {0x13,0x10},
        {0x1B,0x0A}, {0x1C,0x6A}, {0x44,0x08},
        {0x02,0x00}, {0x03,0x10}, {0x04,0x20}, {0x05,0x00},
        {0x07,0x00}, {0x08,0xFF}, {0x09,0x0C}, {0x0A,0x0C},
        {0x17,0xBF}, {0x0E,0x02}, {0x12,0x00}, {0x14,0x1A},
        {0x0D,0x01}, {0x15,0x40}, {0x37,0x08}, {0x45,0x00},
        {0x16,0x04}, // +24 dB microphone gain; desktop provides fine adjustment.
        {0x31,0x00}, {0x32,0xBF} // DAC unmuted at 0 dB; PCM applies volume.
    };
    for (const auto& item : configuration) {
        if (!write_register(item[0], item[1])) { i2s_driver_uninstall(port); return false; }
    }
#endif
    digitalWrite(board_config::amplifier, HIGH);
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
