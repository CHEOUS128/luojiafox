#include "imgui_setup.h"

#include "embedded_zcool_font.h"

namespace RoboGo::app {

void InitializeCommonImGui(const float main_scale) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();

    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
}

void LoadEmbeddedImGuiFont() {
    ImGuiIO &io = ImGui::GetIO();
    ImGuiStyle &style = ImGui::GetStyle();
    style.FontSizeBase = 14.0f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    ImFont *font = io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char *>(embedded_zcool_font_data),
            static_cast<int>(embedded_zcool_font_data_len),
            0.0f,
            &font_config);
    IM_ASSERT(font != nullptr);

    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
}

}// namespace RoboGo::app
