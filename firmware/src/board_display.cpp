#include "board_display.h"

#if defined(BOARD_SPOTPEAR_BALL_V2)
#include <Adafruit_GC9A01A.h>
#include <SPI.h>
#include <Wire.h>
#include <algorithm>

namespace board_display {
namespace {
constexpr uint8_t touch_address = 0x15;
constexpr uint16_t background = 0x0000, foreground = 0xBDF7, accent = 0x3C71;
constexpr int backlight_channel = 6;
constexpr uint32_t screen_timeout_ms = 15000;
// The SPI-pointer overload takes DC before CS (unlike the default-SPI overload).
Adafruit_GC9A01A display(&SPI, 47, 5, 38);
bool touch_ready = false, touch_down = false;
bool asleep = false;
uint32_t last_activity = 0;
uint32_t last_poll = 0, last_draw = 0;
String old_state, old_address;
int old_volume = -1, old_level = -1;

bool touch_read(uint8_t reg, uint8_t* bytes, uint8_t count) {
    Wire1.beginTransmission(touch_address); Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0 || Wire1.requestFrom(touch_address, count) != count) return false;
    for (uint8_t i = 0; i < count; ++i) bytes[i] = Wire1.read();
    return true;
}
void text(const String& value, int y, int size, uint16_t color = foreground) {
    display.setTextSize(size); display.setTextColor(color, background);
    display.setCursor(std::max(0, (240 - static_cast<int>(value.length()) * 6 * size) / 2), y);
    display.print(value);
}
}

void begin() {
    // Backlight uses a P-channel transistor and is active low.
    pinMode(42, OUTPUT); digitalWrite(42, HIGH);
    ledcSetup(backlight_channel, 5000, 8);
    ledcWrite(backlight_channel, 255);
    ledcAttachPin(42, backlight_channel);
    SPI.begin(4, -1, 2, 5);
    display.begin(20000000); display.invertDisplay(true);
    display.setRotation(0); display.setTextWrap(false);
    display.fillScreen(background);
    wake();
    pinMode(6, OUTPUT); digitalWrite(6, HIGH); delay(5);
    digitalWrite(6, LOW); delay(5); digitalWrite(6, HIGH); delay(50);
    pinMode(12, INPUT_PULLUP);
    if (Wire1.begin(11, 7, 100000)) {
        Wire1.setTimeOut(20);
        uint8_t chip = 0;
        touch_ready = touch_read(0xA7, &chip, 1) && (chip == 0xB4 || chip == 0xB5 || chip == 0xB6);
        if (touch_ready) {
            // CST816D/S/T: prevent auto-sleep so the UI can poll touch/release.
            Wire1.beginTransmission(touch_address); Wire1.write(0xFE); Wire1.write(0x01);
            touch_ready = Wire1.endTransmission() == 0;
        }
        Serial.printf("DISPLAY GC9A01 240x240 touch=%s chip=0x%02X\n", touch_ready ? "ready" : "unavailable", chip);
    }
}

void wake() {
    last_activity = millis();
    if (asleep) { old_state = ""; old_address = ""; }
    asleep = false;
    ledcWrite(backlight_channel, 220); // Active low, about 14% backlight duty.
}

Action poll() {
    if (!touch_ready || millis() - last_poll < 30) return Action::none;
    last_poll = millis();
    uint8_t data[5];
    if (!touch_read(0x02, data, sizeof(data))) return Action::none;
    const bool pressed = (data[0] & 3) != 0;
    const bool tapped = pressed && !touch_down;
    touch_down = pressed;
    if (!tapped) return Action::none;
    if (asleep) { wake(); return Action::none; }
    wake();
    const int x = ((data[1] & 15) << 8) | data[2];
    const int y = ((data[3] & 15) << 8) | data[4];
    if (x >= 240 || y >= 240) return Action::none;
    if (y >= 164 && y <= 215) {
        if (x >= 40 && x <= 100) return Action::quieter;
        if (x >= 140 && x <= 200) return Action::louder;
    }
    if (y >= 60 && y <= 145 && x >= 25 && x <= 215) return Action::mute;
    return Action::none;
}

void update(const String& state, uint8_t volume, uint16_t level, const String& address) {
    if (state == "Speaking" || state == "Thinking" || state == "Preparing") wake();
    if (!asleep && millis() - last_activity >= screen_timeout_ms) {
        asleep = true;
        ledcWrite(backlight_channel, 255);
    }
    if (asleep) return;
    if (millis() - last_draw < 100) return;
    last_draw = millis();
    if (state != old_state || address != old_address || volume != old_volume) {
        old_state = state; old_address = address; old_volume = volume; old_level = -1;
        display.fillScreen(background);
        const uint16_t color = state == "Muted" || state == "Audio error" ? 0xFAE8 : accent;
        display.drawCircle(120, 120, 116, color);
        text("SMART SPEAKER", 29, 1, accent);
        text(state, 70, 2, color);
        if (state == "Wi-Fi setup") {
            text("Speaker-setup network", 107, 1);
            text("speaker-setup", 122, 1);
            text("192.168.4.1", 139, 1);
        } else {
            text(touch_ready ? "Tap to mute / unmute" : "BOOT: mute / unmute", 107, 1);
            display.drawRoundRect(44, 137, 152, 11, 4, 0x4208);
            text(address.substring(0, 24), 155, 1);
        }
        text("Vol " + String(volume) + "%", 221, 1);
        display.fillRoundRect(48, 174, 48, 35, 10, 0x2147);
        display.fillRoundRect(144, 174, 48, 35, 10, 0x2147);
        display.setTextColor(foreground); display.setTextSize(2);
        display.setCursor(66, 184); display.print('-');
        display.setCursor(162, 184); display.print('+');
    }
    if (state != "Wi-Fi setup") {
        const int width = std::min(146, static_cast<int>(level) * 146 / 250);
        if (width != old_level) {
            display.fillRect(47, 140, 146, 5, background);
            if (width) display.fillRect(47, 140, width, 5, accent);
            old_level = width;
        }
    }
}
}
#else
namespace board_display {
void begin() {}
void wake() {}
Action poll() { return Action::none; }
void update(const String&, uint8_t, uint16_t, const String&) {}
}
#endif
