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
};

std::unique_ptr<PlatformBackend> CreatePlatformBackend();

}// namespace RoboGo::platform
