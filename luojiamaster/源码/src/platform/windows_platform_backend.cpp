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
        if ((swap_chain_occluded_ && swap_chain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) || ::IsIconic(hwnd_)) {
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

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        return true;
    }

    void RenderFrame(ImDrawData *draw_data) override {
        WindowsFrameContext *frame_context = WaitForNextFrameContext();
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
        command_list_->Close();

        command_queue_->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&command_list_));
        command_queue_->Signal(fence_, ++fence_last_signaled_value_);
        frame_context->fence_value = fence_last_signaled_value_;

        const HRESULT present_result = swap_chain_->Present(1, 0);
        swap_chain_occluded_ = (present_result == DXGI_STATUS_OCCLUDED);
        ++frame_index_;
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

    [[nodiscard]] RoboGo::app::TextureView GetTextureView(const RoboGo::app::TextureSlot slot) const override {
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
        return true;
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
        for (UINT i = 0; i < kBackBuffers; ++i) {
            ID3D12Resource *back_buffer = nullptr;
            swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffer));
            device_->CreateRenderTargetView(back_buffer, nullptr, main_render_target_descriptor_[i]);
            main_render_target_resource_[i] = back_buffer;
        }
    }

    void CleanupRenderTarget() {
        WaitForPendingOperations();
        for (auto &resource: main_render_target_resource_) {
            if (resource != nullptr) {
                resource->Release();
                resource = nullptr;
            }
        }
    }

    void WaitForPendingOperations() {
        if (command_queue_ == nullptr || fence_ == nullptr || fence_event_ == nullptr) {
            return;
        }
        command_queue_->Signal(fence_, ++fence_last_signaled_value_);
        fence_->SetEventOnCompletion(fence_last_signaled_value_, fence_event_);
        ::WaitForSingleObject(fence_event_, INFINITE);
    }

    WindowsFrameContext *WaitForNextFrameContext() {
        WindowsFrameContext *frame_context = &frame_context_[frame_index_ % kFramesInFlight];
        if (fence_->GetCompletedValue() < frame_context->fence_value) {
            fence_->SetEventOnCompletion(frame_context->fence_value, fence_event_);
            HANDLE waitable_objects[] = {swap_chain_waitable_object_, fence_event_};
            ::WaitForMultipleObjects(2, waitable_objects, TRUE, INFINITE);
        } else {
            ::WaitForSingleObject(swap_chain_waitable_object_, INFINITE);
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
        IM_ASSERT(SUCCEEDED(hr));

        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        IM_ASSERT(event != nullptr);

        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        ID3D12CommandQueue *command_queue = nullptr;
        hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue));
        IM_ASSERT(SUCCEEDED(hr));

        ID3D12CommandAllocator *command_allocator = nullptr;
        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator));
        IM_ASSERT(SUCCEEDED(hr));

        ID3D12GraphicsCommandList *command_list = nullptr;
        hr = device_->CreateCommandList(0,
                                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        command_allocator,
                                        nullptr,
                                        IID_PPV_ARGS(&command_list));
        IM_ASSERT(SUCCEEDED(hr));

        command_list->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);
        command_list->ResourceBarrier(1, &barrier);
        hr = command_list->Close();
        IM_ASSERT(SUCCEEDED(hr));

        command_queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList *const *>(&command_list));
        hr = command_queue->Signal(fence, 1);
        IM_ASSERT(SUCCEEDED(hr));
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);

        command_list->Release();
        command_allocator->Release();
        command_queue->Release();
        CloseHandle(event);
        fence->Release();
        upload_buffer->Release();

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
                if (device_ != nullptr && w_param != SIZE_MINIMIZED) {
                    CleanupRenderTarget();
                    DXGI_SWAP_CHAIN_DESC1 desc = {};
                    swap_chain_->GetDesc1(&desc);
                    const HRESULT result = swap_chain_->ResizeBuffers(0,
                                                                      static_cast<UINT>(LOWORD(l_param)),
                                                                      static_cast<UINT>(HIWORD(l_param)),
                                                                      desc.Format,
                                                                      desc.Flags);
                    IM_ASSERT(SUCCEEDED(result) && "Failed to resize swapchain.");
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
};

}// namespace

namespace RoboGo::platform {

std::unique_ptr<PlatformBackend> CreatePlatformBackend() {
    return std::make_unique<WindowsPlatformBackend>();
}

}// namespace RoboGo::platform

#endif
