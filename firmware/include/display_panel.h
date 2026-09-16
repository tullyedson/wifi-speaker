#pragma once
#include <Arduino.h>

#if defined(BOARD_SPOTPEAR_BALL_V2) || defined(BOARD_ESP32_S3_BOX_3)
#include <SPI.h>
#if defined(BOARD_SPOTPEAR_BALL_V2)
#include <Adafruit_GC9A01A.h>
#else
#include <Adafruit_ILI9341.h>
#endif

namespace display_panel {
#if defined(BOARD_SPOTPEAR_BALL_V2)
extern Adafruit_GC9A01A display;
constexpr int left = 0, width = 240;
#else
extern Adafruit_ILI9341 display;
constexpr int left = 40, width = 320;
#endif
struct Touch { bool pressed = false; bool home = false; int x = 0; int y = 0; };
bool begin();
void backlight(uint8_t brightness);
bool read_touch(Touch& touch);
}
#endif
