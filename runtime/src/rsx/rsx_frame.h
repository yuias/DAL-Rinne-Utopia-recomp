#pragma once

// Decoded RSX state and the commands the frame builder hands to the renderer. Plain data only: the
// renderer never sees guest memory, so every guest byte range a command needs is copied into it.
// Bit layouts of the source registers follow RPCS3 `Emu/RSX/rsx_decode.h` and `gcm_enums.h`.

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace dal {

enum class RsxColorFormat : std::uint8_t { A8R8G8B8, Unsupported };
enum class RsxDepthFormat : std::uint8_t { None, Z24S8, Z16, Unsupported };
// RSX BEGIN_END values 1..10.
enum class RsxPrimitive : std::uint8_t {
  Points = 1, Lines, LineLoop, LineStrip, Triangles, TriangleStrip, TriangleFan, Quads, QuadStrip, Polygon
};

struct RsxSurface {  // location 0 = local, 1 = main
  std::uint32_t location = 0, offset = 0, pitch = 0, width = 0, height = 0;
  RsxColorFormat format = RsxColorFormat::Unsupported;
};

struct RsxSurfaceState {
  RsxSurface color;
  bool has_color = false;
  RsxSurface depth;
  RsxDepthFormat depth_format = RsxDepthFormat::None;
  std::uint32_t clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
};

struct RsxVertexAttrib {
  std::uint8_t slot = 0, type = 0, size = 0;
  std::uint16_t stride = 0;
  std::uint32_t location = 0, offset = 0;
  std::uint32_t data_offset = 0;  // into RsxDraw::vertex_data
};

struct RsxTextureDesc {
  std::uint8_t unit = 0, format = 0 /* raw 8-bit */, mips = 0, dimension = 0;
  bool linear_layout = false, normalized = true, cubemap = false;
  // remap/filter/address/border are the raw SET_TEXTURE_CONTROL1/FILTER/ADDRESS/BORDER_COLOR words.
  std::uint32_t location = 0, offset = 0, pitch = 0, width = 0, height = 0, remap = 0, filter = 0, address = 0,
                border = 0;
  std::int32_t rt_alias = -1;  // index into the builder's known surfaces, -1 = bytes in texture_data
  std::uint32_t data_offset = 0, data_size = 0;
};

struct RsxRasterState {
  bool blend = false, alpha_test = false, depth_test = false, depth_write = false, cull = false;
  std::uint32_t blend_sfactor_rgb = 0, blend_dfactor_rgb = 0, blend_sfactor_a = 0, blend_dfactor_a = 0,
                blend_eq_rgb = 0, blend_eq_a = 0, blend_color = 0, alpha_func = 0, alpha_ref = 0, depth_func = 0,
                cull_face = 0, front_face = 0, color_mask = 0, shade_model = 0;
  float viewport_scale[4] = {}, viewport_offset[4] = {};
  std::uint32_t scissor_x = 0, scissor_y = 0, scissor_w = 0, scissor_h = 0, window_origin = 0, pixel_center = 0;
};

struct RsxClear {
  RsxSurfaceState surfaces;
  std::uint32_t mask = 0, color_argb = 0, depth_stencil = 0;
  std::uint32_t scissor[4] = {};
};

struct RsxDraw {
  RsxSurfaceState surfaces;
  RsxRasterState raster;
  RsxPrimitive primitive = RsxPrimitive::Triangles;
  // Contiguous DRAW_* ranges are merged; otherwise count `state: multiple ranges` and keep the first.
  bool indexed = false;
  std::uint8_t index_type = 0;
  std::uint32_t first = 0, count = 0, min_index = 0;
  std::vector<RsxVertexAttrib> attribs;
  std::vector<std::uint8_t> vertex_data;  // guest bytes, BE
  std::vector<std::uint8_t> index_data;   // guest bytes, BE
  std::array<float, 16 * 4> default_attribs{};  // SET_VERTEX_DATA4F_M for unbound slots
  std::vector<RsxTextureDesc> textures;
  std::vector<std::uint8_t> texture_data;
  std::uint64_t vp_hash = 0;
  std::vector<std::uint32_t> vp_ucode;
  std::uint32_t vp_input_mask = 0, vp_output_mask = 0;
  std::vector<std::uint32_t> vp_constants;  // full constant snapshot, kRsxVpConstantCount * 4 words
  std::uint64_t fp_hash = 0;                // hash of the ucode with inline constants zeroed
  std::vector<std::uint32_t> fp_ucode;      // decoded words
  std::vector<std::uint32_t> fp_constants;  // inline constants in program order
  std::uint32_t fp_control = 0, texcoord_control = 0;  // SHADER_CONTROL, texcoord 2D/3D/centroid masks
};

struct RsxFlip {
  std::uint32_t buffer = 0, offset = 0, pitch = 0, width = 0, height = 0;
  std::uint64_t flip_index = 0;
};

using RsxCommand = std::variant<RsxClear, RsxDraw, RsxFlip>;

}  // namespace dal
