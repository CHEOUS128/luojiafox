#ifdef _WIN32

#include "platform/platform_backend.h"

#include "app/imgui_setup.h"
#include "keyboard_mouse_state_manager.h"
#include "logging/logging.h"
#include "resources.h"

#ifdef _DEBUG
#define DX12_ENABLE_DEBUG_LAYER
#endif

#ifdef DX12_ENABLE_DEBUG_LAYER
#pragma comment(lib, "dxguid.lib")
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr int kFramesInFlight = 2;
constexpr int kBackBuffers = 2;
constexpr int kSrvHeapSize = 64;

struct WindowsFrameContext {
    ID3D12CommandAllocator *command_allocator = nullptr;
    UINT64 fence_value = 0;
};

struct DescriptorHeapAllocator {
    ID3D12DescriptorHeap *heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
    D3D12_CPU_DESCRIPTOR_HANDLE heap_start_cpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE heap_start_gpu = {};
    UINT heap_handle_increment = 0;
    ImVector<int> free_indices;

    void Create(ID3D12Device *device, ID3D12DescriptorHeap *descriptor_heap) {
        IM_ASSERT(heap == nullptr && free_indices.empty());
        heap = descriptor_heap;
        const D3D12_DESCRIPTOR_HEAP_DESC desc = descriptor_heap->GetDesc();
        heap_type = desc.Type;
        heap_start_cpu = heap->GetCPUDescriptorHandleForHeapStart();
        heap_start_gpu = heap->GetGPUDescriptorHandleForHeapStart();
        heap_handle_increment = device->GetDescriptorHandleIncrementSize(heap_type);
        free_indices.reserve(static_cast<int>(desc.NumDescriptors));
        for (int n = static_cast<int>(desc.NumDescriptors); n > 0; --n) {
            free_indices.push_back(n - 1);
        }
    }

    void Destroy() {
        heap = nullptr;
        free_indices.clear();
    }

    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_desc_handle) {
        IM_ASSERT(free_indices.Size > 0);
        const int index = free_indices.back();
        free_indices.pop_back();
        out_cpu_desc_handle->ptr = heap_start_cpu.ptr + (static_cast<SIZE_T>(index) * heap_handle_increment);
        out_gpu_desc_handle->ptr = heap_start_gpu.ptr + (static_cast<UINT64>(index) * heap_handle_increment);
    }

    void Free(const D3D12_CPU_DESCRIPTOR_HANDLE cpu_desc_handle, const D3D12_GPU_DESCRIPTOR_HANDLE gpu_desc_handle) {
        const int cpu_index = static_cast<int>((cpu_desc_handle.ptr - heap_start_cpu.ptr) / heap_handle_increment);
        const int gpu_index = static_cast<int>((gpu_desc_handle.ptr - heap_start_gpu.ptr) / heap_handle_increment);
        IM_ASSERT(cpu_index == gpu_index);
        free_indices.push_back(cpu_index);
    }

};

struct WindowsVideoTexture {
    ID3D12Resource *texture = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = {};
    int width = 0;
    int height = 0;
    bool ready = false;
    bool descriptor_allocated = false;
};

struct WindowsDynamicTextureSet {
    std::array<WindowsVideoTexture, 2> textures;
    int current_index = 0;
    int next_index = 1;
};

class WindowsPlatformBackend final : public RoboGo::platform::PlatformBackend {
public:
    [[nodiscard]] const char *application_name() const override {
        return "luojiamaster-windows";
    }

    bool Initialize() override {
        ImGui_ImplWin32_EnableDpiAwareness();
        main_scale_ = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

        window_class_ = {sizeof(window_class_),
                         CS_CLASSDC,
                         StaticWndProc,
                         0L,
                         0L,
                         GetModuleHandleW(nullptr),
                         LoadIcon(GetModuleHandleW(nullptr), MAKEINTRESOURCE(IDI_APP_ICON)),
                         nullptr,
                         nullptr,
                         nullptr,
                         L"luojiamaster",
                         nullptr};
        if (::RegisterClassExW(&window_class_) == 0) {
            SPDLOG_CRITICAL("注册Windows窗口类失败");
            return false;
        }
        window_class_registered_ = true;

        hwnd_ = ::CreateWindowW(window_class_.lpszClassName,
                                L"luojiamaster",
                                WS_OVERLAPPEDWINDOW,
                                100,
                                100,
                                1280,
                                800,
                                nullptr,
                                nullptr,
                                window_class_.hInstance,
                                this);
        if (hwnd_ == nullptr) {
            SPDLOG_CRITICAL("创建Windows窗口失败");
            CleanupWindow();
            return false;
        }

        if (!CreateDeviceD3D()) {
            SPDLOG_CRITICAL("Direct3D初始化失败");
            CleanupDeviceD3D();
            CleanupWindow();
            return false;
        }
        SPDLOG_INFO("Direct3D初始化成功");

        ::ShowWindow(hwnd_, SW_SHOWDEFAULT);
        ::UpdateWindow(hwnd_);

        RoboGo::app::InitializeCommonImGui(main_scale_);

        ImGui_ImplWin32_Init(hwnd_);

        ImGui_ImplDX12_InitInfo init_info = {};
        init_info.Device = device_;
        init_info.CommandQueue = command_queue_;
        init_info.NumFramesInFlight = kFramesInFlight;
        init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        init_info.SrvDescriptorHeap = srv_descriptor_heap_;
        init_info.UserData = this;
        init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *info,
                                            D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_handle,
                                            D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_handle) {
            auto *backend = static_cast<WindowsPlatformBackend *>(info->UserData);
            backend->srv_descriptor_heap_allocator_.Alloc(out_cpu_handle, out_gpu_handle);
        };
        init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *info,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                           const D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {
            auto *backend = static_cast<WindowsPlatformBackend *>(info->UserData);
            backend->srv_descriptor_heap_allocator_.Free(cpu_handle, gpu_handle);
        };
        ImGui_ImplDX12_Init(&init_info);

        RoboGo::app::LoadEmbeddedImGuiFont();
        AllocateVideoTextureDescriptors();
        initialized_ = true;
        return true;
    }

    void Shutdown() override {
        if (!initialized_) {
            return;
        }

        SetGameInputCapture(false);
        WaitForPendingOperations();

        ReleaseDynamicTextureSet(udp_video_textures_, true);
        ReleaseDynamicTextureSet(custom_video_textures_, true);
        ReleaseTexture(mini_map_texture_, true);

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        CleanupDeviceD3D();
        CleanupWindow();
        initialized_ = false;
    }

    [[nodiscard]] bool ProcessEvents() override {
        MSG message;
        while (::PeekMessage(&message, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessage(&message);
            if (message.message == WM_QUIT) {
                should_close_ = true;
            }
        }
        return !should_close_;
    }

    [[nodiscard]] bool BeginFrame() override {
        render_paused_by_window_ = false;
        if (swap_chain_ == nullptr || !render_targets_valid_) {
            // 设备重建失败/交换链坏了时不要再往下走，否则会拿空后台缓冲去画。
            ::Sleep(10);
            return false;
        }
        if ((swap_chain_occluded_ && swap_chain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) || ::IsIconic(hwnd_)) {
            render_paused_by_window_ = true;
            ::Sleep(10);
            return false;
        }
        swap_chain_occluded_ = false;

        // 游戏模式：先读出“真实的”鼠标位移（因为上一帧我们可能把光标重置回中心），
        // 再把光标拉回中心，保证下一帧总有移动空间。
        // 顺序很关键：必须在 ImGui 采样之前、重置之前读取。
        if (game_input_captured_) {
            POINT cursor{};
            if (::GetCursorPos(&cursor)) {
                POINT center{};
                RECT rect;
                ::GetClientRect(hwnd_, &rect);
                center.x = (rect.right - rect.left) / 2;
                center.y = (rect.bottom - rect.top) / 2;
                ::ClientToScreen(hwnd_, &center);

                const float dx = static_cast<float>(cursor.x - center.x);
                const float dy = static_cast<float>(cursor.y - center.y);
                add_raw_mouse_delta(dx, dy);

                ::SetCursorPos(center.x, center.y);
            }
        }

        // 上一帧没画完（等待 GPU 超时、渲染失败等）时必须先收尾，
        // 否则下一次 ImGui::NewFrame 会直接断言，界面还是会卡死。
        if (frame_in_progress_) {
            ImGui::EndFrame();
            frame_in_progress_ = false;
        }

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        // 只有真正开始画的一帧才点亮：用它可以区分「界面在转」还是「卡在某处」。
        frame_in_progress_ = true;
        return true;
    }

    void RenderFrame(ImDrawData *draw_data) override {
        if (swap_chain_ == nullptr || !render_targets_valid_) {
            frame_in_progress_ = false;
            return;
        }
        WindowsFrameContext *frame_context = WaitForNextFrameContext();
        if (frame_context == nullptr) {
            SPDLOG_ERROR("等待GPU/交换链超时（2秒），跳过这一帧");
            ImGui::EndFrame();
            frame_in_progress_ = false;
            return;
        }
        const UINT back_buffer_index = swap_chain_->GetCurrentBackBufferIndex();
        frame_context->command_allocator->Reset();

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = main_render_target_resource_[back_buffer_index];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

        command_list_->Reset(frame_context->command_allocator, nullptr);
        command_list_->ResourceBarrier(1, &barrier);

        constexpr float clear_color_with_alpha[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        command_list_->ClearRenderTargetView(main_render_target_descriptor_[back_buffer_index], clear_color_with_alpha, 0, nullptr);
        command_list_->OMSetRenderTargets(1, &main_render_target_descriptor_[back_buffer_index], FALSE, nullptr);
        command_list_->SetDescriptorHeaps(1, &srv_descriptor_heap_);
        ImGui_ImplDX12_RenderDrawData(draw_data, command_list_);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        command_list_->ResourceBarrier(1, &barrier);
        const HRESULT close_result = command_list_->Close();
        if (FAILED(close_result)) {
            // 命令列表没 Close 就拿不到新的帧，必须重建设备才能继续画。
            SPDLOG_ERROR("关闭渲染命令列表失败 hr=0x{:08X}，下一帧重建设备", static_cast<unsigned int>(close_result));
            frame_in_progress_ = false;
            return;
        }

        command_queue_->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&command_list_));
        if (FAILED(command_queue_->Signal(fence_, ++fence_last_signaled_value_))) {
            SPDLOG_ERROR("渲染命令队列Signal失败，跳过这一帧");
            frame_in_progress_ = false;
            return;
        }
        frame_context->fence_value = fence_last_signaled_value_;

        const HRESULT present_result = swap_chain_->Present(1, 0);
        if (FAILED(present_result)) {
            // 设备丢失时不涨帧数，主循环的看门狗 2 秒后会自动重建设备。
            SPDLOG_ERROR("Present失败 hr=0x{:08X}（设备可能已丢失）", static_cast<unsigned int>(present_result));
            swap_chain_occluded_ = false;
            frame_in_progress_ = false;
            return;
        }
        swap_chain_occluded_ = (present_result == DXGI_STATUS_OCCLUDED);
        ++frame_index_;
        ++rendered_frame_count_;
        frame_in_progress_ = false;
    }

    [[nodiscard]] std::uint64_t rendered_frame_count() const override {
        return rendered_frame_count_;
    }

    // 返回 false 表示主循环当前卡在 BeginFrame 之后没能画完一帧。
    [[nodiscard]] bool CanRecreateDeviceNow() const override {
        return !frame_in_progress_;
    }

    // 窗口最小化/被遮挡时本来就不出帧，不能当成“画面挂住”。
    [[nodiscard]] bool RenderPausedByWindow() const override {
        return render_paused_by_window_;
    }

    void SetGameInputCapture(const bool enabled) override {
        if (hwnd_ == nullptr || game_input_captured_ == enabled) {
            return;
        }

        if (enabled) {
            RECT rect;
            GetClientRect(hwnd_, &rect);
            POINT top_left = {rect.left, rect.top};
            POINT bottom_right = {rect.right, rect.bottom};
            ClientToScreen(hwnd_, &top_left);
            ClientToScreen(hwnd_, &bottom_right);
            rect.left = top_left.x;
            rect.top = top_left.y;
            rect.right = bottom_right.x;
            rect.bottom = bottom_right.y;
            ClipCursor(&rect);
            ShowCursor(FALSE);
            // 切换为“原始位移”模式：光标每帧会被重置到中心，ImGui 的增量不可用
            set_use_raw_mouse(true);
            SPDLOG_DEBUG("已锁定Windows鼠标光标区域 left={} top={} right={} bottom={}",
                         rect.left,
                         rect.top,
                         rect.right,
                         rect.bottom);
        } else {
            ClipCursor(nullptr);
            ShowCursor(TRUE);
            set_use_raw_mouse(false);
            SPDLOG_DEBUG("已解除Windows鼠标光标锁定");
        }

        game_input_captured_ = enabled;
    }

    [[nodiscard]] bool UploadTexture(const RoboGo::app::TextureSlot slot, const RoboGo::app::VideoFrame &frame) override {
        switch (slot) {
            case RoboGo::app::TextureSlot::UdpVideo:
                return UploadDynamicTexture(udp_video_textures_, frame);
            case RoboGo::app::TextureSlot::CustomVideo:
                return UploadDynamicTexture(custom_video_textures_, frame);
            case RoboGo::app::TextureSlot::MiniMap:
                return UploadStaticTexture(mini_map_texture_, frame.buffer.data(), frame.width, frame.height);
        }
        return false;
    }

    [[nodiscard]] bool UploadTexture(const RoboGo::app::TextureSlot slot, const RoboGo::resource::DecodedImage &image) override {
        if (slot != RoboGo::app::TextureSlot::MiniMap) {
            return false;
        }
        return UploadStaticTexture(mini_map_texture_, image.pixels.data(), image.width, image.height);
    }

    RoboGo::app::TextureView GetTextureView(const RoboGo::app::TextureSlot slot) const override {
        switch (slot) {
            case RoboGo::app::TextureSlot::UdpVideo:
                return BuildTextureView(udp_video_textures_.textures[udp_video_textures_.current_index]);
            case RoboGo::app::TextureSlot::CustomVideo:
                return BuildTextureView(custom_video_textures_.textures[custom_video_textures_.current_index]);
            case RoboGo::app::TextureSlot::MiniMap:
                return BuildTextureView(mini_map_texture_);
        }
        return {};
    }

    // 画面挂住（设备丢失 / 等待 fence 永远不返回）后的自愈：重新建 D3D12 设备和 ImGui。
    // 只允许在主线程、且没有半帧未完成时调用。
    bool TryRecoverRenderDevice() override {
        if (!initialized_ || frame_in_progress_) {
            return false;
        }
        SPDLOG_ERROR("渲染设备疑似丢失，开始重建 D3D12 与 ImGui（累计已画帧数={}）", rendered_frame_count_);

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        CleanupDeviceD3D();
        if (!CreateDeviceD3D()) {
            SPDLOG_CRITICAL("重建 Direct3D 设备失败");
            return false;
        }
        RoboGo::app::InitializeCommonImGui(main_scale_);
        ImGui_ImplWin32_Init(hwnd_);
        ImGui_ImplDX12_InitInfo init_info = {};
        init_info.Device = device_;
        init_info.CommandQueue = command_queue_;
        init_info.NumFramesInFlight = kFramesInFlight;
        init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        init_info.SrvDescriptorHeap = srv_descriptor_heap_;
        init_info.UserData = this;
        init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *info,
                                            D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_handle,
                                            D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_handle) {
            auto *backend = static_cast<WindowsPlatformBackend *>(info->UserData);
            backend->srv_descriptor_heap_allocator_.Alloc(out_cpu_handle, out_gpu_handle);
        };
        init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *info,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                           const D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle) {
            auto *backend = static_cast<WindowsPlatformBackend *>(info->UserData);
            backend->srv_descriptor_heap_allocator_.Free(cpu_handle, gpu_handle);
        };
        ImGui_ImplDX12_Init(&init_info);
        RoboGo::app::LoadEmbeddedImGuiFont();

        // 旧纹理是挂在旧设备上的，全部作废并按新堆重新分配描述符。
        ReleaseDynamicTextureSet(udp_video_textures_, false);
        ReleaseDynamicTextureSet(custom_video_textures_, false);
        ReleaseTexture(mini_map_texture_, false);
        AllocateVideoTextureDescriptors();

        frame_index_ = 0;
        fence_last_signaled_value_ = 0;
        for (auto &context: frame_context_) {
            context.fence_value = 0;
        }
        swap_chain_occluded_ = false;
        SPDLOG_WARN("渲染设备重建完成，视频纹理已清空，等待下一帧重新上传");
        return true;
    }

private:
    bool CreateDeviceD3D() {
        DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
        swap_chain_desc.BufferCount = kBackBuffers;
        swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_desc.SampleDesc.Count = 1;
        swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;

#ifdef DX12_ENABLE_DEBUG_LAYER
        ID3D12Debug *dx12_debug = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dx12_debug)))) {
            dx12_debug->EnableDebugLayer();
        }
#endif

        if (D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)) != S_OK) {
            return false;
        }

#ifdef DX12_ENABLE_DEBUG_LAYER
        if (dx12_debug != nullptr) {
            ID3D12InfoQueue *info_queue = nullptr;
            device_->QueryInterface(IID_PPV_ARGS(&info_queue));
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);

            constexpr int D3D12_MESSAGE_ID_FENCE_ZERO_WAIT_ = 1424;
            D3D12_MESSAGE_ID disabled_messages[] = {static_cast<D3D12_MESSAGE_ID>(D3D12_MESSAGE_ID_FENCE_ZERO_WAIT_)};
            D3D12_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumIDs = 1;
            filter.DenyList.pIDList = disabled_messages;
            info_queue->AddStorageFilterEntries(&filter);

            info_queue->Release();
            dx12_debug->Release();
        }
#endif

        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
        rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_heap_desc.NumDescriptors = kBackBuffers;
        if (device_->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_descriptor_heap_)) != S_OK) {
            return false;
        }

        const SIZE_T rtv_descriptor_size = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kBackBuffers; ++i) {
            main_render_target_descriptor_[i] = rtv_handle;
            rtv_handle.ptr += rtv_descriptor_size;
        }

        D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
        srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_heap_desc.NumDescriptors = kSrvHeapSize;
        srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (device_->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_descriptor_heap_)) != S_OK) {
            return false;
        }
        srv_descriptor_heap_allocator_.Create(device_, srv_descriptor_heap_);

        D3D12_COMMAND_QUEUE_DESC command_queue_desc = {};
        command_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (device_->CreateCommandQueue(&command_queue_desc, IID_PPV_ARGS(&command_queue_)) != S_OK) {
            return false;
        }

        for (UINT i = 0; i < kFramesInFlight; ++i) {
            if (device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame_context_[i].command_allocator)) != S_OK) {
                return false;
            }
        }

        if (device_->CreateCommandList(0,
                                       D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       frame_context_[0].command_allocator,
                                       nullptr,
                                       IID_PPV_ARGS(&command_list_)) != S_OK
            || command_list_->Close() != S_OK) {
            return false;
        }

        if (device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)) != S_OK) {
            return false;
        }

        fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (fence_event_ == nullptr) {
            return false;
        }

        IDXGIFactory5 *dxgi_factory = nullptr;
        IDXGISwapChain1 *swap_chain_1 = nullptr;
        if (CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)) != S_OK) {
            return false;
        }

        BOOL allow_tearing = FALSE;
        dxgi_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing));
        swap_chain_tearing_support_ = (allow_tearing == TRUE);
        if (swap_chain_tearing_support_) {
            swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }

        if (dxgi_factory->CreateSwapChainForHwnd(command_queue_, hwnd_, &swap_chain_desc, nullptr, nullptr, &swap_chain_1) != S_OK) {
            dxgi_factory->Release();
            return false;
        }
        if (swap_chain_1->QueryInterface(IID_PPV_ARGS(&swap_chain_)) != S_OK) {
            swap_chain_1->Release();
            dxgi_factory->Release();
            return false;
        }
        if (swap_chain_tearing_support_) {
            dxgi_factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        }
        swap_chain_1->Release();
        dxgi_factory->Release();

        swap_chain_->SetMaximumFrameLatency(kBackBuffers);
        swap_chain_waitable_object_ = swap_chain_->GetFrameLatencyWaitableObject();
        CreateRenderTarget();
        // 后台缓冲拿不到就不算初始化成功，否则会画出全黑/直接崩。
        return render_targets_valid_;
    }

    void CleanupDeviceD3D() {
        CleanupRenderTarget();
        if (swap_chain_ != nullptr) {
            swap_chain_->SetFullscreenState(false, nullptr);
            swap_chain_->Release();
            swap_chain_ = nullptr;
        }
        if (swap_chain_waitable_object_ != nullptr) {
            CloseHandle(swap_chain_waitable_object_);
            swap_chain_waitable_object_ = nullptr;
        }
        for (auto &context: frame_context_) {
            if (context.command_allocator != nullptr) {
                context.command_allocator->Release();
                context.command_allocator = nullptr;
            }
        }
        if (command_queue_ != nullptr) {
            command_queue_->Release();
            command_queue_ = nullptr;
        }
        if (command_list_ != nullptr) {
            command_list_->Release();
            command_list_ = nullptr;
        }
        if (rtv_descriptor_heap_ != nullptr) {
            rtv_descriptor_heap_->Release();
            rtv_descriptor_heap_ = nullptr;
        }
        if (srv_descriptor_heap_ != nullptr) {
            srv_descriptor_heap_->Release();
            srv_descriptor_heap_ = nullptr;
        }
        srv_descriptor_heap_allocator_.Destroy();
        if (fence_ != nullptr) {
            fence_->Release();
            fence_ = nullptr;
        }
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
        if (device_ != nullptr) {
            device_->Release();
            device_ = nullptr;
        }

#ifdef DX12_ENABLE_DEBUG_LAYER
        IDXGIDebug1 *debug = nullptr;
        if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug)))) {
            debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
            debug->Release();
        }
#endif
    }

    void CleanupWindow() {
        if (hwnd_ != nullptr) {
            ::DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (window_class_registered_) {
            ::UnregisterClassW(window_class_.lpszClassName, window_class_.hInstance);
            window_class_registered_ = false;
        }
    }

    void CreateRenderTarget() {
        render_targets_valid_ = false;
        if (swap_chain_ == nullptr || device_ == nullptr) {
            return;
        }
        for (UINT i = 0; i < kBackBuffers; ++i) {
            ID3D12Resource *back_buffer = nullptr;
            if (FAILED(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffer))) || back_buffer == nullptr) {
                // 拿不到后台缓冲说明交换链已经坏了，这里不能硬画，交给看门狗重建设备。
                SPDLOG_ERROR("获取交换链后台缓冲失败 index={}，等待设备重建", i);
                return;
            }
            device_->CreateRenderTargetView(back_buffer, nullptr, main_render_target_descriptor_[i]);
            main_render_target_resource_[i] = back_buffer;
        }
        render_targets_valid_ = true;
    }

    void CleanupRenderTarget() {
        render_targets_valid_ = false;
        WaitForPendingOperations();
        for (auto &resource: main_render_target_resource_) {
            if (resource != nullptr) {
                resource->Release();
                resource = nullptr;
            }
        }
    }

    // 在等 GPU 的空档里把本线程的窗口消息抽干：否则登录时正好弹出别的窗口
    // （系统提示/杀软/网页登录页），主线程来不及响应，Windows 会先把窗口标成「无响应」。
    static void PumpWindowMessagesWhileWaiting() {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            if (message.message == WM_QUIT) {
                // 退出消息不能吞掉：放回队列，交给主循环的 ProcessEvents 正常退出。
                ::PostQuitMessage(static_cast<int>(message.wParam));
                return;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    void WaitForPendingOperations() {
        if (command_queue_ == nullptr || fence_ == nullptr || fence_event_ == nullptr || swap_chain_waitable_object_ == nullptr) {
            return;
        }
        if (::WaitForSingleObject(swap_chain_waitable_object_, 500) != WAIT_OBJECT_0) {
            // 交换链都等不到，GPU 已经挂了，再 Signal 也只会卡住设备重建，直接返回。
            SPDLOG_WARN("等待交换链空闲超时（500毫秒），跳过GPU等待");
            return;
        }
        command_queue_->Signal(fence_, ++fence_last_signaled_value_);
        fence_->SetEventOnCompletion(fence_last_signaled_value_, fence_event_);
        if (::WaitForSingleObject(fence_event_, 1000) != WAIT_OBJECT_0) {
            SPDLOG_ERROR("等待GPU空闲超时（1秒），设备可能已挂起");
        }
    }

    WindowsFrameContext *WaitForNextFrameContext() {
        WindowsFrameContext *frame_context = &frame_context_[frame_index_ % kFramesInFlight];
        // 原来是 INFINITE：GPU/交换链一挂，主线程就永远停在这里，界面直接“卡死”。
        // 现在最多等 2 秒，超时就跳过这一帧，交给上层判定是否重建设备。
        if (fence_->GetCompletedValue() < frame_context->fence_value) {
            fence_->SetEventOnCompletion(frame_context->fence_value, fence_event_);
            HANDLE waitable_objects[] = {swap_chain_waitable_object_, fence_event_};
            if (::WaitForMultipleObjects(2, waitable_objects, TRUE, 2000) == WAIT_TIMEOUT) {
                return nullptr;
            }
        } else if (::WaitForSingleObject(swap_chain_waitable_object_, 2000) == WAIT_TIMEOUT) {
            return nullptr;
        }
        return frame_context;
    }

    [[nodiscard]] bool LoadTextureFromMemory(const uint8_t *data,
                                             const int width,
                                             const int height,
                                             const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle,
                                             ID3D12Resource **out_texture_resource) const {
        D3D12_HEAP_PROPERTIES props = {};
        props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        ID3D12Resource *texture = nullptr;
        HRESULT hr = device_->CreateCommittedResource(&props,
                                                      D3D12_HEAP_FLAG_NONE,
                                                      &desc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr,
                                                      IID_PPV_ARGS(&texture));
        if (FAILED(hr)) {
            return false;
        }

        const UINT upload_pitch =
                (static_cast<UINT>(width) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)
                & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        const UINT upload_size = static_cast<UINT>(height) * upload_pitch;

        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = upload_size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        props.Type = D3D12_HEAP_TYPE_UPLOAD;

        ID3D12Resource *upload_buffer = nullptr;
        hr = device_->CreateCommittedResource(&props,
                                              D3D12_HEAP_FLAG_NONE,
                                              &desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr,
                                              IID_PPV_ARGS(&upload_buffer));
        if (FAILED(hr)) {
            texture->Release();
            return false;
        }

        void *mapped = nullptr;
        D3D12_RANGE range = {0, upload_size};
        hr = upload_buffer->Map(0, &range, &mapped);
        if (FAILED(hr)) {
            upload_buffer->Release();
            texture->Release();
            return false;
        }
        auto *mapped_bytes = static_cast<uint8_t *>(mapped);
        const size_t row_bytes = static_cast<size_t>(width) * 4u;
        for (int y = 0; y < height; ++y) {
            memcpy(mapped_bytes + static_cast<size_t>(y) * upload_pitch,
                   data + static_cast<size_t>(y) * row_bytes,
                   row_bytes);
        }
        D3D12_RANGE written_range = {0, upload_size};
        upload_buffer->Unmap(0, &written_range);

        D3D12_TEXTURE_COPY_LOCATION src_location = {};
        src_location.pResource = upload_buffer;
        src_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src_location.PlacedFootprint.Footprint.Width = width;
        src_location.PlacedFootprint.Footprint.Height = height;
        src_location.PlacedFootprint.Footprint.Depth = 1;
        src_location.PlacedFootprint.Footprint.RowPitch = upload_pitch;

        D3D12_TEXTURE_COPY_LOCATION dst_location = {};
        dst_location.pResource = texture;
        dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_location.SubresourceIndex = 0;

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        ID3D12Fence *fence = nullptr;
        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) {
            upload_buffer->Release();
            texture->Release();
            SPDLOG_ERROR("创建纹理上传fence失败 hr=0x{:08X} width={} height={}",
                         static_cast<unsigned int>(hr), width, height);
            return false;
        }

        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) {
            fence->Release();
            upload_buffer->Release();
            texture->Release();
            SPDLOG_ERROR("创建纹理上传事件失败 error={} width={} height={}",
                         static_cast<unsigned long>(GetLastError()), width, height);
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        ID3D12CommandQueue *command_queue = nullptr;
        hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue));
        if (FAILED(hr)) {
            CloseHandle(event);
            fence->Release();
            upload_buffer->Release();
            texture->Release();
            SPDLOG_ERROR("创建纹理上传命令队列失败 hr=0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }

        ID3D12CommandAllocator *command_allocator = nullptr;
        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator));
        if (FAILED(hr)) {
            command_queue->Release();
            CloseHandle(event);
            fence->Release();
            upload_buffer->Release();
            texture->Release();
            SPDLOG_ERROR("创建纹理上传命令分配器失败 hr=0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }

        ID3D12GraphicsCommandList *command_list = nullptr;
        hr = device_->CreateCommandList(0,
                                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        command_allocator,
                                        nullptr,
                                        IID_PPV_ARGS(&command_list));
        if (FAILED(hr)) {
            command_allocator->Release();
            command_queue->Release();
            CloseHandle(event);
            fence->Release();
            upload_buffer->Release();
            texture->Release();
            SPDLOG_ERROR("创建纹理上传命令列表失败 hr=0x{:08X}", static_cast<unsigned int>(hr));
            return false;
        }

        command_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
        command_list->ResourceBarrier(1, &barrier);
        hr = command_list->Close();
        if (SUCCEEDED(hr)) {
            command_queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&command_list));
            hr = command_queue->Signal(fence, 1);
        }
        if (SUCCEEDED(hr) && fence->SetEventOnCompletion(1, event) >= 0) {
            // 以前这里是 INFINITE：GPU 一挂起，主线程就永远回不来（表现就是“点登录/进比赛后卡死”）。
            // 现在拆成 100 毫秒一片，每片之间把窗口消息抽干；累计 5 秒还没拷完就当这次上传失败，
            // 返回 false 由上层重试或走设备重建。
            int texture_wait_slices = 0;
            for (;;) {
                const DWORD wait_result = WaitForSingleObject(event, 100);
                if (wait_result == WAIT_OBJECT_0) {
                    break;
                }
                PumpWindowMessagesWhileWaiting();
                if (++texture_wait_slices >= 50) {
                    SPDLOG_ERROR("纹理上传等待GPU超时 width={} height={}", width, height);
                    hr = E_FAIL;
                    break;
                }
            }
        } else if (SUCCEEDED(hr)) {
            hr = E_FAIL;
        }

        command_list->Release();
        command_allocator->Release();
        command_queue->Release();
        CloseHandle(event);
        fence->Release();
        upload_buffer->Release();

        if (FAILED(hr)) {
            texture->Release();
            SPDLOG_ERROR("纹理上传失败 hr=0x{:08X} width={} height={}", static_cast<unsigned int>(hr), width, height);
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = desc.MipLevels;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device_->CreateShaderResourceView(texture, &srv_desc, srv_cpu_handle);

        *out_texture_resource = texture;
        return true;
    }

    void AllocateVideoTextureDescriptors() {
        for (auto &texture: udp_video_textures_.textures) {
            srv_descriptor_heap_allocator_.Alloc(&texture.cpu_handle, &texture.gpu_handle);
            texture.descriptor_allocated = true;
        }
        for (auto &texture: custom_video_textures_.textures) {
            srv_descriptor_heap_allocator_.Alloc(&texture.cpu_handle, &texture.gpu_handle);
            texture.descriptor_allocated = true;
        }
        srv_descriptor_heap_allocator_.Alloc(&mini_map_texture_.cpu_handle, &mini_map_texture_.gpu_handle);
        mini_map_texture_.descriptor_allocated = true;
    }

    [[nodiscard]] bool UploadDynamicTexture(WindowsDynamicTextureSet &texture_set, const RoboGo::app::VideoFrame &frame) const {
        WindowsVideoTexture &next_texture = texture_set.textures[texture_set.next_index];
        if (next_texture.texture != nullptr) {
            next_texture.texture->Release();
            next_texture.texture = nullptr;
        }
        if (!LoadTextureFromMemory(frame.buffer.data(), frame.width, frame.height, next_texture.cpu_handle, &next_texture.texture)) {
            return false;
        }
        next_texture.width = frame.width;
        next_texture.height = frame.height;
        next_texture.ready = true;
        texture_set.current_index = texture_set.next_index;
        texture_set.next_index = (texture_set.next_index + 1) % 2;
        next_texture.ready = false;
        return true;
    }

    [[nodiscard]] bool UploadStaticTexture(WindowsVideoTexture &texture,
                                           const uint8_t *data,
                                           const int width,
                                           const int height) const {
        if (texture.texture != nullptr) {
            texture.texture->Release();
            texture.texture = nullptr;
        }
        if (!LoadTextureFromMemory(data, width, height, texture.cpu_handle, &texture.texture)) {
            return false;
        }
        texture.width = width;
        texture.height = height;
        return true;
    }

    void ReleaseDynamicTextureSet(WindowsDynamicTextureSet &texture_set, const bool free_descriptor) {
        for (auto &texture: texture_set.textures) {
            ReleaseTexture(texture, free_descriptor);
        }
    }

    void ReleaseTexture(WindowsVideoTexture &texture, const bool free_descriptor) {
        if (texture.texture != nullptr) {
            texture.texture->Release();
            texture.texture = nullptr;
        }
        if (free_descriptor && texture.descriptor_allocated) {
            srv_descriptor_heap_allocator_.Free(texture.cpu_handle, texture.gpu_handle);
            texture.descriptor_allocated = false;
        }
        texture.width = 0;
        texture.height = 0;
        texture.ready = false;
    }

    [[nodiscard]] static RoboGo::app::TextureView BuildTextureView(const WindowsVideoTexture &texture) {
        RoboGo::app::TextureView texture_view;
        if (texture.texture != nullptr) {
            texture_view.texture_ref = ImTextureRef(static_cast<ImTextureID>(texture.gpu_handle.ptr));
        }
        texture_view.size = ImVec2(static_cast<float>(texture.width), static_cast<float>(texture.height));
        return texture_view;
    }

    static LRESULT WINAPI StaticWndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        if (message == WM_NCCREATE) {
            auto *create_struct = reinterpret_cast<CREATESTRUCTW *>(l_param);
            auto *backend = static_cast<WindowsPlatformBackend *>(create_struct->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(backend));
            backend->hwnd_ = hwnd;
        }

        auto *backend = reinterpret_cast<WindowsPlatformBackend *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (backend != nullptr) {
            return backend->WndProc(hwnd, message, w_param, l_param);
        }
        return ::DefWindowProcW(hwnd, message, w_param, l_param);
    }

    LRESULT WndProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
        if (ImGui::GetCurrentContext() != nullptr && ImGui_ImplWin32_WndProcHandler(hwnd, message, w_param, l_param)) {
            return TRUE;
        }

        switch (message) {
            case WM_SIZE:
                if (device_ != nullptr && swap_chain_ != nullptr && w_param != SIZE_MINIMIZED) {
                    CleanupRenderTarget();
                    DXGI_SWAP_CHAIN_DESC1 desc = {};
                    swap_chain_->GetDesc1(&desc);
                    const HRESULT result = swap_chain_->ResizeBuffers(0,
                                                                      static_cast<UINT>(LOWORD(l_param)),
                                                                      static_cast<UINT>(HIWORD(l_param)),
                                                                      desc.Format,
                                                                      desc.Flags);
                    if (FAILED(result)) {
                        // 调整交换链失败后后台缓冲就废了，标记成无效，等看门狗重建设备。
                        render_targets_valid_ = false;
                        SPDLOG_ERROR("调整交换链缓冲失败 hr=0x{:08X}，等待设备重建", static_cast<unsigned int>(result));
                        return 0;
                    }
                    CreateRenderTarget();
                }
                return 0;
            case WM_SYSCOMMAND:
                if ((w_param & 0xfff0) == SC_KEYMENU) {
                    return 0;
                }
                break;
            case WM_DESTROY:
                ::PostQuitMessage(0);
                return 0;
            default:
                break;
        }

        return ::DefWindowProcW(hwnd, message, w_param, l_param);
    }

    std::array<WindowsFrameContext, kFramesInFlight> frame_context_ = {};
    UINT frame_index_ = 0;
    ID3D12Device *device_ = nullptr;
    ID3D12DescriptorHeap *rtv_descriptor_heap_ = nullptr;
    ID3D12DescriptorHeap *srv_descriptor_heap_ = nullptr;
    DescriptorHeapAllocator srv_descriptor_heap_allocator_;
    ID3D12CommandQueue *command_queue_ = nullptr;
    ID3D12GraphicsCommandList *command_list_ = nullptr;
    ID3D12Fence *fence_ = nullptr;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_last_signaled_value_ = 0;
    IDXGISwapChain3 *swap_chain_ = nullptr;
    bool swap_chain_tearing_support_ = false;
    bool swap_chain_occluded_ = false;
    HANDLE swap_chain_waitable_object_ = nullptr;
    std::array<ID3D12Resource *, kBackBuffers> main_render_target_resource_ = {};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBackBuffers> main_render_target_descriptor_ = {};
    WNDCLASSEXW window_class_ = {};
    HWND hwnd_ = nullptr;
    bool window_class_registered_ = false;
    float main_scale_ = 1.0f;
    WindowsDynamicTextureSet udp_video_textures_;
    WindowsDynamicTextureSet custom_video_textures_;
    WindowsVideoTexture mini_map_texture_;
    bool initialized_ = false;
    bool should_close_ = false;
    bool game_input_captured_ = false;
    // BeginFrame 已成功但 RenderFrame 还没画完的那一帧，用来判断主循环是否卡在渲染里。
    bool frame_in_progress_ = false;
    std::uint64_t rendered_frame_count_ = 0;
    // 窗口最小化/被遮挡导致这一帧没开始画。
    bool render_paused_by_window_ = false;
    // 后台缓冲是否可用。拿不到后台缓冲时不能 BeginFrame，否则会空指针画到崩溃。
    bool render_targets_valid_ = false;
};

}// namespace

namespace RoboGo::platform {

std::unique_ptr<PlatformBackend> CreatePlatformBackend() {
    return std::make_unique<WindowsPlatformBackend>();
}

}// namespace RoboGo::platform

#endif
