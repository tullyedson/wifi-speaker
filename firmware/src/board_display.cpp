#include "board_display.h"
#include <algorithm>
#include <cmath>

namespace board_display {
bool valid_expression(const String& value) {
    for (const char* name : {"neutral","happy","curious","sleepy","excited","love","sad","surprised","thinking"}) if (value == name) return true;
    return false;
}
}

#if defined(BOARD_SPOTPEAR_BALL_V2)
#include <Adafruit_GC9A01A.h>
#include <SPI.h>
#include <Wire.h>

namespace board_display {
namespace {
constexpr uint8_t touch_address = 0x15;
constexpr uint16_t background = 0x0000, foreground = 0xDF7C, accent = 0x6F9B, blush = 0xFAF3;
constexpr int backlight_channel = 6;
// The SPI-pointer overload takes DC before CS, unlike the default-SPI overload.
Adafruit_GC9A01A display(&SPI, 47, 5, 38);
GFXcanvas16 face(240, 166);
bool touch_ready = false, touch_down = false, asleep = false, manual_sleep = false;
bool eyes = true, fixed_gaze = false, dirty = true;
uint8_t brightness = 30;
uint32_t timeout_ms = 0, last_activity = 0, last_poll = 0, last_draw = 0;
uint32_t expression_until = 0, next_look = 0, blink_started = 0, next_blink = 0;
float look_x = 0, look_y = 0, target_x = 0, target_y = 0;
String mood = "neutral", old_state, old_address;
int old_volume = -1, old_level = -1;
float old_gain = -1;

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
void button(int x, int y, char label) {
    display.fillRoundRect(x, y, 36, 28, 9, 0x2147);
    display.setTextSize(2); display.setTextColor(foreground);
    display.setCursor(x + 12, y + 7); display.print(label);
}
void heart(int x, int y, uint16_t color) {
    face.fillCircle(x - 10, y - 5, 13, color); face.fillCircle(x + 10, y - 5, 13, color);
    face.fillTriangle(x - 22, y, x + 22, y, x, y + 27, color);
}
void draw_eye(int cx, int cy, bool right, float openness, const String& expression) {
    const bool sleepy = expression == "sleepy" || expression == "thinking";
    int height = sleepy ? 34 : expression == "surprised" ? 84 : 72;
    if (expression == "curious" && right) height = 85;
    height = std::max(4, static_cast<int>(height * openness));
    if (height < 12) { face.fillRoundRect(cx - 30, cy - 2, 60, 5, 2, accent); return; }
    if (expression == "love") { heart(cx, cy - 5, blush); return; }
    if (expression == "happy" || expression == "excited") {
        face.fillRoundRect(cx - 31, cy - height / 2, 62, height, 25, accent);
        face.fillRoundRect(cx - 34, cy - height / 2 + 19, 68, height + 10, 29, background);
        if (expression == "excited") {
            face.fillCircle(cx + (right ? 27 : -27), cy - 39, 3, foreground);
            face.drawFastHLine(cx + (right ? 29 : -39), cy - 48, 9, accent);
        }
        return;
    }
    const int top = cy - height / 2;
    face.fillRoundRect(cx - 32, top, 64, height, std::min(27, height / 2), accent);
    const int pupil_y = cy + static_cast<int>(look_y * 9), pupil_x = cx + static_cast<int>(look_x * 12);
    const int pupil_radius = expression == "surprised" ? 12 : 18;
    if (height > 25) {
        face.fillCircle(pupil_x, pupil_y, pupil_radius, 0x0105);
        face.fillCircle(pupil_x - 5, pupil_y - 6, 6, foreground);
        face.fillCircle(pupil_x + 6, pupil_y + 5, 3, 0xB7DE);
        face.fillRect(cx - 33, top - 25, 66, 25, background);
        face.fillRect(cx - 33, top + height, 66, 30, background);
    }
    if (expression == "sad") face.fillTriangle(cx - 34, top - 1, cx + 34, top - 1, cx + (right ? 34 : -34), top + 23, background);
}
void draw_face(uint32_t now) {
    if (!face.getBuffer()) { text("Face unavailable", 115, 1); return; }
    if (!fixed_gaze && static_cast<int32_t>(now - next_look) >= 0) {
        target_x = random(-80, 81) / 100.0f; target_y = random(-45, 46) / 100.0f;
        next_look = now + random(1600, 4100);
    }
    look_x += (target_x - look_x) * 0.18f; look_y += (target_y - look_y) * 0.18f;
    if (!blink_started && static_cast<int32_t>(now - next_blink) >= 0) blink();
    float openness = 1;
    if (blink_started) {
        const uint32_t elapsed = now - blink_started;
        if (elapsed >= 180) { blink_started = 0; next_blink = now + random(2200, 5800); }
        else openness = std::abs(static_cast<float>(elapsed) - 90) / 90;
    }
    face.fillScreen(background);
    const int bob = static_cast<int>(std::sin(now / 1100.0) * 2);
    draw_eye(72, 77 + bob, false, openness, mood); draw_eye(168, 77 + bob, true, openness, mood);
    face.fillRoundRect(35, 121 + bob, 26, 9, 4, 0x91CA); face.fillRoundRect(179, 121 + bob, 26, 9, 4, 0x91CA);
    if (mood == "surprised") { face.drawCircle(120, 137, 7, accent); face.drawCircle(120, 137, 6, accent); }
    else if (mood == "sad") { face.drawLine(113, 141, 120, 136, accent); face.drawLine(120, 136, 127, 141, accent); }
    else { face.drawLine(111, 132, 115, 136, accent); face.drawFastHLine(115, 136, 10, accent); face.drawLine(125, 136, 129, 132, accent); }
    display.drawRGBBitmap(0, 37, face.getBuffer(), 240, 166);
}
}

bool available() { return true; }
void begin() {
    pinMode(42, OUTPUT); digitalWrite(42, HIGH);
    ledcSetup(backlight_channel, 5000, 8); ledcWrite(backlight_channel, 255); ledcAttachPin(42, backlight_channel);
    SPI.begin(4, -1, 2, 5); display.begin(20000000); display.invertDisplay(true);
    display.setRotation(0); display.setTextWrap(false); display.fillScreen(background); wake();
    pinMode(6, OUTPUT); digitalWrite(6, HIGH); delay(5); digitalWrite(6, LOW); delay(5); digitalWrite(6, HIGH); delay(50);
    pinMode(12, INPUT_PULLUP);
    if (Wire1.begin(11, 7, 100000)) {
        Wire1.setTimeOut(20); uint8_t chip = 0;
        touch_ready = touch_read(0xA7, &chip, 1) && (chip == 0xB4 || chip == 0xB5 || chip == 0xB6);
        if (touch_ready) {
            Wire1.beginTransmission(touch_address); Wire1.write(0xFE); Wire1.write(0x01); touch_ready = Wire1.endTransmission() == 0;
        }
        Serial.printf("DISPLAY GC9A01 240x240 touch=%s chip=0x%02X\n", touch_ready ? "ready" : "unavailable", chip);
    }
}
void wake() {
    last_activity = millis(); manual_sleep = false; if (asleep) dirty = true;
    asleep = false; ledcWrite(backlight_channel, 255 - brightness * 255 / 100);
}
void sleep() { manual_sleep = true; asleep = true; ledcWrite(backlight_channel, 255); }
void configure(uint8_t value, uint32_t timeout, const String& screen) {
    brightness = value; timeout_ms = timeout; if (!asleep) ledcWrite(backlight_channel, 255 - brightness * 255 / 100); select(screen);
}
void select(const String& screen) { const bool value = screen == "eyes"; if (value != eyes) { eyes = value; dirty = true; } }
String cycle() { select(eyes ? "status" : "eyes"); wake(); return eyes ? "eyes" : "status"; }
void expression(const String& value, uint32_t duration, bool fixed, float x, float y) {
    mood = value; expression_until = duration ? millis() + duration : 0; fixed_gaze = fixed;
    if (fixed) { target_x = x; target_y = y; }
}
void blink() { blink_started = millis() ? millis() : 1; }
void status(JsonObject object) {
    object["available"] = true; object["awake"] = !asleep; object["screen"] = eyes ? "eyes" : "status";
    object["expression"] = mood; object["brightness"] = brightness; object["timeout_ms"] = timeout_ms;
    object["look_x"] = look_x; object["look_y"] = look_y; object["touch"] = touch_ready;
}
Action poll() {
    if (!touch_ready || millis() - last_poll < 30) return Action::none;
    last_poll = millis(); uint8_t data[5]; if (!touch_read(0x02, data, sizeof(data))) return Action::none;
    const bool pressed = (data[0] & 3) != 0, tapped = pressed && !touch_down; touch_down = pressed;
    if (!tapped) return Action::none;
    if (asleep) { wake(); return Action::none; }
    wake(); if (eyes) { expression("happy", 1800, false, 0, 0); return Action::none; }
    const int x = ((data[1] & 15) << 8) | data[2], y = ((data[3] & 15) << 8) | data[4];
    if (x >= 240 || y >= 240) return Action::none;
    if (y >= 140 && y <= 170) { if (x >= 38 && x <= 82) return Action::quieter; if (x >= 158 && x <= 202) return Action::louder; }
    if (y >= 188 && y <= 219) { if (x >= 57 && x <= 101) return Action::gain_down; if (x >= 139 && x <= 183) return Action::gain_up; }
    if (y >= 90 && y <= 119 && x >= 61 && x <= 179) return Action::mute;
    return Action::none;
}
void update(const String& state, uint8_t volume, float mic_gain, uint16_t level, const String& address) {
    const uint32_t now = millis();
    // Expiry also runs while another screen is selected or the backlight is off.
    if (expression_until && static_cast<int32_t>(now - expression_until) >= 0) { mood = "neutral"; fixed_gaze = false; expression_until = 0; }
    if (!manual_sleep && (state == "Speaking" || state == "Thinking" || state == "Preparing" || state == "Alarm")) wake();
    if (!asleep && timeout_ms && now - last_activity >= timeout_ms) { asleep = true; ledcWrite(backlight_channel, 255); }
    if (asleep || now - last_draw < (eyes ? 50u : 100u)) return; last_draw = now;
    if (dirty || state != old_state || address != old_address || volume != old_volume || mic_gain != old_gain) {
        old_state = state; old_address = address; old_volume = volume; old_gain = mic_gain; old_level = -1; dirty = false;
        display.fillScreen(background);
        const uint16_t color = state == "Muted" || state == "Audio error" || state == "Alarm" ? blush : accent;
        if (eyes) text(state, 218, 1, color);
        else {
            display.drawCircle(120, 120, 116, 0x2147); text("SMART SPEAKER", 26, 1, accent); text(state, 48, 2, color);
            display.drawRoundRect(49, 77, 142, 10, 4, 0x4208); display.fillRoundRect(62, 94, 116, 23, 8, 0x2147);
            text(state == "Muted" ? "UNMUTE MIC" : "MUTE MIC", 102, 1);
            text("VOLUME", 125, 1, accent); button(42, 142, '-'); button(162, 142, '+'); text(String(volume) + "%", 150, 1);
            text("MIC GAIN", 177, 1, accent); button(62, 190, '-'); button(142, 190, '+'); text(String(mic_gain, 2) + "x", 200, 1);
            text(address, 223, 1);
        }
    }
    if (eyes) draw_face(now);
    else {
        const int width = std::min(136, static_cast<int>(level) * 136 / 100);
        if (width != old_level) { display.fillRect(52, 80, 136, 4, background); if (width) display.fillRect(52, 80, width, 4, accent); old_level = width; }
    }
}
}
#else
namespace board_display {
bool available() { return false; }
void begin() {}
void wake() {}
void sleep() {}
void configure(uint8_t, uint32_t, const String&) {}
String cycle() { return "status"; }
void expression(const String&, uint32_t, bool, float, float) {}
void blink() {}
void select(const String&) {}
void status(JsonObject object) { object["available"] = false; object["awake"] = false; }
Action poll() { return Action::none; }
void update(const String&, uint8_t, float, uint16_t, const String&) {}
}
#endif
