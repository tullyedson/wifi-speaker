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
#if defined(BOARD_ESP32_S3_BOX_3)
constexpr uint8_t address = 0x38;
bool bus_ready = false, measuring = false, valid = false;
uint32_t next_at = 0, started_at = 0, sampled_at = 0;
float temperature = 0, humidity = 0;
const char* state = "starting";

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
#endif
}

void update() {
#if defined(BOARD_ESP32_S3_BOX_3)
    const uint32_t now = millis();
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

void status(JsonObject output) {
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
