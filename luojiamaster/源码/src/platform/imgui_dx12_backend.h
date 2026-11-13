#pragma once

#include <cstdint>
#include <exception>

namespace RoboGo::platform {

// Static stage strings only: reporting a GPU failure must not allocate memory.
class GpuBackendError final : public std::exception {
public:
    GpuBackendError(const char *stage, std::int64_t hresult) noexcept : stage_(stage), hresult_(hresult) {}
    [[nodiscard]] const char *stage() const noexcept { return stage_; }
    [[nodiscard]] std::int64_t hresult() const noexcept { return hresult_; }
    [[nodiscard]] const char *what() const noexcept override { return stage_; }
private:
    const char *stage_;
    std::int64_t hresult_;
};

} // namespace RoboGo::platform

// Optional isolated-test hook. The owner gates its callback on offline test mode.
// No hook is installed by this snapshot itself.
using ImGuiDx12FailureHook = bool (*)(const char *stage, void *user);
void ImGuiDx12SetFailureHook(ImGuiDx12FailureHook hook, void *user);

// Ownership contract: a GpuBackendError after Execute retains all renderer data.
// The platform must mark same-queue work unfenced, then call DX12 Shutdown only
// after a new queue-fence reaches completion or device removal is confirmed.
