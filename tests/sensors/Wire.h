#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

struct SensorWire {
    bool present = true, bus_ok = true, busy = false, short_read = false, write_error = false;
    unsigned commands = 0;
    bool radar_present = true, radar_write_error = false;
    uint8_t selected_address = 0;
    std::vector<std::vector<uint8_t>> radar_writes;
    std::vector<uint8_t> sample{0x18, 0x80, 0, 0x06, 0, 0, 0x23}; // 25 C, 50% RH, valid CRC.
    std::vector<uint8_t> transmit, receive;
    size_t offset = 0;
    bool begin(int sda, int scl, uint32_t hz) { assert(sda == 41 && scl == 40 && hz == 100000); return bus_ok; }
    void setTimeOut(uint16_t ms) { assert(ms <= 20); }
    void beginTransmission(uint8_t address) { assert(address == 0x38 || address == 0x28); selected_address = address; transmit.clear(); }
    size_t write(const uint8_t* data, size_t count) { transmit.assign(data, data + count); return count; }
    uint8_t endTransmission() {
        if (selected_address == 0x28) {
            if (!radar_present) return 2;
            if (!transmit.empty()) {
                assert(transmit.size() == 2);
                if (radar_write_error) return 4;
                radar_writes.push_back(transmit);
            }
            return 0;
        }
        if (!present) return 2;
        if (!transmit.empty()) {
            assert((transmit == std::vector<uint8_t>{0xAC, 0x33, 0}));
            ++commands;
            if (write_error) return 4;
        }
        return 0;
    }
    size_t requestFrom(uint8_t address, uint8_t count) {
        assert(address == 0x38 && (count == 1 || count == 7));
        offset = 0;
        if (!present) { receive.clear(); return 0; }
        receive = count == 1 ? std::vector<uint8_t>{static_cast<uint8_t>(busy ? 0x98 : 0x18)} : sample;
        if (short_read) receive.pop_back();
        return receive.size();
    }
    uint8_t read() { assert(offset < receive.size()); return receive[offset++]; }
};
extern SensorWire Wire1;
