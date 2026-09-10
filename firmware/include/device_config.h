#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

namespace device_config {
struct Config {
    String ssid, password, hub, id, token, control_token;
    String name = "Speaker", default_screen = "eyes";
    std::vector<String> tags;
    uint8_t mic_channel = 0, brightness = 30;
    float mic_gain = 1.0f;
    int volume = -1; // Until a local setting is saved, accept the hub's initial volume.
    bool muted = false;
    uint32_t screen_timeout_ms = 0; // Display stays on; this never controls audio capture.
};
bool valid_id(const String& value);
bool parse_hub(const String& url, String& host, uint16_t& port);
bool read(JsonVariantConst object, Config& output);
void write(const Config& value, JsonDocument& output, bool secrets);
}
