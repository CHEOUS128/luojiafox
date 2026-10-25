#pragma once

#include <pch.h>

#include "app/application_types.h"
#include "resource/resources.h"

namespace RoboGo::platform {

class PlatformBackend {
public:
    virtual ~PlatformBackend() = default;

    [[nodiscard]] virtual const char *application_name() const = 0;

    virtual bool Initialize() = 0;

    virtual void Shutdown() = 0;

    [[nodiscard]] virtual bool ProcessEvents() = 0;

    [[nodiscard]] virtual bool BeginFrame() = 0;

    virtual void RenderFrame(ImDrawData *draw_data) = 0;

    virtual void SetGameInputCapture(bool enabled) = 0;

    [[nodiscard]] virtual bool UploadTexture(app::TextureSlot slot, const app::VideoFrame &frame) = 0;

    [[nodiscard]] virtual bool UploadTexture(app::TextureSlot slot, const RoboGo::resource::DecodedImage &image) = 0;

    [[nodiscard]] virtual app::TextureView GetTextureView(app::TextureSlot slot) const = 0;

    // 已经成功 Present 的帧数，主循环用它判断画面是不是停了。
    [[nodiscard]] virtual std::uint64_t rendered_frame_count() const = 0;

    // 只有在没有半帧未完成时才允许重建设备（否则会和主线程抢 D3D 对象）。
    [[nodiscard]] virtual bool CanRecreateDeviceNow() const = 0;

    // BeginFrame 返回 false 是不是因为窗口最小化/被遮挡（正常不出帧，不算卡死）。
    [[nodiscard]] virtual bool RenderPausedByWindow() const = 0;

    // 画面挂住后重建 D3D12/ImGui。返回 false 表示重建失败。
    virtual bool TryRecoverRenderDevice() = 0;
};

std::unique_ptr<PlatformBackend> CreatePlatformBackend();

}// namespace RoboGo::platform
