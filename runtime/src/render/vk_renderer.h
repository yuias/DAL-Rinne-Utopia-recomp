#pragma once

// RSX command executor on Vulkan: render target cache, clears, draws with textures and flips. A sampled
// unit without a usable texture reads a 1x1 magenta placeholder.

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "render/fp_translator.h"
#include "render/frame_dump.h"
#include "render/present_overlay.h"
#include "render/vk_context.h"
#include "render/vk_pipelines.h"
#include "render/vk_textures.h"
#include "render/vk_window.h"
#include "rsx/rsx_frame.h"

namespace dal {

struct RenderOptions {
    // Every dump_every-th flip (counted from 1) is written to `<dump_dir>/frame_<flip number>.png`. An
    // empty directory disables dumping.
    std::filesystem::path dump_dir;
    std::uint32_t dump_every = 1;
    // Where requestDisplayDump() writes `frame_<flip number>.png`, unless dump_dir already dumps that flip.
    std::filesystem::path requested_dump_dir = "generated/frames";
    std::filesystem::path shader_cache;
    // Copy the display buffer back at every flip so readDisplay works after each one (replay, tests). The
    // copy is 8 MiB at 1080p, so a live run leaves this off and only reads back the flips that are dumped.
    bool readback_all = false;
};

struct RendererStats {
    std::uint64_t clears = 0;         // clears recorded (a clear whose mask is not modelled still counts)
    std::uint64_t draws = 0;          // draws handled: recorded into a command buffer, or dropped with an empty scissor
    std::uint64_t draws_skipped = 0;  // draws seen but not rendered (unimplemented state, program or primitive)
    std::uint64_t flips = 0;
    std::uint64_t texture_uploads = 0;  // texture images written from guest bytes (once per new content id per image)
    std::uint64_t feedback_copies = 0;  // render target copies made for draws that sample their own target
    std::uint64_t target_copies = 0;    // render target images copied for RsxCopy
};

// Single-threaded: execute, readDisplay and stats are called from one thread. The renderer never
// touches guest memory; every command carries the bytes it needs.
class RsxRenderer {
public:
    RsxRenderer(VkContext& context, RenderOptions options);
    ~RsxRenderer();
    RsxRenderer(const RsxRenderer&) = delete;
    RsxRenderer& operator=(const RsxRenderer&) = delete;

    // False when construction or a later Vulkan call failed; `error()` says why. Commands are
    // ignored afterwards.
    bool valid() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    // Records into the frame's command buffer; a flip submits it.
    void execute(std::span<RsxCommand> commands);

    // BGRA8 rows of the display buffer of the last flip (waits for the GPU). A flip whose buffer is
    // not a known render target reads back as opaque black at the flip's size. False (outputs untouched)
    // before any flip, and when the last flip was gated off: without `readback_all` the pixels are only
    // copied for flips that are dumped, and a dump due at that flip reads them itself.
    bool readDisplay(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height);

    const RendererStats& stats() const { return stats_; }

    // Frames written, failed or dropped by the dump worker so far (dump_dir and requested dumps). Any thread.
    DumpCounts dumpCounts() const;
    // Waits until the dumps queued so far are written, at most `timeout`; returns how many are still
    // pending (0 when all are done). Any thread.
    std::uint64_t flushDumps(std::chrono::milliseconds timeout);

    // The next flip is read back and written as `frame_<flip number>.png`: into dump_dir when that flip is
    // dumped anyway (one file, no duplicate), else into requested_dump_dir. Renderer thread.
    void requestDisplayDump() { dump_requested_ = true; }

    // Drawn over the presented image after the blit; only used while a presenter is set. Not owned; call
    // before the first flip.
    void setPresentOverlay(PresentOverlay* overlay) { overlay_ = overlay; }

    // Every flip is also blitted to the presenter's next swapchain image (letterboxed 16:9) and presented.
    // Not owned; null (the default) presents nothing. Call before the first flip.
    void setPresenter(Presenter* presenter) {
        if (presenter != nullptr && presenter->frameSlots() < kFramesInFlight) {
            fail("the presenter has fewer frame slots than the renderer");
            return;
        }
        presenter_ = presenter;
    }

    using Clock = std::chrono::steady_clock;

    // Frames recorded before a slot is reused; a presenter needs one acquire semaphore per slot.
    static constexpr std::size_t kFramesInFlight = 2;
    // The game counts as stopped when no real flip happened for this long. Above any normal frame interval (a 30 fps
    // game flips every 33 ms), so idle presents never interleave with its own flips.
    static constexpr std::chrono::milliseconds kIdleStallThreshold{100};
    // Once the game has stopped, the minimum time between idle presents (about 30 per second).
    static constexpr std::chrono::milliseconds kIdlePresentInterval{33};

    // True when no real flip happened for kIdleStallThreshold (measured from `last_flip`, not from the last idle
    // present) and the last idle present is at least kIdlePresentInterval ago. An idle present older than the last
    // flip is then older than the threshold too, so it never holds the next one back.
    static bool idlePresentDue(Clock::time_point last_flip, Clock::time_point last_idle_present,
                               Clock::time_point now) {
        return now - last_flip >= kIdleStallThreshold && now - last_idle_present >= kIdlePresentInterval;
    }

    // Re-presents the last flip's image with the overlays while an overlay wants it (wantsIdlePresent()) and the game
    // has stopped flipping, so e.g. a system dialog stays visible and animated. Does nothing without a presenter and
    // overlay, before the first flip, while a frame is open (a draw or clear was recorded since the last flip: the
    // game is mid-frame and its next flip presents), or before idlePresentDue(). It is not a flip: no onFlip, no
    // stats_.flips, no readback or dump; readDisplay keeps reading the last real flip. Renderer thread, between
    // commands.
    void presentIdle();

private:
    struct RenderTarget;
    struct FrameSlot;
    struct UploadRef;
    using TargetKey = std::pair<std::uint32_t, std::uint32_t>;  // location, offset
    using TargetMap = std::map<TargetKey, std::shared_ptr<RenderTarget>>;

    void fail(std::string message);
    bool beginFrame();
    bool waitSlot(FrameSlot& slot);
    bool submitFrame();

    std::shared_ptr<RenderTarget> acquireColor(const RsxSurface& surface);
    std::shared_ptr<RenderTarget> acquireDepth(const RsxSurfaceState& surfaces);
    std::shared_ptr<RenderTarget> acquire(TargetMap& map, bool depth, const RsxSurface& surface, VkFormat format);
    std::shared_ptr<RenderTarget> createTarget(bool depth, const RsxSurface& surface, VkFormat format);
    void touch(const std::shared_ptr<RenderTarget>& target);
    void transition(RenderTarget& target, VkImageLayout layout);

    // Host-visible memory of the current frame slot for vertex, index and uniform data, valid until the
    // slot's fence has signaled. False (and fail()) when the allocation fails.
    bool allocUpload(std::size_t size, UploadRef& out);
    bool ensurePlaceholder();
    bool prepareTextures(const RsxDraw& draw, const DrawShaders& shaders, RenderTarget& color,
                         VkDescriptorImageInfo (&infos)[kFpTextureUnitCount], float (&scales)[kFpTextureUnitCount][4]);
    bool vertexFormatSupported(VkFormat format);

    void recordClear(const RsxClear& clear);
    void recordMaskedColorClear(const RsxClear& clear, RenderTarget& color, std::uint32_t color_bits);
    void recordDraw(const RsxDraw& draw);
    void recordCopy(const RsxCopy& copy);
    void recordFlip(const RsxFlip& flip);
    void recordPresentBlit(FrameSlot& slot, RenderTarget* source);
    bool ensureReadback(FrameSlot& slot, VkDeviceSize size);
    bool dumpDue() const;
    bool dumpDisplay(const std::filesystem::path& path);

    VkContext& context_;
    RenderOptions options_;
    std::string error_;
    RendererStats stats_;
    // Always created: its worker thread only starts with the first submitted frame.
    std::unique_ptr<FrameDumper> dumper_;
    bool dump_requested_ = false;  // requestDisplayDump() was called and no flip has taken it yet
    bool dump_this_flip_ = false;  // the flip being recorded was requested
    std::filesystem::path last_requested_dump_;

    // Color and depth targets live in separate maps: a surface of each kind may share an offset.
    TargetMap color_targets_;
    TargetMap depth_targets_;

    std::unique_ptr<DrawPipelines> pipelines_;
    std::unique_ptr<TextureCache> textures_;
    std::map<VkFormat, bool> vertex_format_support_;

    // Bound to a sampled texture unit that has no usable texture.
    VkImage placeholder_image_ = VK_NULL_HANDLE;
    VmaAllocation placeholder_allocation_ = nullptr;
    VkImageView placeholder_view_ = VK_NULL_HANDLE;
    VkSampler placeholder_sampler_ = VK_NULL_HANDLE;
    bool placeholder_ready_ = false;

    std::array<std::unique_ptr<FrameSlot>, kFramesInFlight> slots_;
    std::size_t current_slot_ = 0;
    bool recording_ = false;

    Presenter* presenter_ = nullptr;
    Presenter::Target present_target_;  // acquired for the frame being recorded
    bool present_pending_ = false;      // present_target_ is valid and its submit has not been queued yet
    PresentOverlay* overlay_ = nullptr;

    bool has_flip_ = false;
    // The target the last flip showed, for presentIdle(); null when that flip was black. Held so the image stays alive
    // when the target map drops or replaces its entry. presentIdle() reads it only while no command was recorded since
    // that flip, so its contents are still what the flip showed.
    std::shared_ptr<RenderTarget> last_display_;
    Clock::time_point last_flip_{};          // the last real flip
    Clock::time_point last_idle_present_{};  // the last idle present
    bool display_black_ = false;
    bool display_copied_ = false;  // the last flip's pixels are in its slot's readback buffer
    std::size_t display_slot_ = 0;
    std::uint32_t display_width_ = 0;
    std::uint32_t display_height_ = 0;
};

}  // namespace dal
