#include "app/local_input_probe.h"
#include "app/offline_test_summary.h"

#include <cstdio>
#include <sstream>

namespace RoboGo::io {
void MqttClient::publish(const std::string &, const std::string &) {
    throw std::logic_error("local_input_probe_test must never publish to MQTT");
}
}

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

namespace {
using Probe = RoboGo::app::offline::LocalInputProbe;
using Clock = Probe::Clock;
struct Fixture {
    Clock::time_point now = Clock::time_point{} + std::chrono::seconds(10);
    std::ostringstream csv;
    std::unique_ptr<Probe> probe;
    Fixture() {
        ImGui::CreateContext();
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr; io.DisplaySize = ImVec2(640, 480); io.DeltaTime = 1.0f / 60;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        CHECK(io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 16) != nullptr);
        probe = std::make_unique<Probe>(csv, now - std::chrono::seconds(2), [&] { return now; });
    }
    ~Fixture() { probe.reset(); ImGui::DestroyContext(); }
    void Frame(int advance_ms = 14, bool excluded = false) {
        now += std::chrono::milliseconds(advance_ms);
        ImGui::GetIO().DeltaTime = 1.0f / 60;
        ImGui::NewFrame(); probe->Sample(excluded); ImGui::EndFrame();
    }
    void CheckAccounting() {
        CHECK(probe->Generated() == probe->Matched() + probe->Lost());
        CHECK(probe->Scheduled() == probe->Generated() + probe->Skipped());
        std::size_t lines = 0;
        for (char c : csv.str()) lines += c == '\n';
        CHECK(lines == probe->Generated() + 1);
    }
};

void NormalEvents() {
    Fixture f;
    for (int i = 0; i < 12; ++i) {
        if (i) f.now = Clock::time_point{} + std::chrono::seconds(10) + std::chrono::milliseconds(i * 250);
        f.probe->QueueIfDue(); f.Frame();
    }
    f.probe->Finish(); f.CheckAccounting();
    CHECK(f.probe->Generated() == 12 && f.probe->Matched() == 12 && f.probe->Lost() == 0 && f.probe->Mismatched() == 0);
    CHECK(f.csv.str().find(",outcome,lost,loss_reason,resolved_us\n") != std::string::npos);
    std::puts("PASS normal: 12 real ImGui events matched; 13ms sampler unchanged; complete CSV accounting");
}
void FocusLossAndRecovery() {
    Fixture f;
    f.probe->QueueIfDue(); ImGui::GetIO().AddFocusEvent(false); f.Frame();
    CHECK(f.probe->Lost() == 1 && f.probe->Matched() == 0 && !f.probe->Pending());
    CHECK(f.csv.str().find(",lost,1,focus_lost,") != std::string::npos);
    ImGui::GetIO().AddFocusEvent(true); f.Frame(); f.Frame();
    f.now = Clock::time_point{} + std::chrono::milliseconds(10250);
    f.probe->QueueIfDue(); f.Frame(); f.probe->Finish(); f.CheckAccounting();
    CHECK(f.probe->Generated() == 2 && f.probe->Matched() == 1 && f.probe->Lost() == 1 && f.probe->Mismatched() == 0);
    std::puts("PASS focus: lost event recorded once; actual neutral output drains old motion; later event matches");
}
void KeyUpInsideGate() {
    Fixture f;
    f.Frame(0); // Starts the real 13ms gate with a neutral packet.
    f.now += std::chrono::milliseconds(5);
    f.probe->QueueIfDue(); f.Frame(0); // Motion accumulates but cannot publish yet.
    CHECK(f.probe->Pending() && f.probe->Matched() == 0);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false); f.Frame(9);
    CHECK(f.probe->Lost() == 1 && f.probe->Mismatched() == 1);
    CHECK(f.csv.str().find(",lost,1,key_state_mismatch,") != std::string::npos);
    f.Frame(); f.Frame();
    f.now = Clock::time_point{} + std::chrono::milliseconds(10250);
    f.probe->QueueIfDue(); f.Frame(); f.probe->Finish(); f.CheckAccounting();
    CHECK(f.probe->Generated() == 2 && f.probe->Matched() == 1 && f.probe->Lost() == 1);
    std::puts("PASS gated key-up: wrong key/motion pair is lost, never matched or reused; probe continues");
}
void TimeoutNoRenderAndNoCatchup() {
    Fixture f;
    f.probe->QueueIfDue();
    f.now += std::chrono::milliseconds(1001); f.probe->QueueIfDue();
    CHECK(!f.probe->Pending() && f.probe->Lost() == 1 && f.probe->Generated() == 1);
    CHECK(f.csv.str().find(",lost,1,timeout,") != std::string::npos);
    f.probe->QueueIfDue(); CHECK(f.probe->Generated() == 1); // No catch-up burst.
    f.Frame(); f.Frame(); f.Frame(); // Real key-up and neutral packet, no scope reset.
    f.now = Clock::time_point{} + std::chrono::milliseconds(11250);
    f.probe->QueueIfDue(); f.Frame(); f.probe->Finish(); f.CheckAccounting();
    CHECK(f.probe->Generated() == 2 && f.probe->Matched() == 1 && f.probe->Lost() == 1);
    std::puts("PASS timeout: no BeginFrame for >1s still resolves lost event; absolute schedule does not catch up");
}
void BadMotionAndPendingStop() {
    {
        Fixture f; f.probe->QueueIfDue(); add_raw_mouse_delta(5, 0); f.Frame();
        CHECK(f.probe->Matched() == 0 && f.probe->Lost() == 1 && f.probe->Mismatched() == 1);
        CHECK(f.csv.str().find(",lost,1,payload_mismatch,") != std::string::npos);
        f.probe->Finish(); f.CheckAccounting();
    }
    {
        Fixture f; f.probe->QueueIfDue(); f.probe->Finish(); f.probe->Finish(); f.CheckAccounting();
        CHECK(f.probe->Generated() == 1 && f.probe->Lost() == 1 && !f.probe->Pending());
        CHECK(f.csv.str().find(",lost,1,run_stopped,") != std::string::npos);
        f.now += std::chrono::seconds(2); f.probe->QueueIfDue(); CHECK(f.probe->Generated() == 1);
    }
    std::puts("PASS failure accounting: bad payload and pending-at-stop remain lost; Finish is idempotent");
}
void FinalGpuCounter() {
    RoboGo::platform::RenderDiagnostics before, final;
    before.wait_timeouts = 3; final = before; final.wait_timeouts = 4;
    final.frames_submitted = 20; final.resource_creations = 19;
    std::ostringstream json;
    json << "{\"test\":true";
    RoboGo::app::offline::WriteFinalRenderCounters(json, before, final);
    json << '}';
    CHECK(json.str().find("\"backend_wait_timeouts\": 4") != std::string::npos);
    CHECK(json.str().find("\"backend_wait_timeouts_before_shutdown\": 3") != std::string::npos);
    CHECK(json.str().find("\"backend_diagnostics_phase\": \"after_shutdown\"") != std::string::npos);
    std::puts("PASS final GPU counters: post-shutdown=4, before=3; explicit final field, no inferred zero");
}
}

int main() {
    try {
        spdlog::set_level(spdlog::level::warn);
        NormalEvents(); FocusLossAndRecovery(); KeyUpInsideGate();
        TimeoutNoRenderAndNoCatchup(); BadMotionAndPendingStop(); FinalGpuCounter();
        std::puts("PASS all: real ImGui + unchanged sampler/protobuf; no OS keys, renderer, MQTT, network or vehicle");
        return 0;
    } catch (const std::exception &error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
