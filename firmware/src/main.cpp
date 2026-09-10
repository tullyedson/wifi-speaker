#include <Arduino.h>
#include <algorithm>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <atomic>
#include <cmath>
#include "board_audio.h"
#include "board_config.h"
#include "board_controls.h"
#include "board_display.h"
#include "device_config.h"

namespace {
using device_config::Config;
using device_config::parse_hub;
Config config;
struct Frame { uint32_t epoch; uint16_t count; int16_t samples[board_audio::frame_samples]; };
struct PlayJob { uint32_t epoch; char request_id[37]; char url[384]; };
struct PlaybackEvent { char request_id[37]; bool success; bool started; };
struct AlarmJob { uint32_t generation, duration_ms; uint8_t pattern, volume; };
QueueHandle_t frames, jobs, results, alarm_jobs;
Preferences preferences;
WebSocketsClient socket;
WebServer portal(80);
DNSServer dns;
Adafruit_NeoPixel led(board_config::led_count, board_config::led, NEO_GRB + NEO_KHZ800);
std::atomic<bool> connected{false}, ready{false}, muted{false}, hub_paused{false}, playing{false};
std::atomic<uint32_t> epoch{0};
std::atomic<uint8_t> volume{35};
std::atomic<uint16_t> microphone_level{0};
std::atomic<float> microphone_gain{1.0f};
std::atomic<uint8_t> microphone_channel{0};
std::atomic<bool> alarm_active{false}, alarm_playing{false};
std::atomic<uint32_t> alarm_generation{0}, alarm_completed{0}, alarm_failed{0};
struct AlarmReceipt { String id; uint32_t duration_ms = 0, at = 0; uint8_t pattern = 0, volume = 0; };
AlarmReceipt alarm_history[8];
size_t alarm_history_next = 0;
String alarm_id, alarm_state = "idle", led_effect = "status";
uint32_t led_color = 0, led_until = 0, controls_changed = 0;
uint8_t led_brightness = 20;
String extra_headers, hub_host, socket_path, state_name = "offline";
uint16_t hub_port = 0;
bool setup_mode = false, audio_ok = false, configured = false, mdns_ready = false;
uint32_t setup_started = 0, last_wifi_attempt = 0, reboot_at = 0;
uint32_t button_since = 0, last_button_change = 0, last_volume_change = 0;
bool previous_button = false, hold_handled = false;

bool read_config(JsonDocument& doc, Config& output) {
    return device_config::read(doc.as<JsonVariantConst>(), output);
}
bool save_config(JsonDocument& doc) { String encoded; serializeJson(doc, encoded); return preferences.putString("config", encoded) == encoded.length(); }
bool save_controls() { JsonDocument doc; device_config::write(config, doc, true); return save_config(doc); }
void report_mute() {
    if (!connected) return;
    JsonDocument doc; doc["type"] = "muted"; doc["value"] = muted.load() || alarm_active.load();
    String text; serializeJson(doc, text); socket.sendTXT(text);
}
void toggle_mute() { muted = !muted.load(); config.muted = muted.load(); controls_changed = millis(); if (frames) xQueueReset(frames); report_mute(); }
void stop_alarm() {
    if (!alarm_active) return;
    ++alarm_generation; alarm_active = false; if (alarm_jobs) xQueueReset(alarm_jobs);
    alarm_state = "cancelled"; report_mute();
}
void send_event(const PlaybackEvent& event) {
    JsonDocument doc;
    doc["type"] = event.started ? "playback_started" : event.success ? "playback_finished" : "playback_error";
    doc["request_id"] = event.request_id;
    if (!event.success && !event.started) doc["code"] = "audio_transfer_or_playback_failed";
    String encoded; serializeJson(doc, encoded); socket.sendTXT(encoded);
}
void report_volume() {
    JsonDocument doc; doc["type"] = "volume"; doc["value"] = volume.load();
    String text; serializeJson(doc, text); socket.sendTXT(text);
}
void hello() {
    JsonDocument doc; doc["type"] = "hello"; doc["protocol"] = 1; doc["speaker_id"] = config.id;
    doc["firmware"] = board_config::firmware; doc["sample_rate"] = board_audio::sample_rate;
    doc["channels"] = 1; doc["sample_format"] = "s16le";
    String text; serializeJson(doc, text); socket.sendTXT(text);
}
void on_socket(WStype_t type, uint8_t* payload, size_t length) {
    if (type == WStype_CONNECTED) { connected = true; ready = false; ++epoch; hello(); }
    else if (type == WStype_DISCONNECTED) { connected = false; ready = false; ++epoch; state_name = "offline"; if (frames) xQueueReset(frames); }
    else if (type == WStype_TEXT) {
        if (length > 2048) return;
        JsonDocument doc; if (deserializeJson(doc, payload, length)) return;
        const String message_type = doc["type"] | "";
        if (message_type == "ready" && doc["protocol"] == 1) {
            const int requested_volume = doc["volume"] | 35;
            if (config.volume < 0 && requested_volume >= 0 && requested_volume <= 100) volume = static_cast<uint8_t>(requested_volume);
            ready = true;
            report_volume();
            report_mute();
        } else if (message_type == "state") { state_name = doc["state"] | ""; hub_paused = state_name == "paused"; }
        else if (message_type == "cancel") { ++epoch; if (jobs) xQueueReset(jobs); }
        else if (message_type == "play") {
            PlayJob job = {}; const String request = doc["request_id"] | ""; const String url = doc["audio_url"] | "";
            if (request.length() != 36 || url.length() >= sizeof(job.url) || !url.startsWith(config.hub + "/v1/audio/") || doc["sample_rate"].as<uint32_t>() != board_audio::sample_rate || doc["channels"].as<int>() != 1) return;
            job.epoch = epoch.load(); request.toCharArray(job.request_id, sizeof(job.request_id)); url.toCharArray(job.url, sizeof(job.url));
            if (!jobs || !audio_ok || playing || alarm_active || xQueueSend(jobs, &job, 0) != pdTRUE) { PlaybackEvent event = {}; strlcpy(event.request_id, job.request_id, sizeof(event.request_id)); if (results) xQueueSend(results, &event, 0); }
        }
    }
}

bool read_exact(WiFiClient& stream, uint8_t* output, size_t count, uint32_t owner) {
    size_t total = 0; uint32_t last_data = millis();
    while (total < count) {
        if (owner != epoch.load() || !connected || alarm_active || millis() - last_data > 5000) return false;
        const int available = stream.available();
        if (available > 0) { const int got = stream.read(output + total, std::min(count - total, static_cast<size_t>(available))); if (got > 0) { total += got; last_data = millis(); } }
        else { if (!stream.connected()) return false; vTaskDelay(pdMS_TO_TICKS(2)); }
    }
    return true;
}
uint16_t le16(const uint8_t* p) { return p[0] | (static_cast<uint16_t>(p[1]) << 8); }
uint32_t le32(const uint8_t* p) { return p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
bool play_job(const PlayJob& job) {
    HTTPClient http; http.setConnectTimeout(5000); http.setTimeout(5000);
    WiFiClient client;
    if (!http.begin(client, job.url)) return false;
    http.addHeader("Authorization", "Bearer " + config.token);
    if (http.GET() != HTTP_CODE_OK || job.epoch != epoch.load()) { http.end(); return false; }
    WiFiClient& stream = *http.getStreamPtr();
    uint8_t header[12]; bool valid = read_exact(stream, header, sizeof(header), job.epoch) && memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0;
    bool format_ok = false; uint32_t remaining = 0; uint32_t headers_read = 12;
    while (valid && headers_read < 4096) {
        uint8_t chunk[8]; valid = read_exact(stream, chunk, sizeof(chunk), job.epoch); if (!valid) break;
        headers_read += 8; const uint32_t size = le32(chunk + 4);
        if (memcmp(chunk, "data", 4) == 0) { remaining = size; break; }
        if (size > 1024) { valid = false; break; }
        uint8_t body[1024]; valid = read_exact(stream, body, size + (size & 1), job.epoch); headers_read += size + (size & 1);
        if (memcmp(chunk, "fmt ", 4) == 0) { format_ok = size >= 16 && le16(body) == 1 && le16(body + 2) == 1 && le32(body + 4) == board_audio::sample_rate && le16(body + 14) == 16; }
    }
    valid = valid && format_ok && remaining > 0 && remaining <= 5760000 && remaining % 2 == 0;
    if (valid) { PlaybackEvent event = {}; strlcpy(event.request_id, job.request_id, sizeof(event.request_id)); event.started = true; xQueueSend(results, &event, 0); }
    int16_t audio[board_audio::frame_samples];
    while (valid && remaining > 0) {
        const size_t bytes = std::min(remaining, static_cast<uint32_t>(sizeof(audio)));
        valid = read_exact(stream, reinterpret_cast<uint8_t*>(audio), bytes, job.epoch) && board_audio::play(audio, bytes / 2, volume.load());
        remaining -= bytes;
    }
    if (valid) vTaskDelay(pdMS_TO_TICKS(180));
    board_audio::silence(); http.end(); return valid;
}
void audio_task(void*) {
    Frame frame = {}; PlayJob job; AlarmJob alarm;
    while (true) {
        if (xQueueReceive(alarm_jobs, &alarm, 0) == pdTRUE) {
            if (alarm.generation != alarm_generation.load() || !alarm_active) continue;
            playing = true; alarm_playing = true; xQueueReset(frames);
            const uint32_t count = alarm.duration_ms * 16;
            float phase = 0;
            bool success = true;
            for (uint32_t offset = 0; offset < count && alarm.generation == alarm_generation.load(); offset += board_audio::frame_samples) {
                const size_t n = std::min(static_cast<uint32_t>(board_audio::frame_samples), count - offset);
                for (size_t i = 0; i < n; ++i) {
                    const float time = (offset + i) / 16000.0f;
                    const float beat = std::fmod(time, alarm.pattern == 0 ? 1.2f : 0.7f);
                    const float frequency = alarm.pattern == 2 ? 700 + 260 * std::sin(time * 6.2831853f) : alarm.pattern == 0 ? (beat < 0.16f ? 880 : 1174.66f) : 880;
                    const float envelope = alarm.pattern == 0 ? std::exp(-beat * 6) : alarm.pattern == 1 ? (beat < 0.3f ? 1.0f : 0.0f) : 0.65f;
                    const float fade = std::min(1.0f, std::min((offset + i) / 160.0f, (count - offset - i) / 320.0f));
                    phase += 6.2831853f * frequency / 16000; if (phase > 6.2831853f) phase -= 6.2831853f;
                    frame.samples[i] = static_cast<int16_t>(std::sin(phase) * 10000 * envelope * fade);
                }
                if (!board_audio::play(frame.samples, n, alarm.volume)) { success = false; break; }
            }
            board_audio::silence();
            const uint32_t discard_until = millis() + 350;
            while (static_cast<int32_t>(millis() - discard_until) < 0) board_audio::capture(frame.samples, board_audio::frame_samples, microphone_channel.load(), microphone_gain.load());
            playing = false; alarm_playing = false;
            if (alarm.generation == alarm_generation.load()) { if (!success) alarm_failed = alarm.generation; alarm_completed = alarm.generation; }
        } else if (xQueueReceive(jobs, &job, 0) == pdTRUE) {
            if (job.epoch != epoch.load()) continue;
            playing = true; xQueueReset(frames);
            const bool success = play_job(job);
            const uint32_t discard_until = millis() + 350;
            while (static_cast<int32_t>(millis() - discard_until) < 0) board_audio::capture(frame.samples, board_audio::frame_samples, microphone_channel.load(), microphone_gain.load());
            playing = false;
            if (job.epoch == epoch.load()) { PlaybackEvent event = {}; strlcpy(event.request_id, job.request_id, sizeof(event.request_id)); event.success = success; xQueueSend(results, &event, 0); }
        } else {
            frame.epoch = epoch.load();
            frame.count = board_audio::capture(frame.samples, board_audio::frame_samples, microphone_channel.load(), microphone_gain.load());
            uint64_t squares = 0;
            for (size_t i = 0; i < frame.count; ++i) { const int32_t sample = frame.samples[i]; squares += static_cast<uint64_t>(sample * sample); }
            microphone_level = frame.count ? static_cast<uint16_t>(sqrt(static_cast<double>(squares) / frame.count) * 1000 / 32768) : 0;
            if (!frame.count) vTaskDelay(pdMS_TO_TICKS(10));
            if (frame.count && ready && !muted && !hub_paused && !alarm_active) {
                if (xQueueSend(frames, &frame, 0) != pdTRUE) { Frame discard; xQueueReceive(frames, &discard, 0); xQueueSend(frames, &frame, 0); }
            }
        }
    }
}

const char setup_html[] PROGMEM = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Speaker setup</title><style>body{font:17px system-ui;background:#111a23;color:#edf4f7;margin:30px auto;max-width:520px;padding:20px}label{display:block;margin-top:18px}input,select,button{box-sizing:border-box;width:100%;padding:12px;font:inherit;margin-top:6px;border-radius:7px;border:1px solid #516270;background:#1b2a36;color:white}button{background:#347b6f;cursor:pointer}small{color:#b1c2cb}</style></head><body><h1>Connect your speaker</h1><p>Use the speaker ID and token from the desktop app.</p><form method="post" action="/configure"><label>Wi-Fi name<input name="wifi_ssid" maxlength="32" required></label><label>Wi-Fi password<input name="wifi_password" type="password" maxlength="64"></label><label>Hub address<input name="hub_url" placeholder="http://192.0.2.10:48490" required></label><label>Speaker ID<input name="speaker_id" maxlength="64" required></label><label>Speaker token<input name="speaker_token" type="password" maxlength="256" required></label><label>Microphone channel<select name="mic_channel"><option value="0">Channel 0</option><option value="1">Channel 1</option></select></label><label>Microphone gain<input name="mic_gain" type="number" value="1" min="0.25" max="8" step="0.25"></label><button>Save and connect</button></form><p><small>Settings stay on this speaker. Setup closes after ten minutes. Hold the setup button (Muse: middle; SpotPear: BOOT) for five seconds to reopen setup.</small></p></body></html>)HTML";

void json_response(int code, JsonDocument& doc) {
    String text; serializeJson(doc, text); portal.sendHeader("Cache-Control", "no-store"); portal.send(code, "application/json", text);
}
void api_error(int code, const char* error) { JsonDocument doc; doc["error"] = error; json_response(code, doc); }
bool authorized() {
    if (!configured || setup_mode) { api_error(503, "Device is not provisioned or is in setup mode"); return false; }
    const String supplied = portal.header("Authorization"), expected = "Bearer " + config.control_token;
    uint32_t difference = supplied.length() ^ expected.length();
    for (size_t i = 0; i < expected.length(); ++i) difference |= expected[i] ^ (i < supplied.length() ? supplied[i] : 0);
    if (difference) { api_error(401, "Device control bearer token required"); return false; }
    if (reboot_at) { api_error(503, "Configuration saved; device is restarting"); return false; }
    return true;
}
bool api_body(JsonDocument& doc, std::initializer_list<const char*> keys) {
    if (!authorized()) return false;
    if (portal.arg("plain").length() > 4096) { api_error(413, "JSON exceeds 4096 bytes"); return false; }
    if (!portal.header("Content-Type").startsWith("application/json") || deserializeJson(doc, portal.arg("plain")) || !doc.is<JsonObject>() || doc.size() == 0) { api_error(400, "A nonempty JSON object is required"); return false; }
    for (JsonPair item : doc.as<JsonObject>()) {
        bool known = false; for (const char* key : keys) if (strcmp(item.key().c_str(), key) == 0) known = true;
        if (!known || item.value().isNull()) { api_error(400, "Unknown or null field"); return false; }
    }
    return true;
}
bool integer_between(JsonVariantConst value, int low, int high) { return value.is<int>() && value.as<int>() >= low && value.as<int>() <= high; }
void device_status() {
    if (!authorized()) return;
    JsonDocument doc; doc["api_version"] = 1; doc["speaker_id"] = config.id; doc["name"] = config.name;
    JsonArray tags = doc["tags"].to<JsonArray>(); for (const String& tag : config.tags) tags.add(tag);
    doc["firmware"] = board_config::firmware; doc["audio_ready"] = audio_ok; doc["hub_connected"] = ready.load();
    doc["muted"] = muted.load(); doc["volume"] = volume.load(); doc["mic_gain"] = microphone_gain.load();
    doc["mic_level"] = microphone_level.load() / 1000.0f; doc["mic_channel"] = microphone_channel.load();
    doc["audio_uploading"] = ready && !muted && !hub_paused && !playing && !alarm_active && !setup_mode;
    doc["uptime_ms"] = millis(); doc["free_heap"] = ESP.getFreeHeap(); doc["settings_pending"] = controls_changed != 0;
    board_display::status(doc["display"].to<JsonObject>());
    doc["led"]["count"] = board_config::led_count; doc["led"]["effect"] = led_effect;
    char color[8]; snprintf(color, sizeof(color), "#%06X", led_color); doc["led"]["color"] = color;
    doc["led"]["brightness"] = led_brightness;
    doc["alarm"]["id"] = alarm_id; doc["alarm"]["state"] = alarm_active && alarm_playing ? "playing" : alarm_state;
    json_response(200, doc);
}
void configure_api() {
    JsonDocument patch;
    if (!api_body(patch, {"wifi_ssid","wifi_password","hub_url","speaker_token","control_token","name","tags","mic_channel","mic_gain","volume","muted","brightness","screen_timeout_ms","default_screen"})) return;
    if (!patch["volume"].isNull() && !integer_between(patch["volume"], 0, 100)) { api_error(400, "Volume must be 0 to 100"); return; }
    JsonDocument merged; device_config::write(config, merged, true);
    for (JsonPair item : patch.as<JsonObject>()) merged[item.key()] = item.value();
    Config candidate; if (!read_config(merged, candidate)) { api_error(400, "Invalid device configuration"); return; }
    const bool restart = candidate.ssid != config.ssid || candidate.password != config.password || candidate.hub != config.hub || candidate.token != config.token || candidate.control_token != config.control_token;
    if (!save_config(merged)) { api_error(500, "Could not persist configuration"); return; }
    controls_changed = 0;
    if (restart) reboot_at = millis() + 750;
    else {
        // Audio worker reads the transport strings. They remain immutable until reboot.
        config.name = candidate.name; config.tags = candidate.tags; config.default_screen = candidate.default_screen;
        config.volume = candidate.volume; config.mic_gain = candidate.mic_gain; config.mic_channel = candidate.mic_channel;
        config.muted = candidate.muted; config.brightness = candidate.brightness; config.screen_timeout_ms = candidate.screen_timeout_ms;
        if (config.volume >= 0) volume = config.volume;
        microphone_gain = config.mic_gain; microphone_channel = config.mic_channel; muted = config.muted;
        if (frames) xQueueReset(frames); report_volume(); report_mute();
        board_display::configure(config.brightness, config.screen_timeout_ms, config.default_screen);
    }
    JsonDocument response; response["saved"] = true; response["restarting"] = restart; json_response(200, response);
}
void display_api() {
    JsonDocument doc;
    if (!api_body(doc, {"screen","awake","expression","duration_ms","look_x","look_y","blink"})) return;
    if (!board_display::available()) { api_error(409, "This board has no display"); return; }
    if ((!doc["screen"].isNull() && (!doc["screen"].is<const char*>() || (doc["screen"] != "eyes" && doc["screen"] != "status")))
        || (!doc["expression"].isNull() && (!doc["expression"].is<const char*>() || !board_display::valid_expression(doc["expression"].as<String>())))
        || (!doc["awake"].isNull() && !doc["awake"].is<bool>()) || (!doc["blink"].isNull() && !doc["blink"].is<bool>())
        || (!doc["duration_ms"].isNull() && !integer_between(doc["duration_ms"], 0, 600000))) { api_error(400, "Invalid display command"); return; }
    for (const char* key : {"look_x","look_y"}) if (!doc[key].isNull() && (!doc[key].is<float>() || !std::isfinite(doc[key].as<float>()) || std::abs(doc[key].as<float>()) > 1)) { api_error(400, "Gaze coordinates must be between -1 and 1"); return; }
    if (!doc["screen"].isNull()) board_display::select(doc["screen"].as<String>());
    if (!doc["expression"].isNull() || !doc["look_x"].isNull() || !doc["look_y"].isNull()) board_display::expression(doc["expression"] | "neutral", doc["duration_ms"] | 0u, !doc["look_x"].isNull() || !doc["look_y"].isNull(), doc["look_x"] | 0.0f, doc["look_y"] | 0.0f);
    if (doc["blink"] == true) board_display::blink();
    if (!doc["awake"].isNull()) { if (doc["awake"].as<bool>()) board_display::wake(); else board_display::sleep(); }
    JsonDocument response; board_display::status(response.to<JsonObject>()); json_response(200, response);
}
void led_api() {
    JsonDocument doc; if (!api_body(doc, {"color","effect","brightness","duration_ms"})) return;
    const String color = doc["color"] | "#77EEDD", effect = doc["effect"] | "solid";
    if ((!doc["color"].isNull() && !doc["color"].is<const char*>()) || color.length() != 7 || color[0] != '#'
        || (!doc["effect"].isNull() && !doc["effect"].is<const char*>()) || (effect != "status" && effect != "solid" && effect != "breathe" && effect != "blink")
        || (!doc["brightness"].isNull() && !integer_between(doc["brightness"], 0, 100))
        || (!doc["duration_ms"].isNull() && !integer_between(doc["duration_ms"], 0, 600000))) { api_error(400, "Invalid LED command"); return; }
    for (size_t i = 1; i < 7; ++i) if (!isxdigit(static_cast<unsigned char>(color[i]))) { api_error(400, "Color must be #RRGGBB"); return; }
    led_color = strtoul(color.c_str() + 1, nullptr, 16); led_effect = effect; led_brightness = doc["brightness"] | 20;
    const uint32_t duration = doc["duration_ms"] | 0u; led_until = duration ? millis() + duration : 0;
    JsonDocument response; response["accepted"] = true; json_response(200, response);
}
void alarm_api() {
    JsonDocument doc; if (!api_body(doc, {"id","duration_ms","pattern","volume"})) return;
    const String id = doc["id"] | "", pattern = doc["pattern"] | "chime";
    if (!doc["id"].is<const char*>() || !device_config::valid_id(id) || (!doc["pattern"].isNull() && !doc["pattern"].is<const char*>())
        || (pattern != "chime" && pattern != "beep" && pattern != "siren")
        || !integer_between(doc["duration_ms"], 100, 60000)
        || (!doc["volume"].isNull() && !integer_between(doc["volume"], 0, 100))) { api_error(400, "Alarm needs an ID, duration 100..60000 ms, valid pattern and volume"); return; }
    const uint8_t selected = pattern == "chime" ? 0 : pattern == "beep" ? 1 : 2, loudness = doc["volume"] | 50;
    const uint32_t duration = doc["duration_ms"].as<uint32_t>();
    for (const AlarmReceipt& receipt : alarm_history) if (receipt.id == id && millis() - receipt.at < 600000) {
        if (receipt.duration_ms != duration || receipt.pattern != selected || receipt.volume != loudness) { api_error(409, "Alarm ID was used with different parameters"); return; }
        JsonDocument response; response["status"] = "duplicate"; json_response(200, response); return;
    }
    if (!audio_ok || !alarm_jobs) { api_error(503, "Audio hardware is unavailable"); return; }
    if (alarm_active) { api_error(409, "Stop the active alarm before starting another"); return; }
    AlarmJob job = {++alarm_generation, duration, selected, loudness};
    alarm_active = true;
    if (xQueueSend(alarm_jobs, &job, 0) != pdTRUE) { alarm_active = false; api_error(409, "Alarm queue is busy"); return; }
    alarm_id = id; alarm_state = "queued";
    AlarmReceipt& receipt = alarm_history[alarm_history_next];
    receipt.id = id; receipt.duration_ms = duration; receipt.at = millis(); receipt.pattern = selected; receipt.volume = loudness;
    alarm_history_next = (alarm_history_next + 1) % 8;
    ++epoch; if (jobs) xQueueReset(jobs); if (frames) xQueueReset(frames); report_mute();
    board_display::wake(); board_display::expression("surprised", duration + 400, false, 0, 0);
    JsonDocument response; response["status"] = "accepted"; response["id"] = id; json_response(202, response);
}
bool setup_client() { return setup_mode && portal.client().localIP() == WiFi.softAPIP(); }
void start_api() {
    const char* headers[] = {"Authorization", "Content-Type"}; portal.collectHeaders(headers, 2);
    portal.on("/v1/device", HTTP_GET, device_status);
    portal.on("/v1/config", HTTP_GET, [] { if (!authorized()) return; JsonDocument doc; device_config::write(config, doc, false); json_response(200, doc); });
    portal.on("/v1/config", HTTP_PATCH, configure_api);
    portal.on("/v1/display", HTTP_POST, display_api);
    portal.on("/v1/led", HTTP_POST, led_api);
    portal.on("/v1/alarms", HTTP_POST, alarm_api);
    portal.on("/v1/alarms/stop", HTTP_POST, [] {
        JsonDocument doc; if (!api_body(doc, {"id"})) return;
        if (!doc["id"].is<const char*>() || doc["id"].as<String>() != alarm_id) { api_error(404, "Alarm ID does not match"); return; }
        stop_alarm(); JsonDocument response; response["status"] = alarm_state; json_response(200, response);
    });
    portal.on("/", HTTP_GET, [] { if (setup_client()) portal.send_P(200, "text/html", setup_html); else portal.send(200, "text/plain", "WiFi Speaker. Authenticated device API: /v1/device"); });
    portal.on("/configure", HTTP_POST, [] {
        if (!setup_client()) { api_error(403, "Join the physically opened setup network first"); return; }
        JsonDocument doc;
        for (const char* key : {"wifi_ssid", "wifi_password", "hub_url", "speaker_id", "speaker_token"}) doc[key] = portal.arg(key);
        doc["mic_channel"] = portal.arg("mic_channel").toInt(); doc["mic_gain"] = portal.arg("mic_gain").toFloat();
        Config candidate;
        if (!read_config(doc, candidate)) { portal.send(400, "text/plain", "Check Wi-Fi, hub URL, ID, token and microphone settings."); return; }
        if (!save_config(doc)) { api_error(500, "Could not save settings"); return; }
        controls_changed = 0; portal.send(200, "text/plain", "Saved. Restarting and joining your network."); reboot_at = millis() + 1500;
    });
    portal.onNotFound([] { if (setup_client()) { portal.sendHeader("Location", "http://192.168.4.1/", true); portal.send(302, "text/plain", ""); } else api_error(404, "No such endpoint"); });
    portal.begin();
}

void start_setup() {
    if (setup_mode) return;
    setup_mode = true; setup_started = millis(); connected = false; ready = false; ++epoch;
    stop_alarm(); socket.disconnect(); WiFi.mode(WIFI_AP_STA);
    String suffix = WiFi.macAddress(); suffix.replace(":", ""); suffix = suffix.substring(6);
    WiFi.softAP(("Speaker-setup-" + suffix).c_str(), "speaker-setup");
    dns.start(53, "*", WiFi.softAPIP());
    board_display::select("status"); board_display::wake();
    Serial.println("SETUP: join Speaker-setup network, password speaker-setup, open http://192.168.4.1");
}

void check_serial() {
    static String line;
    while (Serial.available()) {
        const char c = Serial.read();
        if (c == '\n') {
            JsonDocument doc; Config candidate;
            if (!deserializeJson(doc, line) && doc["type"] == "provision" && read_config(doc, candidate) && save_config(doc)) { controls_changed = 0; Serial.println("PROVISIONED"); reboot_at = millis() + 500; }
            else Serial.println("INVALID_CONFIGURATION");
            line = "";
        } else if (c != '\r') { if (line.length() < 2048) line += c; else line = ""; }
    }
}
}

void setup() {
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    // Native USB arrives in bursts; accept the entire provisioning message.
    Serial.setRxBufferSize(4096);
#endif
    Serial.begin(115200); preferences.begin("smart-speaker", false);
    pinMode(board_config::button, INPUT_PULLUP);
    if (board_config::volume_up >= 0) pinMode(board_config::volume_up, INPUT_PULLUP);
    if (board_config::volume_down >= 0) pinMode(board_config::volume_down, INPUT_PULLUP);
    board_display::begin();
    led.begin(); led.setBrightness(255); led.fill(led.Color(4, 6, 8)); led.show();
    frames = xQueueCreate(6, sizeof(Frame)); jobs = xQueueCreate(1, sizeof(PlayJob)); results = xQueueCreate(4, sizeof(PlaybackEvent)); alarm_jobs = xQueueCreate(1, sizeof(AlarmJob));
    JsonDocument saved;
    configured = !deserializeJson(saved, preferences.getString("config", "{}")) && read_config(saved, config);
    microphone_gain = config.mic_gain; microphone_channel = config.mic_channel; muted = config.muted;
    if (config.volume >= 0) volume = config.volume;
    board_display::configure(config.brightness, config.screen_timeout_ms, config.default_screen);
    audio_ok = frames && jobs && results && alarm_jobs && board_audio::begin();
    Serial.printf("SMART_SPEAKER %s audio=%s flash=%u psram=%u\n", board_config::firmware, audio_ok ? "ready" : "error", ESP.getFlashChipSize(), ESP.getPsramSize());
    if (audio_ok && configured) audio_ok = xTaskCreatePinnedToCore(audio_task, "speaker-audio", 16384, nullptr, 2, nullptr, 1) == pdPASS;
    if (!configured) { start_setup(); start_api(); return; }
    parse_hub(config.hub, hub_host, hub_port);
    WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setAutoReconnect(true); WiFi.begin(config.ssid.c_str(), config.password.c_str());
    socket_path = "/v1/speakers/" + config.id + "/audio";
    extra_headers = "Authorization: Bearer " + config.token;
    socket.setExtraHeaders(extra_headers.c_str());
    socket.begin(hub_host, hub_port, socket_path); socket.onEvent(on_socket);
    socket.setReconnectInterval(3000); socket.enableHeartbeat(15000, 3000, 2);
    start_api();
}

void loop() {
    check_serial();
    if (reboot_at && static_cast<int32_t>(millis() - reboot_at) >= 0) ESP.restart();
    const auto touch_action = board_display::poll();
    const bool button = digitalRead(board_config::button) == LOW;
    if (button != previous_button && millis() - last_button_change > 40) {
        last_button_change = millis(); previous_button = button;
        if (button) { button_since = millis(); hold_handled = false; board_display::wake(); }
        else if (!hold_handled && millis() - button_since > 40) {
            if (alarm_active) stop_alarm();
            else if (board_display::available()) { config.default_screen = board_display::cycle(); controls_changed = millis(); }
            else toggle_mute();
        }
    }
    if (button && !hold_handled && millis() - button_since >= 5000) { hold_handled = true; start_setup(); }
    static uint8_t key_state = 0;
    static uint32_t keys_at = 0;
    uint8_t key_pressed = 0;
    if (audio_ok && millis() - keys_at > 100) {
        const uint8_t current = board_controls::keys();
        key_pressed = current & ~key_state; key_state = current; keys_at = millis();
    }
    if ((touch_action == board_display::Action::mute || (key_pressed & 2)) && !setup_mode) {
        if (alarm_active) stop_alarm(); else toggle_mute();
    }
    if (millis() - last_volume_change > 200) {
        const uint8_t previous_volume = volume.load();
        if ((board_config::volume_up >= 0 && digitalRead(board_config::volume_up) == LOW) || touch_action == board_display::Action::louder || (key_pressed & 4)) { volume = std::min(100, volume.load() + 5); last_volume_change = millis(); if (ready) report_volume(); }
        if ((board_config::volume_down >= 0 && digitalRead(board_config::volume_down) == LOW) || touch_action == board_display::Action::quieter || (key_pressed & 1)) { volume = std::max(0, volume.load() - 5); last_volume_change = millis(); if (ready) report_volume(); }
        if (volume != previous_volume) { config.volume = volume.load(); controls_changed = millis(); }
    }
    if (touch_action == board_display::Action::gain_up || touch_action == board_display::Action::gain_down) {
        config.mic_gain = std::max(0.25f, std::min(8.0f, microphone_gain.load() + (touch_action == board_display::Action::gain_up ? 0.25f : -0.25f)));
        microphone_gain = config.mic_gain; controls_changed = millis();
    }
    if (controls_changed && configured && !reboot_at && millis() - controls_changed >= 2000) {
        if (save_controls()) controls_changed = 0; else controls_changed = millis();
    }
    if (alarm_active && alarm_completed.load() == alarm_generation.load()) { alarm_active = false; alarm_state = alarm_failed.load() == alarm_generation.load() ? "error" : "finished"; report_mute(); }
    portal.handleClient();
    if (setup_mode) { dns.processNextRequest(); if (millis() - setup_started > 600000) { dns.stop(); WiFi.softAPdisconnect(true); setup_mode = false; if (configured) ESP.restart(); } }
    else if (configured) {
        socket.loop();
        if (WiFi.status() != WL_CONNECTED && millis() - last_wifi_attempt > 15000) { WiFi.reconnect(); last_wifi_attempt = millis(); }
        if (!mdns_ready && WiFi.status() == WL_CONNECTED) { mdns_ready = MDNS.begin(config.id.c_str()); if (mdns_ready) { MDNS.addService("http", "tcp", 80); MDNS.addService("wifi-speaker", "tcp", 80); } }
        Frame frame;
        // Catch up after a display refresh without accumulating unbounded audio.
        for (int i = 0; i < 4 && frames && ready && !muted && !playing && !hub_paused && !alarm_active; ++i) {
            if (xQueueReceive(frames, &frame, 0) != pdTRUE) break;
            if (frame.epoch == epoch.load()) socket.sendBIN(reinterpret_cast<uint8_t*>(frame.samples), frame.count * 2);
        }
        PlaybackEvent event; while (results && xQueueReceive(results, &event, 0) == pdTRUE) { if (connected) send_event(event); }
    }
    uint32_t color = !audio_ok ? led.Color(180, 0, 0) : setup_mode ? led.Color(40, 80, 180) : muted || hub_paused ? led.Color(150, 45, 0) : playing ? led.Color(90, 20, 130) : !ready ? led.Color(130, 10, 10) : state_name == "waiting" || state_name == "transcribing" || state_name == "synthesizing" ? led.Color(90, 80, 0) : led.Color(0, 70, 25);
    float intensity = 0.2f;
    if (led_until && static_cast<int32_t>(millis() - led_until) >= 0) { led_effect = "status"; led_until = 0; }
    if (alarm_active) color = millis() % 600 < 300 ? led.Color(255, 90, 10) : 0;
    else if (led_effect != "status" && audio_ok && !setup_mode && !muted && !hub_paused) {
        color = led_color; intensity = led_brightness / 100.0f;
        if (led_effect == "breathe") intensity *= 0.2f + 0.8f * (1 + std::sin(millis() / 700.0)) / 2;
        if (led_effect == "blink" && millis() % 1000 >= 500) intensity = 0;
    }
    color = led.Color(((color >> 16) & 255) * intensity, ((color >> 8) & 255) * intensity, (color & 255) * intensity);
    static uint32_t previous_color = 0xFFFFFFFF;
    if (color != previous_color) { led.fill(color); led.show(); previous_color = color; }
    const String display_state = !audio_ok ? "Audio error" : setup_mode ? "Wi-Fi setup" : alarm_active ? "Alarm" : muted ? "Muted" : hub_paused ? "Paused" : playing ? "Speaking" : !ready ? "Connecting" : state_name == "waiting" ? "Thinking" : state_name == "transcribing" ? "Recognizing" : state_name == "synthesizing" ? "Preparing" : "Listening";
    board_display::update(display_state, volume.load(), microphone_gain.load(), muted || playing || hub_paused ? 0 : microphone_level.load(), setup_mode ? "192.168.4.1" : WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Wi-Fi offline");
    delay(1);
}
