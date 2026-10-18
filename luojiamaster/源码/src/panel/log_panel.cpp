#include "log_panel.h"

namespace {

ImVec4 GetLogTextColor(const LogEntry &log) {
    switch (log.type) {
        case 0:
            return ImVec4(221.0f / 255.0f, 221.0f / 255.0f, 221.0f / 255.0f, 221.0f / 255.0f);
        case 1:
            return ImVec4(230.0f / 255.0f, 162.0f / 255.0f, 60.0f / 255.0f, 1.0f);
        case 2:
            return ImVec4(245.0f / 255.0f, 108.0f / 255.0f, 108.0f / 255.0f, 1.0f);
        default:
            return ImVec4(221.0f / 255.0f, 221.0f / 255.0f, 221.0f / 255.0f, 1.0f);
    }
}

struct CombinedLogLine {
    const LogEntry *entry = nullptr;
    const char *source = nullptr;
};

}// namespace

void ShowLogPanel(const State &state) {
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("LogPanel", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    ImGui::Text("本车日志 + 系统日志");

    std::vector<CombinedLogLine> combined_logs;
    combined_logs.reserve(state.vehicle_logs.size() + state.system_logs.size());
    for (const auto &log: state.vehicle_logs) {
        combined_logs.push_back({&log, "本车"});
    }
    for (const auto &log: state.system_logs) {
        combined_logs.push_back({&log, "系统"});
    }
    std::ranges::sort(combined_logs, [](const CombinedLogLine &lhs, const CombinedLogLine &rhs) {
        return lhs.entry->sequence < rhs.entry->sequence;
    });

    ImGui::BeginChild("CombinedLogs", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (combined_logs.empty()) {
        ImGui::TextDisabled("暂无日志");
    } else {
        static uint64_t last_rendered_sequence = 0;
        const uint64_t newest_sequence = combined_logs.back().entry->sequence;
        ImGui::PushTextWrapPos(0.0f);
        for (const CombinedLogLine &line: combined_logs) {
            const ImVec4 color = GetLogTextColor(*line.entry);
            ImGui::TextColored(color, "[%s][%s] %s", line.entry->time, line.source, line.entry->text.c_str());
        }
        ImGui::PopTextWrapPos();
        if (newest_sequence != last_rendered_sequence) {
            ImGui::SetScrollHereY(1.0f);
            last_rendered_sequence = newest_sequence;
        }
    }
    ImGui::EndChild();

    ImGui::End();
}
