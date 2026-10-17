#include "overlay/debug_overlay.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "core/log.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "overlay/overlay_panels.h"
#include "overlay/overlay_platform.h"
#include "render/vk_renderer.h"

namespace dal {

namespace {

constexpr float kBaseFontSize = 13.0f;
constexpr float kMinDeltaTime = 1e-4f;
constexpr float kMaxDeltaTime = 0.1f;
constexpr float kFirstDeltaTime = 1.0f / 60.0f;
// Only the font atlas needs a descriptor; the rest is room for textures a later panel may register.
constexpr std::uint32_t kDescriptorCount = 4;

// The backend reports failures through a plain function pointer without user data. One overlay exists
// at a time, so a global is enough.
std::atomic<VkResult> g_first_vk_error{VK_SUCCESS};

void recordVkResult(VkResult result) {
    if (result >= VK_SUCCESS) return;
    VkResult expected = VK_SUCCESS;
    g_first_vk_error.compare_exchange_strong(expected, result);
}

}  // namespace

DebugOverlay::DebugOverlay(VkContext& context, OverlaySources sources)
    : context_(context), sources_(std::move(sources)) {}

DebugOverlay::~DebugOverlay() {
    if (imgui_ != nullptr || descriptor_pool_ != VK_NULL_HANDLE) vkDeviceWaitIdle(context_.device());
    shutdown();
}

void DebugOverlay::attachWindow(void* native_window) { native_window_.store(native_window, std::memory_order_release); }

void DebugOverlay::setVisible(bool visible) {
    if (visible_.exchange(visible) == visible || !initialized_.load(std::memory_order_acquire)) return;
    // ImGui keeps the focus state of the last focus event. Hiding drops held keys and mouse buttons;
    // showing has to restore the focus, or ImGui would clear its input every frame.
    std::lock_guard lock(mutex_);
    ImGui::GetIO().AddFocusEvent(visible);
}

MessageRoute DebugOverlay::onWindowMessage(const WindowMessage& message) {
    MessageRoute route;
    // A key-up is not delivered once the window has lost the focus, so the hold would stay on.
    if (overlay_platform::isFocusLoss(message) && sources_.set_fast_forward_held) sources_.set_fast_forward_held(false);

    const std::optional<KeyMessage> key = overlay_platform::keyOf(message);
    if (key && key->scan_code == kOverlayToggleScanCode) {
        if (key->down && !key->repeat) setVisible(!visible());
        route.withhold_key = true;
        return route;
    }
    if (key && key->scan_code == kFastForwardHoldScanCode) {
        if (sources_.set_fast_forward_held && !key->repeat) sources_.set_fast_forward_held(key->down);
        route.withhold_key = true;
        return route;
    }

    if (!visible() || !initialized_.load(std::memory_order_acquire) ||
        native_window_.load(std::memory_order_acquire) == nullptr) {
        return route;
    }

    // The platform backend may need the thread that owns the window, so the input is fed from here and not
    // queued to the renderer's thread.
    std::lock_guard lock(mutex_);
    overlay_platform::forwardMessage(message, route);
    // Key-ups always reach the pad, so a key that went down before the overlay took the focus is released.
    if (overlay_platform::isKeyDown(message)) route.withhold_key = ImGui::GetIO().WantCaptureKeyboard;
    return route;
}

PresentOverlayRequests DebugOverlay::onFlip(const PresentFrameInfo& info) {
    if (info.stats != nullptr) state_.frames.onFlip(*info.stats, FrameStatsHistory::Clock::now());
    state_.dumps = info.dumps;
    state_.last_dump = info.last_requested_dump;

    PresentOverlayRequests requests;
    requests.dump_display = state_.dump_requested;
    state_.dump_requested = false;
    return requests;
}

void DebugOverlay::fail(const char* what) {
    failed_.store(true, std::memory_order_relaxed);
    logWarn("dal: overlay: {} failed, overlay disabled", what);
    shutdown();
}

void DebugOverlay::shutdown() {
    std::lock_guard lock(mutex_);
    initialized_.store(false, std::memory_order_release);
    if (imgui_ != nullptr) ImGui::SetCurrentContext(imgui_);
    if (vulkan_backend_) ImGui_ImplVulkan_Shutdown();
    if (platform_backend_) overlay_platform::shutdownBackend();
    vulkan_backend_ = false;
    platform_backend_ = false;
    if (imgui_ != nullptr) {
        ImGui::DestroyContext(imgui_);
        imgui_ = nullptr;
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(context_.device(), descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
}

bool DebugOverlay::initialize(const Presenter::Target& target) {
    void* native_window = native_window_.load(std::memory_order_acquire);
    color_format_ = target.format;
    g_first_vk_error.store(VK_SUCCESS);

    // Nothing here is shared with the window thread yet: it forwards messages only once initialized_ is set.
    IMGUI_CHECKVERSION();
    imgui_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // The window class keeps the arrow cursor; the backend must not change it.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();

    const float scale = overlay_platform::windowScale(native_window);
    scale_ = scale;
    ImFontConfig font_config;
    font_config.SizePixels = std::round(kBaseFontSize * scale);
    io.Fonts->AddFontDefault(&font_config);
    if (overlay_platform::usesPointCoordinates()) {
        // Layout stays in points; the atlas is rasterised at the framebuffer's pixel density.
        io.FontGlobalScale = 1.0f / scale;
    } else {
        ImGui::GetStyle().ScaleAllSizes(scale);
    }

    if (native_window != nullptr) {
        if (!overlay_platform::initBackend(native_window)) {
            fail("window backend");
            return false;
        }
        platform_backend_ = true;
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = kDescriptorCount;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = kDescriptorCount;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(context_.device(), &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        descriptor_pool_ = VK_NULL_HANDLE;
        fail("descriptor pool");
        return false;
    }

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_3;
    init.Instance = context_.instance();
    init.PhysicalDevice = context_.physicalDevice();
    init.Device = context_.device();
    init.QueueFamily = context_.graphicsQueueFamily();
    init.Queue = context_.graphicsQueue();
    init.DescriptorPool = descriptor_pool_;
    init.MinImageCount = 2;
    // The backend reuses a vertex buffer every ImageCount draws. The renderer has waited a slot's fence
    // after kFramesInFlight frames, so one more is margin.
    init.ImageCount = static_cast<std::uint32_t>(RsxRenderer::kFramesInFlight + 1);
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.UseDynamicRendering = true;
    init.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format_;
    init.CheckVkResultFn = &recordVkResult;
    if (!ImGui_ImplVulkan_Init(&init)) {
        fail("Vulkan backend");
        return false;
    }
    vulkan_backend_ = true;

    // One queue submit and a wait: the one-time hitch when the overlay first shows.
    if (!ImGui_ImplVulkan_CreateFontsTexture() || g_first_vk_error.load() != VK_SUCCESS) {
        fail("font upload");
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        initialized_.store(true, std::memory_order_release);
    }
    logInfo("dal: overlay: ready (imgui {}, scale {})", IMGUI_VERSION, scale);
    return true;
}

void DebugOverlay::record(VkCommandBuffer cmd, const Presenter::Target& target) {
    if (failed()) return;
    if (!initialized_.load(std::memory_order_acquire)) {
        if (target.view == VK_NULL_HANDLE || !initialize(target)) return;
    } else if (target.format != color_format_) {
        logOnce("overlay-format", "dal: overlay: surface format changed, overlay skipped for that image");
        return;
    }

    const auto now = FrameStatsHistory::Clock::now();
    state_.refresh(sources_, now);

    std::lock_guard lock(mutex_);
    ImGuiIO& io = ImGui::GetIO();
    if (overlay_platform::usesPointCoordinates()) {
        io.DisplaySize = ImVec2(static_cast<float>(target.width) / scale_, static_cast<float>(target.height) / scale_);
        io.DisplayFramebufferScale = ImVec2(scale_, scale_);
    } else {
        io.DisplaySize = ImVec2(static_cast<float>(target.width), static_cast<float>(target.height));
    }
    // The platform backend's NewFrame is not used: the Win32 one's key workaround calls GetKeyState on the
    // calling thread, which would report every key up here and release Shift and Ctrl each frame.
    io.DeltaTime = last_frame_time_.time_since_epoch().count() == 0
                           ? kFirstDeltaTime
                           : std::clamp(std::chrono::duration<float>(now - last_frame_time_).count(), kMinDeltaTime,
                                        kMaxDeltaTime);
    last_frame_time_ = now;

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    drawOverlayWindow(state_, sources_);
    ImGui::Render();

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {target.width, target.height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

}  // namespace dal
