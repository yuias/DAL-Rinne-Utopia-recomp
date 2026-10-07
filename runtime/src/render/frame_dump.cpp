#include "render/frame_dump.h"

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "png.h"

// libpng reports errors by longjmp. The functions that own a setjmp (pngEncode, pngReadHeader,
// pngReadRows) hold only trivially destructible locals, so a jump crosses nothing but libpng's C
// frames and the plain callbacks below. Buffers are created by the callers and passed as raw
// pointers; the error text lives in a fixed array of PngJob. The error callback never returns.

namespace dal {
namespace {

struct PngJob {
  char error[192] = {};
  std::vector<std::uint8_t>* out = nullptr;  // encode: receives the file bytes
  const std::uint8_t* in = nullptr;          // decode: the file bytes
  std::size_t in_size = 0;
  std::size_t in_pos = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

void pngErrorCallback(png_structp png, png_const_charp message) {
  auto* job = static_cast<PngJob*>(png_get_error_ptr(png));
  std::snprintf(job->error, sizeof(job->error), "%s", message != nullptr ? message : "libpng error");
  png_longjmp(png, 1);
}

void pngWarningCallback(png_structp, png_const_charp) {}

void pngWriteCallback(png_structp png, png_bytep data, png_size_t length) {
  auto* job = static_cast<PngJob*>(png_get_io_ptr(png));
  job->out->insert(job->out->end(), data, data + length);
}

void pngFlushCallback(png_structp) {}

void pngReadCallback(png_structp png, png_bytep data, png_size_t length) {
  auto* job = static_cast<PngJob*>(png_get_io_ptr(png));
  if (length > job->in_size - job->in_pos) png_error(png, "unexpected end of file");
  std::memcpy(data, job->in + job->in_pos, length);
  job->in_pos += length;
}

bool pngEncode(PngJob* job, png_structp png, png_infop info, const std::uint8_t* rgb) {
  if (setjmp(png_jmpbuf(png)) != 0) return false;
  png_set_write_fn(png, job, pngWriteCallback, pngFlushCallback);
  png_set_compression_level(png, 1);
  png_set_IHDR(png, info, job->width, job->height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  const std::size_t stride = static_cast<std::size_t>(job->width) * 3;
  for (std::uint32_t y = 0; y < job->height; y++) {
    png_write_row(png, const_cast<png_bytep>(rgb + y * stride));
  }
  png_write_end(png, nullptr);
  return true;
}

// Reads the header and selects the transforms that turn every input into RGB8.
bool pngReadHeader(PngJob* job, png_structp png, png_infop info) {
  if (setjmp(png_jmpbuf(png)) != 0) return false;
  png_set_read_fn(png, job, pngReadCallback);
  png_read_info(png, info);
  png_uint_32 width = 0;
  png_uint_32 height = 0;
  int bit_depth = 0;
  int color_type = 0;
  png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);
  if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (bit_depth == 16) png_set_strip_16(png);
  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
  // A palette with a tRNS chunk expands to RGBA, so the alpha goes there too.
  if ((color_type & PNG_COLOR_MASK_ALPHA) != 0 || png_get_valid(png, info, PNG_INFO_tRNS) != 0) {
    png_set_strip_alpha(png);
  }
  png_set_interlace_handling(png);
  png_read_update_info(png, info);
  if (png_get_rowbytes(png, info) != static_cast<std::size_t>(width) * 3) png_error(png, "unsupported pixel layout");
  job->width = width;
  job->height = height;
  return true;
}

bool pngReadRows(png_structp png, png_infop info, png_bytep* rows) {
  if (setjmp(png_jmpbuf(png)) != 0) return false;
  png_read_image(png, rows);
  png_read_end(png, info);
  return true;
}

}  // namespace

RgbImage rgbFromBgra(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height) {
  RgbImage image;
  image.width = width;
  image.height = height;
  const std::size_t pixels = static_cast<std::size_t>(width) * height;
  image.rgb.resize(pixels * 3);
  if (bgra.size() < pixels * 4) return RgbImage{};
  for (std::size_t i = 0; i < pixels; i++) {
    image.rgb[i * 3 + 0] = bgra[i * 4 + 2];
    image.rgb[i * 3 + 1] = bgra[i * 4 + 1];
    image.rgb[i * 3 + 2] = bgra[i * 4 + 0];
  }
  return image;
}

bool writePngRgb(const std::filesystem::path& path, const RgbImage& image, std::string& error) {
  if (image.width == 0 || image.height == 0 ||
      image.rgb.size() != static_cast<std::size_t>(image.width) * image.height * 3) {
    error = "image has no pixels or an inconsistent size";
    return false;
  }
  std::vector<std::uint8_t> bytes;
  PngJob job;
  job.out = &bytes;
  job.width = image.width;
  job.height = image.height;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &job, pngErrorCallback, pngWarningCallback);
  png_infop info = png != nullptr ? png_create_info_struct(png) : nullptr;
  if (png == nullptr || info == nullptr) {
    if (png != nullptr) png_destroy_write_struct(&png, nullptr);
    error = "cannot create the PNG encoder";
    return false;
  }
  const bool encoded = pngEncode(&job, png, info, image.rgb.data());
  png_destroy_write_struct(&png, &info);
  if (!encoded) {
    error = std::format("PNG encoding failed: {}", job.error);
    return false;
  }

  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  // Written beside the target and renamed, so a stop in the middle never leaves a truncated PNG there.
  std::filesystem::path temp = path;
  temp += ".tmp";
  std::ofstream file(temp, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  file.close();
  if (!file) {
    std::filesystem::remove(temp, ec);
    error = std::format("cannot write {}", path.string());
    return false;
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    error = std::format("cannot write {}", path.string());
    return false;
  }
  return true;
}

bool readPngRgb(const std::filesystem::path& path, RgbImage& image, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = std::format("cannot open {}", path.string());
    return false;
  }
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  PngJob job;
  job.in = bytes.data();
  job.in_size = bytes.size();
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &job, pngErrorCallback, pngWarningCallback);
  png_infop info = png != nullptr ? png_create_info_struct(png) : nullptr;
  if (png == nullptr || info == nullptr) {
    if (png != nullptr) png_destroy_read_struct(&png, nullptr, nullptr);
    error = "cannot create the PNG decoder";
    return false;
  }
  bool ok = pngReadHeader(&job, png, info);
  std::vector<png_bytep> rows;
  if (ok) {
    const std::size_t pixels = static_cast<std::size_t>(job.width) * job.height;
    if (job.width == 0 || job.height == 0 || pixels > (std::size_t{1} << 28)) {
      std::snprintf(job.error, sizeof(job.error), "unreasonable image size %ux%u", job.width, job.height);
      ok = false;
    } else {
      image.width = job.width;
      image.height = job.height;
      image.rgb.assign(pixels * 3, 0);
      rows.resize(job.height);
      for (std::uint32_t y = 0; y < job.height; y++) {
        rows[y] = image.rgb.data() + static_cast<std::size_t>(y) * job.width * 3;
      }
      ok = pngReadRows(png, info, rows.data());
    }
  }
  png_destroy_read_struct(&png, &info, nullptr);
  if (!ok) {
    error = std::format("cannot decode {}: {}", path.string(), job.error);
    return false;
  }
  return true;
}

bool diffImages(const RgbImage& a, const RgbImage& b, ImageDiff& diff, std::string& error) {
  if (a.width != b.width || a.height != b.height || a.rgb.size() != b.rgb.size()) {
    error = std::format("image sizes differ: {}x{} against {}x{}", a.width, a.height, b.width, b.height);
    return false;
  }
  diff = ImageDiff{};
  const std::size_t pixels = static_cast<std::size_t>(a.width) * a.height;
  if (pixels == 0) return true;
  std::uint64_t sum = 0;
  std::uint64_t differing = 0;
  for (std::size_t p = 0; p < pixels; p++) {
    bool pixel_differs = false;
    for (std::size_t c = 0; c < 3; c++) {
      const int delta = std::abs(static_cast<int>(a.rgb[p * 3 + c]) - static_cast<int>(b.rgb[p * 3 + c]));
      sum += static_cast<std::uint64_t>(delta);
      if (static_cast<std::uint32_t>(delta) > diff.max) diff.max = static_cast<std::uint32_t>(delta);
      if (delta > 2) pixel_differs = true;
    }
    if (pixel_differs) differing++;
  }
  diff.mean = static_cast<double>(sum) / static_cast<double>(pixels * 3);
  diff.differing_percent = 100.0 * static_cast<double>(differing) / static_cast<double>(pixels);
  return true;
}

}  // namespace dal
