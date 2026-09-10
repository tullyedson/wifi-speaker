#pragma once

namespace board_config {
#if defined(BOARD_SPOTPEAR_BALL_V2)
constexpr const char* firmware = "spotpear-ball-v2/0.2.1";
constexpr int sda = 15, scl = 14, mclk = 16, bclk = 9, lrclk = 45;
constexpr int audio_out = 8, audio_in = 10, amplifier = 46;
constexpr int led = 48, button = 0, volume_up = -1, volume_down = -1;
constexpr int led_count = 1;
constexpr bool led_rgb_order = false;
constexpr bool use_apll = false;
#elif defined(BOARD_WAVESHARE_S3_AUDIO)
constexpr const char* firmware = "waveshare-s3-audio/0.2.2";
constexpr int sda = 11, scl = 10, mclk = 12, bclk = 13, lrclk = 14;
constexpr int audio_out = 16, audio_in = 15, amplifier = -1;
constexpr int led = 38, button = 0, volume_up = -1, volume_down = -1;
constexpr int led_count = 7;
// Manufacturer factory driver selects LED_STRIP_COLOR_COMPONENT_FMT_RGB.
constexpr bool led_rgb_order = true;
constexpr bool use_apll = false;
#elif defined(BOARD_MUSE_LUXE)
constexpr const char* firmware = "muse-luxe/0.2.1";
constexpr int sda = 18, scl = 23, mclk = 0, bclk = 5, lrclk = 25;
constexpr int audio_out = 26, audio_in = 35, amplifier = 21;
constexpr int led = 22, button = 12, volume_up = 19, volume_down = 32;
constexpr int led_count = 1;
constexpr bool led_rgb_order = false;
constexpr bool use_apll = true;
#else
#error Select a supported board in platformio.ini
#endif
}
