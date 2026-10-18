#pragma once

#include <pch.h>

bool ShowCustomVideoPanel(ImTextureRef video_texture_ref,
                          const ImVec2 &video_size,
                          const char *panel_title,
                          bool video_display_ready,
                          std::string_view unavailable_message,
                          bool udp_video_inverted);
