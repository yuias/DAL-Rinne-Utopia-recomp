#include "media/ffmpeg_codecs.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <deque>
#include <format>
#include <thread>
#include <vector>

#include "core/log.h"

namespace dal {

namespace {

constexpr std::int64_t kClockHz = 90000;

// Limits for the output format: far above the 1080p the game uses, far below what overflows.
constexpr std::uint32_t kMaxDimension = 8192;
constexpr std::uint32_t kMaxPitch = 4 * kMaxDimension;
constexpr std::uint64_t kMaxFrameBytes = 256ull << 20;

// Frame-rate codes of the PAMF AVC stream info, as numerator / denominator.
struct FrameRate {
  std::int64_t num, den;
};
constexpr FrameRate kFrameRates[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001},
                                     {30, 1},       {50, 1}, {60000, 1001}};

FrameRate frameRateOf(const PamfStream& stream) {
  constexpr FrameRate kDefault{30000, 1001};
  if (stream.codec_info.size() < 3) return kDefault;
  // The stored code is the API value plus one; 0 means "not stated".
  const int code = (stream.codec_info[2] & 0x0F) - 1;
  if (code < 0 || code >= static_cast<int>(std::size(kFrameRates))) return kDefault;
  return kFrameRates[code];
}

std::string errorString(const FfmpegApi& api, int code) {
  char text[AV_ERROR_MAX_STRING_SIZE] = {};
  api.av_strerror(code, text, sizeof(text));
  return std::format("{} ({})", text, code);
}

AVPixelFormat outputPixelFormat(VideoCoding coding) {
  switch (coding) {
    case VideoCoding::Argb: return AV_PIX_FMT_ARGB;
    case VideoCoding::Rgba: return AV_PIX_FMT_RGBA;
    case VideoCoding::Yuv420Planar: return AV_PIX_FMT_YUV420P;
  }
  return AV_PIX_FMT_NONE;
}

// Bilinear because most frames keep their size; the accurate rounding and full chroma
// interpolation keep the colours independent of how swscale splits the work.
constexpr int kScaleFlags = SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;

class FfmpegVideoDecoder final : public VideoDecoder {
 public:
  FfmpegVideoDecoder(const FfmpegApi& api, const VideoOutputFormat& format, FrameRate rate)
      : api_(api), format_(format), rate_(rate), converter_(api), anchor_(format.start_pts) {}

  ~FfmpegVideoDecoder() override {
    if (parser_ != nullptr) api_.av_parser_close(parser_);
    if (frame_ != nullptr) api_.av_frame_free(&frame_);
    if (packet_ != nullptr) api_.av_packet_free(&packet_);
    if (context_ != nullptr) api_.avcodec_free_context(&context_);
  }

  bool open(int threads, std::string& error) {
    const AVCodec* codec = api_.avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
      error = "FFmpeg has no H.264 decoder";
      return false;
    }
    context_ = api_.avcodec_alloc_context3(codec);
    frame_ = api_.av_frame_alloc();
    packet_ = api_.av_packet_alloc();
    parser_ = api_.av_parser_init(AV_CODEC_ID_H264);
    if (context_ == nullptr || frame_ == nullptr || packet_ == nullptr || parser_ == nullptr) {
      error = "out of memory creating the H.264 decoder";
      return false;
    }
    if (threads <= 0) {
      threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
    }
    context_->thread_count = threads;
    context_->thread_type = FF_THREAD_FRAME;
    context_->pkt_timebase = {1, static_cast<int>(kClockHz)};
    const int result = api_.avcodec_open2(context_, codec, nullptr);
    if (result < 0) {
      error = "cannot open the H.264 decoder: " + errorString(api_, result);
      return false;
    }
    return true;
  }

  bool send(std::span<const std::uint8_t> es, std::int64_t pts, std::int64_t dts) override {
    if (draining_ || parser_ == nullptr) return false;
    // The stamps go with the first byte only: the parser attaches them to the access unit that
    // begins in this payload, and repeating them would stamp later units of the same payload.
    std::int64_t parser_pts = pts < 0 ? AV_NOPTS_VALUE : pts;
    std::int64_t parser_dts = dts < 0 ? AV_NOPTS_VALUE : dts;
    // FFmpeg's bitstream readers look a few bytes past the end of their input. The demuxer's
    // buffers know nothing about that, so the payload is copied into one with zeroed padding.
    padded_.assign(es.begin(), es.end());
    padded_.resize(es.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    const std::uint8_t* data = padded_.data();
    std::size_t remaining = es.size();
    bool ok = true;
    while (remaining > 0) {
      std::uint8_t* unit = nullptr;
      int unit_size = 0;
      const int chunk = static_cast<int>(std::min<std::size_t>(remaining, INT_MAX));
      const int used = api_.av_parser_parse2(parser_, context_, &unit, &unit_size, data, chunk,
                                             parser_pts, parser_dts, 0);
      if (used < 0) return false;
      data += used;
      remaining -= static_cast<std::size_t>(used);
      parser_pts = parser_dts = AV_NOPTS_VALUE;
      if (unit_size > 0 && !submitUnit(unit, unit_size)) ok = false;
      if (used == 0 && unit_size == 0) break;
    }
    return ok;
  }

  bool receive(VideoFrame& out) override {
    if (!pending_.empty()) {
      out = std::move(pending_.front());
      pending_.pop_front();
      return true;
    }
    return receiveDecoded(out);
  }

  void flush() override {
    if (draining_ || parser_ == nullptr) return;
    // The parser holds the last access unit until it sees the next start code.
    std::uint8_t* unit = nullptr;
    int unit_size = 0;
    api_.av_parser_parse2(parser_, context_, &unit, &unit_size, nullptr, 0, AV_NOPTS_VALUE,
                          AV_NOPTS_VALUE, 0);
    if (unit_size > 0) submitUnit(unit, unit_size);
    // Only a decoder that accepted the end-of-stream packet has an end to wait for.
    if (sendPacket(nullptr)) draining_ = true;
  }

 private:
  // The parser's output may point into the caller's buffer, so the unit is copied into a packet
  // that owns padded memory.
  bool submitUnit(const std::uint8_t* unit, int size) {
    if (api_.av_new_packet(packet_, size) < 0) return false;
    std::memcpy(packet_->data, unit, static_cast<std::size_t>(size));
    packet_->pts = parser_->pts;
    packet_->dts = parser_->dts;
    const bool ok = sendPacket(packet_);
    api_.av_packet_unref(packet_);
    return ok;
  }

  bool sendPacket(const AVPacket* packet) {
    for (;;) {
      const int result = api_.avcodec_send_packet(context_, packet);
      if (result == 0) return true;
      if (result != AVERROR(EAGAIN)) {
        logDebug("ffmpeg video: send_packet failed: {}", errorString(api_, result));
        return false;
      }
      // The decoder wants its output taken first; keep the frames for receive().
      VideoFrame frame;
      if (!receiveDecoded(frame)) return false;
      pending_.push_back(std::move(frame));
    }
  }

  // Rules for frames without a PTS: last known PTS plus whole frame durations in output order. The
  // product is rounded as a whole so that rounding does not accumulate.
  std::int64_t timestampOf(std::int64_t frame_pts) {
    if (frame_pts != AV_NOPTS_VALUE) {
      anchor_ = frame_pts;
      frames_since_anchor_ = 0;
      return frame_pts;
    }
    ++frames_since_anchor_;
    return anchor_ + (kClockHz * rate_.den * frames_since_anchor_ + rate_.num / 2) / rate_.num;
  }

  bool receiveDecoded(VideoFrame& out) {
    // A decode error belongs to one frame; the next call goes on with the following one. The bound
    // keeps a decoder that fails every time from spinning.
    constexpr int kMaxConsecutiveErrors = 16;
    int errors = 0;
    for (;;) {
      const int result = api_.avcodec_receive_frame(context_, frame_);
      if (result == AVERROR_EOF) {
        // Everything is out; make the decoder and the parser usable for another stream.
        api_.avcodec_flush_buffers(context_);
        api_.av_parser_close(parser_);
        parser_ = api_.av_parser_init(AV_CODEC_ID_H264);
        draining_ = false;
        anchor_ = format_.start_pts;
        frames_since_anchor_ = 0;
        return false;
      }
      if (result == AVERROR(EAGAIN)) return false;  // needs more input
      if (result < 0) {
        ++decode_errors_;
        logDebug("ffmpeg video: receive_frame failed: {}", errorString(api_, result));
        if (++errors >= kMaxConsecutiveErrors) return false;
        continue;
      }
      errors = 0;

      out.pts = timestampOf(frame_->pts);
      std::string error;
      const bool converted = converter_.convert(*frame_, format_, out, error);
      api_.av_frame_unref(frame_);
      if (converted) return true;
      logDebug("ffmpeg video: cannot convert a frame: {}", error);
    }
  }

  FfmpegApi api_;
  VideoOutputFormat format_;
  FrameRate rate_;
  FfmpegVideoConverter converter_;
  AVCodecContext* context_ = nullptr;
  AVCodecParserContext* parser_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  std::deque<VideoFrame> pending_;
  std::vector<std::uint8_t> padded_;  // reused across send() calls
  std::uint64_t decode_errors_ = 0;
  bool draining_ = false;
  std::int64_t anchor_;
  std::int64_t frames_since_anchor_ = 0;
};

}  // namespace

FfmpegVideoConverter::~FfmpegVideoConverter() {
  if (sws_ != nullptr) api_.sws_freeContext(sws_);
}

bool FfmpegVideoConverter::convert(const AVFrame& frame, const VideoOutputFormat& format,
                                   VideoFrame& out, std::string& error) {
  if (frame.width <= 0 || frame.height <= 0 || frame.data[0] == nullptr ||
      frame.format == AV_PIX_FMT_NONE) {
    error = "empty picture";
    return false;
  }
  const AVPixelFormat dst_format = outputPixelFormat(format.coding);
  const std::uint32_t width = format.width != 0 ? format.width : static_cast<std::uint32_t>(frame.width);
  const std::uint32_t height =
      format.height != 0 ? format.height : static_cast<std::uint32_t>(frame.height);
  // The format comes from guest memory, so it is bounded before anything is allocated.
  if (width > kMaxDimension || height > kMaxDimension) {
    error = std::format("{}x{} exceeds the {} pixel limit", width, height, kMaxDimension);
    return false;
  }
  const bool planar = format.coding == VideoCoding::Yuv420Planar;
  const std::uint32_t row_bytes = planar ? width : width * 4;
  const std::uint32_t pitch = format.pitch != 0 ? format.pitch : row_bytes;
  if (pitch < row_bytes || pitch > kMaxPitch) {
    error = std::format("pitch {} is outside {}..{} bytes", pitch, row_bytes, kMaxPitch);
    return false;
  }

  const std::uint64_t chroma_pitch = (std::uint64_t{pitch} + 1) / 2;
  const std::uint64_t chroma_height = (std::uint64_t{height} + 1) / 2;
  const std::uint64_t luma_size = std::uint64_t{pitch} * height;
  const std::uint64_t chroma_size = chroma_pitch * chroma_height;
  const std::uint64_t total_bytes = planar ? luma_size + 2 * chroma_size : luma_size;
  if (total_bytes > kMaxFrameBytes) {
    error = std::format("a frame of {} bytes exceeds the {} byte limit", total_bytes, kMaxFrameBytes);
    return false;
  }
  const std::size_t total = static_cast<std::size_t>(total_bytes);
  // Zero-filled so padding bytes are the same on every run.
  out.bytes.assign(total, 0);
  std::uint8_t* planes[4] = {out.bytes.data(), nullptr, nullptr, nullptr};
  int strides[4] = {static_cast<int>(pitch), 0, 0, 0};
  if (planar) {
    planes[1] = planes[0] + static_cast<std::size_t>(luma_size);
    planes[2] = planes[1] + static_cast<std::size_t>(chroma_size);
    strides[1] = strides[2] = static_cast<int>(chroma_pitch);
  }

  ContextKey key;
  key.src_width = frame.width;
  key.src_height = frame.height;
  key.src_format = frame.format;
  key.dst_width = static_cast<int>(width);
  key.dst_height = static_cast<int>(height);
  key.dst_format = dst_format;
  key.bt709 = format.bt709;
  key.src_full_range = frame.color_range == AVCOL_RANGE_JPEG || frame.format == AV_PIX_FMT_YUVJ420P;
  if (sws_ == nullptr || !(key == key_)) {
    sws_ = api_.sws_getCachedContext(sws_, key.src_width, key.src_height,
                                     static_cast<AVPixelFormat>(key.src_format), key.dst_width,
                                     key.dst_height, dst_format, kScaleFlags, nullptr, nullptr, nullptr);
    if (sws_ == nullptr) {
      key_ = ContextKey{};
      error = "swscale cannot convert this picture";
      return false;
    }
    if (!planar) {
      const int* matrix = api_.sws_getCoefficients(format.bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
      // Output RGB is full range; the destination matrix argument is ignored for RGB.
      if (api_.sws_setColorspaceDetails(sws_, matrix, key.src_full_range ? 1 : 0, matrix, 1, 0,
                                        1 << 16, 1 << 16) < 0) {
        key_ = ContextKey{};
        error = "swscale rejects the colour matrix for this picture";
        return false;
      }
    }
    key_ = key;
  }

  const int rows = api_.sws_scale(sws_, frame.data, frame.linesize, 0, frame.height, planes, strides);
  if (rows != static_cast<int>(height)) {
    error = std::format("swscale produced {} of {} rows", rows, height);
    return false;
  }

  // swscale writes 0xff for pictures without alpha.
  if (!planar && format.alpha != 0xff) {
    const std::size_t offset = format.coding == VideoCoding::Argb ? 0 : 3;
    for (std::uint32_t y = 0; y < height; ++y) {
      std::uint8_t* row = planes[0] + std::size_t{pitch} * y + offset;
      for (std::uint32_t x = 0; x < width; ++x) row[std::size_t{x} * 4] = format.alpha;
    }
  }
  out.width = width;
  out.height = height;
  out.pitch = pitch;
  return true;
}

std::unique_ptr<VideoDecoder> FfmpegCodecFactory::createVideo(const PamfStream& stream,
                                                              const VideoOutputFormat& format,
                                                              std::string& error) {
  if (stream.codec != PamfCodec::Avc) {
    error = "only AVC video is supported";
    return nullptr;
  }
  auto decoder = std::make_unique<FfmpegVideoDecoder>(api_, format, frameRateOf(stream));
  if (!decoder->open(options_.video_threads, error)) return nullptr;
  return decoder;
}

std::unique_ptr<AudioDecoder> FfmpegCodecFactory::createAudio(const PamfStream&,
                                                              const AudioOutputFormat&,
                                                              std::string& error) {
  error = "audio decoding is not implemented";
  return nullptr;
}

std::unique_ptr<MediaCodecFactory> createFfmpegCodecFactory(std::string& error,
                                                            const FfmpegCodecOptions& options) {
  const auto& api = defaultFfmpeg();
  if (!api) {
    error = api.error();
    return nullptr;
  }
  return std::make_unique<FfmpegCodecFactory>(*api, options);
}

}  // namespace dal
