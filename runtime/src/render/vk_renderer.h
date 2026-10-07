#pragma once

// RSX command executor on Vulkan: render target cache, clears, draws with textures and flips. A sampled
// unit without a usable texture reads a 1x1 magenta placeholder.

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "render/fp_translator.h"
#include "render/vk_context.h"
#include "render/vk_pipelines.h"
#include "render/vk_textures.h"
#include "rsx/rsx_frame.h"

namespace dal {

struct RenderOptions {
  std::filesystem::path dump_dir;
  std::uint32_t dump_every = 1;
  std::filesystem::path shader_cache;
};

struct RendererStats {
  std::uint64_t clears = 0;         // clears recorded (a clear whose mask is not modelled still counts)
  std::uint64_t draws = 0;          // draws handled: recorded into a command buffer, or dropped with an empty scissor
  std::uint64_t draws_skipped = 0;  // draws seen but not rendered (unimplemented state, program or primitive)
  std::uint64_t flips = 0;
  std::uint64_t texture_uploads = 0;  // texture images written from guest bytes (once per image per draw)
  std::uint64_t feedback_copies = 0;  // render target copies made for draws that sample their own target
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
  struct UploadRef;
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
  void recordFlip(const RsxFlip& flip);
  bool ensureReadback(FrameSlot& slot, VkDeviceSize size);

  VkContext& context_;
  RenderOptions options_;
  std::string error_;
  RendererStats stats_;

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

  bool has_flip_ = false;
  bool display_black_ = false;
  std::size_t display_slot_ = 0;
  std::uint32_t display_width_ = 0;
  std::uint32_t display_height_ = 0;
};

}  // namespace dal
