#include "render/vk_renderer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "render/shader_compiler.h"
#include "rsx/rsx_decode.h"
#include "rsx/rsx_unimplemented.h"

namespace dal {
namespace {

constexpr std::uint64_t kFenceTimeoutNs = 10'000'000'000ull;

bool formatHasStencil(VkFormat format) {
  return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

VkImageAspectFlags depthAspect(VkFormat format) {
  return formatHasStencil(format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                  : VK_IMAGE_ASPECT_DEPTH_BIT;
}

}  // namespace

struct RsxRenderer::RenderTarget {
  VkContext* context = nullptr;
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = nullptr;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool depth = false;
  // Layout at the end of the commands recorded so far (the frame's command buffer is submitted in order).
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;

  RenderTarget() = default;
  RenderTarget(const RenderTarget&) = delete;
  RenderTarget& operator=(const RenderTarget&) = delete;
  ~RenderTarget() {
    if (view != VK_NULL_HANDLE) vkDestroyImageView(context->device(), view, nullptr);
    if (image != VK_NULL_HANDLE) vmaDestroyImage(context->allocator(), image, allocation);
  }
};

struct RsxRenderer::FrameSlot {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;  // submitted and not yet waited for
  // Targets recorded into this slot's command buffer; kept alive until its fence has signaled, so a
  // target dropped from the cache by a resize outlives the commands that use it.
  std::vector<std::shared_ptr<RenderTarget>> in_use;
  // Host-visible copy of the display buffer written at the flip that submitted this slot.
  VkBuffer readback = VK_NULL_HANDLE;
  VmaAllocation readback_allocation = nullptr;
  void* readback_mapped = nullptr;
  VkDeviceSize readback_size = 0;
};

RsxRenderer::RsxRenderer(VkContext& context, RenderOptions options)
    : context_(context), options_(std::move(options)) {
  if (!options_.shader_cache.empty()) setShaderCacheDir(options_.shader_cache);
  const VkDevice device = context_.device();
  for (std::unique_ptr<FrameSlot>& slot_ptr : slots_) {
    slot_ptr = std::make_unique<FrameSlot>();
    FrameSlot& slot = *slot_ptr;

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = context_.graphicsQueueFamily();
    if (vkCreateCommandPool(device, &pool_info, nullptr, &slot.pool) != VK_SUCCESS) {
      fail("vkCreateCommandPool failed");
      return;
    }
    VkCommandBufferAllocateInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = slot.pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &cmd_info, &slot.cmd) != VK_SUCCESS) {
      fail("vkAllocateCommandBuffers failed");
      return;
    }
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device, &fence_info, nullptr, &slot.fence) != VK_SUCCESS) {
      fail("vkCreateFence failed");
      return;
    }
  }
}

RsxRenderer::~RsxRenderer() {
  const VkDevice device = context_.device();
  vkDeviceWaitIdle(device);
  for (std::unique_ptr<FrameSlot>& slot : slots_) {
    if (!slot) continue;
    slot->in_use.clear();
    if (slot->readback != VK_NULL_HANDLE) vmaDestroyBuffer(context_.allocator(), slot->readback, slot->readback_allocation);
    if (slot->fence != VK_NULL_HANDLE) vkDestroyFence(device, slot->fence, nullptr);
    if (slot->pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, slot->pool, nullptr);
  }
  color_targets_.clear();
  depth_targets_.clear();
}

void RsxRenderer::fail(std::string message) {
  if (error_.empty()) error_ = std::move(message);
}

bool RsxRenderer::waitSlot(FrameSlot& slot) {
  if (!slot.pending) return true;
  const VkResult result = vkWaitForFences(context_.device(), 1, &slot.fence, VK_TRUE, kFenceTimeoutNs);
  if (result != VK_SUCCESS) {
    fail(std::format("waiting for the GPU failed ({})", static_cast<int>(result)));
    return false;
  }
  slot.pending = false;
  slot.in_use.clear();
  return true;
}

bool RsxRenderer::beginFrame() {
  if (!valid()) return false;
  if (recording_) return true;
  FrameSlot& slot = *slots_[current_slot_];
  if (!waitSlot(slot)) return false;
  const VkDevice device = context_.device();
  vkResetFences(device, 1, &slot.fence);
  vkResetCommandPool(device, slot.pool, 0);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(slot.cmd, &begin) != VK_SUCCESS) {
    fail("vkBeginCommandBuffer failed");
    return false;
  }
  recording_ = true;
  return true;
}

bool RsxRenderer::submitFrame() {
  FrameSlot& slot = *slots_[current_slot_];
  if (vkEndCommandBuffer(slot.cmd) != VK_SUCCESS) {
    fail("vkEndCommandBuffer failed");
    return false;
  }
  VkCommandBufferSubmitInfo cmd_submit{};
  cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
  cmd_submit.commandBuffer = slot.cmd;
  VkSubmitInfo2 submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cmd_submit;
  if (vkQueueSubmit2(context_.graphicsQueue(), 1, &submit, slot.fence) != VK_SUCCESS) {
    fail("vkQueueSubmit2 failed");
    return false;
  }
  slot.pending = true;
  recording_ = false;
  display_slot_ = current_slot_;
  current_slot_ = (current_slot_ + 1) % kFramesInFlight;
  return true;
}

void RsxRenderer::touch(const std::shared_ptr<RenderTarget>& target) {
  std::vector<std::shared_ptr<RenderTarget>>& in_use = slots_[current_slot_]->in_use;
  if (std::find(in_use.begin(), in_use.end(), target) == in_use.end()) in_use.push_back(target);
}

// Conservative full barrier: the clears and the readback are not frequent enough to need tighter stages.
void RsxRenderer::transition(RenderTarget& target, VkImageLayout layout) {
  VkImageMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.oldLayout = target.layout;
  barrier.newLayout = layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = target.image;
  barrier.subresourceRange = {target.aspect, 0, 1, 0, 1};
  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(slots_[current_slot_]->cmd, &dependency);
  target.layout = layout;
}

// A target the cache has not seen holds no guest data (nothing is loaded from guest memory), so it
// starts defined instead of undefined: color is transparent black, depth is 1.0 with stencil 0xFF,
// the values RPCS3 gives the aspect a partial clear leaves untouched (VK/VKGSRender.cpp
// VKGSRender::clear_surface, the erase_bkgnd initialization).
std::shared_ptr<RsxRenderer::RenderTarget> RsxRenderer::createTarget(bool depth, const RsxSurface& surface,
                                                                     VkFormat format) {
  const VkPhysicalDeviceProperties& props = context_.deviceProperties();
  if (surface.width == 0 || surface.height == 0 || surface.width > props.limits.maxImageDimension2D ||
      surface.height > props.limits.maxImageDimension2D) {
    rsxNoteUnimplemented("state", std::format("surface size {}x{}", surface.width, surface.height));
    return nullptr;
  }

  VkFormatProperties format_props{};
  vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &format_props);
  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  if ((format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0) {
    usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  }

  auto target = std::make_shared<RenderTarget>();
  target->context = &context_;
  target->format = format;
  target->aspect = depth ? depthAspect(format) : VK_IMAGE_ASPECT_COLOR_BIT;
  target->width = surface.width;
  target->height = surface.height;
  target->depth = depth;

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {surface.width, surface.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = usage;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  alloc_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
  if (vmaCreateImage(context_.allocator(), &image_info, &alloc_info, &target->image, &target->allocation, nullptr) !=
      VK_SUCCESS) {
    target->image = VK_NULL_HANDLE;
    fail("vmaCreateImage failed for a render target");
    return nullptr;
  }

  VkImageViewCreateInfo view_info{};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = target->image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = {target->aspect, 0, 1, 0, 1};
  if (vkCreateImageView(context_.device(), &view_info, nullptr, &target->view) != VK_SUCCESS) {
    target->view = VK_NULL_HANDLE;
    fail("vkCreateImageView failed for a render target");
    return nullptr;
  }

  transition(*target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkCommandBuffer cmd = slots_[current_slot_]->cmd;
  const VkImageSubresourceRange range = {target->aspect, 0, 1, 0, 1};
  if (depth) {
    VkClearDepthStencilValue value{};
    value.depth = 1.0f;
    value.stencil = 0xFF;
    vkCmdClearDepthStencilImage(cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
  } else {
    const VkClearColorValue value{};
    vkCmdClearColorImage(cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
  }
  return target;
}

// One entry per (location, offset) and kind. Another size or format at the same offset replaces the
// entry; the old target stays alive for the commands already recorded against it.
std::shared_ptr<RsxRenderer::RenderTarget> RsxRenderer::acquire(TargetMap& map, bool depth, const RsxSurface& surface,
                                                                VkFormat format) {
  const TargetKey key{surface.location, surface.offset};
  const auto it = map.find(key);
  if (it != map.end()) {
    const std::shared_ptr<RenderTarget>& existing = it->second;
    if (existing->width == surface.width && existing->height == surface.height && existing->format == format) {
      touch(existing);
      return existing;
    }
    rsxNoteUnimplemented("state", "surface resize");
    map.erase(it);
  }
  std::shared_ptr<RenderTarget> target = createTarget(depth, surface, format);
  if (!target) return nullptr;
  map[key] = target;
  touch(target);
  return target;
}

std::shared_ptr<RsxRenderer::RenderTarget> RsxRenderer::acquireColor(const RsxSurface& surface) {
  if (surface.format != RsxColorFormat::A8R8G8B8) return nullptr;
  return acquire(color_targets_, false, surface, VK_FORMAT_B8G8R8A8_UNORM);
}

std::shared_ptr<RsxRenderer::RenderTarget> RsxRenderer::acquireDepth(const RsxSurfaceState& surfaces) {
  switch (surfaces.depth_format) {
    case RsxDepthFormat::Z24S8:
      return acquire(depth_targets_, true, surfaces.depth, context_.depthStencilFormat());
    case RsxDepthFormat::Z16:
      return acquire(depth_targets_, true, surfaces.depth, VK_FORMAT_D16_UNORM);
    default:
      return nullptr;
  }
}

// Draws are not rendered yet, but their targets are created so a flip of one reads back as a render
// target (zero-filled) rather than as a missing one.
void RsxRenderer::resolveDrawTargets(const RsxSurfaceState& surfaces) {
  if (surfaces.has_color) acquireColor(surfaces.color);
  acquireDepth(surfaces);
}

// Facts from RPCS3 `VK/VKGSRender.cpp` `VKGSRender::clear_surface`:
//  - one vkCmdClearAttachments over the clipped scissor rectangle;
//  - color clear values are each 8-bit channel / 255, with alpha in the top byte of the register
//    (`rsx_decode.h` `registers_decoder<NV4097_SET_COLOR_CLEAR_VALUE>`);
//  - the depth value is the register field divided by the format's maximum, 0xFFFF for 16-bit depth
//    and 0xFFFFFF otherwise (`Common/TextureUtils.cpp` `get_max_depth_value`); Z24S8 keeps depth in
//    bits 8..31 and stencil in bits 0..7, Z16 uses bits 0..15 (`rsx_decode.h`
//    `registers_decoder<NV4097_SET_ZSTENCIL_CLEAR_VALUE>`);
//  - the aspects cleared follow the mask bits;
//  - a color mask other than all four channels needs a shader pass, as does a stencil write mask
//    other than 0xFF. Neither is modelled: a partial color clear is skipped and counted (the frame
//    builder counts a partial stencil mask); the clear carries no stencil write mask, so the
//    stencil is cleared in full.
void RsxRenderer::recordClear(const RsxClear& clear) {
  stats_.clears++;
  constexpr std::uint32_t kColorBits = kRsxClearRed | kRsxClearGreen | kRsxClearBlue | kRsxClearAlpha;
  const std::uint32_t color_bits = clear.mask & kColorBits;
  if (color_bits != 0 && color_bits != kColorBits) rsxNoteUnimplemented("state", "partial color clear mask");

  std::shared_ptr<RenderTarget> color;
  if (color_bits == kColorBits && clear.surfaces.has_color) color = acquireColor(clear.surfaces.color);
  std::shared_ptr<RenderTarget> depth;
  if ((clear.mask & (kRsxClearDepth | kRsxClearStencil)) != 0) depth = acquireDepth(clear.surfaces);
  if (!color && !depth) return;

  const VkCommandBuffer cmd = slots_[current_slot_]->cmd;
  std::uint32_t area_width = color ? color->width : depth->width;
  std::uint32_t area_height = color ? color->height : depth->height;
  if (color && depth) {
    area_width = std::min(color->width, depth->width);
    area_height = std::min(color->height, depth->height);
  }

  VkRenderingAttachmentInfo color_attachment{};
  VkRenderingAttachmentInfo depth_attachment{};
  VkRenderingAttachmentInfo stencil_attachment{};
  if (color) {
    transition(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = color->view;
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  if (depth) {
    transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = depth->view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    stencil_attachment = depth_attachment;
  }

  VkRenderingInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering.renderArea = {{0, 0}, {area_width, area_height}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = color ? 1 : 0;
  rendering.pColorAttachments = color ? &color_attachment : nullptr;
  rendering.pDepthAttachment = depth ? &depth_attachment : nullptr;
  rendering.pStencilAttachment = depth && formatHasStencil(depth->format) ? &stencil_attachment : nullptr;

  VkClearAttachment attachments[2]{};
  std::uint32_t attachment_count = 0;
  if (color) {
    const std::uint32_t argb = clear.color_argb;
    VkClearAttachment& a = attachments[attachment_count++];
    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    a.colorAttachment = 0;
    a.clearValue.color.float32[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
    a.clearValue.color.float32[1] = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
    a.clearValue.color.float32[2] = static_cast<float>(argb & 0xFF) / 255.0f;
    a.clearValue.color.float32[3] = static_cast<float>(argb >> 24) / 255.0f;
  }
  if (depth) {
    VkImageAspectFlags aspect = 0;
    VkClearDepthStencilValue value{};
    if ((clear.mask & kRsxClearDepth) != 0) {
      aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
      const bool z24 = clear.surfaces.depth_format == RsxDepthFormat::Z24S8;
      const std::uint32_t raw = z24 ? (clear.depth_stencil >> 8) : (clear.depth_stencil & 0xFFFF);
      value.depth = static_cast<float>(raw) / (z24 ? 16777215.0f : 65535.0f);
    }
    if ((clear.mask & kRsxClearStencil) != 0 && formatHasStencil(depth->format)) {
      aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
      value.stencil = clear.depth_stencil & 0xFF;
    }
    if (aspect != 0) {
      VkClearAttachment& a = attachments[attachment_count++];
      a.aspectMask = aspect;
      a.clearValue.depthStencil = value;
    }
  }

  // The builder clipped the rectangle to the clip size, which is also the target size; the clamp only
  // guards the attachment bounds.
  const std::uint32_t x = std::min(clear.scissor[0], area_width);
  const std::uint32_t y = std::min(clear.scissor[1], area_height);
  VkClearRect rect{};
  rect.rect.offset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
  rect.rect.extent = {std::min(clear.scissor[2], area_width - x), std::min(clear.scissor[3], area_height - y)};
  rect.layerCount = 1;

  vkCmdBeginRendering(cmd, &rendering);
  if (attachment_count != 0 && rect.rect.extent.width != 0 && rect.rect.extent.height != 0) {
    vkCmdClearAttachments(cmd, attachment_count, attachments, 1, &rect);
  }
  vkCmdEndRendering(cmd);
}

bool RsxRenderer::ensureReadback(FrameSlot& slot, VkDeviceSize size) {
  if (slot.readback != VK_NULL_HANDLE && slot.readback_size >= size) return true;
  if (slot.readback != VK_NULL_HANDLE) {
    vmaDestroyBuffer(context_.allocator(), slot.readback, slot.readback_allocation);
    slot.readback = VK_NULL_HANDLE;
    slot.readback_size = 0;
  }
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo alloc_info{};
  alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
  alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo mapped{};
  if (vmaCreateBuffer(context_.allocator(), &buffer_info, &alloc_info, &slot.readback, &slot.readback_allocation,
                      &mapped) != VK_SUCCESS ||
      mapped.pMappedData == nullptr) {
    slot.readback = VK_NULL_HANDLE;
    fail("vmaCreateBuffer failed for the display readback");
    return false;
  }
  slot.readback_mapped = mapped.pMappedData;
  slot.readback_size = size;
  return true;
}

// The display buffer is the color target at (location 0, offset). Its pixels are copied into the
// slot's readback buffer here, inside the frame's command buffer, so readDisplay needs no further
// GPU work. A buffer that was never a render target reads back as black.
void RsxRenderer::recordFlip(const RsxFlip& flip) {
  stats_.flips++;
  FrameSlot& slot = *slots_[current_slot_];
  const auto it = color_targets_.find(TargetKey{0, flip.offset});
  display_black_ = it == color_targets_.end();
  if (display_black_) {
    rsxNoteUnimplemented("state", "flip of non-render-target");
    display_width_ = flip.width;
    display_height_ = flip.height;
  } else {
    const std::shared_ptr<RenderTarget> target = it->second;
    touch(target);
    display_width_ = target->width;
    display_height_ = target->height;
    if (ensureReadback(slot, static_cast<VkDeviceSize>(display_width_) * display_height_ * 4)) {
      transition(*target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {display_width_, display_height_, 1};
      vkCmdCopyImageToBuffer(slot.cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.readback, 1, &region);
      VkMemoryBarrier2 host_barrier{};
      host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
      host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
      host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
      VkDependencyInfo dependency{};
      dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      dependency.memoryBarrierCount = 1;
      dependency.pMemoryBarriers = &host_barrier;
      vkCmdPipelineBarrier2(slot.cmd, &dependency);
    }
  }
  if (!valid()) return;
  if (submitFrame()) has_flip_ = true;
}

void RsxRenderer::execute(std::span<RsxCommand> commands) {
  for (RsxCommand& command : commands) {
    if (!beginFrame()) return;
    if (const auto* clear = std::get_if<RsxClear>(&command)) {
      recordClear(*clear);
    } else if (const auto* draw = std::get_if<RsxDraw>(&command)) {
      stats_.draws_skipped++;
      resolveDrawTargets(draw->surfaces);
    } else {
      recordFlip(std::get<RsxFlip>(command));
    }
    if (!valid()) return;
  }
}

bool RsxRenderer::readDisplay(std::vector<std::uint8_t>& bgra, std::uint32_t& width, std::uint32_t& height) {
  if (!has_flip_ || !valid() || display_width_ == 0 || display_height_ == 0) return false;
  width = display_width_;
  height = display_height_;
  const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
  if (display_black_) {
    bgra.assign(bytes, 0);
    for (std::size_t i = 3; i < bytes; i += 4) bgra[i] = 0xFF;
    return true;
  }
  FrameSlot& slot = *slots_[display_slot_];
  if (!waitSlot(slot)) return false;
  vmaInvalidateAllocation(context_.allocator(), slot.readback_allocation, 0, VK_WHOLE_SIZE);
  bgra.resize(bytes);
  std::memcpy(bgra.data(), slot.readback_mapped, bytes);
  return true;
}

}  // namespace dal
