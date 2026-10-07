// dal_moviedump: parses a .pam movie, demuxes it and decodes the video with the real FFmpeg codec
// factory. Prints the stream table, then (with --hash) one line per decoded frame with the SHA-256
// of the decoder's Y, U and V planes and of the converted ARGB picture, and a summary line. Exits 1 on
// any error, so it doubles as the headless decode check.

#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include "core/log.h"
#include "media/ffmpeg_codecs.h"
#include "media/movie_decoder.h"
#include "media/pamf.h"
#include "render/frame_dump.h"

namespace {

using Digest = std::array<std::uint8_t, 32>;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string movie;
  std::uint64_t max_frames = 0;  // 0: whole movie
  std::string png_dir;
  std::uint64_t png_every = 0;   // also write every Nth frame
  bool hash = false;
  bool verbose = false;
  int threads = 0;
  std::optional<bool> bt709;
  std::optional<std::uint64_t> expect_frames;
  std::map<std::uint64_t, std::string> expect_yuv;  // frame index -> lower-case hex
  std::map<std::uint64_t, std::string> expect_rgb;
};

void printUsage() {
  std::fputs(
      "usage: dal_moviedump MOVIE.pam [--frames N] [--hash] [--png-dir DIR [--png-every N]]\n"
      "                     [--threads N] [--bt601 | --bt709] [--verbose]\n"
      "                     [--expect-frames N] [--expect-yuv INDEX:SHA256]... [--expect-rgb INDEX:SHA256]...\n"
      "  --frames N          stop after N decoded frames (default: the whole movie)\n"
      "  --hash              print one line per frame with the YUV and ARGB SHA-256\n"
      "  --png-dir DIR       write the first and the last frame (and every --png-every th) as PNG\n"
      "  --threads N         H.264 frame threads (default: min(4, cores))\n"
      "  --bt601 | --bt709   colour matrix of the ARGB conversion (default: from the stream info)\n"
      "  --expect-frames N   fail unless exactly N frames were decoded\n"
      "  --expect-yuv I:H    fail unless frame I has the YUV SHA-256 H\n"
      "  --expect-rgb I:H    fail unless frame I has the ARGB SHA-256 H\n",
      stderr);
}

bool parseUnsigned(std::string_view text, std::uint64_t& value) {
  if (text.empty()) return false;
  value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return true;
}

bool parseOptions(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto value = [&](std::string_view& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    std::string_view text;
    std::uint64_t number = 0;
    if (arg == "--hash") {
      options.hash = true;
    } else if (arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--bt601") {
      options.bt709 = false;
    } else if (arg == "--bt709") {
      options.bt709 = true;
    } else if (arg == "--frames") {
      if (!value(text) || !parseUnsigned(text, options.max_frames)) return false;
    } else if (arg == "--png-every") {
      if (!value(text) || !parseUnsigned(text, options.png_every)) return false;
    } else if (arg == "--threads") {
      if (!value(text) || !parseUnsigned(text, number) || number > 64) return false;
      options.threads = static_cast<int>(number);
    } else if (arg == "--png-dir") {
      if (!value(text)) return false;
      options.png_dir = std::string(text);
    } else if (arg == "--expect-frames") {
      if (!value(text) || !parseUnsigned(text, number)) return false;
      options.expect_frames = number;
    } else if (arg == "--expect-yuv" || arg == "--expect-rgb") {
      const bool yuv = arg == "--expect-yuv";
      if (!value(text)) return false;
      const std::size_t colon = text.find(':');
      if (colon == std::string_view::npos || !parseUnsigned(text.substr(0, colon), number)) return false;
      std::string hash(text.substr(colon + 1));
      for (char& c : hash) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (hash.size() != 64) return false;
      (yuv ? options.expect_yuv : options.expect_rgb)[number] = hash;
    } else if (!arg.empty() && arg[0] == '-') {
      return false;
    } else if (options.movie.empty()) {
      options.movie = std::string(arg);
    } else {
      return false;
    }
  }
  return !options.movie.empty();
}

std::optional<Digest> sha256(std::span<const std::uint8_t> data) {
  Digest digest{};
  const NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                     const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
                                     digest.data(), static_cast<ULONG>(digest.size()));
  if (status < 0) return std::nullopt;
  return digest;
}

std::string hex(const Digest& digest) {
  std::string text;
  for (const std::uint8_t byte : digest) text += std::format("{:02x}", byte);
  return text;
}

// A hash over the per-frame digests, so the summary line fingerprints the whole run.
class RunningDigest {
 public:
  void add(const Digest& digest) { bytes_.insert(bytes_.end(), digest.begin(), digest.end()); }
  std::string finish() const {
    const auto digest = sha256(bytes_);
    return digest ? hex(*digest) : std::string("unavailable");
  }

 private:
  std::vector<std::uint8_t> bytes_;
};

std::uint32_t be32(const std::uint8_t* p) {
  return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

const char* codecName(dal::PamfCodec codec) {
  switch (codec) {
    case dal::PamfCodec::Avc: return "avc";
    case dal::PamfCodec::M2v: return "mpeg2";
    case dal::PamfCodec::Atrac3plus: return "atrac3plus";
    case dal::PamfCodec::Lpcm: return "lpcm";
    case dal::PamfCodec::Ac3: return "ac3";
    case dal::PamfCodec::UserData: return "userdata";
  }
  return "?";
}

bool writeArgbPng(const std::filesystem::path& path, const dal::VideoFrame& frame) {
  dal::RgbImage image;
  image.width = frame.width;
  image.height = frame.height;
  image.rgb.resize(std::size_t{frame.width} * frame.height * 3);
  for (std::uint32_t y = 0; y < frame.height; ++y) {
    const std::uint8_t* src = frame.bytes.data() + std::size_t{frame.pitch} * y;
    std::uint8_t* dst = image.rgb.data() + std::size_t{frame.width} * 3 * y;
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      dst[x * 3 + 0] = src[x * 4 + 1];
      dst[x * 3 + 1] = src[x * 4 + 2];
      dst[x * 3 + 2] = src[x * 4 + 3];
    }
  }
  std::string error;
  if (!dal::writePngRgb(path, image, error)) {
    std::fprintf(stderr, "error: cannot write %s: %s\n", path.string().c_str(), error.c_str());
    return false;
  }
  return true;
}

// Both decoders see the same data; one hands out the decoder's own planes (planar output at the
// decoded size is a plain copy), the other the ARGB conversion.
struct Decoders {
  std::unique_ptr<dal::VideoDecoder> yuv, argb;
  std::deque<dal::VideoFrame> yuv_queue, argb_queue;
  double yuv_seconds = 0, argb_seconds = 0;
  bool send_failed = false;
};

double secondsSince(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void sendUnit(Decoders& d, const dal::PamfAccessUnit& unit) {
  auto start = Clock::now();
  if (!d.yuv->send(unit.bytes, unit.pts, unit.dts)) d.send_failed = true;
  d.yuv_seconds += secondsSince(start);
  start = Clock::now();
  if (!d.argb->send(unit.bytes, unit.pts, unit.dts)) d.send_failed = true;
  d.argb_seconds += secondsSince(start);
}

void receiveReady(Decoders& d) {
  dal::VideoFrame frame;
  auto start = Clock::now();
  while (d.yuv->receive(frame)) d.yuv_queue.push_back(std::move(frame));
  d.yuv_seconds += secondsSince(start);
  start = Clock::now();
  while (d.argb->receive(frame)) d.argb_queue.push_back(std::move(frame));
  d.argb_seconds += secondsSince(start);
}

int run(const Options& options) {
  namespace fs = std::filesystem;
  const auto run_start = Clock::now();
  std::ifstream in(options.movie, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "error: cannot open %s\n", options.movie.c_str());
    return 1;
  }
  std::vector<std::uint8_t> header(2048);
  in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
  if (static_cast<std::size_t>(in.gcount()) < header.size()) {
    std::fprintf(stderr, "error: %s is too short\n", options.movie.c_str());
    return 1;
  }
  // The header size is in 2048-byte units at offset 8; the parser needs the whole header.
  const std::uint64_t header_size = std::uint64_t{be32(header.data() + 8)} * 2048;
  if (header_size > header.size() && header_size <= (1u << 20)) {
    header.resize(static_cast<std::size_t>(header_size));
    in.read(reinterpret_cast<char*>(header.data() + 2048), static_cast<std::streamsize>(header.size() - 2048));
  }
  const auto info = dal::parsePamfHeader(header);
  if (!info) {
    std::fprintf(stderr, "error: %s: %s\n", options.movie.c_str(), info.error().c_str());
    return 1;
  }

  std::size_t video_index = info->streams.size();
  for (std::size_t i = 0; i < info->streams.size(); ++i) {
    const dal::PamfStream& s = info->streams[i];
    std::printf("stream %zu: %s id 0x%02x/0x%02x", i, codecName(s.codec), s.stream_id, s.private_stream_id);
    if (s.width != 0) std::printf(" %ux%u", s.width, s.height);
    if (s.codec == dal::PamfCodec::Avc && s.codec_info.size() > 0x17) {
      const std::vector<std::uint8_t>& c = s.codec_info;
      std::printf(", frame-rate code %d", (c[2] & 0x0F) - 1);
      if ((c[2] & 0x40) != 0) {
        std::printf(", signal info: range %s, primaries %u, matrix %u", (c[0x14] & 0x10) != 0 ? "full" : "limited",
                    c[0x15], c[0x17]);
      } else {
        std::printf(", no signal info");
      }
    }
    if (s.sample_rate != 0) std::printf(" %u Hz %u ch", s.sample_rate, s.channels);
    std::printf("\n");
    if (video_index == info->streams.size() && s.codec == dal::PamfCodec::Avc) video_index = i;
  }
  std::printf("data: offset %u, %u bytes, pts %llu..%llu\n", info->data_offset, info->data_size,
              static_cast<unsigned long long>(info->start_pts),
              static_cast<unsigned long long>(info->end_pts));
  if (video_index == info->streams.size()) {
    std::fprintf(stderr, "error: no AVC video stream\n");
    return 1;
  }
  const dal::PamfStream& video = info->streams[video_index];
  const std::vector<std::uint8_t>& block = video.codec_info;

  // Matrix coefficients 1 = BT.709 when the stream info carries video-signal data; without it HD
  // is taken as BT.709 and SD as BT.601, as the PS3 does not state otherwise.
  bool bt709 = video.height > 576;
  if (block.size() > 0x17 && (block[2] & 0x40) != 0) bt709 = block[0x17] == 1;
  if (options.bt709) bt709 = *options.bt709;

  dal::FfmpegCodecOptions codec_options;
  codec_options.video_threads = options.threads;
  std::string error;
  const std::unique_ptr<dal::MediaCodecFactory> factory = dal::createFfmpegCodecFactory(error, codec_options);
  if (!factory) {
    std::fprintf(stderr, "error: codecs unavailable: %s\n", error.c_str());
    return 1;
  }
  Decoders d;
  dal::VideoOutputFormat format;
  format.start_pts = static_cast<std::int64_t>(info->start_pts);
  format.coding = dal::VideoCoding::Yuv420Planar;
  d.yuv = factory->createVideo(video, format, error);
  if (!d.yuv) {
    std::fprintf(stderr, "error: cannot create the video decoder: %s\n", error.c_str());
    return 1;
  }
  format.coding = dal::VideoCoding::Argb;
  format.bt709 = bt709;
  d.argb = factory->createVideo(video, format, error);
  if (!d.argb) {
    std::fprintf(stderr, "error: cannot create the video decoder: %s\n", error.c_str());
    return 1;
  }

  const fs::path stem = fs::path(options.movie).stem();
  const fs::path png_dir = options.png_dir;
  bool failed = false;
  std::uint64_t frames = 0;
  std::int64_t first_pts = -1, last_pts = -1;
  std::uint32_t width = 0, height = 0;
  RunningDigest yuv_run, rgb_run;
  dal::VideoFrame last_argb;
  bool last_written = false;

  // Pairs the YUV and ARGB outputs, which come out in the same order.
  auto consume = [&]() {
    while (!d.yuv_queue.empty() && !d.argb_queue.empty()) {
      if (options.max_frames != 0 && frames >= options.max_frames) {
        d.yuv_queue.clear();
        d.argb_queue.clear();
        return;
      }
      dal::VideoFrame& y = d.yuv_queue.front();
      dal::VideoFrame& a = d.argb_queue.front();
      if (y.pts != a.pts || y.width != a.width || y.height != a.height) {
        std::fprintf(stderr, "error: frame %llu differs between the two decoders\n",
                     static_cast<unsigned long long>(frames));
        failed = true;
      }
      const auto yuv_hash = sha256(y.bytes);
      const auto rgb_hash = sha256(a.bytes);
      if (!yuv_hash || !rgb_hash) {
        std::fprintf(stderr, "error: SHA-256 failed\n");
        failed = true;
      } else {
        yuv_run.add(*yuv_hash);
        rgb_run.add(*rgb_hash);
        if (options.hash) {
          std::printf("frame %llu pts %lld %ux%u yuv-sha256 %s rgb-sha256 %s\n",
                      static_cast<unsigned long long>(frames), static_cast<long long>(y.pts), y.width, y.height,
                      hex(*yuv_hash).c_str(), hex(*rgb_hash).c_str());
        }
        const auto expected = options.expect_yuv.find(frames);
        if (expected != options.expect_yuv.end() && expected->second != hex(*yuv_hash)) {
          std::fprintf(stderr, "error: frame %llu yuv-sha256 %s, expected %s\n",
                       static_cast<unsigned long long>(frames), hex(*yuv_hash).c_str(),
                       expected->second.c_str());
          failed = true;
        }
        const auto expected_rgb = options.expect_rgb.find(frames);
        if (expected_rgb != options.expect_rgb.end() && expected_rgb->second != hex(*rgb_hash)) {
          std::fprintf(stderr, "error: frame %llu rgb-sha256 %s, expected %s\n",
                       static_cast<unsigned long long>(frames), hex(*rgb_hash).c_str(),
                       expected_rgb->second.c_str());
          failed = true;
        }
      }
      if (frames == 0) {
        first_pts = y.pts;
        width = y.width;
        height = y.height;
      }
      last_pts = y.pts;
      if (!png_dir.empty()) {
        const bool periodic = options.png_every != 0 && frames % options.png_every == 0;
        if (frames == 0 || periodic) {
          if (!writeArgbPng(png_dir / std::format("{}_{:05}.png", stem.string(), frames), a)) failed = true;
          last_written = true;
        } else {
          last_written = false;
        }
        last_argb = std::move(a);
      }
      ++frames;
      d.yuv_queue.pop_front();
      d.argb_queue.pop_front();
    }
  };

  const bool limited = options.max_frames != 0;
  dal::PamfDemuxer demux(*info);
  in.clear();
  in.seekg(info->data_offset);
  std::vector<std::uint8_t> chunk(1u << 20);
  std::uint64_t video_units = 0;
  auto pull = [&]() {
    while (auto unit = demux.next(static_cast<std::uint32_t>(video_index))) {
      ++video_units;
      sendUnit(d, *unit);
      receiveReady(d);
      consume();
      if (limited && frames >= options.max_frames) return false;
    }
    // Other streams are not decoded; drop them so the demuxer does not buffer the whole file.
    for (std::uint32_t i = 0; i < info->streams.size(); ++i) {
      if (i != video_index) {
        while (demux.next(i)) {
        }
      }
    }
    return true;
  };

  bool more = true;
  while (more && in) {
    in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize got = in.gcount();
    if (got <= 0) break;
    demux.feed(std::span<const std::uint8_t>(chunk).first(static_cast<std::size_t>(got)));
    more = pull();
  }
  if (more) {
    demux.endOfStream();
    more = pull();
  }
  if (more) {
    // Frame threading holds back the last pictures until the decoder is told the stream ended.
    auto start = Clock::now();
    d.yuv->flush();
    d.yuv_seconds += secondsSince(start);
    start = Clock::now();
    d.argb->flush();
    d.argb_seconds += secondsSince(start);
    receiveReady(d);
    consume();
  }

  if (!png_dir.empty() && frames > 0 && !last_written) {
    if (!writeArgbPng(png_dir / std::format("{}_{:05}.png", stem.string(), frames - 1), last_argb)) failed = true;
  }
  if (!d.yuv_queue.empty() || !d.argb_queue.empty()) {
    if (!limited) {
      std::fprintf(stderr, "error: the decoders returned different frame counts\n");
      failed = true;
    }
  }
  if (demux.errors() != 0) {
    std::fprintf(stderr, "error: %llu demux errors\n", static_cast<unsigned long long>(demux.errors()));
    failed = true;
  }
  if (d.send_failed) {
    std::fprintf(stderr, "error: the decoder rejected data\n");
    failed = true;
  }
  if (options.expect_frames && *options.expect_frames != frames) {
    std::fprintf(stderr, "error: %llu frames decoded, expected %llu\n", static_cast<unsigned long long>(frames),
                 static_cast<unsigned long long>(*options.expect_frames));
    failed = true;
  }
  for (const auto* expected : {&options.expect_yuv, &options.expect_rgb}) {
    for (const auto& [index, hash] : *expected) {
      if (index >= frames) {
        std::fprintf(stderr, "error: frame %llu was not decoded\n", static_cast<unsigned long long>(index));
        failed = true;
      }
    }
  }

  const double per_frame = frames != 0 ? 1000.0 / static_cast<double>(frames) : 0.0;
  std::printf("moviedump %s: %ux%u, %llu video units, %llu frames, pts %lld..%lld, bt%s\n",
              stem.string().c_str(), width, height, static_cast<unsigned long long>(video_units),
              static_cast<unsigned long long>(frames), static_cast<long long>(first_pts),
              static_cast<long long>(last_pts), bt709 ? "709" : "601");
  std::printf("moviedump yuv-sha256 %s rgb-sha256 %s\n", yuv_run.finish().c_str(), rgb_run.finish().c_str());
  // The decoders run on worker threads, so the time spent inside the calls is the time the caller
  // waited; the wall time covers everything including hashing and PNG output.
  std::printf("moviedump time: yuv decoder calls %.2f ms/frame, argb decoder calls %.2f ms/frame, wall %.2f ms/frame\n",
              d.yuv_seconds * per_frame, d.argb_seconds * per_frame, secondsSince(run_start) * per_frame);
  std::printf("moviedump %s\n", failed ? "failed" : "ok");
  return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseOptions(argc, argv, options)) {
    printUsage();
    return 2;
  }
  dal::setLogLevel(options.verbose ? dal::LogLevel::Debug : dal::LogLevel::Warn);
  return run(options);
}
