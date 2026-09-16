#include "board_sensors.h"
#include "Wire.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

uint32_t test_clock = 0;
SensorWire Wire1;

JsonDocument snapshot() {
    JsonDocument doc;
    board_sensors::status(doc.to<JsonObject>());
    return doc;
}

void unavailable(const char* expected) {
    const JsonDocument doc = snapshot();
    const auto value = doc["temperature_humidity"];
    assert(value["available"] == false);
    assert(std::strcmp(value["state"].as<const char*>(), expected) == 0);
    for (const char* field : {"temperature_c", "temperature_f", "humidity_percent", "sample_age_ms", "sampled_at_uptime_ms"})
        assert(value[field].isNull());
}

void reset(uint32_t time = 0) {
    Wire1 = SensorWire{};
    test_clock = time;
    board_sensors::begin();
}

void convert() {
    test_clock += 100;
    board_sensors::update();
    test_clock += 80;
    board_sensors::update();
}

int main() {
#if defined(BOARD_ESP32_S3_BOX_3)
    reset();
    unavailable("starting");
    convert();
    auto doc = snapshot();
    auto reading = doc["temperature_humidity"];
    assert(reading["available"] == true && reading["supported"] == true);
    assert(reading["temperature_c"] == 25 && reading["temperature_f"] == 77 && reading["humidity_percent"] == 50);
    assert(reading["sample_age_ms"] == 0 && reading["sampled_at_uptime_ms"] == 180);
    const auto command_count = Wire1.commands;
    for (int i = 0; i < 100; ++i) snapshot();
    assert(Wire1.commands == command_count); // HTTP snapshots never trigger conversions.
    test_clock += 9999;
    board_sensors::update();
    assert(Wire1.commands == command_count);

    // Disconnection invalidates the prior sample and a later reattachment recovers.
    Wire1.present = false;
    ++test_clock;
    board_sensors::update();
    unavailable("not_detected");
    Wire1.present = true;
    test_clock += 10000;
    board_sensors::update();
    test_clock += 80;
    board_sensors::update();
    assert(snapshot()["temperature_humidity"]["available"] == true);
    test_clock += 30001;
    unavailable("stale");

    // A damaged sample must not become an environmental observation.
    for (int bit = 0; bit < 56; ++bit) {
        reset();
        Wire1.sample[bit / 8] ^= 1 << (bit % 8);
        convert();
        unavailable("invalid_data");
    }
    reset();
    Wire1.short_read = true;
    convert();
    unavailable("read_error");
    reset();
    Wire1.write_error = true;
    convert();
    unavailable("read_error");

    reset();
    Wire1.busy = true;
    convert();
    unavailable("starting");
    test_clock = 350;
    board_sensors::update();
    unavailable("timeout");

    reset();
    Wire1.bus_ok = false;
    board_sensors::begin();
    convert();
    unavailable("bus_error");

    reset(0xFFFFFFA0u);
    convert();
    assert(snapshot()["temperature_humidity"]["temperature_c"] == 25);
    test_clock += 200;
    assert(snapshot()["temperature_humidity"]["sample_age_ms"] == 200);
    std::puts("Sensor checks passed: conversion, cadence, stale data, CRC, errors, recovery and clock rollover.");
#else
    reset();
    convert();
    unavailable("unsupported");
    assert(snapshot()["temperature_humidity"]["supported"] == false && Wire1.commands == 0);
    std::puts("Unsupported board reports null readings without touching the sensor bus.");
#endif
}
