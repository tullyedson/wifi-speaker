#include "board_sensors.h"
#include "display_power.h"
#include "Arduino.h"
#include "Wire.h"
#include <cstdio>
#include <cstring>

void test_display_power() {
    DisplayPower power;
    power.wake(0);
    power.configure(0, 5000, 60000);
    power.update(0, false, true, true);
    power.update(120000, false, true, true);
    assert(power.awake()); // Presence keeps it on past the normal idle timeout.
    power.update(179999, false, true, false);
    assert(power.awake());
    power.update(180000, false, true, false);
    assert(!power.awake() && power.reason() == DisplayPower::Reason::absence);
    power.update(180050, false, true, true);
    assert(power.awake());
    power.update(240050, false, true, false);
    assert(!power.awake());
    power.update(240100, true, true, false); // Reply processing wakes an automatic sleep.
    assert(power.awake());
    power.update(300100, false, true, false);
    assert(!power.awake());
    power.update(300150, false, false, false); // Sensor loss is not absence.
    assert(power.awake() && !power.presence_active());
    power.update(305150, false, false, false);
    assert(power.reason() == DisplayPower::Reason::idle);
    power.update(305200, false, true, true);
    assert(power.awake());
    power.sleep();
    power.update(500000, true, true, true);
    assert(power.reason() == DisplayPower::Reason::manual);
    power.wake(500001); // Touch, button, explicit API wake and alarm use this path.
    power.update(500001, false, true, false);
    assert(power.awake());
    power.update(560001, false, true, false);
    assert(!power.awake());
    power.configure(560002, 0, 0);
    power.update(560002, false, true, false);
    assert(power.awake() && !power.presence_active());

    DisplayPower startup;
    startup.configure(0, 0, 5000);
    startup.update(100000, false, false, false);
    assert(startup.awake()); // Missing base never masquerades as an empty room.
    startup.update(100001, false, true, false);
    startup.update(105000, false, true, false);
    assert(startup.awake());
    startup.update(105001, false, true, false);
    assert(startup.reason() == DisplayPower::Reason::absence);
    startup.update(105002, false, false, false);
    startup.update(200000, false, true, false);
    assert(startup.awake()); // Recovery starts a complete new grace period.

    DisplayPower rollover;
    rollover.configure(0xFFFFFF00u, 0, 5000);
    rollover.wake(0xFFFFFF00u);
    rollover.update(0xFFFFFF00u, false, true, true);
    rollover.update(4743, false, true, false);
    assert(rollover.awake());
    rollover.update(4744, false, true, false);
    assert(rollover.reason() == DisplayPower::Reason::absence);
    std::puts("Display checks passed: detection, timeout, manual sleep, activity, failure/recovery, disable and rollover.");
}

#if defined(BOARD_ESP32_S3_BOX_3)
namespace {
JsonDocument radar_snapshot() {
    JsonDocument result; board_sensors::status(result.to<JsonObject>()); return result;
}
void unavailable_radar(const char* state) {
    const auto doc = radar_snapshot(); const auto value = doc["presence"];
    assert(!board_sensors::presence().available && value["available"] == false);
    assert(std::strcmp(value["state"].as<const char*>(), state) == 0);
    for (const char* key : {"detected", "sampled_at_uptime_ms", "sample_age_ms", "last_detected_uptime_ms", "last_detection_age_ms"}) assert(value[key].isNull());
}
void radar_reset(uint32_t now = 0) {
    test_clock = now; radar_level = 0; Wire1 = SensorWire{}; board_sensors::begin();
}
void advance(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) { ++test_clock; board_sensors::update(); }
}
}
void test_presence() {
    radar_reset();
    unavailable_radar("starting");
    advance(100);
    unavailable_radar("initializing");
    advance(100);
    unavailable_radar("warming_up");
    radar_level = HIGH;
    advance(1900); // Reject the module's self-test output before the complete warm-up.
    unavailable_radar("warming_up");
    advance(100);
    assert(board_sensors::presence().available && board_sensors::presence().detected);
    const auto before = radar_snapshot();
    assert(before["presence"]["last_detected_uptime_ms"].is<uint32_t>());
    assert(Wire1.radar_writes.size() == 24);
    assert((Wire1.radar_writes.front() == std::vector<uint8_t>{0x68, 0x68}));
    assert((Wire1.radar_writes.back() == std::vector<uint8_t>{0, 1}));
    const auto reads = radar_pin_reads;
    for (int i = 0; i < 100; ++i) radar_snapshot();
    assert(reads == radar_pin_reads); // API reads are cached snapshots.
    radar_level = 0;
    advance(50);
    assert(board_sensors::presence().available && !board_sensors::presence().detected);
    test_clock += 501;
    unavailable_radar("stale");
    board_sensors::update();
    assert(board_sensors::presence().available);

    Wire1.radar_present = false;
    radar_level = HIGH; // A floating/high pin must not claim occupancy without the I2C module.
    advance(1000);
    unavailable_radar("not_detected");
    Wire1.radar_present = true;
    advance(7200);
    assert(board_sensors::presence().available && board_sensors::presence().detected);
    assert(radar_snapshot()["temperature_humidity"]["available"] == true);

    radar_reset(); Wire1.radar_write_error = true;
    advance(100);
    unavailable_radar("write_error");
    Wire1.radar_write_error = false;
    advance(7200);
    assert(board_sensors::presence().available);
    assert(radar_snapshot()["presence"]["last_detected_uptime_ms"].isNull());

    radar_reset(); Wire1.bus_ok = false; board_sensors::begin();
    advance(5000);
    unavailable_radar("bus_error");
    radar_reset(0xFFFFFF00u);
    advance(2200);
    assert(board_sensors::presence().available);
    radar_level = HIGH; advance(50);
    assert(board_sensors::presence().detected);
    std::puts("Radar checks passed: initialization, self-test, GPIO, snapshot cadence, errors, recovery and rollover.");
}
#else
void test_presence() {}
#endif
