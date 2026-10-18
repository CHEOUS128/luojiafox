#include "custom_control_payload.h"

namespace {

    inline constexpr auto kAirSupportCounterCooldownDuration = std::chrono::seconds(45);

} // namespace

void NoteOpponentAirSupportCalled(AirSupportCustomControlState &state) {
    state.pending_air_support_call = true;
}

void ResetAirSupportCounterForMatchStart(AirSupportCustomControlState &state) {
    state.previous_counter_remaining = kInitialAirSupportCounterRemaining;
    state.counter_cooldown_started_at.reset();
}

bool NoteAirSupportCounterRemaining(AirSupportCustomControlState &state,
                                    uint8_t remaining,
                                    std::chrono::steady_clock::time_point now) {
    const bool decreased_by_one = state.previous_counter_remaining.has_value()
                                  && static_cast<int>(*state.previous_counter_remaining)
                                         == static_cast<int>(remaining) + 1;
    state.previous_counter_remaining = remaining;
    if (decreased_by_one) {
        state.counter_cooldown_started_at = now;
    }
    return decreased_by_one;
}

uint8_t ConsumeAirSupportCallFlag(AirSupportCustomControlState &state) {
    if (!state.pending_air_support_call) {
        return 0;
    }
    state.pending_air_support_call = false;
    return 1;
}

uint8_t GetAirSupportCounterCooldownByte(const AirSupportCustomControlState &state,
                                         std::chrono::steady_clock::time_point now) {
    if (!state.counter_cooldown_started_at.has_value()) {
        return kInactiveAirSupportCounterCooldown;
    }

    const auto elapsed = now - *state.counter_cooldown_started_at;
    if (elapsed < std::chrono::seconds(0) || elapsed >= kAirSupportCounterCooldownDuration) {
        return kInactiveAirSupportCounterCooldown;
    }

    const auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
    return static_cast<uint8_t>((kAirSupportCounterCooldownDuration - elapsed_seconds).count());
}

std::array<uint8_t, kCustomControlPayloadSize> BuildCustomControlPayload(
    std::span<const uint16_t, kCustomControlRobotCount> enemy_health,
    AirSupportCustomControlState &state,
    uint8_t debug_counter,
    std::chrono::steady_clock::time_point now) {
    std::array<uint8_t, kCustomControlPayloadSize> data{};
    for (size_t k = 0; k < kCustomControlRobotCount; k++) {
        const uint16_t hp = enemy_health[k];
        data[k * 2] = static_cast<uint8_t>(hp & 0xFF);
        data[k * 2 + 1] = static_cast<uint8_t>((hp >> 8) & 0xFF);
    }

    data[kCustomControlAirSupportCallOffset] = ConsumeAirSupportCallFlag(state);
    data[kCustomControlCounterCooldownOffset] = GetAirSupportCounterCooldownByte(state, now);
    data[kCustomControlDebugCounterOffset] = debug_counter;
    return data;
}

bool IsCustomControlPublishDue(std::optional<std::chrono::steady_clock::time_point> last_publish_time,
                               std::chrono::steady_clock::time_point now) {
    return !last_publish_time.has_value() || now - *last_publish_time >= kCustomControlPublishInterval;
}
