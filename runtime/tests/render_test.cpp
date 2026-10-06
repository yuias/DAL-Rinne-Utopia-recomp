// Self-checking tests for the Vulkan renderer library. They skip when no Vulkan device exists.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "render/vk_context.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (cond) {                                                              \
      g_passed++;                                                            \
    } else {                                                                 \
      g_failed++;                                                            \
      std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
    }                                                                        \
  } while (0)

// Informational only: which static shaderc libraries the SDK provides.
void listShadercLibraries() {
  char* sdk = nullptr;
  std::size_t sdk_length = 0;
  _dupenv_s(&sdk, &sdk_length, "VULKAN_SDK");
  if (sdk == nullptr) {
    std::printf("shaderc: VULKAN_SDK is not set\n");
    return;
  }
  const std::filesystem::path lib_dir = std::filesystem::path(sdk) / "Lib";
  std::free(sdk);
  std::error_code ec;
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(lib_dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.starts_with("shaderc_combined") && name.ends_with(".lib")) names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  std::printf("shaderc: %zu static libraries in the SDK\n", names.size());
  for (const std::string& name : names) std::printf("shaderc:   %s\n", name.c_str());
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkPipelineStageFlags2 src_stage,
                  VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                  VkAccessFlags2 dst_access, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  barrier.srcStageMask = src_stage;
  barrier.srcAccessMask = src_access;
  barrier.dstStageMask = dst_stage;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{};
  dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
}

// Clears a 4x4 B8G8R8A8 image to ARGB 0xFF102030 and reads it back through a host buffer.
void testClearAndReadback(dal::VkContext& ctx) {
  constexpr std::uint32_t kSize = 4;
  constexpr VkDeviceSize kBytes = kSize * kSize * 4;
  VkDevice device = ctx.device();

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
  image_info.extent = {kSize, kSize, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo image_alloc_info{};
  image_alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation image_alloc = nullptr;
  CHECK(vmaCreateImage(ctx.allocator(), &image_info, &image_alloc_info, &image, &image_alloc,
                       nullptr) == VK_SUCCESS);

  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = kBytes;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo buffer_alloc_info{};
  buffer_alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
  buffer_alloc_info.flags =
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation buffer_alloc = nullptr;
  VmaAllocationInfo buffer_mapped{};
  CHECK(vmaCreateBuffer(ctx.allocator(), &buffer_info, &buffer_alloc_info, &buffer, &buffer_alloc,
                        &buffer_mapped) == VK_SUCCESS);

  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = ctx.graphicsQueueFamily();
  CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS);

  VkFence fence = VK_NULL_HANDLE;
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  CHECK(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS);

  const bool ready = image != VK_NULL_HANDLE && buffer != VK_NULL_HANDLE &&
                     pool != VK_NULL_HANDLE && fence != VK_NULL_HANDLE &&
                     buffer_mapped.pMappedData != nullptr;
  if (ready) {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd) == VK_SUCCESS);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue clear{};
    clear.float32[0] = 0x10 / 255.0f;
    clear.float32[1] = 0x20 / 255.0f;
    clear.float32[2] = 0x30 / 255.0f;
    clear.float32[3] = 1.0f;
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    imageBarrier(cmd, image, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kSize, kSize, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

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
    vkCmdPipelineBarrier2(cmd, &dependency);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);

    VkCommandBufferSubmitInfo cmd_submit{};
    cmd_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_submit.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_submit;
    CHECK(vkQueueSubmit2(ctx.graphicsQueue(), 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS);

    vmaInvalidateAllocation(ctx.allocator(), buffer_alloc, 0, VK_WHOLE_SIZE);
    const auto* bytes = static_cast<const std::uint8_t*>(buffer_mapped.pMappedData);
    bool all_match = true;
    for (std::uint32_t i = 0; i < kSize * kSize; i++) {
      const std::uint8_t expected[4] = {0x30, 0x20, 0x10, 0xFF};
      if (std::memcmp(bytes + i * 4, expected, 4) != 0) all_match = false;
    }
    CHECK(all_match);
  }

  if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
  if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
  if (buffer != VK_NULL_HANDLE) vmaDestroyBuffer(ctx.allocator(), buffer, buffer_alloc);
  if (image != VK_NULL_HANDLE) vmaDestroyImage(ctx.allocator(), image, image_alloc);
}

}  // namespace

int main() {
  const bool layer = dal::vkValidationLayerAvailable();
  std::printf("validation layer: %s\n", layer ? "installed" : "not installed");
  listShadercLibraries();

  dal::VkContextOptions options;
  options.validation = true;
  std::string error;
  std::unique_ptr<dal::VkContext> ctx = dal::createVkContext(options, error);
  if (!ctx) {
    std::printf("vulkan: %s\n", error.c_str());
    std::printf("render tests: skipped: no Vulkan device\n");
    return 0;
  }

  const std::uint32_t api = ctx->deviceProperties().apiVersion;
  std::printf("device: %s (Vulkan %u.%u.%u), D24S8 %s, validation %s\n", ctx->deviceName().data(),
              VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
              ctx->hasD24S8() ? "supported" : "unsupported (D32S8 fallback)",
              ctx->validationEnabled() ? "active" : "inactive");
  CHECK(layer == ctx->validationEnabled());

  testClearAndReadback(*ctx);
  CHECK(ctx->validationErrorCount() == 0);

  ctx.reset();
  std::printf("render tests: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed != 0 ? 1 : 0;
}
