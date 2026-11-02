#pragma once

#include <pch.h>

// 队友视角窗口（2027 赛季两台自定义客户端互联）。
// 画面来源是对方电脑转过来的官方 UDP 图传原始 HEVC，解码后就是一帧普通纹理。
//
// 三种形态：
//   小窗   —— 默认，放在右侧栏最上面
//   K 折叠 —— 只剩一条标题，显示连接状态
//   L 接管 —— 占满主画面，滚轮 1x~4x 放大（放大时以中心为基准裁切）
//
// 按键（K/L/滚轮）统一在 application.cpp 里处理，这里只负责画。
struct TeammateViewState {
    bool enabled = false;         // 小窗要不要出现
    char role = 'A';              // 当前角色：'A' / 'B'（标题条上可点，切换后写回 ini）
    bool role_from_default = false;// 真：地址是内置默认（标题条上标 [默认A]）
    ImTextureRef texture_ref;     // 队友画面纹理（可能还没创建）
    ImVec2 texture_size;          // 纹理尺寸，用于等比缩放
    bool display_ready = false;   // 超时内有过帧 → 可以显示最后一帧
    bool live = false;            // 在线
    bool collapsed = false;       // K 折叠
    bool take_main_screen = false;// L 占主画面
    float zoom = 1.0f;            // 主画面模式放大倍数
    bool peer_connected = false;  // 我们连上了对方
    bool peer_served = false;     // 对方连上了我们
    // 对方画面倒置（随握手传过来的「他那边 UDP 画面是否倒置」）。
    // 我们转过去的是原始字节，所以这个值就是画面的真实朝向，直接用。
    bool frame_inverted = false;
    std::uint64_t frames_sent = 0;
    std::uint64_t frames_decoded = 0;
    std::string peer_address;
};

// 小窗 / 折叠条。pos/size 由 main_window.cpp 算好。
// 返回 true 表示用户这一帧点了标题条上的「角色」——调用方负责换地址并在内存里切角色。
bool ShowTeammateViewport(const TeammateViewState &view,
                          const ImVec2 &pos,
                          const ImVec2 &size,
                          bool collapsed);

// L 模式：把队友画面画到主画面上（viewport 全屏，不画背景）。
void ShowTeammateFullscreen(const TeammateViewState &view);

// 折叠条高度（像素）
float TeammateCollapsedHeight();
