#pragma once

// FFmpeg implementation of the movie codec interfaces (see media/movie_decoder.h). Everything goes
// through the run-time loader in media/ffmpeg_api.h, so building this needs the FFmpeg headers only.

#include <memory>
#include <string>

#include "media/ffmpeg_api.h"
#include "media/movie_decoder.h"

namespace dal {

struct FfmpegCodecOptions {
  // H.264 frame-thread count; 0 picks min(4, hardware threads). More threads delay the first output
  // frame but do not change the decoded pictures.
  int video_threads = 0;
};

// Converts decoded pictures into the layout the game asked for. Holds a cached swscale context, so
// one instance serves one stream on one thread.
class FfmpegVideoConverter {
 public:
  explicit FfmpegVideoConverter(const FfmpegApi& api) : api_(api) {}
  ~FfmpegVideoConverter();
  FfmpegVideoConverter(const FfmpegVideoConverter&) = delete;
  FfmpegVideoConverter& operator=(const FfmpegVideoConverter&) = delete;

  // `format.width`/`height` of 0 keep the decoded size, `pitch` of 0 packs the rows tightly (the
  // luma pitch for Yuv420Planar). `out.pts` is left alone. Planar output is the Y plane
  // (pitch x height) followed by the U and V planes (each (pitch + 1) / 2 x (height + 1) / 2, same
  // order), and the picture's own range is kept. ARGB/RGBA output is full-range RGB converted with
  // BT.601 or BT.709 per `format.bt709`; the source range comes from the picture. False with a
  // reason in `error` for an unsupported picture or a pitch smaller than a row.
  bool convert(const AVFrame& frame, const VideoOutputFormat& format, VideoFrame& out,
               std::string& error);

 private:
  struct ContextKey {
    int src_width = 0, src_height = 0, src_format = -1;
    int dst_width = 0, dst_height = 0, dst_format = -1;
    bool bt709 = false, src_full_range = false;
    bool operator==(const ContextKey&) const = default;
  };

  FfmpegApi api_;
  SwsContext* sws_ = nullptr;
  ContextKey key_;
};

class FfmpegCodecFactory final : public MediaCodecFactory {
 public:
  explicit FfmpegCodecFactory(const FfmpegApi& api, const FfmpegCodecOptions& options = {})
      : api_(api), options_(options) {}

  std::unique_ptr<VideoDecoder> createVideo(const PamfStream& stream, const VideoOutputFormat& format,
                                            std::string& error) override;
  // Not available yet: reports an error so the player falls back to silence.
  std::unique_ptr<AudioDecoder> createAudio(const PamfStream& stream, const AudioOutputFormat& format,
                                            std::string& error) override;

 private:
  FfmpegApi api_;
  FfmpegCodecOptions options_;
};

// A factory over the FFmpeg DLLs next to the executable; null with the reason in `error` when they
// cannot be loaded.
std::unique_ptr<MediaCodecFactory> createFfmpegCodecFactory(std::string& error,
                                                            const FfmpegCodecOptions& options = {});

}  // namespace dal
