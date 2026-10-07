#pragma once

// RSX command executor on Vulkan: render target cache, clears and flips. Draws are resolved to their
// render targets and counted as skipped until the draw path exists.

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "render/vk_context.h"
#include "rsx/rsx_frame.h"

namespace dal {

struct RenderOptions {
  std::filesystem::path dump_dir;
  std::uint32_t dump_every = 1;
  std::filesystem::path shader_cache;
};

struct RendererStats {
  std::uint64_t clears = 0;         // clears recorded (a clear whose mask is not modelled still counts)
  std::uint64_t draws_skipped = 0;  // draws seen; none is rendered yet
  std::uint64_t flips = 0;
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
  // not a known render target reads back as opaque black at the flip's size. False before any flip.
  bool readDisplay(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height);

  const RendererStats& stats() const { return stats_; }

 private:
  struct RenderTarget;
  struct FrameSlot;
  using TargetKey = std::pair<std::uint32_t, std::uint32_t>;  // location, offset
  using TargetMap = std::map<TargetKey, std::shared_ptr<RenderTarget>>;

  static constexpr std::size_t kFramesInFlight = 2;

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
  void resolveDrawTargets(const RsxSurfaceState& surfaces);

  void recordClear(const RsxClear& clear);
  void recordFlip(const RsxFlip& flip);
  bool ensureReadback(FrameSlot& slot, VkDeviceSize size);

  VkContext& context_;
  RenderOptions options_;
  std::string error_;
  RendererStats stats_;

  // Color and depth targets live in separate maps: a surface of each kind may share an offset.
  TargetMap color_targets_;
  TargetMap depth_targets_;

  std::array<std::unique_ptr<FrameSlot>, kFramesInFlight> slots_;
  std::size_t current_slot_ = 0;
  bool recording_ = false;

  bool has_flip_ = false;
  bool display_black_ = false;
  std::size_t display_slot_ = 0;
  std::uint32_t display_width_ = 0;
  std::uint32_t display_height_ = 0;
};

}  // namespace dal
