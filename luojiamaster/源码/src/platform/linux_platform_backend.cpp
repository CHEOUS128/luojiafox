#ifndef _WIN32

#include "platform/platform_backend.h"

#include "app/imgui_setup.h"
#include "logging/logging.h"

#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
#endif

namespace {

struct LinuxVideoTexture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;
    bool ready = false;
};

struct LinuxDynamicTextureSet {
    std::array<LinuxVideoTexture, 2> textures;
    int current_index = 0;
    int next_index = 1;
};

class LinuxPlatformBackend final : public RoboGo::platform::PlatformBackend {
public:
    [[nodiscard]] const char *application_name() const override {
        return "luojiamaster-linux";
    }

    bool Initialize() override {
        glfwSetErrorCallback(GlfwErrorCallback);
        if (!glfwInit()) {
            SPDLOG_CRITICAL("glfwInit失败");
            return false;
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        main_scale_ = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
        window_ = glfwCreateWindow(static_cast<int>(1280 * main_scale_),
                                   static_cast<int>(800 * main_scale_),
                                   "luojiamaster",
                                   nullptr,
                                   nullptr);
        if (window_ == nullptr) {
            SPDLOG_CRITICAL("创建GLFW窗口失败");
            glfwTerminate();
            return false;
        }
        if (!glfwVulkanSupported()) {
            SPDLOG_CRITICAL("当前环境不支持Vulkan");
            glfwDestroyWindow(window_);
            window_ = nullptr;
            glfwTerminate();
            return false;
        }
        SPDLOG_INFO("GLFW窗口创建成功 size={}x{} scale={}",
                    static_cast<int>(1280 * main_scale_),
                    static_cast<int>(800 * main_scale_),
                    main_scale_);

        ImVector<const char *> extensions;
        uint32_t extensions_count = 0;
        const char **glfw_extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
        for (uint32_t i = 0; i < extensions_count; ++i) {
            extensions.push_back(glfw_extensions[i]);
        }

        SetupVulkan(extensions);

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkResult err = glfwCreateWindowSurface(instance_, window_, allocator_, &surface);
        CheckVkResult(err);

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        SetupVulkanWindow(surface, width, height);

        RoboGo::app::InitializeCommonImGui(main_scale_);

        ImGui_ImplGlfw_InitForVulkan(window_, true);
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = instance_;
        init_info.PhysicalDevice = physical_device_;
        init_info.Device = device_;
        init_info.QueueFamily = queue_family_;
        init_info.Queue = queue_;
        init_info.DescriptorPool = descriptor_pool_;
        init_info.MinImageCount = min_image_count_;
        init_info.ImageCount = main_window_data_.ImageCount;
        init_info.Allocator = allocator_;
        init_info.PipelineInfoMain.RenderPass = main_window_data_.RenderPass;
        init_info.PipelineInfoMain.Subpass = 0;
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        init_info.CheckVkResultFn = CheckVkResult;
        ImGui_ImplVulkan_Init(&init_info);

        RoboGo::app::LoadEmbeddedImGuiFont();
        initialized_ = true;
        return true;
    }

    void Shutdown() override {
        if (!initialized_) {
            return;
        }

        SetGameInputCapture(false);

        VkResult err = vkDeviceWaitIdle(device_);
        CheckVkResult(err);

        DestroyDynamicTextureSet(udp_video_textures_);
        DestroyDynamicTextureSet(custom_video_textures_);
        DestroyVulkanTexture(mini_map_texture_);

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        CleanupVulkanWindow();
        CleanupVulkan();

        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
        initialized_ = false;
    }

    [[nodiscard]] bool ProcessEvents() override {
        glfwPollEvents();
        return !glfwWindowShouldClose(window_);
    }

    [[nodiscard]] bool BeginFrame() override {
        int fb_width = 0;
        int fb_height = 0;
        glfwGetFramebufferSize(window_, &fb_width, &fb_height);
        if (fb_width > 0
            && fb_height > 0
            && (swap_chain_rebuild_
                || main_window_data_.Width != fb_width
                || main_window_data_.Height != fb_height)) {
            ImGui_ImplVulkan_SetMinImageCount(min_image_count_);
            ImGui_ImplVulkanH_CreateOrResizeWindow(instance_,
                                                  physical_device_,
                                                  device_,
                                                  &main_window_data_,
                                                  queue_family_,
                                                  allocator_,
                                                  fb_width,
                                                  fb_height,
                                                  min_image_count_,
                                                  0);
            main_window_data_.FrameIndex = 0;
            swap_chain_rebuild_ = false;
        }
        if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED) != 0) {
            ImGui_ImplGlfw_Sleep(10);
            return false;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        return true;
    }

    void RenderFrame(ImDrawData *draw_data) override {
        if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) {
            return;
        }

        main_window_data_.ClearValue.color.float32[0] = 0.0f;
        main_window_data_.ClearValue.color.float32[1] = 0.0f;
        main_window_data_.ClearValue.color.float32[2] = 0.0f;
        main_window_data_.ClearValue.color.float32[3] = 1.0f;
        FrameRender(draw_data);
        FramePresent();
    }

    void SetGameInputCapture(const bool enabled) override {
        if (window_ == nullptr || game_input_captured_ == enabled) {
            return;
        }

        glfwSetInputMode(window_, GLFW_CURSOR, enabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        if (enabled) {
            SPDLOG_DEBUG("已启用Linux隐藏并锁定鼠标模式");
        } else {
            SPDLOG_DEBUG("已恢复Linux鼠标模式");
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
    static void GlfwErrorCallback(const int error, const char *description) {
        SPDLOG_ERROR("GLFW错误 code={} description={}", error, description);
    }

    static void CheckVkResult(const VkResult err) {
        if (err == VK_SUCCESS) {
            return;
        }
        SPDLOG_ERROR("Vulkan错误 result={}", static_cast<int>(err));
        if (err < 0) {
            std::abort();
        }
    }

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugReportCallback(VkDebugReportFlagsEXT flags,
                                                              VkDebugReportObjectTypeEXT object_type,
                                                              uint64_t object,
                                                              size_t location,
                                                              int32_t message_code,
                                                              const char *layer_prefix,
                                                              const char *message,
                                                              void *user_data) {
        (void) flags;
        (void) object_type;
        (void) object;
        (void) location;
        (void) message_code;
        (void) layer_prefix;
        (void) user_data;
        SPDLOG_WARN("Vulkan调试报告 message={}", message);
        return VK_FALSE;
    }
#endif

    [[nodiscard]] static bool IsExtensionAvailable(const ImVector<VkExtensionProperties> &properties, const char *extension) {
        for (const VkExtensionProperties &property: properties) {
            if (strcmp(property.extensionName, extension) == 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] uint32_t FindMemoryType(const uint32_t type_filter, const VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memory_properties;
        vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
        for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((memory_properties.memoryTypes[i].propertyFlags & properties) == properties
                && (type_filter & (1u << i))) {
                return i;
            }
        }
        return static_cast<uint32_t>(-1);
    }

    void SetupVulkan(ImVector<const char *> instance_extensions) {
        VkResult err;

        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        uint32_t properties_count = 0;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        CheckVkResult(err);

        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        }
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        create_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.Size);
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, allocator_, &instance_);
        CheckVkResult(err);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto *create_debug_report_callback =
                reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugReportCallbackEXT"));
        IM_ASSERT(create_debug_report_callback != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT
                                | VK_DEBUG_REPORT_WARNING_BIT_EXT
                                | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = DebugReportCallback;
        debug_report_ci.pUserData = this;
        err = create_debug_report_callback(instance_, &debug_report_ci, allocator_, &debug_report_);
        CheckVkResult(err);
#endif

        physical_device_ = ImGui_ImplVulkanH_SelectPhysicalDevice(instance_);
        IM_ASSERT(physical_device_ != VK_NULL_HANDLE);

        queue_family_ = ImGui_ImplVulkanH_SelectQueueFamilyIndex(physical_device_);
        IM_ASSERT(queue_family_ != static_cast<uint32_t>(-1));

        ImVector<const char *> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        properties_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &properties_count, properties.Data);
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        }
#endif

        const float queue_priority[] = {1.0f};
        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = queue_priority;

        VkDeviceCreateInfo device_create_info = {};
        device_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_create_info.queueCreateInfoCount = 1;
        device_create_info.pQueueCreateInfos = &queue_info;
        device_create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.Size);
        device_create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(physical_device_, &device_create_info, allocator_, &device_);
        CheckVkResult(err);
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

        VkDescriptorPoolSize pool_sizes[] = {
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE},
                {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.poolSizeCount = static_cast<uint32_t>(IM_ARRAYSIZE(pool_sizes));
        pool_info.pPoolSizes = pool_sizes;
        pool_info.maxSets = 0;
        for (const auto &pool_size : pool_sizes)
            pool_info.maxSets += pool_size.descriptorCount;
        err = vkCreateDescriptorPool(device_, &pool_info, allocator_, &descriptor_pool_);
        CheckVkResult(err);

        VkCommandPoolCreateInfo command_pool_info = {};
        command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_pool_info.queueFamilyIndex = queue_family_;
        err = vkCreateCommandPool(device_, &command_pool_info, allocator_, &texture_upload_command_pool_);
        CheckVkResult(err);

        VkSamplerCreateInfo sampler_info = {};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.minLod = -1000.0f;
        sampler_info.maxLod = 1000.0f;
        sampler_info.maxAnisotropy = 1.0f;
        err = vkCreateSampler(device_, &sampler_info, allocator_, &video_texture_sampler_);
        CheckVkResult(err);
    }

    void SetupVulkanWindow(const VkSurfaceKHR surface, const int width, const int height) {
        VkBool32 result = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_family_, surface, &result);
        if (result != VK_TRUE) {
            SPDLOG_CRITICAL("当前GPU不支持Vulkan窗口表面");
            std::exit(-1);
        }

        const VkFormat request_surface_image_format[] = {
                VK_FORMAT_B8G8R8A8_UNORM,
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_FORMAT_B8G8R8_UNORM,
                VK_FORMAT_R8G8B8_UNORM,
        };
        const VkColorSpaceKHR request_surface_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
        main_window_data_.Surface = surface;
        main_window_data_.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
                physical_device_,
                main_window_data_.Surface,
                request_surface_image_format,
                static_cast<size_t>(IM_ARRAYSIZE(request_surface_image_format)),
                request_surface_color_space);

        VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
        main_window_data_.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
                physical_device_,
                main_window_data_.Surface,
                &present_modes[0],
                IM_ARRAYSIZE(present_modes));

        ImGui_ImplVulkanH_CreateOrResizeWindow(instance_,
                                              physical_device_,
                                              device_,
                                              &main_window_data_,
                                              queue_family_,
                                              allocator_,
                                              width,
                                              height,
                                              min_image_count_,
                                              0);
    }

    void CleanupVulkanWindow() {
        ImGui_ImplVulkanH_DestroyWindow(instance_, device_, &main_window_data_, allocator_);
        if (main_window_data_.Surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, main_window_data_.Surface, allocator_);
            main_window_data_.Surface = VK_NULL_HANDLE;
        }
    }

    void CleanupVulkan() {
        vkDestroyDescriptorPool(device_, descriptor_pool_, allocator_);
        vkDestroySampler(device_, video_texture_sampler_, allocator_);
        vkDestroyCommandPool(device_, texture_upload_command_pool_, allocator_);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto *destroy_debug_report_callback =
                reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugReportCallbackEXT"));
        destroy_debug_report_callback(instance_, debug_report_, allocator_);
#endif

        vkDestroyDevice(device_, allocator_);
        vkDestroyInstance(instance_, allocator_);
    }

    void FrameRender(ImDrawData *draw_data) {
        VkSemaphore image_acquired_semaphore = main_window_data_.FrameSemaphores[main_window_data_.SemaphoreIndex].ImageAcquiredSemaphore;
        VkSemaphore render_complete_semaphore = main_window_data_.FrameSemaphores[main_window_data_.SemaphoreIndex].RenderCompleteSemaphore;
        VkResult err = vkAcquireNextImageKHR(device_,
                                             main_window_data_.Swapchain,
                                             UINT64_MAX,
                                             image_acquired_semaphore,
                                             VK_NULL_HANDLE,
                                             &main_window_data_.FrameIndex);
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
            swap_chain_rebuild_ = true;
        }
        if (err == VK_ERROR_OUT_OF_DATE_KHR) {
            return;
        }
        if (err != VK_SUBOPTIMAL_KHR) {
            CheckVkResult(err);
        }

        ImGui_ImplVulkanH_Frame *frame = &main_window_data_.Frames[main_window_data_.FrameIndex];
        err = vkWaitForFences(device_, 1, &frame->Fence, VK_TRUE, UINT64_MAX);
        CheckVkResult(err);
        err = vkResetFences(device_, 1, &frame->Fence);
        CheckVkResult(err);

        err = vkResetCommandPool(device_, frame->CommandPool, 0);
        CheckVkResult(err);

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(frame->CommandBuffer, &begin_info);
        CheckVkResult(err);

        VkRenderPassBeginInfo render_pass_info = {};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_info.renderPass = main_window_data_.RenderPass;
        render_pass_info.framebuffer = frame->Framebuffer;
        render_pass_info.renderArea.extent.width = main_window_data_.Width;
        render_pass_info.renderArea.extent.height = main_window_data_.Height;
        render_pass_info.clearValueCount = 1;
        render_pass_info.pClearValues = &main_window_data_.ClearValue;
        vkCmdBeginRenderPass(frame->CommandBuffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

        ImGui_ImplVulkan_RenderDrawData(draw_data, frame->CommandBuffer);
        vkCmdEndRenderPass(frame->CommandBuffer);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &image_acquired_semaphore;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &frame->CommandBuffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(frame->CommandBuffer);
        CheckVkResult(err);
        err = vkQueueSubmit(queue_, 1, &submit_info, frame->Fence);
        CheckVkResult(err);
    }

    void FramePresent() {
        if (swap_chain_rebuild_) {
            return;
        }

        VkSemaphore render_complete_semaphore = main_window_data_.FrameSemaphores[main_window_data_.SemaphoreIndex].RenderCompleteSemaphore;
        VkPresentInfoKHR present_info = {};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &render_complete_semaphore;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &main_window_data_.Swapchain;
        present_info.pImageIndices = &main_window_data_.FrameIndex;
        VkResult err = vkQueuePresentKHR(queue_, &present_info);
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
            swap_chain_rebuild_ = true;
        }
        if (err == VK_ERROR_OUT_OF_DATE_KHR) {
            return;
        }
        if (err != VK_SUBOPTIMAL_KHR) {
            CheckVkResult(err);
        }
        main_window_data_.SemaphoreIndex = (main_window_data_.SemaphoreIndex + 1) % main_window_data_.SemaphoreCount;
    }

    [[nodiscard]] bool CreateVulkanImage(const uint8_t *data, const int width, const int height, LinuxVideoTexture &texture) {
        VkResult err;

        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent.width = static_cast<uint32_t>(width);
        image_info.extent.height = static_cast<uint32_t>(height);
        image_info.extent.depth = 1;
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;

        err = vkCreateImage(device_, &image_info, allocator_, &texture.image);
        if (err != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements image_memory_requirements;
        vkGetImageMemoryRequirements(device_, texture.image, &image_memory_requirements);

        VkMemoryAllocateInfo image_alloc_info = {};
        image_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        image_alloc_info.allocationSize = image_memory_requirements.size;
        image_alloc_info.memoryTypeIndex = FindMemoryType(image_memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        err = vkAllocateMemory(device_, &image_alloc_info, allocator_, &texture.memory);
        if (err != VK_SUCCESS) {
            vkDestroyImage(device_, texture.image, allocator_);
            texture.image = VK_NULL_HANDLE;
            return false;
        }

        vkBindImageMemory(device_, texture.image, texture.memory, 0);

        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = texture.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        err = vkCreateImageView(device_, &view_info, allocator_, &texture.view);
        if (err != VK_SUCCESS) {
            vkDestroyImage(device_, texture.image, allocator_);
            vkFreeMemory(device_, texture.memory, allocator_);
            texture.image = VK_NULL_HANDLE;
            texture.memory = VK_NULL_HANDLE;
            return false;
        }

        VkBufferCreateInfo staging_buffer_info = {};
        staging_buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        staging_buffer_info.size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4u;
        staging_buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        staging_buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer staging_buffer = VK_NULL_HANDLE;
        err = vkCreateBuffer(device_, &staging_buffer_info, allocator_, &staging_buffer);
        if (err != VK_SUCCESS) {
            DestroyVulkanTexture(texture);
            return false;
        }

        VkMemoryRequirements staging_memory_requirements;
        vkGetBufferMemoryRequirements(device_, staging_buffer, &staging_memory_requirements);

        VkMemoryAllocateInfo staging_alloc_info = {};
        staging_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        staging_alloc_info.allocationSize = staging_memory_requirements.size;
        staging_alloc_info.memoryTypeIndex = FindMemoryType(staging_memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        err = vkAllocateMemory(device_, &staging_alloc_info, allocator_, &staging_memory);
        if (err != VK_SUCCESS) {
            vkDestroyBuffer(device_, staging_buffer, allocator_);
            DestroyVulkanTexture(texture);
            return false;
        }

        vkBindBufferMemory(device_, staging_buffer, staging_memory, 0);

        void *mapped_data = nullptr;
        err = vkMapMemory(device_, staging_memory, 0, staging_buffer_info.size, 0, &mapped_data);
        if (err != VK_SUCCESS) {
            vkFreeMemory(device_, staging_memory, allocator_);
            vkDestroyBuffer(device_, staging_buffer, allocator_);
            DestroyVulkanTexture(texture);
            return false;
        }
        memcpy(mapped_data, data, static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        vkUnmapMemory(device_, staging_memory);

        VkCommandBufferAllocateInfo command_buffer_alloc_info = {};
        command_buffer_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_buffer_alloc_info.commandPool = texture_upload_command_pool_;
        command_buffer_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_buffer_alloc_info.commandBufferCount = 1;

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        err = vkAllocateCommandBuffers(device_, &command_buffer_alloc_info, &command_buffer);
        if (err != VK_SUCCESS) {
            vkFreeMemory(device_, staging_memory, allocator_);
            vkDestroyBuffer(device_, staging_buffer, allocator_);
            DestroyVulkanTexture(texture);
            return false;
        }

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(command_buffer, &begin_info);

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command_buffer,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        vkCmdCopyBufferToImage(command_buffer,
                               staging_buffer,
                               texture.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier);

        vkEndCommandBuffer(command_buffer);

        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;

        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fence_info = {};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(device_, &fence_info, allocator_, &fence);

        vkQueueSubmit(queue_, 1, &submit_info, fence);
        vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device_, fence, allocator_);

        vkFreeCommandBuffers(device_, texture_upload_command_pool_, 1, &command_buffer);
        vkFreeMemory(device_, staging_memory, allocator_);
        vkDestroyBuffer(device_, staging_buffer, allocator_);

        texture.width = width;
        texture.height = height;
        return true;
    }

    void DestroyVulkanTexture(LinuxVideoTexture &texture) {
        if (texture.descriptor_set != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(texture.descriptor_set);
        }
        if (texture.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, texture.view, allocator_);
        }
        if (texture.image != VK_NULL_HANDLE) {
            vkDestroyImage(device_, texture.image, allocator_);
        }
        if (texture.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, texture.memory, allocator_);
        }

        texture.image = VK_NULL_HANDLE;
        texture.view = VK_NULL_HANDLE;
        texture.memory = VK_NULL_HANDLE;
        texture.descriptor_set = VK_NULL_HANDLE;
        texture.width = 0;
        texture.height = 0;
        texture.ready = false;
    }

    void CreateVideoDescriptorSet(LinuxVideoTexture &texture) {
        texture.descriptor_set = ImGui_ImplVulkan_AddTexture(
                texture.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    [[nodiscard]] bool UploadDynamicTexture(LinuxDynamicTextureSet &texture_set, const RoboGo::app::VideoFrame &frame) {
        LinuxVideoTexture &next_texture = texture_set.textures[texture_set.next_index];
        if (next_texture.image != VK_NULL_HANDLE) {
            DestroyVulkanTexture(next_texture);
        }
        if (!CreateVulkanImage(frame.buffer.data(), frame.width, frame.height, next_texture)) {
            return false;
        }
        CreateVideoDescriptorSet(next_texture);
        next_texture.ready = true;
        texture_set.current_index = texture_set.next_index;
        texture_set.next_index = (texture_set.next_index + 1) % 2;
        next_texture.ready = false;
        return true;
    }

    [[nodiscard]] bool UploadStaticTexture(LinuxVideoTexture &texture,
                                           const uint8_t *data,
                                           const int width,
                                           const int height) {
        if (texture.image != VK_NULL_HANDLE) {
            DestroyVulkanTexture(texture);
        }
        if (!CreateVulkanImage(data, width, height, texture)) {
            return false;
        }
        CreateVideoDescriptorSet(texture);
        return true;
    }

    void DestroyDynamicTextureSet(LinuxDynamicTextureSet &texture_set) {
        for (auto &texture: texture_set.textures) {
            DestroyVulkanTexture(texture);
        }
    }

    [[nodiscard]] static RoboGo::app::TextureView BuildTextureView(const LinuxVideoTexture &texture) {
        RoboGo::app::TextureView texture_view;
        if (texture.descriptor_set != VK_NULL_HANDLE) {
            texture_view.texture_ref = ImTextureRef(reinterpret_cast<ImTextureID>(texture.descriptor_set));
        }
        texture_view.size = ImVec2(static_cast<float>(texture.width), static_cast<float>(texture.height));
        return texture_view;
    }

    VkAllocationCallbacks *allocator_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = static_cast<uint32_t>(-1);
    VkQueue queue_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkCommandPool texture_upload_command_pool_ = VK_NULL_HANDLE;
    VkSampler video_texture_sampler_ = VK_NULL_HANDLE;
#ifdef APP_USE_VULKAN_DEBUG_REPORT
    VkDebugReportCallbackEXT debug_report_ = VK_NULL_HANDLE;
#endif
    GLFWwindow *window_ = nullptr;
    ImGui_ImplVulkanH_Window main_window_data_ = {};
    uint32_t min_image_count_ = 2;
    bool swap_chain_rebuild_ = false;
    float main_scale_ = 1.0f;
    LinuxDynamicTextureSet udp_video_textures_;
    LinuxDynamicTextureSet custom_video_textures_;
    LinuxVideoTexture mini_map_texture_;
    bool initialized_ = false;
    bool game_input_captured_ = false;
};

}// namespace

namespace RoboGo::platform {

std::unique_ptr<PlatformBackend> CreatePlatformBackend() {
    return std::make_unique<LinuxPlatformBackend>();
}

}// namespace RoboGo::platform

#endif
