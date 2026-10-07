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

#include "render/frame_dump.h"
#include "render/shader_compiler.h"
#include "render/vk_context.h"
#include "render/vk_renderer.h"
#include "rsx/rsx_capture.h"
#include "rsx/rsx_frame_builder.h"
#include "rsx/rsx_registers.h"
#include "rsx/rsx_unimplemented.h"
#include "rsx_synthetic.h"

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

std::uint64_t unimplementedCount(const std::string& kind) {
  for (const auto& [name, count] : dal::rsxUnimplementedSnapshot()) {
    if (name == kind) return count;
  }
  return 0;
}

std::vector<dal::RsxCommand> replaySynthetic(const std::vector<std::uint8_t>& file) {
  dal::RsxCaptureReader reader;
  std::string error;
  CHECK(reader.parse(file, error));
  dal::ReplayRsxMemoryView view;
  dal::RsxFrameBuilder builder(view);
  dal::replayCapture(reader, view, builder);
  return builder.takeCommands();
}

bool displayIsUniform(const std::vector<std::uint8_t>& bgra, std::uint32_t width, std::uint32_t height,
                      const std::uint8_t (&expected)[4]) {
  if (bgra.size() != static_cast<std::size_t>(width) * height * 4) return false;
  for (std::size_t i = 0; i < bgra.size(); i += 4) {
    if (std::memcmp(bgra.data() + i, expected, 4) != 0) return false;
  }
  return true;
}

// Needs no Vulkan device.
void testFrameDump() {
  dal::RgbImage image;
  image.width = 5;
  image.height = 3;
  image.rgb.resize(5 * 3 * 3);
  for (std::size_t i = 0; i < image.rgb.size(); i++) image.rgb[i] = static_cast<std::uint8_t>(i * 7);

  // BGRA to RGB drops alpha and swaps the first and third channel.
  const std::uint8_t bgra[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  const dal::RgbImage converted = dal::rgbFromBgra(bgra, 2, 1);
  CHECK(converted.rgb == std::vector<std::uint8_t>({3, 2, 1, 7, 6, 5}));

  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "dal_frame_dump_test";
  std::filesystem::remove_all(dir, ec);
  const std::filesystem::path path = dir / "nested" / "image.png";
  std::string error;
  CHECK(dal::writePngRgb(path, image, error));
  dal::RgbImage read;
  CHECK(dal::readPngRgb(path, read, error));
  CHECK(read.width == 5 && read.height == 3 && read.rgb == image.rgb);

  dal::ImageDiff diff;
  CHECK(dal::diffImages(image, read, diff, error));
  CHECK(diff.max == 0 && diff.mean == 0.0 && diff.differing_percent == 0.0);

  // One pixel off by 10 in one channel, another by 2 (not counted as differing).
  dal::RgbImage other = image;
  other.rgb[0] = static_cast<std::uint8_t>(other.rgb[0] + 10);
  other.rgb[3 * 4 + 1] = static_cast<std::uint8_t>(other.rgb[3 * 4 + 1] + 2);
  CHECK(dal::diffImages(image, other, diff, error));
  CHECK(diff.max == 10);
  CHECK(diff.mean > 12.0 / 45.0 - 1e-9 && diff.mean < 12.0 / 45.0 + 1e-9);
  CHECK(diff.differing_percent > 100.0 / 15.0 - 1e-9 && diff.differing_percent < 100.0 / 15.0 + 1e-9);

  dal::RgbImage small = image;
  small.width = 4;
  small.rgb.resize(4 * 3 * 3);
  CHECK(!dal::diffImages(image, small, diff, error));
  CHECK(!dal::readPngRgb(dir / "missing.png", read, error));
  std::filesystem::remove_all(dir, ec);
}

// Clear-only captures through the renderer: the display equals the clear color, across more frames
// than there are frames in flight, and a flip of an unseen buffer reads back black.
void testRendererClear(dal::VkContext& ctx) {
  dal::rsxUnimplementedReset();
  dal::RsxRenderer renderer(ctx, dal::RenderOptions{});
  CHECK(renderer.valid());

  std::vector<std::uint8_t> bgra;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  CHECK(!renderer.readDisplay(bgra, width, height));

  constexpr std::uint32_t kColors[] = {0xFF102030, 0xFF405060, 0x80FF0000, 0xFF000000};
  for (const std::uint32_t argb : kColors) {
    std::vector<dal::RsxCommand> commands = replaySynthetic(dal::rsx_synthetic::makeClearCapture(argb));
    renderer.execute(commands);
    CHECK(renderer.valid());
    CHECK(renderer.readDisplay(bgra, width, height));
    CHECK(width == dal::rsx_synthetic::kSurfaceSize && height == dal::rsx_synthetic::kSurfaceSize);
    const std::uint8_t expected[4] = {static_cast<std::uint8_t>(argb), static_cast<std::uint8_t>(argb >> 8),
                                      static_cast<std::uint8_t>(argb >> 16), static_cast<std::uint8_t>(argb >> 24)};
    CHECK(displayIsUniform(bgra, width, height, expected));
  }
  CHECK(renderer.stats().clears == std::size(kColors) && renderer.stats().flips == std::size(kColors));
  CHECK(unimplementedCount("state surface resize") == 0);

  // A buffer no clear or draw touched: black, and counted.
  std::vector<dal::RsxCommand> flip_only;
  dal::RsxFlip flip;
  flip.offset = 0x40000;
  flip.width = 8;
  flip.height = 4;
  flip_only.emplace_back(flip);
  renderer.execute(flip_only);
  CHECK(renderer.readDisplay(bgra, width, height));
  CHECK(width == 8 && height == 4);
  CHECK(displayIsUniform(bgra, width, height, {0, 0, 0, 0xFF}));
  CHECK(unimplementedCount("state flip of non-render-target") == 1);
  CHECK(renderer.valid());
  dal::rsxUnimplementedReset();
}

std::uint32_t pixelArgb(const std::vector<std::uint8_t>& bgra, std::uint32_t width, std::uint32_t x,
                        std::uint32_t y) {
  const std::uint8_t* p = bgra.data() + (static_cast<std::size_t>(y) * width + x) * 4;
  return (static_cast<std::uint32_t>(p[3]) << 24) | (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[1]) << 8) | p[0];
}

// Clear to `first`, then clear with `mask` to `second` inside the scissor rectangle (x, y, w, h).
std::vector<std::uint8_t> makeScissoredClearCapture(std::uint32_t first, std::uint32_t second, std::uint32_t mask,
                                                    std::uint32_t x, std::uint32_t y, std::uint32_t w,
                                                    std::uint32_t h) {
  dal::rsx_synthetic::SceneBuilder scene;
  scene.setSurface();
  scene.clear(first);
  scene.write(dal::kMethodNv4097SetScissorHorizontal, x | (w << 16));
  scene.write(dal::kMethodNv4097SetScissorVertical, y | (h << 16));
  scene.write(dal::kMethodNv4097SetColorClearValue, second);
  scene.write(dal::kMethodNv4097ClearSurface, mask);
  return scene.finish();
}

// A clear inside a sub-rectangle changes only that rectangle; a partial color mask changes nothing
// and is counted. Depth and stencil values are not checked: the renderer has no depth readback.
void testRendererScissorAndMask(dal::VkContext& ctx) {
  dal::rsxUnimplementedReset();
  dal::RsxRenderer renderer(ctx, dal::RenderOptions{});
  CHECK(renderer.valid());
  std::vector<std::uint8_t> bgra;
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  constexpr std::uint32_t kA = 0xFF102030;
  constexpr std::uint32_t kB = 0xFF405060;
  std::vector<dal::RsxCommand> commands = replaySynthetic(makeScissoredClearCapture(kA, kB, 0xF3, 8, 16, 16, 8));
  renderer.execute(commands);
  CHECK(renderer.readDisplay(bgra, width, height) && width == 64 && height == 64);
  if (bgra.size() == 64 * 64 * 4) {
    CHECK(pixelArgb(bgra, width, 8, 16) == kB);
    CHECK(pixelArgb(bgra, width, 23, 23) == kB);
    CHECK(pixelArgb(bgra, width, 15, 20) == kB);
    CHECK(pixelArgb(bgra, width, 7, 16) == kA);
    CHECK(pixelArgb(bgra, width, 24, 16) == kA);
    CHECK(pixelArgb(bgra, width, 8, 15) == kA);
    CHECK(pixelArgb(bgra, width, 8, 24) == kA);
    CHECK(pixelArgb(bgra, width, 0, 0) == kA && pixelArgb(bgra, width, 63, 63) == kA);
  }
  CHECK(unimplementedCount("state partial color clear mask") == 0);

  // Red and green only (0x30): not modelled, so color A stays.
  commands = replaySynthetic(makeScissoredClearCapture(kA, kB, 0x30, 0, 0, 64, 64));
  renderer.execute(commands);
  CHECK(renderer.readDisplay(bgra, width, height));
  const std::uint8_t expected_a[4] = {0x30, 0x20, 0x10, 0xFF};
  CHECK(displayIsUniform(bgra, width, height, expected_a));
  CHECK(unimplementedCount("state partial color clear mask") == 1);
  CHECK(renderer.valid());
  dal::rsxUnimplementedReset();
}

// A clear that includes stencil under a write mask other than 0 or 0xFF is counted by the frame
// builder (the renderer clears the whole byte).
void testPartialStencilNote() {
  const auto clearWithStencilMask = [](std::uint32_t stencil_mask) {
    dal::rsx_synthetic::SceneBuilder scene;
    scene.setSurface();
    scene.write(dal::kMethodNv4097SetStencilMask, stencil_mask);
    scene.clear(0xFF000000);
    // Recording runs a frame builder of its own; only the replay's notes are counted.
    const std::vector<std::uint8_t> file = scene.finish();
    dal::rsxUnimplementedReset();
    return replaySynthetic(file);
  };
  clearWithStencilMask(0xFF);
  CHECK(unimplementedCount("state partial stencil clear mask") == 0);
  clearWithStencilMask(0x00);
  CHECK(unimplementedCount("state partial stencil clear mask") == 0);
  const std::vector<dal::RsxCommand> commands = clearWithStencilMask(0x0F);
  CHECK(commands.size() == 2 && std::get<dal::RsxClear>(commands[0]).mask == 0xF3);
  CHECK(unimplementedCount("state partial stencil clear mask") == 1);
  dal::rsxUnimplementedReset();
}

}  // namespace

int main() {
  const bool layer = dal::vkValidationLayerAvailable();
  std::printf("validation layer: %s\n", layer ? "installed" : "not installed");
  testShaderCompiler();
  testFrameDump();
  testPartialStencilNote();

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
  testRendererClear(*ctx);
  testRendererScissorAndMask(*ctx);
  CHECK(ctx->validationErrorCount() == 0);

  ctx.reset();
  std::printf("render tests: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed != 0 ? 1 : 0;
}
