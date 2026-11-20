#pragma once

#include <pch.h>

// 槽位按本方岗位固定排序。即便其中一人断线，另一个人的位置也不变。
struct TeammatePeerView {
    std::string label;
    ImTextureRef texture_ref;
    ImVec2 texture_size;
    bool display_ready = false;
    bool live = false;
    bool peer_connected = false;
    bool peer_served = false;
    bool frame_inverted = false;
    bool paused = false;  // 本地测试中手动断开这一份录像
    int robot_id = 0;
};

struct TeammateViewState {
    std::array<TeammatePeerView, 3> peers;
    int count = 0;
    bool collapsed = false;
    bool take_main_screen = false;
    int focused = -1; // -1 为总览，否则是放大的槽位
    float zoom = 1.f;
};

// 仅当该格实际提交了视频画面时返回 true，等待文字不算更新。
bool DrawTeammateTile(ImDrawList *draw, const TeammatePeerView &peer, const ImRect &rect,
                      float zoom, bool large);

// 返回右栏实际占用的高度，其他状态从小窗下方开始排。
float ShowTeammateSidebar(const TeammateViewState &view, ImVec2 top_left, float width, float max_height);

// 返回被单击的槽位。菜单打开和联盟赛的单路查看都没有切换槽位的动作。
int ShowTeammateOverview(const TeammateViewState &view, bool menu_open, bool local_test, unsigned& drawn_mask);
