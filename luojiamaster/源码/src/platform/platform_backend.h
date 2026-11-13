#pragma once

#include <pch.h>

#include "app/application_types.h"
#include "resource/resources.h"

namespace RoboGo::platform {

struct RenderDiagnostics {
    std::uint64_t frames_submitted = 0;
    std::uint64_t uploads_succeeded = 0;
    std::uint64_t upload_failures = 0;
    std::uint64_t skipped_busy = 0;
    std::uint64_t recoveries = 0;
    std::uint64_t resource_creations = 0;
    std::uint64_t wait_timeouts = 0;
    std::uint64_t deferred_resources = 0;
    std::uint64_t max_deferred_resources = 0;
    std::string first_failure_stage;
    std::int64_t first_hresult = 0;
};

class PlatformBackend {
public:
    virtual ~PlatformBackend() = default;

    [[nodiscard]] virtual const char *application_name() const = 0;

    virtual bool Initialize() = 0;

    virtual void Shutdown() = 0;

    [[nodiscard]] virtual bool ProcessEvents() = 0;

    [[nodiscard]] virtual bool BeginFrame() = 0;

    [[nodiscard]] virtual bool RenderFrame(ImDrawData *draw_data) = 0;

    // Explicitly enabled by the isolated offline runner; no network/control objects exist there.
    virtual void EnableOfflineTestMode(bool) {}
    virtual void SetTestFailure(std::string_view, int = 1) {}
    virtual void RequestTestWindowAction(int) {}
    [[nodiscard]] virtual RenderDiagnostics GetRenderDiagnostics() const { return {}; }

    virtual void SetGameInputCapture(bool enabled) = 0;

    [[nodiscard]] virtual bool UploadTexture(app::TextureSlot slot, const app::VideoFrame &frame) = 0;

    [[nodiscard]] virtual bool UploadTexture(app::TextureSlot slot, const RoboGo::resource::DecodedImage &image) = 0;

    [[nodiscard]] virtual app::TextureView GetTextureView(app::TextureSlot slot) const = 0;
};

std::unique_ptr<PlatformBackend> CreatePlatformBackend();

}// namespace RoboGo::platform
