#pragma once

#include "main_window.h"

void ShowBuffPanel(const State &state);
std::string GetBuffName(uint32_t buff_type, int32_t buff_level);
ImVec4 GetBuffTypeColor(uint32_t buff_type);
