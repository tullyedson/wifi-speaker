#include <Arduino.h>
#include <algorithm>
#include <WiFi.h>
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
#include "board_display.h"

namespace {
struct Config { String ssid, password, hub, id, token; uint8_t mic_channel = 0; float mic_gain = 1.0f; } config;
struct Frame { uint32_t epoch; uint16_t count; int16_t samples[board_audio::frame_samples]; };
struct PlayJob { uint32_t epoch; char request_id[37]; char url[384]; };
struct PlaybackEvent { char request_id[37]; bool success; bool started; };
QueueHandle_t frames, jobs, results;
Preferences preferences;
WebSocketsClient socket;
WebServer portal(80);
DNSServer dns;
Adafruit_NeoPixel led(1, board_config::led, NEO_GRB + NEO_KHZ800);
std::atomic<bool> connected{false}, ready{false}, muted{false}, hub_paused{false}, playing{false};
std::atomic<uint32_t> epoch{0};
std::atomic<uint8_t> volume{35};
std::atomic<uint16_t> microphone_level{0};
String extra_headers, hub_host, socket_path, state_name = "offline";
uint16_t hub_port = 0;
bool setup_mode = false, audio_ok = false;
uint32_t setup_started = 0, last_wifi_attempt = 0, reboot_at = 0;
uint32_t button_since = 0, last_button_change = 0, last_volume_change = 0;
bool previous_button = false, hold_handled = false;

bool valid_id(const String& id) {
    if (id.isEmpty() || id.length() > 64) return false;
    for (size_t i = 0; i < id.length(); ++i) { const char c = id[i]; if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false; }
    return true;
}
bool parse_hub(const String& url, String& host, uint16_t& port) {
    if (!url.startsWith("http://") || url.length() > 256) return false;
    String authority = url.substring(7);
    if (authority.endsWith("/")) authority.remove(authority.length() - 1);
    if (authority.isEmpty() || authority.indexOf('/') >= 0 || authority.indexOf('@') >= 0 || authority.indexOf('?') >= 0 || authority.indexOf('#') >= 0) return false;
    const int colon = authority.lastIndexOf(':');
    host = colon < 0 ? authority : authority.substring(0, colon);
    const long parsed_port = colon < 0 ? 80 : authority.substring(colon + 1).toInt();
    if (host.isEmpty() || parsed_port < 1 || parsed_port > 65535) return false;
    port = static_cast<uint16_t>(parsed_port); return true;
}
bool read_config(JsonDocument& doc, Config& output) {
    output.ssid = doc["wifi_ssid"] | ""; output.password = doc["wifi_password"] | "";
    output.hub = doc["hub_url"] | ""; output.id = doc["speaker_id"] | ""; output.token = doc["speaker_token"] | "";
    output.mic_channel = doc["mic_channel"] | 0; output.mic_gain = doc["mic_gain"] | 1.0f;
    if (output.hub.endsWith("/")) output.hub.remove(output.hub.length() - 1);
    String host; uint16_t port;
    return !output.ssid.isEmpty() && output.ssid.length() <= 32 && output.password.length() <= 64 && valid_id(output.id)
        && output.token.length() >= 32 && output.token.length() <= 256 && output.mic_channel <= 1
        && output.mic_gain >= 0.25f && output.mic_gain <= 8.0f && parse_hub(output.hub, host, port);
}
bool save_config(JsonDocument& doc) { String encoded; serializeJson(doc, encoded); return preferences.putString("config", encoded) == encoded.length(); }
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
            if (requested_volume >= 0 && requested_volume <= 100) volume = static_cast<uint8_t>(requested_volume);
            ready = true;
            report_volume();
            JsonDocument response; response["type"] = "muted"; response["value"] = muted.load();
            String text; serializeJson(response, text); socket.sendTXT(text);
        } else if (message_type == "state") { state_name = doc["state"] | ""; hub_paused = state_name == "paused"; }
        else if (message_type == "cancel") { ++epoch; if (jobs) xQueueReset(jobs); }
        else if (message_type == "play") {
            PlayJob job = {}; const String request = doc["request_id"] | ""; const String url = doc["audio_url"] | "";
            if (request.length() != 36 || url.length() >= sizeof(job.url) || !url.startsWith(config.hub + "/v1/audio/") || doc["sample_rate"].as<uint32_t>() != board_audio::sample_rate || doc["channels"].as<int>() != 1) return;
            job.epoch = epoch.load(); request.toCharArray(job.request_id, sizeof(job.request_id)); url.toCharArray(job.url, sizeof(job.url));
            if (!jobs || !audio_ok || playing || xQueueSend(jobs, &job, 0) != pdTRUE) { PlaybackEvent event = {}; strlcpy(event.request_id, job.request_id, sizeof(event.request_id)); if (results) xQueueSend(results, &event, 0); }
        }
    }
}

bool read_exact(WiFiClient& stream, uint8_t* output, size_t count, uint32_t owner) {
    size_t total = 0; uint32_t last_data = millis();
    while (total < count) {
        if (owner != epoch.load() || !connected || millis() - last_data > 5000) return false;
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
    Frame frame = {}; PlayJob job;
    while (true) {
        if (xQueueReceive(jobs, &job, 0) == pdTRUE) {
            if (job.epoch != epoch.load()) continue;
            playing = true; xQueueReset(frames);
            const bool success = play_job(job);
            const uint32_t discard_until = millis() + 350;
            while (static_cast<int32_t>(millis() - discard_until) < 0) board_audio::capture(frame.samples, board_audio::frame_samples, config.mic_channel, config.mic_gain);
            playing = false;
            if (job.epoch == epoch.load()) { PlaybackEvent event = {}; strlcpy(event.request_id, job.request_id, sizeof(event.request_id)); event.success = success; xQueueSend(results, &event, 0); }
        } else {
            frame.epoch = epoch.load();
            frame.count = board_audio::capture(frame.samples, board_audio::frame_samples, config.mic_channel, config.mic_gain);
            uint64_t squares = 0;
            for (size_t i = 0; i < frame.count; ++i) { const int32_t sample = frame.samples[i]; squares += static_cast<uint64_t>(sample * sample); }
            microphone_level = frame.count ? static_cast<uint16_t>(sqrt(static_cast<double>(squares) / frame.count) * 1000 / 32768) : 0;
            if (!frame.count) vTaskDelay(pdMS_TO_TICKS(10));
            if (frame.count && ready && !muted && !hub_paused) {
                if (xQueueSend(frames, &frame, 0) != pdTRUE) { Frame discard; xQueueReceive(frames, &discard, 0); xQueueSend(frames, &frame, 0); }
            }
        }
    }
}

const char setup_html[] PROGMEM = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Speaker setup</title><style>body{font:17px system-ui;background:#111a23;color:#edf4f7;margin:30px auto;max-width:520px;padding:20px}label{display:block;margin-top:18px}input,select,button{box-sizing:border-box;width:100%;padding:12px;font:inherit;margin-top:6px;border-radius:7px;border:1px solid #516270;background:#1b2a36;color:white}button{background:#347b6f;cursor:pointer}small{color:#b1c2cb}</style></head><body><h1>Connect your speaker</h1><p>Use the speaker ID and token from the desktop app.</p><form method="post" action="/configure"><label>Wi-Fi name<input name="wifi_ssid" maxlength="32" required></label><label>Wi-Fi password<input name="wifi_password" type="password" maxlength="64"></label><label>Hub address<input name="hub_url" placeholder="http://192.0.2.10:48490" required></label><label>Speaker ID<input name="speaker_id" maxlength="64" required></label><label>Speaker token<input name="speaker_token" type="password" maxlength="256" required></label><label>Microphone channel<select name="mic_channel"><option value="0">Channel 0</option><option value="1">Channel 1</option></select></label><label>Microphone gain<input name="mic_gain" type="number" value="1" min="0.25" max="8" step="0.25"></label><button>Save and connect</button></form><p><small>Settings stay on this speaker. Setup closes after ten minutes. Hold the setup button (Muse: middle; SpotPear: BOOT) for five seconds to reopen setup.</small></p></body></html>)HTML";

void start_setup() {
    if (setup_mode) return;
    setup_mode = true; setup_started = millis(); connected = false; ready = false; ++epoch;
    socket.disconnect(); WiFi.mode(WIFI_AP_STA);
    String suffix = WiFi.macAddress(); suffix.replace(":", ""); suffix = suffix.substring(6);
    WiFi.softAP(("Speaker-setup-" + suffix).c_str(), "speaker-setup");
    dns.start(53, "*", WiFi.softAPIP());
    portal.on("/", HTTP_GET, [] { portal.send_P(200, "text/html", setup_html); });
    portal.on("/configure", HTTP_POST, [] {
        JsonDocument doc;
        for (const char* key : {"wifi_ssid", "wifi_password", "hub_url", "speaker_id", "speaker_token"}) doc[key] = portal.arg(key);
        doc["mic_channel"] = portal.arg("mic_channel").toInt(); doc["mic_gain"] = portal.arg("mic_gain").toFloat();
        Config candidate;
        if (!read_config(doc, candidate)) { portal.send(400, "text/plain", "Check the Wi-Fi name, HTTP hub address, speaker ID, token and microphone settings."); return; }
        if (!save_config(doc)) { portal.send(500, "text/plain", "Could not save settings. Check USB power and retry."); return; }
        portal.send(200, "text/plain", "Saved. The speaker will restart and join your network."); reboot_at = millis() + 1500;
    });
    portal.onNotFound([] { portal.sendHeader("Location", "http://192.168.4.1/", true); portal.send(302, "text/plain", ""); });
    portal.begin(); Serial.println("SETUP: join Speaker-setup network, password speaker-setup, open http://192.168.4.1");
}

void check_serial() {
    static String line;
    while (Serial.available()) {
        const char c = Serial.read();
        if (c == '\n') {
            JsonDocument doc; Config candidate;
            if (!deserializeJson(doc, line) && doc["type"] == "provision" && read_config(doc, candidate) && save_config(doc)) { Serial.println("PROVISIONED"); reboot_at = millis() + 500; }
            else Serial.println("INVALID_CONFIGURATION");
            line = "";
        } else if (c != '\r') { if (line.length() < 2048) line += c; else line = ""; }
    }
}
}

void setup() {
#if defined(BOARD_SPOTPEAR_BALL_V2)
    // Native USB arrives in bursts; accept the entire provisioning message.
    Serial.setRxBufferSize(4096);
#endif
    Serial.begin(115200); preferences.begin("smart-speaker", false);
    pinMode(board_config::button, INPUT_PULLUP);
    if (board_config::volume_up >= 0) pinMode(board_config::volume_up, INPUT_PULLUP);
    if (board_config::volume_down >= 0) pinMode(board_config::volume_down, INPUT_PULLUP);
    board_display::begin();
    led.begin(); led.setBrightness(24); led.setPixelColor(0, led.Color(20, 30, 40)); led.show();
    frames = xQueueCreate(6, sizeof(Frame)); jobs = xQueueCreate(1, sizeof(PlayJob)); results = xQueueCreate(4, sizeof(PlaybackEvent));
    JsonDocument saved;
    const bool configured = !deserializeJson(saved, preferences.getString("config", "{}")) && read_config(saved, config);
    audio_ok = frames && jobs && results && board_audio::begin();
    Serial.printf("SMART_SPEAKER %s audio=%s flash=%u psram=%u\n", board_config::firmware, audio_ok ? "ready" : "error", ESP.getFlashChipSize(), ESP.getPsramSize());
    if (audio_ok && configured) audio_ok = xTaskCreatePinnedToCore(audio_task, "speaker-audio", 16384, nullptr, 2, nullptr, 1) == pdPASS;
    if (!configured) { start_setup(); return; }
    parse_hub(config.hub, hub_host, hub_port);
    WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setAutoReconnect(true); WiFi.begin(config.ssid.c_str(), config.password.c_str());
    socket_path = "/v1/speakers/" + config.id + "/audio";
    extra_headers = "Authorization: Bearer " + config.token;
    socket.setExtraHeaders(extra_headers.c_str());
    socket.begin(hub_host, hub_port, socket_path); socket.onEvent(on_socket);
    socket.setReconnectInterval(3000); socket.enableHeartbeat(15000, 3000, 2);
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
            muted = !muted.load(); if (frames) xQueueReset(frames);
            if (connected) { JsonDocument doc; doc["type"] = "muted"; doc["value"] = muted.load(); String text; serializeJson(doc, text); socket.sendTXT(text); }
        }
    }
    if (button && !hold_handled && millis() - button_since >= 5000) { hold_handled = true; start_setup(); }
    if (touch_action == board_display::Action::mute && !setup_mode) {
        muted = !muted.load(); if (frames) xQueueReset(frames);
        if (connected) { JsonDocument doc; doc["type"] = "muted"; doc["value"] = muted.load(); String text; serializeJson(doc, text); socket.sendTXT(text); }
    }
    if (millis() - last_volume_change > 200) {
        if ((board_config::volume_up >= 0 && digitalRead(board_config::volume_up) == LOW) || touch_action == board_display::Action::louder) { volume = std::min(100, volume.load() + 5); last_volume_change = millis(); if (ready) report_volume(); }
        if ((board_config::volume_down >= 0 && digitalRead(board_config::volume_down) == LOW) || touch_action == board_display::Action::quieter) { volume = std::max(0, volume.load() - 5); last_volume_change = millis(); if (ready) report_volume(); }
    }
    if (setup_mode) { dns.processNextRequest(); portal.handleClient(); if (millis() - setup_started > 600000) { portal.stop(); dns.stop(); WiFi.softAPdisconnect(true); setup_mode = false; } }
    else {
        socket.loop();
        if (WiFi.status() != WL_CONNECTED && millis() - last_wifi_attempt > 15000) { WiFi.reconnect(); last_wifi_attempt = millis(); }
        Frame frame;
        if (frames && ready && !muted && !playing && !hub_paused && xQueueReceive(frames, &frame, 0) == pdTRUE && frame.epoch == epoch.load()) socket.sendBIN(reinterpret_cast<uint8_t*>(frame.samples), frame.count * 2);
        PlaybackEvent event; while (results && xQueueReceive(results, &event, 0) == pdTRUE) { if (connected) send_event(event); }
    }
    uint32_t color = !audio_ok ? led.Color(180, 0, 0) : setup_mode ? led.Color(40, 80, 180) : muted || hub_paused ? led.Color(150, 45, 0) : playing ? led.Color(90, 20, 130) : !ready ? led.Color(130, 10, 10) : state_name == "waiting" || state_name == "transcribing" || state_name == "synthesizing" ? led.Color(90, 80, 0) : led.Color(0, 70, 25);
    static uint32_t previous_color = 0xFFFFFFFF;
    if (color != previous_color) { led.setPixelColor(0, color); led.show(); previous_color = color; }
    const String display_state = !audio_ok ? "Audio error" : setup_mode ? "Wi-Fi setup" : muted ? "Muted" : hub_paused ? "Paused" : playing ? "Speaking" : !ready ? "Connecting" : state_name == "waiting" ? "Thinking" : state_name == "transcribing" ? "Recognizing" : state_name == "synthesizing" ? "Preparing" : "Listening";
    board_display::update(display_state, volume.load(), muted || playing || hub_paused ? 0 : microphone_level.load(), WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Wi-Fi offline");
    delay(1);
}
