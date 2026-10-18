#pragma once

#include <pch.h>

inline constexpr size_t kCustomControlRobotCount = 5;
inline constexpr size_t kCustomControlHealthDataSize = kCustomControlRobotCount * sizeof(uint16_t);
inline constexpr size_t kCustomControlAirSupportCallOffset = kCustomControlHealthDataSize;
inline constexpr size_t kCustomControlCounterCooldownOffset = kCustomControlAirSupportCallOffset + 1;
inline constexpr size_t kCustomControlDebugCounterOffset = kCustomControlCounterCooldownOffset + 1;
inline constexpr size_t kCustomControlPayloadSize = kCustomControlDebugCounterOffset + 1;
inline constexpr uint8_t kInactiveAirSupportCounterCooldown = 255;
inline constexpr uint8_t kInitialAirSupportCounterRemaining = 5;
inline constexpr auto kCustomControlPublishInterval = std::chrono::milliseconds(100);

struct AirSupportCustomControlState {
    bool pending_air_support_call = false;
    std::optional<uint8_t> previous_counter_remaining;
    std::optional<std::chrono::steady_clock::time_point> counter_cooldown_started_at;
};

void NoteOpponentAirSupportCalled(AirSupportCustomControlState &state);

void ResetAirSupportCounterForMatchStart(AirSupportCustomControlState &state);

bool NoteAirSupportCounterRemaining(AirSupportCustomControlState &state,
                                    uint8_t remaining,
                                    std::chrono::steady_clock::time_point now);

uint8_t ConsumeAirSupportCallFlag(AirSupportCustomControlState &state);

uint8_t GetAirSupportCounterCooldownByte(const AirSupportCustomControlState &state,
                                         std::chrono::steady_clock::time_point now);

std::array<uint8_t, kCustomControlPayloadSize> BuildCustomControlPayload(
    std::span<const uint16_t, kCustomControlRobotCount> enemy_health,
    AirSupportCustomControlState &state,
    uint8_t debug_counter,
    std::chrono::steady_clock::time_point now);

bool IsCustomControlPublishDue(std::optional<std::chrono::steady_clock::time_point> last_publish_time,
                               std::chrono::steady_clock::time_point now);
