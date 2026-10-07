#pragma once

// PAMF container: header parser and incremental MPEG-PS demuxer. Pure host code with no guest or
// codec dependency. Only the interfaces are declared here; the implementation is separate.

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dal {

// The values equal the cellPamf stream-type numbers of the codec-specific selectors.
enum class PamfCodec : std::uint8_t { Avc = 0, M2v = 1, Atrac3plus = 2, Lpcm = 3, Ac3 = 4, UserData = 5 };

struct PamfStream {
  PamfCodec codec = PamfCodec::UserData;
  std::uint8_t stream_id = 0;          // MPEG-PS PES stream_id
  std::uint8_t private_stream_id = 0;  // sub-stream id inside private stream 1, when used
  std::uint8_t channel = 0;
  std::uint32_t width = 0, height = 0;          // video, from the header
  std::uint32_t sample_rate = 0, channels = 0;  // audio
  std::vector<std::uint8_t> codec_info;         // raw per-codec info block
};

struct PamfInfo {
  std::uint32_t header_size = 0, data_offset = 0, data_size = 0;
  std::uint64_t start_pts = 0, end_pts = 0;  // 90 kHz
  std::uint32_t mux_rate_bound = 0;
  std::vector<PamfStream> streams;
};

// Parses the PAMF header (the first `header_size` bytes of the file). Returns a message on failure.
std::expected<PamfInfo, std::string> parsePamfHeader(std::span<const std::uint8_t> header);

struct PamfAccessUnit {
  std::uint32_t stream_index = 0;  // index into PamfInfo::streams
  std::int64_t pts = -1, dts = -1;  // 90 kHz, -1 if absent
  // Video: the elementary-stream bytes of one PES payload (access units need not align with PES
  // packets). Audio: one ATRAC3plus frame.
  std::vector<std::uint8_t> bytes;
};

// Incremental: feed file bytes starting at `data_offset` in any chunking, pull access units per
// stream. Damaged input is counted in errors() and skipped, never thrown.
class PamfDemuxer {
 public:
  explicit PamfDemuxer(const PamfInfo& info);
  ~PamfDemuxer();
  PamfDemuxer(const PamfDemuxer&) = delete;
  PamfDemuxer& operator=(const PamfDemuxer&) = delete;

  void feed(std::span<const std::uint8_t> bytes);
  // No more input: bytes held back for a split packet are flushed or counted as errors.
  void endOfStream();
  std::optional<PamfAccessUnit> next(std::uint32_t stream_index);
  std::uint64_t errors() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace dal
