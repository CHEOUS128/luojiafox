#ifdef _WIN32
#include "platform/platform_backend.h"
#include "platform/imgui_dx12_backend.h"
#include "app/imgui_setup.h"
#include "keyboard_mouse_state_manager.h"
#include "logging/logging.h"
#include "resources.h"
#include <wrl/client.h>
#include <d3d12sdklayers.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr unsigned kFrames = 2, kBackBuffers = 2, kDescriptors = 64;

struct FrameContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    UINT64 fence = 0;
};

struct DescriptorAllocator {
    ID3D12DescriptorHeap *heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    UINT increment = 0;
    std::vector<unsigned> free;
    std::array<bool, kDescriptors> used{};
    void Create(ID3D12Device *device, ID3D12DescriptorHeap *value) {
        heap = value;
        cpu = heap->GetCPUDescriptorHandleForHeapStart();
        gpu = heap->GetGPUDescriptorHandleForHeapStart();
        increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        free.clear(); used.fill(false);
        for (unsigned i = kDescriptors; i; --i) free.push_back(i - 1);
    }
    void Destroy() { heap = nullptr; free.clear(); used.fill(false); }
    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu) {
        if (!heap || free.empty()) throw std::runtime_error("D3D12 descriptor heap exhausted");
        const unsigned i = free.back(); free.pop_back(); used[i] = true;
        out_cpu->ptr = cpu.ptr + static_cast<SIZE_T>(i) * increment;
        out_gpu->ptr = gpu.ptr + static_cast<UINT64>(i) * increment;
    }
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE value_cpu, D3D12_GPU_DESCRIPTOR_HANDLE value_gpu) {
        if (!heap || value_cpu.ptr < cpu.ptr || value_gpu.ptr < gpu.ptr || !increment) return;
        const auto i = (value_cpu.ptr - cpu.ptr) / increment;
        if (i >= used.size() || !used[i] || (value_cpu.ptr - cpu.ptr) % increment ||
            value_gpu.ptr != gpu.ptr + i * increment) {
            SPDLOG_ERROR("GPU invalid/double descriptor free rejected"); return;
        }
        used[i] = false; free.push_back(static_cast<unsigned>(i));
    }
};

struct VideoTexture {
    ComPtr<ID3D12Resource> texture, upload;
    uint8_t *mapped = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    UINT pitch = 0;
    int width = 0, height = 0;
    UINT64 last_use = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    bool deferred = false;
    void Release() {
        if (mapped && upload) upload->Unmap(0, nullptr);
        mapped = nullptr; texture.Reset(); upload.Reset();
        pitch = 0; width = height = 0; last_use = 0;
        state = D3D12_RESOURCE_STATE_COPY_DEST; deferred = false;
    }
};
struct TextureSet {
    std::array<VideoTexture, 2> slots;
    int current = -1, pending = -1;
};

class WindowsPlatformBackend final : public RoboGo::platform::PlatformBackend {
public:
    const char *application_name() const override { return "luojiamaster-windows"; }
    void EnableOfflineTestMode(bool value) override {
        offline_test_ = value;
        if (value) {
            wchar_t stage[128]{};
            const DWORD n = GetEnvironmentVariableW(L"RM26_GPU_TEST_FAILURE", stage, 128);
            if (n && n < 128) {
                failure_stage_.assign(stage, stage + n); failure_remaining_ = 1;
            }
        } else { failure_stage_.clear(); failure_remaining_ = 0; }
    }
    void SetTestFailure(std::string_view stage, int count) override {
        if (!offline_test_) return;
        failure_stage_ = stage; failure_remaining_ = count;
        if (initialized_ && count && (stage == "create_queue" || stage == "create_allocator" ||
                                      stage == "create_list" || stage == "create_fence" || stage.starts_with("font_"))) recovery_requested_ = true;
        if (count && (stage == "create_texture" || stage == "create_upload")) recreate_texture_ = true;
    }
    void RequestTestWindowAction(int action) override {
        if (offline_test_ && hwnd_) ShowWindow(hwnd_, action == 1 ? SW_MAXIMIZE : action == 2 ? SW_MINIMIZE : SW_RESTORE);
    }
    RoboGo::platform::RenderDiagnostics GetRenderDiagnostics() const override { return diagnostics_; }

    bool Initialize() override {
        ImGui_ImplWin32_EnableDpiAwareness();
        main_scale_ = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
        window_class_ = {sizeof(window_class_), CS_CLASSDC, StaticWndProc, 0L, 0L, GetModuleHandleW(nullptr),
                         LoadIcon(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APP_ICON)), nullptr, nullptr, nullptr,
                         L"luojiamaster", nullptr};
        if (!RegisterClassExW(&window_class_)) { RecordFailure("register_window", HRESULT_FROM_WIN32(GetLastError())); return false; }
        window_class_registered_ = true;
        hwnd_ = CreateWindowW(window_class_.lpszClassName, L"luojiamaster", WS_OVERLAPPEDWINDOW,
                              100, 100, 1280, 800, nullptr, nullptr, window_class_.hInstance, this);
        if (!hwnd_) { RecordFailure("create_window", HRESULT_FROM_WIN32(GetLastError())); CleanupWindow(); return false; }
        if (!CreateGpu()) { CleanupGpu(true); CleanupWindow(); return false; }
        ShowWindow(hwnd_, SW_SHOWDEFAULT); UpdateWindow(hwnd_);
        RoboGo::app::InitializeCommonImGui(main_scale_);
        context_initialized_ = true;
        if (!ImGui_ImplWin32_Init(hwnd_)) { RecordFailure("imgui_win32", E_FAIL); Shutdown(); return false; }
        win32_initialized_ = true;
        if (!InitializeRenderer()) { Shutdown(); return false; }
        RoboGo::app::LoadEmbeddedImGuiFont();
        initialized_ = true;
        SPDLOG_INFO("GPU persistent renderer ready: one queue, {} frame contexts, 2 slots/stream", kFrames);
        return true;
    }

    void Shutdown() override {
        if (!offline_test_) SetGameInputCapture(false);
        AbortFrame();
        const bool safe = DrainForShutdown();
        if (renderer_initialized_ && safe) ImGui_ImplDX12_Shutdown();
        // A live GPU timeout is NOT completion. In that exceptional shutdown path,
        // keep its COM references alive until process teardown instead of freeing in-flight work.
        renderer_initialized_ = false;
        if (win32_initialized_) ImGui_ImplWin32_Shutdown();
        win32_initialized_ = false;
        if (context_initialized_ && safe) ImGui::DestroyContext();
        context_initialized_ = false;
        ImGuiDx12SetFailureHook(nullptr, nullptr);
        CleanupGpu(safe); CleanupWindow(); initialized_ = false;
    }

    bool ProcessEvents() override {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
            if (message.message == WM_QUIT) should_close_ = true;
        }
        if (!should_close_) MaintainGpu();
        return !should_close_;
    }

    bool BeginFrame() override {
        if (!PrepareFrame()) return false;
        if (game_input_captured_) {
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                POINT center{}; RECT rect{}; GetClientRect(hwnd_, &rect);
                center.x = (rect.right - rect.left) / 2; center.y = (rect.bottom - rect.top) / 2;
                ClientToScreen(hwnd_, &center);
                add_raw_mouse_delta(static_cast<float>(cursor.x - center.x), static_cast<float>(cursor.y - center.y));
                SetCursorPos(center.x, center.y);
            }
        }
        try { ImGui_ImplDX12_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame(); }
        catch (const RoboGo::platform::GpuBackendError &e) { RecordFailure(e.stage(), static_cast<HRESULT>(e.hresult())); unfenced_work_ = true; AbortFrame(); recovery_requested_ = true; return false; }
        catch (const std::exception &e) { SPDLOG_ERROR("GPU ImGui NewFrame exception: {}", e.what()); RecordFailure("imgui_new_frame", E_FAIL); unfenced_work_ = true; AbortFrame(); recovery_requested_ = true; return false; }
        return true;
    }

    bool RenderFrame(ImDrawData *draw_data) override {
        if (!prepared_ || !active_ || !draw_data || !back_buffers_valid_ ||
            draw_data->DisplaySize.x <= 0 || draw_data->DisplaySize.y <= 0) { AbortFrame(); return false; }
        const UINT back = swap_chain_->GetCurrentBackBufferIndex();
        if (back >= kBackBuffers || !back_buffers_[back]) { RecordFailure("back_buffer_index", E_UNEXPECTED); AbortFrame(); recovery_requested_ = true; return false; }
        auto *commands = active_->commands.Get();
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = back_buffers_[back].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commands->ResourceBarrier(1, &barrier);
        const float clear[4] = {0, 0, 0, 1};
        commands->ClearRenderTargetView(rtv_handles_[back], clear, 0, nullptr);
        commands->OMSetRenderTargets(1, &rtv_handles_[back], FALSE, nullptr);
        auto *heap = srv_heap_.Get(); commands->SetDescriptorHeaps(1, &heap);
        try { ImGui_ImplDX12_RenderDrawData(draw_data, commands); }
        catch (const RoboGo::platform::GpuBackendError &e) { RecordFailure(e.stage(), static_cast<HRESULT>(e.hresult())); unfenced_work_ = true; AbortFrame(); recovery_requested_ = true; return false; }
        catch (const std::exception &e) { SPDLOG_ERROR("GPU ImGui render exception: {}", e.what()); RecordFailure("imgui_render", E_FAIL); unfenced_work_ = true; AbortFrame(); recovery_requested_ = true; return false; }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands->ResourceBarrier(1, &barrier);
        if (!Check("close", Fail("close") ? E_FAIL : commands->Close())) { AbandonFrame(); recovery_requested_ = true; return false; }
        // Inject BEFORE Execute: do not manufacture an unfenced real GPU submission.
        if (Fail("signal")) { RecordFailure("signal", E_FAIL); AbandonFrame(); recovery_requested_ = true; return false; }
        ID3D12CommandList *lists[] = {commands};
        queue_->ExecuteCommandLists(1, lists);
        unfenced_work_ = true;
        const UINT64 fence_value = ++last_signal_;
        if (!Check("signal", queue_->Signal(fence_.Get(), fence_value))) { AbandonFrame(); recovery_requested_ = true; return false; }
        unfenced_work_ = false; last_submitted_ = fence_value; active_->fence = fence_value;
        for (auto &set : textures_) {
            if (set.pending >= 0) {
                auto &slot = set.slots[set.pending];
                slot.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                set.current = set.pending; set.pending = -1;
                ++diagnostics_.uploads_succeeded;
            }
            if (set.current >= 0) set.slots[set.current].last_use = fence_value;
        }
        prepared_ = false; active_ = nullptr; ++frame_index_;
        HRESULT result = Fail("present") ? E_FAIL : swap_chain_->Present(1, 0);
        latency_slot_acquired_ = false;
        occluded_ = result == DXGI_STATUS_OCCLUDED;
        if (occluded_) return false;
        if (!Check("present", result)) { recovery_requested_ = true; return false; }
        ++diagnostics_.frames_submitted;
        return true;
    }

    void SetGameInputCapture(bool enabled) override {
        if (!hwnd_ || game_input_captured_ == enabled) return;
        if (enabled) {
            RECT rect{}; GetClientRect(hwnd_, &rect);
            POINT first{rect.left, rect.top}, last{rect.right, rect.bottom};
            ClientToScreen(hwnd_, &first); ClientToScreen(hwnd_, &last);
            rect = {first.x, first.y, last.x, last.y}; ClipCursor(&rect); ShowCursor(FALSE);
            set_use_raw_mouse(true);
        } else { ClipCursor(nullptr); ShowCursor(TRUE); set_use_raw_mouse(false); }
        game_input_captured_ = enabled;
    }

    bool UploadTexture(RoboGo::app::TextureSlot slot, const RoboGo::app::VideoFrame &frame) override {
        return Upload(slot, frame.buffer.data(), frame.buffer.size(), frame.width, frame.height);
    }
    bool UploadTexture(RoboGo::app::TextureSlot slot, const RoboGo::resource::DecodedImage &image) override {
        if (slot != RoboGo::app::TextureSlot::MiniMap) return false;
        return Upload(slot, image.pixels.data(), image.pixels.size(), image.width, image.height);
    }
    RoboGo::app::TextureView GetTextureView(RoboGo::app::TextureSlot slot) const override {
        const auto &set = textures_[SlotIndex(slot)];
        const int index = set.pending >= 0 ? set.pending : set.current;
        if (index < 0 || !set.slots[index].texture) return {};
        const auto &value = set.slots[index];
        RoboGo::app::TextureView result;
        result.texture_ref = ImTextureRef(static_cast<ImTextureID>(value.gpu.ptr));
        result.size = ImVec2(static_cast<float>(value.width), static_cast<float>(value.height));
        return result;
    }

private:
    static unsigned SlotIndex(RoboGo::app::TextureSlot slot) {
        switch (slot) {
            case RoboGo::app::TextureSlot::UdpVideo: return 0;
            case RoboGo::app::TextureSlot::CustomVideo: return 1;
            case RoboGo::app::TextureSlot::MiniMap: return 2;
            case RoboGo::app::TextureSlot::TeammateVideo: return 3;
        }
        return 0;
    }
    bool Fail(std::string_view stage) {
        if (!offline_test_ || !failure_remaining_ || failure_stage_ != stage) return false;
        if (failure_remaining_ > 0) --failure_remaining_;
        return true;
    }
    bool Check(std::string_view stage, HRESULT result) {
        if (SUCCEEDED(result)) return true;
        RecordFailure(stage, result); return false;
    }
    void RecordFailure(std::string_view stage, HRESULT result) {
        const bool first = diagnostics_.first_failure_stage.empty();
        if (first) {
            diagnostics_.first_failure_stage = stage;
            diagnostics_.first_hresult = static_cast<uint32_t>(result);
        }
        const HRESULT removed = device_ ? device_->GetDeviceRemovedReason() : S_OK;
        const auto now = Clock::now();
        if (last_error_log_.time_since_epoch().count() == 0 || now - last_error_log_ > std::chrono::seconds(1)) {
            SPDLOG_ERROR("GPU failure stage={} HRESULT=0x{:08X} device_removed=0x{:08X}; keeping last safe image", stage, static_cast<uint32_t>(result), static_cast<uint32_t>(removed));
            last_error_log_ = now;
        }
        if (FAILED(removed)) recovery_requested_ = true;
        if (FAILED(removed) && !device_removed_logged_) {
            device_removed_logged_ = true;
#if defined(__ID3D12DeviceRemovedExtendedData1_INTERFACE_DEFINED__)
            ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
            if (SUCCEEDED(device_.As(&dred)) && dred) {
                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
                if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs))) {
                    unsigned count = 0;
                    for (auto *node = breadcrumbs.pHeadAutoBreadcrumbNode; node && count++ < 8; node = node->pNext)
                        SPDLOG_ERROR("GPU DRED queue={} list={} breadcrumb={}/{}", node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "unnamed", node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "unnamed", node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0, node->BreadcrumbCount);
                }
                D3D12_DRED_PAGE_FAULT_OUTPUT1 page{};
                if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&page)))
                    SPDLOG_ERROR("GPU DRED page_fault_va=0x{:016X}", static_cast<uint64_t>(page.PageFaultVA));
            }
#endif
        }
        if (first && device_) {
            ComPtr<ID3D12InfoQueue> info;
            if (SUCCEEDED(device_.As(&info)) && info) {
                const auto count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
                const auto start = count > 8 ? count - 8 : 0;
                for (auto i = start; i < count; ++i) {
                    SIZE_T bytes = 0;
                    if (FAILED(info->GetMessage(i, nullptr, &bytes)) || !bytes || bytes > 1024 * 1024) continue;
                    std::vector<uint8_t> storage(bytes);
                    auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
                    if (SUCCEEDED(info->GetMessage(i, message, &bytes)))
                        SPDLOG_ERROR("GPU debug message id={} severity={} text={}", static_cast<int>(message->ID), static_cast<int>(message->Severity), message->pDescription ? message->pDescription : "");
                }
            }
        }
    }
    bool DeviceRemoved() const { return device_ && FAILED(device_->GetDeviceRemovedReason()); }
    UINT64 CompletedFence() {
        if (!fence_) return 0;
        const UINT64 value = fence_->GetCompletedValue();
        if (value == UINT64_MAX) { RecordFailure("get_completed_fence", DXGI_ERROR_DEVICE_REMOVED); return 0; }
        return value;
    }
    bool QueueIdle() {
        if (!device_ || DeviceRemoved()) return true;
        if (unfenced_work_) {
            const UINT64 value = ++last_signal_;
            if (!Check("recovery_signal", queue_->Signal(fence_.Get(), value))) return false;
            last_submitted_ = value; unfenced_work_ = false;
        }
        return CompletedFence() >= last_submitted_;
    }
    void AbandonFrame() {
        prepared_ = false; active_ = nullptr;
        for (auto &set : textures_) set.pending = -1;
    }
    void AbortFrame() {
        if (prepared_ && active_ && !Check("abort_close", active_->commands->Close())) recovery_requested_ = true;
        AbandonFrame();
    }

    // The remaining helpers use this one render queue. No per-image command objects.
    bool CreateGpu();
    bool InitializeRenderer();
    bool CreateRenderTargets();
    void MaintainGpu();
    void RecoverGpu();
    bool PrepareFrame(bool restore_map = true);
    bool WaitForFence(UINT64 value);
    static DWORD WaitResponsive(HANDLE handle, DWORD milliseconds = 5) {
        return MsgWaitForMultipleObjectsEx(handle ? 1 : 0, handle ? &handle : nullptr,
                                           milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    bool Upload(RoboGo::app::TextureSlot, const uint8_t *, size_t, int, int);
    bool StageUpload(TextureSet &, const uint8_t *, int, int);
    bool CreateTexture(VideoTexture &, int, int);
    bool DrainForShutdown();
    void CleanupGpu(bool safe);
    void CleanupWindow();
    static LRESULT WINAPI StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    std::array<FrameContext, kFrames> frames_;
    std::array<TextureSet, 4> textures_;
    std::vector<uint8_t> map_pixels_;
    int map_width_ = 0, map_height_ = 0;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_, srv_heap_;
    DescriptorAllocator descriptors_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<IDXGISwapChain3> swap_chain_;
    std::array<ComPtr<ID3D12Resource>, kBackBuffers> back_buffers_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBackBuffers> rtv_handles_{};
    HANDLE swap_wait_ = nullptr;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_event_target_ = 0;
    UINT64 last_signal_ = 0, last_submitted_ = 0, frame_index_ = 0;
    FrameContext *active_ = nullptr;
    WNDCLASSEXW window_class_{};
    HWND hwnd_ = nullptr;
    float main_scale_ = 1;
    UINT resize_width_ = 0, resize_height_ = 0;
    bool initialized_ = false, context_initialized_ = false, win32_initialized_ = false;
    bool renderer_initialized_ = false, window_class_registered_ = false, should_close_ = false;
    bool game_input_captured_ = false, prepared_ = false, back_buffers_valid_ = false;
    bool occluded_ = false, resize_pending_ = false, recovery_requested_ = false;
    bool unfenced_work_ = false, latency_slot_acquired_ = false, recreate_texture_ = false;
    bool offline_test_ = false;
    bool device_removed_logged_ = false;
    std::string failure_stage_;
    int failure_remaining_ = 0;
    Clock::time_point retry_at_{}, recovery_not_before_{}, last_error_log_{};
    RoboGo::platform::RenderDiagnostics diagnostics_;
};

bool WindowsPlatformBackend::CreateGpu() {
    device_removed_logged_ = false;
    // Debug validation is opt-in for the isolated runner, never a production startup requirement.
    wchar_t debug_value[8]{};
    if (offline_test_ && GetEnvironmentVariableW(L"RM26_GPU_DEBUG", debug_value, 8) && debug_value[0] == L'1') {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) && debug) {
            debug->EnableDebugLayer(); SPDLOG_INFO("GPU offline D3D12 debug layer enabled");
        } else SPDLOG_WARN("GPU offline debug layer unavailable; continuing without it");
    }
    if (!Check("create_device", D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_))) || !device_) return false;
    device_->SetName(L"RM26 renderer device");
    ++diagnostics_.resource_creations;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = kBackBuffers;
    if (!Check("create_rtv_heap", device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_))) || !rtv_heap_) return false;
    ++diagnostics_.resource_creations;
    auto handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const UINT increment = device_->GetDescriptorHandleIncrementSize(heap.Type);
    for (unsigned i = 0; i < kBackBuffers; ++i) { rtv_handles_[i] = handle; handle.ptr += increment; }
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = kDescriptors;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!Check("create_srv_heap", device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_))) || !srv_heap_) return false;
    ++diagnostics_.resource_creations; descriptors_.Create(device_.Get(), srv_heap_.Get());
    D3D12_COMMAND_QUEUE_DESC queue_desc{}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!Check("create_queue", Fail("create_queue") ? E_FAIL : device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_))) || !queue_) return false;
    queue_->SetName(L"RM26 shared upload/render queue");
    ++diagnostics_.resource_creations;
    for (auto &frame : frames_) {
        if (!Check("create_allocator", Fail("create_allocator") ? E_FAIL : device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator))) || !frame.allocator) return false;
        ++diagnostics_.resource_creations;
        if (!Check("create_list", Fail("create_list") ? E_FAIL : device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(), nullptr, IID_PPV_ARGS(&frame.commands))) || !frame.commands) return false;
        frame.commands->SetName(L"RM26 frame upload/render list");
        ++diagnostics_.resource_creations;
        if (!Check("initial_close", frame.commands->Close())) return false;
    }
    if (!Check("create_fence", Fail("create_fence") ? E_FAIL : device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))) || !fence_) return false;
    ++diagnostics_.resource_creations;
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) { RecordFailure("create_fence_event", HRESULT_FROM_WIN32(GetLastError())); return false; }
    ComPtr<IDXGIFactory5> factory;
    if (!Check("create_factory", CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return false;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.BufferCount = kBackBuffers; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.SampleDesc.Count = 1;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Scaling = DXGI_SCALING_STRETCH;
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    ComPtr<IDXGISwapChain1> first;
    if (!Check("create_swap_chain", factory->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &desc, nullptr, nullptr, &first)) || !first) return false;
    if (!Check("query_swap_chain", first.As(&swap_chain_)) || !swap_chain_) return false;
    Check("window_association", factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER));
    if (!Check("frame_latency", swap_chain_->SetMaximumFrameLatency(kBackBuffers))) return false;
    swap_wait_ = swap_chain_->GetFrameLatencyWaitableObject();
    if (!swap_wait_) { RecordFailure("swap_wait_handle", E_HANDLE); return false; }
    return CreateRenderTargets();
}

bool WindowsPlatformBackend::InitializeRenderer() {
    ImGuiDx12SetFailureHook([](const char *stage, void *user) {
        return static_cast<WindowsPlatformBackend *>(user)->Fail(stage);
    }, this);
    ImGui_ImplDX12_InitInfo info{};
    info.Device = device_.Get(); info.CommandQueue = queue_.Get(); info.NumFramesInFlight = kFrames;
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM; info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = srv_heap_.Get(); info.UserData = this;
    info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *value, D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu) {
        static_cast<WindowsPlatformBackend *>(value->UserData)->descriptors_.Alloc(cpu, gpu);
    };
    info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *value, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
        static_cast<WindowsPlatformBackend *>(value->UserData)->descriptors_.Free(cpu, gpu);
    };
    try {
        const bool success = ImGui_ImplDX12_Init(&info);
        renderer_initialized_ = ImGui::GetIO().BackendRendererUserData != nullptr;
        if (!success) { RecordFailure("imgui_dx12_init", E_FAIL); return false; }
        for (auto &set : textures_) for (auto &slot : set.slots) descriptors_.Alloc(&slot.cpu, &slot.gpu);
    } catch (const std::exception &e) {
        SPDLOG_ERROR("GPU descriptor/renderer initialization exception: {}", e.what()); RecordFailure("imgui_dx12_init", E_FAIL); return false;
    }
    return true;
}

bool WindowsPlatformBackend::CreateRenderTargets() {
    std::array<ComPtr<ID3D12Resource>, kBackBuffers> buffers;
    for (unsigned i = 0; i < kBackBuffers; ++i) {
        if (!Check("get_buffer", Fail("get_buffer") ? E_FAIL : swap_chain_->GetBuffer(i, IID_PPV_ARGS(&buffers[i]))) || !buffers[i]) return false;
        device_->CreateRenderTargetView(buffers[i].Get(), nullptr, rtv_handles_[i]);
    }
    back_buffers_ = std::move(buffers); back_buffers_valid_ = true;
    return true;
}

void WindowsPlatformBackend::MaintainGpu() {
    if (!initialized_) return;
    if (DeviceRemoved()) { RecordFailure("device_removed", device_->GetDeviceRemovedReason()); recovery_requested_ = true; }
    if (recovery_requested_) {
        AbortFrame();
        const auto now = Clock::now();
        if (now >= retry_at_ && now >= recovery_not_before_ && QueueIdle()) RecoverGpu();
        return;
    }
    if (!resize_pending_ || Clock::now() < retry_at_) return;
    AbortFrame();
    if (recovery_requested_) return;
    if (!QueueIdle()) return;
    for (auto &buffer : back_buffers_) buffer.Reset();
    back_buffers_valid_ = false;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (!Check("get_swap_desc", swap_chain_->GetDesc1(&desc))) { recovery_requested_ = true; return; }
    const HRESULT result = Fail("resize") ? E_INVALIDARG : swap_chain_->ResizeBuffers(0, resize_width_, resize_height_, desc.Format, desc.Flags);
    if (!Check("resize", result)) {
        // Resize failure does not justify destroying a healthy device. Reacquire
        // the unchanged old buffers and retry later, leaving the event loop live.
        if (!CreateRenderTargets()) recovery_requested_ = true;
        retry_at_ = Clock::now() + std::chrono::milliseconds(250);
        return;
    }
    if (!CreateRenderTargets()) { recovery_requested_ = true; return; }
    resize_pending_ = false; occluded_ = false;
}

void WindowsPlatformBackend::RecoverGpu() {
    if (!QueueIdle()) return;
    AbortFrame();
    if (renderer_initialized_) ImGui_ImplDX12_Shutdown();
    renderer_initialized_ = false;
    CleanupGpu(true);
    if (!CreateGpu() || !InitializeRenderer()) {
        if (renderer_initialized_) ImGui_ImplDX12_Shutdown();
        renderer_initialized_ = false; CleanupGpu(true);
        recovery_requested_ = true; retry_at_ = Clock::now() + std::chrono::seconds(1);
        SetWindowTextW(hwnd_, L"luojiamaster - 正在恢复画面");
        return;
    }
    recovery_requested_ = false; resize_pending_ = false; occluded_ = false;
    retry_at_ = {};
    recovery_not_before_ = Clock::now() + std::chrono::milliseconds(250);
    ++diagnostics_.recoveries;
    SetWindowTextW(hwnd_, L"luojiamaster");
    SPDLOG_INFO("GPU renderer recovered; preserved UI state, queued cached map restore");
}

bool WindowsPlatformBackend::PrepareFrame(bool restore_map) {
    if (prepared_) return true;
    auto busy = [&] { ++diagnostics_.skipped_busy; return false; };
    if (!initialized_ || !renderer_initialized_ || !device_ || recovery_requested_ || !back_buffers_valid_) {
        if (!fence_ || !last_submitted_ || unfenced_work_ || WaitForFence(last_submitted_)) WaitResponsive(nullptr);
        return busy();
    }
    if (IsIconic(hwnd_)) { WaitResponsive(nullptr, 20); return busy(); }
    if (resize_pending_ && Clock::now() >= retry_at_) {
        if (WaitForFence(last_submitted_)) WaitResponsive(nullptr);
        return busy();
    }
    if (occluded_) {
        const HRESULT result = swap_chain_->Present(0, DXGI_PRESENT_TEST);
        if (result == DXGI_STATUS_OCCLUDED) { WaitResponsive(nullptr, 20); return false; }
        if (!Check("occlusion_test", result)) { recovery_requested_ = true; return false; }
        occluded_ = false;
    }
    if (Fail("wait")) { ++diagnostics_.wait_timeouts; WaitResponsive(nullptr); return busy(); }
    auto &frame = frames_[frame_index_ % kFrames];
    const UINT64 complete = CompletedFence();
    unsigned deferred = 0;
    for (auto &set : textures_) for (auto &slot : set.slots) {
        if (slot.deferred && slot.last_use <= complete) slot.deferred = false;
        deferred += slot.deferred ? 1 : 0;
    }
    diagnostics_.deferred_resources = deferred;
    diagnostics_.max_deferred_resources = std::max<uint64_t>(diagnostics_.max_deferred_resources, deferred);
    if (complete < frame.fence && !WaitForFence(frame.fence)) return busy();
    if (!latency_slot_acquired_) {
        const DWORD status = WaitResponsive(swap_wait_);
        if (status == WAIT_TIMEOUT) return busy();
        if (status == WAIT_OBJECT_0 + 1) return busy();
        if (status != WAIT_OBJECT_0) { RecordFailure("swap_wait", HRESULT_FROM_WIN32(GetLastError())); recovery_requested_ = true; return false; }
        // Keep this token across an unsubmitted/cancelled frame. It is only
        // consumed by Present; waiting again would deadlock on an auto-reset event.
        latency_slot_acquired_ = true;
    }
    if (!Check("reset_allocator", Fail("reset_allocator") ? E_FAIL : frame.allocator->Reset())) { recovery_requested_ = true; return false; }
    if (!Check("reset_list", Fail("reset_list") ? E_FAIL : frame.commands->Reset(frame.allocator.Get(), nullptr))) { recovery_requested_ = true; return false; }
    active_ = &frame; prepared_ = true;
    if (restore_map && !map_pixels_.empty() && textures_[2].current < 0) StageUpload(textures_[2], map_pixels_.data(), map_width_, map_height_);
    return true;
}

bool WindowsPlatformBackend::WaitForFence(UINT64 value) {
    if (!value || CompletedFence() >= value) return true;
    if (!fence_ || !fence_event_) return false;
    if (fence_event_target_ != value) {
        if (!ResetEvent(fence_event_)) {
            RecordFailure("reset_fence_event", HRESULT_FROM_WIN32(GetLastError())); recovery_requested_ = true; return false;
        }
        if (!Check("set_fence_event", fence_->SetEventOnCompletion(value, fence_event_))) {
            recovery_requested_ = true; return false;
        }
        fence_event_target_ = value;
    }
    const DWORD status = WaitResponsive(fence_event_);
    if (status == WAIT_FAILED) { RecordFailure("fence_wait", HRESULT_FROM_WIN32(GetLastError())); recovery_requested_ = true; return false; }
    return CompletedFence() >= value;
}

bool WindowsPlatformBackend::Upload(RoboGo::app::TextureSlot slot, const uint8_t *pixels, size_t size, int width, int height) {
    if (!pixels || width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        static_cast<uint64_t>(width) * height * 4 > size) {
        ++diagnostics_.upload_failures; RecordFailure("invalid_pixels", E_INVALIDARG); return false;
    }
    if (slot == RoboGo::app::TextureSlot::MiniMap) {
        map_pixels_.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
        map_width_ = width; map_height_ = height;
    }
    if (!PrepareFrame(slot != RoboGo::app::TextureSlot::MiniMap)) return false;
    return StageUpload(textures_[SlotIndex(slot)], pixels, width, height);
}

bool WindowsPlatformBackend::StageUpload(TextureSet &set, const uint8_t *pixels, int width, int height) {
    if (!prepared_ || !active_ || set.pending >= 0) { ++diagnostics_.skipped_busy; return false; }
    const int index = set.current < 0 ? 0 : 1 - set.current;
    auto &slot = set.slots[index];
    // This fence includes both prior copy and every conservative render use.
    // Neither mapped bytes nor descriptor contents may change before it completes.
    if (slot.last_use > CompletedFence()) {
        slot.deferred = true; ++diagnostics_.skipped_busy;
        unsigned count = 0;
        for (auto &stream : textures_) for (auto &entry : stream.slots) count += entry.deferred ? 1 : 0;
        diagnostics_.deferred_resources = count;
        diagnostics_.max_deferred_resources = std::max<uint64_t>(diagnostics_.max_deferred_resources, count);
        return false;
    }
    slot.deferred = false;
    if (!slot.texture || slot.width != width || slot.height != height || recreate_texture_) {
        if (!CreateTexture(slot, width, height)) { ++diagnostics_.upload_failures; return false; }
    }
    if (Fail("map")) { RecordFailure("map", E_FAIL); ++diagnostics_.upload_failures; return false; }
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y) memcpy(slot.mapped + static_cast<size_t>(y) * slot.pitch, pixels + static_cast<size_t>(y) * row_bytes, row_bytes);
    auto *commands = active_->commands.Get();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = slot.texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = slot.state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    if (slot.state != D3D12_RESOURCE_STATE_COPY_DEST) commands->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{}, target{};
    source.pResource = slot.upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(width), static_cast<UINT>(height), 1, slot.pitch};
    target.pResource = slot.texture.Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commands->ResourceBarrier(1, &barrier);
    // State is committed only after Execute+Signal. Aborted lists never ran.
    set.pending = index;
    return true;
}

bool WindowsPlatformBackend::CreateTexture(VideoTexture &slot, int width, int height) {
    ComPtr<ID3D12Resource> texture, upload;
    D3D12_HEAP_PROPERTIES props{}; props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (!Check("create_texture", Fail("create_texture") ? E_FAIL : device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))) || !texture) return false;
    ++diagnostics_.resource_creations;
    const UINT pitch = (static_cast<UINT>(width) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = static_cast<UINT64>(pitch) * height;
    desc.Height = 1; desc.Format = DXGI_FORMAT_UNKNOWN; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    props.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (!Check("create_upload", Fail("create_upload") ? E_FAIL : device_->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) || !upload) return false;
    ++diagnostics_.resource_creations;
    void *mapped = nullptr; const D3D12_RANGE no_reads{0, 0};
    if (!Check("initial_map", upload->Map(0, &no_reads, &mapped)) || !mapped) return false;
    // All prerequisites succeeded. Only now replace the fence-safe old slot.
    slot.Release(); slot.texture = std::move(texture); slot.upload = std::move(upload);
    slot.mapped = static_cast<uint8_t *>(mapped); slot.pitch = pitch; slot.width = width; slot.height = height;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(slot.texture.Get(), &srv, slot.cpu);
    if (!failure_remaining_) recreate_texture_ = false;
    return true;
}

bool WindowsPlatformBackend::DrainForShutdown() {
    if (!device_ || DeviceRemoved()) return true;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!QueueIdle()) {
        if (Clock::now() >= deadline) { ++diagnostics_.wait_timeouts; SPDLOG_ERROR("GPU shutdown wait exceeded 2s; quarantining live resources until process exit"); return false; }
        Sleep(1);
    }
    return true;
}

void WindowsPlatformBackend::CleanupGpu(bool safe) {
    for (auto &set : textures_) {
        for (auto &slot : set.slots) {
            if (safe) slot.Release();
            else { slot.texture.Detach(); slot.upload.Detach(); slot.mapped = nullptr; }
            slot.cpu = {}; slot.gpu = {}; slot.last_use = 0;
        }
        set.current = set.pending = -1;
    }
    for (auto &frame : frames_) {
        if (safe) { frame.commands.Reset(); frame.allocator.Reset(); }
        else { frame.commands.Detach(); frame.allocator.Detach(); }
        frame.fence = 0;
    }
    for (auto &buffer : back_buffers_) { if (safe) buffer.Reset(); else buffer.Detach(); }
    if (safe) { swap_chain_.Reset(); queue_.Reset(); fence_.Reset(); rtv_heap_.Reset(); srv_heap_.Reset(); device_.Reset(); }
    else { swap_chain_.Detach(); queue_.Detach(); fence_.Detach(); rtv_heap_.Detach(); srv_heap_.Detach(); device_.Detach(); }
    if (swap_wait_) CloseHandle(swap_wait_);
    if (fence_event_) CloseHandle(fence_event_);
    fence_event_ = nullptr; fence_event_target_ = 0;
    swap_wait_ = nullptr; descriptors_.Destroy(); back_buffers_valid_ = false;
    prepared_ = false; active_ = nullptr; unfenced_work_ = false; latency_slot_acquired_ = false;
    frame_index_ = last_signal_ = last_submitted_ = 0;
    diagnostics_.deferred_resources = 0;
}

void WindowsPlatformBackend::CleanupWindow() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    if (window_class_registered_) { UnregisterClassW(window_class_.lpszClassName, window_class_.hInstance); window_class_registered_ = false; }
}

LRESULT WINAPI WindowsPlatformBackend::StaticWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
        auto *backend = static_cast<WindowsPlatformBackend *>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(backend)); backend->hwnd_ = hwnd;
    }
    auto *backend = reinterpret_cast<WindowsPlatformBackend *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return backend ? backend->WndProc(hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT WindowsPlatformBackend::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) return TRUE;
    switch (message) {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED && LOWORD(lparam) && HIWORD(lparam)) {
                resize_width_ = LOWORD(lparam); resize_height_ = HIWORD(lparam); resize_pending_ = true;
            }
            return 0;
        case WM_SYSCOMMAND: if ((wparam & 0xfff0) == SC_KEYMENU) return 0; break;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

} // namespace
namespace RoboGo::platform {
std::unique_ptr<PlatformBackend> CreatePlatformBackend() { return std::make_unique<WindowsPlatformBackend>(); }
}
#endif
