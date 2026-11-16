#pragma once

#include "keyboard_mouse_state_manager.h"

namespace RoboGo::app::offline {

// The offline runner alone owns this scope. Events go to its real ImGui context
// and the unchanged sampler's local sink, never to OS input or MQTT.
class LocalInputProbe {
public:
    using Clock = std::chrono::steady_clock;
    using TestClock = std::function<Clock::time_point()>;

private:
    struct Event {
        std::uint64_t id;
        bool down;
        Clock::time_point enqueued;
        std::optional<Clock::time_point> sampled;
        bool excluded = false;
    };
    RoboGo::testing::OfflineKeyboardMouseTestScope scope_;
    std::ostream &output_;
    Clock::time_point start_, next_;
    TestClock test_clock_;
    std::optional<Event> pending_;
    bool quarantine_ = false, finished_ = false;
    std::uint64_t generated_ = 0, matched_ = 0, lost_ = 0, mismatched_ = 0;
    std::uint64_t packets_ = 0, scheduled_ = 0, skipped_ = 0;

    Clock::time_point Now() const { return test_clock_ ? test_clock_() : Clock::now(); }
    std::int64_t Us(Clock::time_point value) const {
        return std::chrono::duration_cast<std::chrono::microseconds>(value - start_).count();
    }
    static bool Neutral(const KeyboardMouseControl &message) {
        return message.keyboard_value() == 0 && message.mouse_x() == 0
            && message.mouse_y() == 0 && message.mouse_z() == 0
            && !message.left_button_down() && !message.right_button_down()
            && !message.mid_button_down();
    }
    void WriteResult(bool matched, const char *reason, Clock::time_point resolved,
                     const KeyboardMouseControl *message = nullptr) {
        const auto &event = *pending_;
        output_ << event.id << ',' << event.down << ',' << Us(event.enqueued) << ',';
        if (event.sampled) output_ << Us(*event.sampled);
        output_ << ',';
        if (message) output_ << Us(resolved);
        output_ << ',';
        if (message) output_ << std::chrono::duration<double, std::micro>(resolved - event.enqueued).count();
        output_ << ',';
        if (message && event.sampled) output_ << std::chrono::duration<double, std::micro>(resolved - *event.sampled).count();
        output_ << ',';
        if (message) output_ << message->mouse_x();
        output_ << ',';
        if (message) output_ << message->keyboard_value();
        output_ << ',' << matched << ',' << event.excluded << ','
                << (matched ? "matched" : "lost") << ',' << !matched << ','
                << reason << ',' << Us(resolved) << '\n';
    }
    void Lose(const char *reason, Clock::time_point now,
              const KeyboardMouseControl *message = nullptr, bool drain = true) {
        if (!pending_) return;
        WriteResult(false, reason, now, message);
        ++lost_;
        pending_.reset();
        quarantine_ = drain;
        // Release only this test's synthetic key. Do not clear input globals or
        // reset the real sampling gate. Wait for an actual neutral sink packet
        // before reusing the probe, so old motion cannot match a later event.
        if (drain) ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
    }
    void Expire(Clock::time_point now) {
        if (pending_ && now - pending_->enqueued >= std::chrono::seconds(1))
            Lose("timeout", now);
    }

public:
    LocalInputProbe(std::ostream &output, Clock::time_point start, TestClock test_clock = {})
        : output_(output), start_(start), next_(start + std::chrono::seconds(2)),
          test_clock_(std::move(test_clock)) {
        scope_.Reset();
        set_use_raw_mouse(true);
        enter_game_mode();
        output_ << "event_id,key_down,event_enqueued_us,sample_started_us,sink_received_us,event_to_sink_us,sample_to_sink_us,mouse_x,keyboard_value,matched,excluded,outcome,lost,loss_reason,resolved_us\n";
    }
    void QueueIfDue() {
        if (finished_) return;
        const auto now = Now();
        Expire(now); // Runs even when a render frame cannot begin.
        if (now < next_) return;
        const auto overdue = std::chrono::duration_cast<std::chrono::milliseconds>(now - next_).count();
        const auto slots = static_cast<std::uint64_t>(overdue / 250) + 1;
        scheduled_ += slots;
        next_ += std::chrono::milliseconds(slots * 250);
        if (pending_ || quarantine_ || ImGui::GetIO().AppFocusLost) {
            skipped_ += slots;
            return;
        }
        skipped_ += slots - 1; // Absolute slots, no late catch-up burst.
        const bool down = (scheduled_ % 2) != 0;
        ImGui::GetIO().AddKeyEvent(ImGuiKey_W, down);
        add_raw_mouse_delta(5.0f, 0.0f); // Real 0.4 sensitivity serializes x=2.
        pending_ = Event{++generated_, down, now, std::nullopt, false};
    }
    void Sample(bool excluded) {
        if (finished_) return;
        const auto sampled = Now();
        if (test_clock_) scope_.SetNowForTest(sampled); // Unit fixture only.
        if (pending_) {
            pending_->sampled = sampled;
            pending_->excluded = pending_->excluded || excluded;
        }
        // NewFrame consumes focus events and clears keys; EndFrame clears this
        // transient flag. Observing only before NewFrame misses the loss.
        if (ImGui::GetIO().AppFocusLost) Lose("focus_lost", sampled);
        Expire(sampled);
        update_keyboard_mouse_state(KeyboardMouseSink([&](const std::string &topic, const std::string &payload) {
            ++packets_;
            KeyboardMouseControl message;
            if (topic != "KeyboardMouseControl" || !message.ParseFromString(payload)) {
                ++mismatched_;
                Lose("invalid_payload", Now());
                return;
            }
            if (quarantine_) {
                if (Neutral(message)) quarantine_ = false;
                return;
            }
            if (!pending_) return;
            if (bool(message.keyboard_value() & 1U) != pending_->down) {
                // Nonzero motion proves part of this synthetic pair was
                // consumed with the wrong key. Never attach it to a later event.
                if (message.mouse_x() != 0 || message.mouse_y() != 0 || message.mouse_z() != 0) {
                    ++mismatched_;
                    Lose("key_state_mismatch", Now(), &message);
                }
                return;
            }
            const bool correct = message.keyboard_value() == (pending_->down ? 1U : 0U)
                && message.mouse_x() == 2 && message.mouse_y() == 0 && message.mouse_z() == 0
                && !message.left_button_down() && !message.right_button_down() && !message.mid_button_down();
            if (!correct) {
                ++mismatched_;
                Lose("payload_mismatch", Now(), &message);
                return;
            }
            WriteResult(true, "", Now(), &message);
            ++matched_;
            pending_.reset();
        }));
    }
    void Finish() {
        if (finished_) return;
        Lose("run_stopped", Now(), nullptr, false);
        finished_ = true;
    }
    std::uint64_t Generated() const { return generated_; }
    std::uint64_t Matched() const { return matched_; }
    std::uint64_t Lost() const { return lost_; }
    std::uint64_t Mismatched() const { return mismatched_; }
    std::uint64_t Packets() const { return packets_; }
    std::uint64_t Scheduled() const { return scheduled_; }
    std::uint64_t Skipped() const { return skipped_; }
    bool Pending() const { return pending_.has_value(); }
};

} // namespace RoboGo::app::offline
