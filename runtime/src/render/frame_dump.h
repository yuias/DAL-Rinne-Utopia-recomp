#pragma once

// PNG output of rendered frames and the image comparison used by `dal_rsxreplay --compare`.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace dal {

// Tightly packed 8-bit RGB rows, top row first.
struct RgbImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;
};

// `bgra` holds width * height * 4 bytes (B, G, R, A per pixel); alpha is dropped.
RgbImage rgbFromBgra(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height);

// Writes an RGB8 PNG (zlib level 1) and creates the parent directory. False with a reason in `error`.
bool writePngRgb(const std::filesystem::path& path, const RgbImage& image, std::string& error);

// Reads any PNG as RGB8 (palette, gray, 16-bit and alpha are converted or dropped).
bool readPngRgb(const std::filesystem::path& path, RgbImage& image, std::string& error);

struct ImageDiff {
  std::uint32_t max = 0;         // largest absolute per-channel difference
  double mean = 0.0;             // mean absolute per-channel difference
  double differing_percent = 0.0;  // share of pixels with any channel differing by more than 2
};

// False with a reason in `error` when the sizes differ.
bool diffImages(const RgbImage& a, const RgbImage& b, ImageDiff& diff, std::string& error);

}  // namespace dal
