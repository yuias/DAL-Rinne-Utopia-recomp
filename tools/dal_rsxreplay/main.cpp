// dal_rsxreplay: feeds a .dalcap capture (see rsx/rsx_capture.h) through the RSX frame builder without
// the game and prints the commands it produces, the summary line and the unimplemented census section.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/census.h"
#include "rsx/rsx_capture.h"
#include "rsx/rsx_frame.h"
#include "rsx/rsx_frame_builder.h"
#include "rsx/rsx_unimplemented.h"

namespace {

void printUsage() {
  std::fputs(
      "usage: dal_rsxreplay CAPTURE [--out DIR] [--compare PNG] [--no-validation] [--shader-cache DIR]\n"
      "  CAPTURE  a .dalcap file written by dal_game --rsx-capture\n"
      "  Only the command list is printed for now; the other options are accepted and ignored.\n",
      stderr);
}

const char* primitiveName(dal::RsxPrimitive primitive) {
  switch (primitive) {
    case dal::RsxPrimitive::Points: return "POINTS";
    case dal::RsxPrimitive::Lines: return "LINES";
    case dal::RsxPrimitive::LineLoop: return "LINE_LOOP";
    case dal::RsxPrimitive::LineStrip: return "LINE_STRIP";
    case dal::RsxPrimitive::Triangles: return "TRIANGLES";
    case dal::RsxPrimitive::TriangleStrip: return "TRIANGLE_STRIP";
    case dal::RsxPrimitive::TriangleFan: return "TRIANGLE_FAN";
    case dal::RsxPrimitive::Quads: return "QUADS";
    case dal::RsxPrimitive::QuadStrip: return "QUAD_STRIP";
    case dal::RsxPrimitive::Polygon: return "POLYGON";
  }
  return "?";
}

// The unimplemented census provider is registered here because only the game installs the RSX
// consumer's own.
dal::RsxUnimplementedCensus unimplementedCensus() {
  return {dal::rsxUnimplementedSnapshot(), dal::rsxUnimplementedDropped()};
}

// The same text the game prints at stop, cut out of the whole census.
std::string unimplementedSection() {
  const std::string census = dal::formatCensus();
  constexpr std::string_view kHeader = "census: rsx unimplemented";
  const std::size_t begin = census.find(kHeader);
  if (begin == std::string::npos) return {};
  const std::size_t end = census.find("\ncensus:", begin);
  return census.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

void printClear(const dal::RsxClear& clear) {
  const dal::RsxSurface& color = clear.surfaces.color;
  std::printf("clear mask=%#x color=0x%08x %ux%u@0x%x\n", clear.mask, clear.color_argb, color.width, color.height,
              color.offset);
}

void printDraw(const dal::RsxDraw& draw, std::span<const dal::RsxSurface> surfaces) {
  std::printf("draw %s count=%u vp=%016llx fp=%016llx", primitiveName(draw.primitive), draw.count,
              static_cast<unsigned long long>(draw.vp_hash), static_cast<unsigned long long>(draw.fp_hash));
  for (const dal::RsxTextureDesc& texture : draw.textures) {
    if (texture.rt_alias >= 0 && static_cast<std::size_t>(texture.rt_alias) < surfaces.size()) {
      const dal::RsxSurface& surface = surfaces[static_cast<std::size_t>(texture.rt_alias)];
      std::printf(" tex%u=alias:%d(%ux%u)", texture.unit, texture.rt_alias, surface.width, surface.height);
    } else {
      std::printf(" tex%u=bytes(%ux%u)", texture.unit, texture.width, texture.height);
    }
  }
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string capture_path;
  for (int i = 1; i < argc; i++) {
    const std::string_view arg = argv[i];
    if (arg == "--no-validation") continue;
    if (arg == "--out" || arg == "--compare" || arg == "--shader-cache") {
      if (i + 1 >= argc) {
        printUsage();
        return 2;
      }
      i++;
      continue;
    }
    if (arg.starts_with("--") || !capture_path.empty()) {
      printUsage();
      return 2;
    }
    capture_path = std::string(arg);
  }
  if (capture_path.empty()) {
    printUsage();
    return 2;
  }

  dal::RsxCaptureReader reader;
  std::string error;
  if (!reader.load(capture_path, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  dal::setRsxUnimplementedProvider(&unimplementedCensus, &dal::rsxUnimplementedReset);
  dal::rsxUnimplementedReset();
  dal::ReplayRsxMemoryView memory;
  dal::RsxFrameBuilder builder(memory);
  dal::replayCapture(reader, memory, builder);
  const std::vector<dal::RsxCommand> commands = builder.takeCommands();

  std::size_t draws = 0;
  for (const dal::RsxCommand& command : commands) {
    if (const auto* clear = std::get_if<dal::RsxClear>(&command)) {
      printClear(*clear);
    } else if (const auto* draw = std::get_if<dal::RsxDraw>(&command)) {
      printDraw(*draw, builder.knownSurfaces());
      draws++;
    } else {
      const auto& flip = std::get<dal::RsxFlip>(command);
      std::printf("flip buffer=%u %ux%u@0x%x\n", flip.buffer, flip.width, flip.height, flip.offset);
    }
  }
  // Nothing is skipped until a renderer exists to refuse commands.
  std::printf("replay: %zu commands, %zu draws, 0 skipped\n", commands.size(), draws);
  // The game's census omits an empty section; a replay says so explicitly.
  const std::string section = unimplementedSection();
  std::printf("%s\n", section.empty() ? "census: rsx unimplemented 0 kinds" : section.c_str());
  return 0;
}
