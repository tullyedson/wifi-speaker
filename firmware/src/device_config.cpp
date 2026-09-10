#include "device_config.h"
#include <cmath>

namespace device_config {
namespace {
bool token_valid(const String& value) {
    if (value.length() < 32 || value.length() > 256) return false;
    for (char c : value) if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
    return true;
}
bool optional_number(JsonVariantConst v) { return v.isNull() || v.is<float>(); }
}
bool valid_id(const String& value) {
    if (value.isEmpty() || value.length() > 64) return false;
    for (char c : value) if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
    return true;
}
bool parse_hub(const String& url, String& host, uint16_t& port) {
    if (!url.startsWith("http://") || url.length() > 256) return false;
    String authority = url.substring(7);
    if (authority.endsWith("/")) authority.remove(authority.length() - 1);
    const int colon = authority.indexOf(':');
    host = colon < 0 ? authority : authority.substring(0, colon);
    if (host.isEmpty() || host.length() > 253) return false;
    for (char c : host) if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') return false;
    uint32_t number = 80;
    if (colon >= 0) {
        const String digits = authority.substring(colon + 1);
        if (digits.isEmpty() || digits.length() > 5) return false;
        number = 0;
        for (char c : digits) { if (c < '0' || c > '9') return false; number = number * 10 + c - '0'; }
    }
    if (number < 1 || number > 65535) return false;
    port = number; return true;
}
bool read(JsonVariantConst object, Config& output) {
    if (!object.is<JsonObjectConst>()) return false;
    for (JsonPairConst item : object.as<JsonObjectConst>()) {
        const String key = item.key().c_str();
        bool known = false;
        for (const char* allowed : {"type","wifi_ssid","wifi_password","hub_url","speaker_id","speaker_token","control_token","name","tags","mic_channel","mic_gain","volume","muted","brightness","screen_timeout_ms","default_screen"}) if (key == allowed) known = true;
        if (!known || item.value().isNull()) return false;
    }
    for (const char* key : {"wifi_ssid","wifi_password","hub_url","speaker_id","speaker_token"}) if (!object[key].is<const char*>()) return false;
    for (const char* key : {"control_token","name","default_screen"}) if (!object[key].isNull() && !object[key].is<const char*>()) return false;
    if (!optional_number(object["mic_gain"]) || (!object["muted"].isNull() && !object["muted"].is<bool>())) return false;
    for (const char* key : {"mic_channel","volume","brightness","screen_timeout_ms"}) if (!object[key].isNull() && !object[key].is<int>()) return false;
    output = Config{};
    output.ssid = object["wifi_ssid"].as<String>(); output.password = object["wifi_password"].as<String>();
    output.hub = object["hub_url"].as<String>(); output.id = object["speaker_id"].as<String>(); output.token = object["speaker_token"].as<String>();
    output.control_token = object["control_token"] | output.token;
    output.name = object["name"] | "Speaker"; output.default_screen = object["default_screen"] | "eyes";
    const int channel = object["mic_channel"] | 0, brightness = object["brightness"] | 30;
    const int timeout = object["screen_timeout_ms"] | 0;
    output.volume = object["volume"] | -1; output.muted = object["muted"] | false;
    output.mic_gain = object["mic_gain"] | 1.0f;
    if (output.hub.endsWith("/")) output.hub.remove(output.hub.length() - 1);
    String host; uint16_t port;
    if (output.ssid.isEmpty() || output.ssid.length() > 32 || output.password.length() > 64 || !valid_id(output.id)
        || !token_valid(output.token) || !token_valid(output.control_token) || channel < 0 || channel > 1
        || !std::isfinite(output.mic_gain) || output.mic_gain < 0.25f || output.mic_gain > 8.0f
        || output.volume < -1 || output.volume > 100 || brightness < 1 || brightness > 100
        || timeout < 0 || timeout > 3600000 || (timeout != 0 && timeout < 5000)
        || output.name.isEmpty() || output.name.length() > 64
        || (output.default_screen != "eyes" && output.default_screen != "status") || !parse_hub(output.hub, host, port)) return false;
    if (!object["tags"].isNull()) {
        if (!object["tags"].is<JsonArrayConst>() || object["tags"].size() > 16) return false;
        for (JsonVariantConst tag : object["tags"].as<JsonArrayConst>()) {
            if (!tag.is<const char*>()) return false;
            String value = tag.as<String>(); if (value.isEmpty() || value.length() > 64) return false;
            output.tags.push_back(value);
        }
    }
    output.mic_channel = channel; output.brightness = brightness; output.screen_timeout_ms = timeout;
    return true;
}
void write(const Config& value, JsonDocument& output, bool secrets) {
    output.clear();
    if (secrets) {
        output["wifi_ssid"] = value.ssid; output["wifi_password"] = value.password;
        output["speaker_token"] = value.token; output["control_token"] = value.control_token;
    }
    output["hub_url"] = value.hub; output["speaker_id"] = value.id; output["name"] = value.name;
    JsonArray tags = output["tags"].to<JsonArray>(); for (const String& tag : value.tags) tags.add(tag);
    output["mic_channel"] = value.mic_channel; output["mic_gain"] = value.mic_gain;
    output["volume"] = value.volume; output["muted"] = value.muted; output["brightness"] = value.brightness;
    output["screen_timeout_ms"] = value.screen_timeout_ms; output["default_screen"] = value.default_screen;
}
}
