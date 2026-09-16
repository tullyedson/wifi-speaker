#pragma once
#include <cstdint>

// Backlight policy only. No Wi-Fi, microphone or CPU power state is changed.
class DisplayPower {
public:
    enum class Reason { awake, manual, idle, absence };
    void configure(uint32_t now, uint32_t idle_ms, uint32_t presence_ms) {
        if (idle_ms != idle_timeout_ || presence_ms != presence_timeout_) last_activity_ = now;
        idle_timeout_ = idle_ms; presence_timeout_ = presence_ms;
    }
    void wake(uint32_t now) { last_activity_ = now; reason_ = Reason::awake; }
    void sleep() { reason_ = Reason::manual; }
    void update(uint32_t now, bool busy, bool radar_available, bool detected) {
        const bool active = presence_timeout_ != 0 && radar_available;
        if (active && !presence_active_) last_activity_ = now; // Full grace period after recovery.
        presence_active_ = active;
        if (reason_ == Reason::manual) return;
        if (busy) wake(now);
        if (active) {
            if (detected) wake(now);
            else if (now - last_activity_ >= presence_timeout_) reason_ = Reason::absence;
        } else {
            // Losing the sensor or disabling presence must not leave the face stuck off.
            if (reason_ == Reason::absence) wake(now);
            if (idle_timeout_ && now - last_activity_ >= idle_timeout_) reason_ = Reason::idle;
        }
    }
    bool awake() const { return reason_ == Reason::awake; }
    bool presence_active() const { return presence_active_; }
    uint32_t idle_timeout() const { return idle_timeout_; }
    uint32_t presence_timeout() const { return presence_timeout_; }
    Reason reason() const { return reason_; }
    const char* sleep_reason() const {
        switch (reason_) {
            case Reason::manual: return "manual";
            case Reason::idle: return "idle";
            case Reason::absence: return "absence";
            default: return nullptr;
        }
    }
private:
    Reason reason_ = Reason::awake;
    bool presence_active_ = false;
    uint32_t last_activity_ = 0, idle_timeout_ = 0, presence_timeout_ = 0;
};
