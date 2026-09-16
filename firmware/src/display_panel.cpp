#include "display_panel.h"
#include <Wire.h>

#if defined(BOARD_SPOTPEAR_BALL_V2) || defined(BOARD_ESP32_S3_BOX_3)
namespace display_panel {
namespace {
constexpr int backlight_channel = 6;
}
#if defined(BOARD_SPOTPEAR_BALL_V2)
// The SPI-pointer overload takes DC before CS.
Adafruit_GC9A01A display(&SPI, 47, 5, 38);
namespace {
bool read_register(uint8_t reg, uint8_t* bytes, uint8_t count) {
    Wire1.beginTransmission(0x15); Wire1.write(reg);
    if (Wire1.endTransmission(false) != 0 || Wire1.requestFrom(0x15, count) != count) return false;
    for (uint8_t i = 0; i < count; ++i) bytes[i] = Wire1.read();
    return true;
}
}
void backlight(uint8_t value) { ledcWrite(backlight_channel, 255 - value * 255 / 100); }
bool begin() {
    pinMode(42, OUTPUT); digitalWrite(42, HIGH);
    ledcSetup(backlight_channel, 5000, 8); backlight(0); ledcAttachPin(42, backlight_channel);
    SPI.begin(4, -1, 2, 5); display.begin(20000000); display.invertDisplay(true); display.setRotation(0);
    pinMode(6, OUTPUT); digitalWrite(6, HIGH); delay(5); digitalWrite(6, LOW); delay(5); digitalWrite(6, HIGH); delay(50);
    pinMode(12, INPUT_PULLUP);
    uint8_t chip = 0; bool ready = false;
    if (Wire1.begin(11, 7, 100000)) {
        Wire1.setTimeOut(20);
        ready = read_register(0xA7, &chip, 1) && (chip == 0xB4 || chip == 0xB5 || chip == 0xB6);
        if (ready) {
            Wire1.beginTransmission(0x15); Wire1.write(0xFE); Wire1.write(0x01); ready = Wire1.endTransmission() == 0;
        }
    }
    Serial.printf("DISPLAY GC9A01 240x240 touch=%s chip=0x%02X\n", ready ? "ready" : "unavailable", chip);
    return ready;
}
bool read_touch(Touch& touch) {
    uint8_t data[5]; if (!read_register(0x02, data, sizeof(data))) return false;
    touch.pressed = (data[0] & 3) != 0;
    touch.x = ((data[1] & 15) << 8) | data[2]; touch.y = ((data[3] & 15) << 8) | data[4];
    return true;
}
#else
// BOX-3 reset is active HIGH and shared with touch. Reset it ourselves once.
Adafruit_ILI9341 display(&SPI, 4, 5, -1);
namespace {
uint8_t touch_address = 0;
bool probe(uint8_t address) { Wire.beginTransmission(address); return Wire.endTransmission() == 0; }
bool receive(uint8_t* bytes, uint8_t count) {
    if (Wire.requestFrom(touch_address, count) != count) return false;
    for (uint8_t i = 0; i < count; ++i) bytes[i] = Wire.read();
    return true;
}
bool gt_read(uint16_t reg, uint8_t* bytes, uint8_t count) {
    Wire.beginTransmission(touch_address); Wire.write(reg >> 8); Wire.write(reg & 255);
    return Wire.endTransmission(false) == 0 && receive(bytes, count);
}
bool gt_ack() {
    Wire.beginTransmission(touch_address); Wire.write(0x81); Wire.write(0x4E); Wire.write(0);
    return Wire.endTransmission() == 0;
}
uint16_t little_endian(const uint8_t* bytes) { return bytes[0] | (bytes[1] << 8); }
}
void backlight(uint8_t value) { ledcWrite(backlight_channel, value * 255 / 100); }
bool begin() {
    pinMode(47, OUTPUT); digitalWrite(47, LOW);
    ledcSetup(backlight_channel, 5000, 8); backlight(0); ledcAttachPin(47, backlight_channel);
    pinMode(48, OUTPUT); digitalWrite(48, HIGH); delay(20); digitalWrite(48, LOW); delay(120);
    SPI.begin(7, -1, 6, 5); display.begin(20000000);
    // This panel's native address space is landscape. Set GFX dimensions, then
    // retain BOX-3's native row/column order instead of the portrait ILI9341 swap.
    display.setRotation(1); uint8_t orientation = 0xC8; display.sendCommand(0x36, &orientation, 1);
    display.invertDisplay(false);
    pinMode(3, INPUT_PULLUP);
    if (Wire.begin(8, 18, 100000)) {
        Wire.setTimeOut(20);
        for (uint8_t address : {0x5D, 0x14, 0x24}) if (probe(address)) { touch_address = address; break; }
    }
    Serial.printf("DISPLAY BOX-3 320x240 touch=%s address=0x%02X\n", touch_address ? "ready" : "unavailable", touch_address);
    return touch_address != 0;
}
bool read_touch(Touch& touch) {
    if (!touch_address) return false;
    if (touch_address == 0x24) {
        // TT21100 length-prefixed reports, as documented by Espressif's driver.
        if (digitalRead(3) != LOW) return false;
        uint8_t header[2]; if (!receive(header, 2)) return false;
        const uint16_t length = little_endian(header);
        uint8_t data[57];
        if (length < 7 || length > sizeof(data) || !receive(data, length)) return false;
        if (data[2] == 3 && length == 14) { touch.home = (data[5] & 1) != 0; return true; }
        if (data[2] != 1) return false;
        touch.pressed = length >= 17 && (data[5] >> 3) != 0 && (data[8] & 1) != 0;
        if (touch.pressed) { touch.x = 319 - little_endian(data + 9); touch.y = little_endian(data + 11); }
        return true;
    }
    uint8_t status;
    if (!gt_read(0x814E, &status, 1) || !(status & 0x80)) return false;
    if (status & 0x10) {
        uint8_t key = 0;
        const bool received = gt_read(0x8093, &key, 1);
        const bool acknowledged = gt_ack();
        if (!received || !acknowledged) return false;
        touch.home = key != 0; return true;
    }
    uint8_t data[9] = {};
    const uint8_t count = status & 15;
    // Only the first contact controls the interface; always acknowledge releases.
    const bool received = count <= 5 && gt_read(0x814F, data, count ? 8 : 1);
    const bool acknowledged = gt_ack();
    if (!received || !acknowledged) return false;
    touch.pressed = count != 0;
    if (touch.pressed) { touch.x = little_endian(data + 1); touch.y = little_endian(data + 3); }
    return true;
}
#endif
}
#endif
