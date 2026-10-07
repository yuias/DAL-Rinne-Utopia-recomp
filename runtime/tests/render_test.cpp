// Self-checking tests for the Vulkan renderer library. They skip when no Vulkan device exists.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "render/shader_compiler.h"
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

constexpr const char* kTrivialVertex = R"(#version 450
layout(location = 0) in vec4 position;
void main() { gl_Position = position; }
)";

constexpr const char* kTrivialFragment = R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }
)";

// Needs no Vulkan device, so it runs before the context is created.
void testShaderCompiler() {
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(ec) / "dal_shader_compiler_test";
  std::filesystem::remove_all(dir, ec);
  dal::setShaderCacheDir(dir);

  const dal::ShaderCompilerStats start = dal::shaderCompilerStats();
  auto vert = dal::compileGlsl(dal::ShaderStage::Vertex, kTrivialVertex, 0x1111);
  auto frag = dal::compileGlsl(dal::ShaderStage::Fragment, kTrivialFragment, 0x2222);
  CHECK(vert.has_value() && vert->size() > 5 && vert->front() == 0x07230203);
  CHECK(frag.has_value() && frag->size() > 5 && frag->front() == 0x07230203);
  if (!vert) std::fprintf(stderr, "vertex: %s\n", vert.error().c_str());
  if (!frag) std::fprintf(stderr, "fragment: %s\n", frag.error().c_str());

  // Not cached, and the shaderc message reaches the caller.
  auto bad = dal::compileGlsl(dal::ShaderStage::Fragment, "#version 450\nvoid main() { oops }\n",
                              0x3333);
  CHECK(!bad.has_value());
  if (!bad) {
    std::printf("shader syntax error message: %s", bad.error().c_str());
    CHECK(bad.error().find("error") != std::string::npos);
  }

  const dal::ShaderCompilerStats compiled = dal::shaderCompilerStats();
  CHECK(compiled.compiles - start.compiles == 3);
  CHECK(compiled.cache_hits == start.cache_hits);

  // Second round: both good shaders come from disk and match the first result.
  auto vert2 = dal::compileGlsl(dal::ShaderStage::Vertex, kTrivialVertex, 0x1111);
  auto frag2 = dal::compileGlsl(dal::ShaderStage::Fragment, kTrivialFragment, 0x2222);
  const dal::ShaderCompilerStats cached = dal::shaderCompilerStats();
  CHECK(vert2.has_value() && vert && *vert2 == *vert);
  CHECK(frag2.has_value() && frag && *frag2 == *frag);
  CHECK(cached.cache_hits - compiled.cache_hits == 2);
  CHECK(cached.compiles == compiled.compiles);

  // A truncated file is recompiled and rewritten.
  std::filesystem::path file;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().filename().string().starts_with("vert_0000000000001111_")) {
      file = entry.path();
    }
  }
  CHECK(!file.empty() && std::filesystem::exists(file, ec));
  std::filesystem::resize_file(file, 10, ec);
  auto vert3 = dal::compileGlsl(dal::ShaderStage::Vertex, kTrivialVertex, 0x1111);
  const dal::ShaderCompilerStats repaired = dal::shaderCompilerStats();
  CHECK(vert3.has_value() && vert && *vert3 == *vert);
  CHECK(repaired.compiles - cached.compiles == 1);
  CHECK(std::filesystem::file_size(file, ec) == vert->size() * 4);

  // Same cache_key, different GLSL: two compiles, no hit, and the SPIR-V differs.
  const char* variant_vertex = R"(#version 450
layout(location = 0) in vec4 position;
void main() { gl_Position = position * 2.0; }
)";
  auto vert4 = dal::compileGlsl(dal::ShaderStage::Vertex, variant_vertex, 0x1111);
  const dal::ShaderCompilerStats variant = dal::shaderCompilerStats();
  CHECK(vert4.has_value() && vert && *vert4 != *vert);
  CHECK(variant.compiles - repaired.compiles == 1);
  CHECK(variant.cache_hits == repaired.cache_hits);
  auto vert5 = dal::compileGlsl(dal::ShaderStage::Vertex, variant_vertex, 0x1111);
  auto vert6 = dal::compileGlsl(dal::ShaderStage::Vertex, kTrivialVertex, 0x1111);
  const dal::ShaderCompilerStats variant_cached = dal::shaderCompilerStats();
  CHECK(vert5.has_value() && vert4 && *vert5 == *vert4);
  CHECK(vert6.has_value() && vert && *vert6 == *vert);
  CHECK(variant_cached.cache_hits - variant.cache_hits == 2);
  CHECK(variant_cached.compiles == variant.compiles);

  std::printf("shader cache: %llu hits, %llu compiles\n",
              static_cast<unsigned long long>(variant_cached.cache_hits - start.cache_hits),
              static_cast<unsigned long long>(variant_cached.compiles - start.compiles));
  dal::setShaderCacheDir({});
  std::filesystem::remove_all(dir, ec);
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
  testShaderCompiler();

  dal::VkContextOptions options;
  options.validation = true;
  std::string error;
  std::unique_ptr<dal::VkContext> ctx = dal::createVkContext(options, error);
  if (!ctx) {
    std::printf("vulkan: %s\n", error.c_str());
    std::printf("render tests: skipped: no Vulkan device\n");
    return g_failed != 0 ? 1 : 0;
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
