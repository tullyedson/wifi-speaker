// AHT30 command, checksum and conversion follow Espressif's Apache-2.0 driver.
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// Arduino transport and nonblocking polling adaptation: see docs/third-party/aht30.md.
#include "board_sensors.h"
#include <Arduino.h>
#include <Wire.h>
#include <cmath>

namespace board_sensors {
namespace {
constexpr uint32_t interval_ms = 10000;
constexpr uint32_t radar_interval_ms = 50;
#if defined(BOARD_ESP32_S3_BOX_3)
constexpr uint8_t address = 0x38;
bool bus_ready = false, measuring = false, valid = false;
uint32_t next_at = 0, started_at = 0, sampled_at = 0;
float temperature = 0, humidity = 0;
const char* state = "starting";

// AT581x sequence adapted from Espressif's Apache-2.0 driver.
// SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
// Wiring, source revision and changes: docs/third-party/at581x.md.
constexpr uint8_t radar_address = 0x28, radar_pin = 21;
constexpr uint32_t radar_retry_ms = 5000, radar_check_ms = 1000, radar_warmup_ms = 2000;
constexpr uint8_t radar_registers[][2] = {
    {0x68, 0x68}, {0x67, 0x8B}, // Default 70 uA power setting.
    {0x10, 200}, {0x11, 0}, {0x5C, 0x1B}, // Espressif default threshold and gain.
    {0x3D, 0xF4}, {0x3E, 1}, {0x3F, 0}, {0x40, 0}, // 500 ms minimum pulse.
    {0x41, 1}, {0x42, 0xDC}, {0x43, 5}, {0x44, 0}, {0x45, 0}, // 1500 ms hold.
    {0x38, 0xD0}, {0x39, 7}, // 2000 ms self-test.
    {0x4E, 0xE8}, {0x4F, 3}, // 1000 ms protection interval.
    {0x55, 4}, {0x5D, 0x45}, {0x62, 0x55}, {0x51, 0xA0}, // Enable RF.
    {0x00, 0}, {0x00, 1} // Reset and release after applying settings.
};
enum class RadarStage { probe, configure, warmup, ready };
RadarStage radar_stage = RadarStage::probe;
uint8_t radar_register = 0;
uint32_t radar_next = 0, radar_checked = 0, radar_sampled = 0, radar_detected_at = 0;
bool radar_detected = false, radar_seen = false;
const char* radar_state = "starting";

void radar_fail(const char* reason, uint32_t now) {
    radar_stage = RadarStage::probe; radar_state = reason;
    radar_seen = radar_detected = false;
    radar_next = now + radar_retry_ms;
}

bool radar_probe() {
    Wire1.beginTransmission(radar_address);
    return Wire1.endTransmission() == 0;
}

void radar_update(uint32_t now) {
    if (!bus_ready || static_cast<int32_t>(now - radar_next) < 0) return;
    if (radar_stage == RadarStage::probe) {
        if (!radar_probe()) { radar_fail("not_detected", now); return; }
        radar_stage = RadarStage::configure; radar_state = "initializing"; radar_register = 0;
    }
    if (radar_stage == RadarStage::configure) {
        // One bounded transaction per loop, never a blocking initialization delay.
        Wire1.beginTransmission(radar_address);
        Wire1.write(radar_registers[radar_register], 2);
        if (Wire1.endTransmission() != 0) { radar_fail("write_error", now); return; }
        radar_next = now + 2;
        if (++radar_register == sizeof(radar_registers) / sizeof(radar_registers[0])) {
            radar_stage = RadarStage::warmup; radar_state = "warming_up";
            radar_next = now + radar_warmup_ms;
        }
        return;
    }
    if (radar_stage == RadarStage::warmup || now - radar_checked >= radar_check_ms) {
        if (!radar_probe()) { radar_fail("not_detected", now); return; }
        radar_checked = now;
    }
    radar_stage = RadarStage::ready; radar_state = "ready";
    radar_sampled = now; radar_detected = digitalRead(radar_pin) == HIGH;
    if (radar_detected) { radar_seen = true; radar_detected_at = now; }
    radar_next = now + radar_interval_ms;
}

void fail(const char* reason, uint32_t now) {
    valid = false;
    measuring = false;
    state = reason;
    next_at = now + interval_ms;
}

bool receive(uint8_t* data, uint8_t size) {
    if (Wire1.requestFrom(address, size) != size) return false;
    for (uint8_t i = 0; i < size; ++i) data[i] = Wire1.read();
    return true;
}

bool checksum_valid(const uint8_t* data) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < 6; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
    }
    return crc == data[6];
}
#endif
}

void begin() {
#if defined(BOARD_ESP32_S3_BOX_3)
    // The SENSOR base has a separate I2C bus from audio and touchscreen.
    bus_ready = Wire1.begin(41, 40, 100000);
    Wire1.setTimeOut(20);
    measuring = valid = false;
    state = bus_ready ? "starting" : "bus_error";
    next_at = millis() + 100;
    pinMode(radar_pin, INPUT_PULLUP);
    radar_stage = RadarStage::probe; radar_seen = radar_detected = false;
    radar_state = bus_ready ? "starting" : "bus_error";
    radar_next = millis() + 100;
#endif
}

void update() {
#if defined(BOARD_ESP32_S3_BOX_3)
    const uint32_t now = millis();
    radar_update(now);
    if (!bus_ready || static_cast<int32_t>(now - next_at) < 0) return;
    if (!measuring) {
        Wire1.beginTransmission(address);
        if (Wire1.endTransmission() != 0) { fail("not_detected", now); return; }
        const uint8_t command[] = {0xAC, 0x33, 0x00};
        Wire1.beginTransmission(address);
        Wire1.write(command, sizeof(command));
        if (Wire1.endTransmission() != 0) { fail("read_error", now); return; }
        measuring = true;
        started_at = now;
        next_at = now + 80;
        return;
    }

    uint8_t busy;
    if (!receive(&busy, 1)) { fail("read_error", now); return; }
    if (busy & 0x80) {
        if (now - started_at >= 250) fail("timeout", now);
        else next_at = now + 10;
        return;
    }
    uint8_t data[7];
    if (!receive(data, sizeof(data))) { fail("read_error", now); return; }
    if ((data[0] & 0x80) || !(data[0] & 0x08) || !checksum_valid(data)) {
        fail("invalid_data", now); return;
    }
    const uint32_t raw_temperature = (static_cast<uint32_t>(data[3] & 0x0F) << 16) | (data[4] << 8) | data[5];
    const uint32_t raw_humidity = (static_cast<uint32_t>(data[1]) << 12) | (data[2] << 4) | (data[3] >> 4);
    temperature = raw_temperature * (200.0f / 1048576.0f) - 50.0f;
    humidity = raw_humidity * (100.0f / 1048576.0f);
    if (temperature < -40 || temperature > 120 || humidity < 0 || humidity > 100) {
        fail("invalid_data", now); return;
    }
    sampled_at = now;
    valid = true;
    measuring = false;
    state = "ready";
    next_at = now + interval_ms;
#endif
}

Presence presence() {
#if defined(BOARD_ESP32_S3_BOX_3)
    const bool fresh = radar_stage == RadarStage::ready && millis() - radar_sampled <= 500;
    Presence result; result.available = fresh; result.detected = fresh && radar_detected;
    return result;
#else
    return {};
#endif
}

void status(JsonObject output) {
    JsonObject radar = output["presence"].to<JsonObject>();
    radar["poll_interval_ms"] = radar_interval_ms;
    for (const char* field : {"detected", "sampled_at_uptime_ms", "sample_age_ms", "last_detected_uptime_ms", "last_detection_age_ms"}) radar[field] = nullptr;
#if defined(BOARD_ESP32_S3_BOX_3)
    const Presence reading = presence();
    radar["supported"] = true; radar["sensor"] = "at581x";
    radar["available"] = reading.available;
    radar["state"] = radar_stage == RadarStage::ready && !reading.available ? "stale" : radar_state;
    if (reading.available) {
        radar["detected"] = reading.detected;
        radar["sampled_at_uptime_ms"] = radar_sampled; radar["sample_age_ms"] = millis() - radar_sampled;
        if (radar_seen) {
            radar["last_detected_uptime_ms"] = radar_detected_at;
            radar["last_detection_age_ms"] = millis() - radar_detected_at;
        }
    }
#else
    radar["supported"] = false; radar["sensor"] = nullptr;
    radar["available"] = false; radar["state"] = "unsupported";
#endif
    JsonObject sensor = output["temperature_humidity"].to<JsonObject>();
    sensor["poll_interval_ms"] = interval_ms;
    sensor["temperature_c"] = nullptr;
    sensor["temperature_f"] = nullptr;
    sensor["humidity_percent"] = nullptr;
    sensor["sampled_at_uptime_ms"] = nullptr;
    sensor["sample_age_ms"] = nullptr;
#if defined(BOARD_ESP32_S3_BOX_3)
    const uint32_t age = millis() - sampled_at;
    const bool fresh = valid && age <= interval_ms * 3;
    sensor["supported"] = true;
    sensor["sensor"] = "aht30";
    sensor["available"] = fresh;
    sensor["state"] = valid && !fresh ? "stale" : state;
    if (fresh) {
        sensor["temperature_c"] = std::round(temperature * 100) / 100;
        sensor["temperature_f"] = std::round((temperature * 1.8f + 32) * 100) / 100;
        sensor["humidity_percent"] = std::round(humidity * 100) / 100;
        sensor["sampled_at_uptime_ms"] = sampled_at;
        sensor["sample_age_ms"] = age;
    }
#else
    sensor["supported"] = false;
    sensor["sensor"] = nullptr;
    sensor["available"] = false;
    sensor["state"] = "unsupported";
#endif
}
}
