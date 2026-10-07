// Self-checking tests for the run-time FFmpeg loader.

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/log.h"
#include "media/ffmpeg_api.h"

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

// Runs first: once the real libraries are mapped, a later load by the same file name could be
// answered from the already loaded modules and no longer test the directory.
void testEmptyDirectory() {
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(ec) / "dal_ffmpeg_empty_dir_test";
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  const auto result = dal::loadFfmpeg(dir);
  CHECK(!result.has_value());
  if (!result) {
    std::printf("empty directory: unavailable: %s\n", result.error().c_str());
    CHECK(result.error().find("avcodec-") != std::string::npos);
  }

  const auto absent = dal::loadFfmpeg(dir / "does_not_exist");
  CHECK(!absent.has_value());
  std::filesystem::remove_all(dir, ec);
}

void testLoad() {
  const auto& loaded = dal::defaultFfmpeg();
  if (!loaded) std::fprintf(stderr, "load failed: %s\n", loaded.error().c_str());
  CHECK(loaded.has_value());
  if (!loaded) return;
  const dal::FfmpegApi& api = *loaded;

  std::printf("avutil %u.%u.%u, avcodec %u.%u.%u, swscale %u.%u.%u\n", api.avutil_version() >> 16,
              (api.avutil_version() >> 8) & 0xff, api.avutil_version() & 0xff,
              api.avcodec_version() >> 16, (api.avcodec_version() >> 8) & 0xff,
              api.avcodec_version() & 0xff, api.swscale_version() >> 16,
              (api.swscale_version() >> 8) & 0xff, api.swscale_version() & 0xff);
  CHECK((api.avcodec_version() >> 16) == LIBAVCODEC_VERSION_MAJOR);

  const AVCodec* h264 = api.avcodec_find_decoder(AV_CODEC_ID_H264);
  CHECK(h264 != nullptr);
  if (h264) CHECK(std::string(h264->name) == "h264");
  const AVCodec* atrac3p = api.avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
  CHECK(atrac3p != nullptr);
  // The codec id is ATRAC3P; the decoder itself is named atrac3plus.
  if (atrac3p) CHECK(std::string(atrac3p->name) == "atrac3plus");
  CHECK(api.avcodec_find_decoder_by_name("atrac3plus") == atrac3p);
  CHECK(api.avcodec_find_decoder_by_name("h264") == h264);

  SwsContext* sws = api.sws_getCachedContext(nullptr, 16, 16, AV_PIX_FMT_YUV420P, 16, 16,
                                             AV_PIX_FMT_ARGB, SWS_BILINEAR, nullptr, nullptr,
                                             nullptr);
  CHECK(sws != nullptr);
  if (sws) api.sws_freeContext(sws);

  // Exercises the frame and packet allocators through FFmpeg's own free functions.
  AVFrame* frame = api.av_frame_alloc();
  CHECK(frame != nullptr);
  api.av_frame_free(&frame);
  CHECK(frame == nullptr);
  AVPacket* packet = api.av_packet_alloc();
  CHECK(packet != nullptr);
  api.av_packet_free(&packet);
  CHECK(packet == nullptr);

  // Opening ATRAC3plus without stream parameters fails and makes FFmpeg log, which must arrive
  // through the project logger.
  std::vector<std::string> lines;
  dal::setLogLevel(dal::LogLevel::Debug);
  dal::setLogSink([&lines](std::string_view line) { lines.emplace_back(line); });
  AVCodecContext* context = atrac3p ? api.avcodec_alloc_context3(atrac3p) : nullptr;
  CHECK(context != nullptr);
  if (context) {
    CHECK(api.avcodec_open2(context, atrac3p, nullptr) < 0);
    api.avcodec_free_context(&context);
  }
  dal::setLogSink({});
  dal::setLogLevel(dal::LogLevel::Info);
  bool bridged = false;
  for (const std::string& line : lines) {
    if (line.find("ffmpeg: ") != std::string::npos) bridged = true;
  }
  CHECK(bridged);

  // The same result comes back on every call.
  CHECK(&dal::defaultFfmpeg() == &loaded);
}

}  // namespace

int main() {
  testEmptyDirectory();
  testLoad();
  std::printf("media ffmpeg tests: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
