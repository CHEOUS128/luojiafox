#include "keyboard_mouse_state_manager.h"

#include <cstdio>

// This executable links the real sampler/serializer, with no MQTT implementation.
// An accidental call to the network overload fails locally instead of connecting.
namespace RoboGo::io {
void MqttClient::publish(const std::string &, const std::string &) {
    throw std::logic_error("input_response_test must never publish to MQTT");
}
}

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

namespace {
using Clock = std::chrono::steady_clock;
struct Context {
    Context() { ImGui::CreateContext(); ImGui::GetIO().IniFilename = nullptr; }
    ~Context() { ImGui::DestroyContext(); }
};

// These are the exact post-NewFrame fields read by the production sampler. The
// separate render harness exercises real AddKeyEvent/NewFrame processing/timing.
void Frame() {
    ++ImGui::GetCurrentContext()->FrameCount;
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        auto *state = ImGui::GetKeyData(static_cast<ImGuiKey>(key));
        state->Down = false; state->DownDuration = state->DownDurationPrev = -1;
    }
    auto &io = ImGui::GetIO();
    for (auto &button : io.MouseDown) button = false;
    io.MouseDelta = ImVec2(); io.MouseWheel = 0;
}
void Key(ImGuiKey key, bool newly_pressed = true) {
    auto *state = ImGui::GetKeyData(key);
    state->Down = true; state->DownDuration = newly_pressed ? 0.0f : 1.0f;
}
void ExpectWire(const std::string &wire, int x, int y, int z,
                std::uint32_t keyboard, bool left = false, bool right = false, bool middle = false) {
    KeyboardMouseControl expected;
    expected.set_mouse_x(x); expected.set_mouse_y(y); expected.set_mouse_z(z);
    expected.set_keyboard_value(keyboard);
    expected.set_left_button_down(left); expected.set_right_button_down(right); expected.set_mid_button_down(middle);
    std::string expected_wire;
    CHECK(expected.SerializeToString(&expected_wire));
    CHECK(wire == expected_wire); // exact protobuf bytes, including signs and field defaults
}

int Run() {
    try {
        Context context;
        std::vector<std::string> packets;
        std::vector<bool> captures;
        const KeyboardMouseSink sink = [&](const std::string &topic, const std::string &payload) {
            CHECK(topic == "KeyboardMouseControl"); packets.push_back(payload);
        };
        {
            RoboGo::testing::OfflineKeyboardMouseTestScope scope;
            const auto base = Clock::time_point{} + std::chrono::seconds(10);
            scope.SetNowForTest(base);
            set_game_input_capture_handler([&](bool enabled) {
                captures.push_back(enabled); set_use_raw_mouse(enabled);
            });

            Frame(); Key(ImGuiKey_W); ImGui::GetIO().MouseDown[0] = true;
            update_keyboard_mouse_state(sink);
            CHECK(packets.empty()); // menus never send ordinary keyboard/mouse packets
            enter_game_mode(); // StartPanel button after the early sample in this same UI frame
            synchronize_game_input_capture(); synchronize_game_input_capture();
            CHECK(captures == std::vector<bool>{true}); CHECK(packets.empty());
            update_keyboard_mouse_state(sink); // a defensive repeated call is also sampling-idempotent
            CHECK(packets.empty());

            Frame(); Key(ImGuiKey_W); Key(ImGuiKey_A); Key(ImGuiKey_LeftShift); Key(ImGuiKey_Q);
            ImGui::GetIO().MouseDown[1] = ImGui::GetIO().MouseDown[2] = true;
            ImGui::GetIO().MouseWheel = 2;
            add_raw_mouse_delta(10, -5);
            update_keyboard_mouse_state(sink);
            CHECK(packets.size() == 1);
            ExpectWire(packets.back(), 4, 2, 2, 0x55, false, true, true);

            scope.SetNowForTest(base + std::chrono::milliseconds(5));
            Frame(); add_raw_mouse_delta(10, 5); ImGui::GetIO().MouseWheel = 1;
            update_keyboard_mouse_state(sink); update_keyboard_mouse_state(sink);
            synchronize_game_input_capture();
            CHECK(packets.size() == 1);
            scope.SetNowForTest(base + std::chrono::milliseconds(12));
            Frame(); add_raw_mouse_delta(5, -10); ImGui::GetIO().MouseWheel = 1;
            update_keyboard_mouse_state(sink); CHECK(packets.size() == 1);
            scope.SetNowForTest(base + std::chrono::milliseconds(13));
            Frame(); Key(ImGuiKey_D); ImGui::GetIO().MouseDown[0] = true;
            add_raw_mouse_delta(2, -2); // original per-sample truncation: .8 and -.8 both become zero
            update_keyboard_mouse_state(sink); update_keyboard_mouse_state(sink);
            CHECK(packets.size() == 2);
            ExpectWire(packets.back(), 6, 2, 2, 0x08, true);

            scope.SetNowForTest(base + std::chrono::milliseconds(14));
            Frame(); Key(ImGuiKey_Escape); add_raw_mouse_delta(999, 999);
            update_keyboard_mouse_state(sink); update_keyboard_mouse_state(sink);
            synchronize_game_input_capture();
            CHECK(!is_in_game_mode()); CHECK(packets.size() == 2);
            CHECK(captures == (std::vector<bool>{true, false}));
            scope.SetNowForTest(base + std::chrono::milliseconds(30));
            Frame(); Key(ImGuiKey_Escape, false);
            update_keyboard_mouse_state(sink);
            CHECK(!is_in_game_mode()); CHECK(packets.size() == 2);
            enter_game_mode(); synchronize_game_input_capture();
            CHECK(captures == (std::vector<bool>{true, false, true}));
            scope.SetNowForTest(base + std::chrono::milliseconds(31));
            Frame(); update_keyboard_mouse_state(sink);
            CHECK(packets.size() == 3); ExpectWire(packets.back(), 0, 0, 0, 0);

            scope.Reset(); packets.clear(); captures.clear(); scope.SetNowForTest(base);
            enter_game_mode(); synchronize_game_input_capture(); // no platform handler: ImGui MouseDelta branch
            Frame(); ImGui::GetIO().MouseDelta = ImVec2(-10, 5); ImGui::GetIO().MouseWheel = .5f;
            update_keyboard_mouse_state(sink);
            CHECK(packets.size() == 1); ExpectWire(packets.back(), -4, -2, 0, 0);
            scope.SetNowForTest(base + std::chrono::milliseconds(13));
            Frame(); ImGui::GetIO().MouseDelta = ImVec2(1, -1);
            update_keyboard_mouse_state(sink);
            CHECK(packets.size() == 2); ExpectWire(packets.back(), 0, 0, 0, 0);

            scope.Reset(); packets.clear(); scope.SetNowForTest(base);
            enter_game_mode(); set_use_raw_mouse(true);
            TeammatePointerGuard guard;
            auto step = [&](bool viewing) {
                auto result = guard.Advance(viewing, any_match_controls_held());
                update_keyboard_mouse_state(sink, result.sample);
                return result;
            };
            Frame(); Key(ImGuiKey_W); ImGui::GetIO().MouseDown[0] = true;
            step(false); CHECK(packets.size() == 1); ExpectWire(packets.back(), 0, 0, 0, 1, true);
            scope.SetNowForTest(base + std::chrono::milliseconds(1));
            Frame(); Key(ImGuiKey_W); ImGui::GetIO().MouseDown[0] = true; add_raw_mouse_delta(900, 800);
            CHECK(step(true).release_cursor); CHECK(packets.size() == 2); ExpectWire(packets.back(), 0, 0, 0, 0);
            for (int tick = 1; tick <= 5; ++tick) {
                scope.SetNowForTest(base + std::chrono::milliseconds(1 + 14 * tick));
                Frame();
                if (tick <= 2) ImGui::GetIO().MouseDown[0] = true;
                if (tick == 3) Key(ImGuiKey_A); // Mouse up but a game key is still held.
                ImGui::GetIO().MouseWheel = 1; add_raw_mouse_delta(900, 800);
                const auto transition = step(tick == 1);
                CHECK(transition.restore_cursor == (tick == 4));
                CHECK(transition.sample == KeyboardMouseSample::Neutral);
                CHECK(packets.size() == static_cast<size_t>(2 + tick));
                ExpectWire(packets.back(), 0, 0, 0, 0);
            }
            scope.SetNowForTest(base + std::chrono::milliseconds(100));
            Frame(); Key(ImGuiKey_D); add_raw_mouse_delta(5, 10);
            CHECK(step(false).sample == KeyboardMouseSample::Match);
            ExpectWire(packets.back(), 2, -4, 0, 8);
            std::puts("PASS teammate overview: immediate neutral, held mouse/key blocked, recenter discarded, next fresh sample restored");
        }
        CHECK(!is_in_game_mode());
        set_game_input_capture_handler([](bool) {});
        bool rejected_live_scope = false;
        try { RoboGo::testing::OfflineKeyboardMouseTestScope forbidden; }
        catch (const std::logic_error &) { rejected_live_scope = true; }
        set_game_input_capture_handler({});
        CHECK(rejected_live_scope);
        std::puts("PASS input response: exact protobuf; 13ms gate; accumulated integer motion; no duplicate sample; ESC once; same-frame menu capture; menus silent; exit clears pending motion; offline scope guard");
        return 0;
    } catch (const std::exception &error) {
        set_game_input_capture_handler({});
        std::fprintf(stderr, "FAIL input response: %s\n", error.what()); return 1;
    }
}
}

int main() { return Run(); }
