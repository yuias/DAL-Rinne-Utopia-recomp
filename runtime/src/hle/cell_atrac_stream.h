#pragma once

// The host-side state of one cellAtrac handle: the stream buffer bookkeeping, the decode cursor and
// the loop rules. The model never touches guest memory itself; the HLE layer passes the guest bytes
// in. The game's decode thread owns both sides of the protocol (it only follows the write pointer
// and read position it is given), so the rules here are consistent rather than a copy of the
// firmware library.

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "media/at3_file.h"
#include "media/atrac_decoder.h"

namespace dal {

// RPCS3 `rpcs3/Emu/Cell/Modules/cellAtrac.h` `CellAtracError`: the values this module returns.
inline constexpr std::uint32_t kAtracErrorReadsizeOverBuffer = 0x80610311;
inline constexpr std::uint32_t kAtracErrorUnknownFormat = 0x80610312;
inline constexpr std::uint32_t kAtracErrorIllegalSamplingRate = 0x80610314;
inline constexpr std::uint32_t kAtracErrorIllegalData = 0x80610315;
inline constexpr std::uint32_t kAtracErrorNoDecoder = 0x80610321;
inline constexpr std::uint32_t kAtracErrorUnsetData = 0x80610322;
inline constexpr std::uint32_t kAtracErrorDecoderWasCreated = 0x80610323;
inline constexpr std::uint32_t kAtracErrorAllDataIsOnMemory = 0x80610341;
inline constexpr std::uint32_t kAtracErrorAddDataIsTooBig = 0x80610342;

// `remain` values meaning "nothing more to buffer" (the game only tests the sign).
inline constexpr std::int32_t kAtracRemainAllDataOnMemory = -1;
inline constexpr std::int32_t kAtracRemainNonLoopStreamOnMemory = -2;

inline constexpr std::uint32_t kAtracWorkMemoryBytes = 0x1000;
inline constexpr std::uint32_t kAtracMaxSample = kAtrac3plusFrameSamples;
inline constexpr std::uint32_t kAtracMaxChannels = 2;

struct AtracStreamInfo {
  std::uint32_t write_addr = 0;  // guest address the next file bytes go to
  std::uint32_t writable = 0;    // contiguous bytes there; 0 when nothing can be read now
  std::uint32_t read_pos = 0;    // file offset of the byte at write_addr
};

struct AtracDecodeResult {
  std::uint32_t samples = 0;  // frames written to the output, at most kAtracMaxSample
  bool finish = false;
  std::int32_t remain = 0;
};

struct AtracSoundInfo {
  std::int32_t end_sample = 0;  // last sample index of the valid audio
  std::int32_t loop_start = 0;  // in valid-audio coordinates, 0 without a loop
  std::int32_t loop_end = 0;
};

class AtracStream {
 public:
  // `head` is the first read_bytes bytes of the file (the RIFF header at byte 0), which sit at
  // `buffer_addr`. read_bytes == buffer_bytes is memory mode: the whole file is in the buffer.
  // Otherwise the buffer is a ring of buffer_bytes in which file byte f lives at f % buffer_bytes.
  // On failure returns the cellAtrac error code and describes the problem in `error`.
  static std::expected<AtracStream, std::uint32_t> open(std::span<const std::uint8_t> head,
                                                        std::uint32_t buffer_addr,
                                                        std::uint32_t buffer_bytes, std::string& error);

  const At3Info& info() const { return info_; }
  std::uint32_t bufferAddr() const { return buffer_addr_; }
  std::uint32_t bufferBytes() const { return buffer_bytes_; }
  bool memoryMode() const { return memory_mode_; }
  std::uint32_t frames() const { return frames_; }

  // The decoder may be null: Decode then outputs silence of the right length. kAtracErrorDecoderWasCreated
  // when called twice.
  std::uint32_t createDecoder(std::unique_ptr<AtracFrameDecoder> decoder);
  bool decoderCreated() const { return created_; }

  std::int32_t remain() const;
  void setLoopNum(std::int32_t count) { loop_num_ = count; }
  std::int32_t loopNum() const { return loop_num_; }
  std::uint32_t loopStatus() const { return info_.loop ? 1 : 0; }
  AtracSoundInfo soundInfo() const;

  // Streaming only (kAtracErrorAllDataIsOnMemory in memory mode).
  std::expected<AtracStreamInfo, std::uint32_t> streamDataInfo();
  // Commits n bytes at the write pointer of the last streamDataInfo(). Bytes of a read that was
  // requested before a loop seek are dropped (success), because they no longer match the position.
  std::uint32_t addStreamData(std::uint32_t n);

  // Decodes the next frame that has output into `out` (host floats, interleaved, room for
  // kAtracMaxSample * channels). `ring` is the guest buffer, bufferBytes() long. Starvation is
  // success with no samples; the error codes are for a bad handle state only.
  std::expected<AtracDecodeResult, std::uint32_t> decode(std::span<const std::uint8_t> ring,
                                                         std::span<float> out);

  std::uint64_t decodeErrors() const { return decode_errors_; }
  std::uint64_t framesDecoded() const { return frames_decoded_; }

 private:
  AtracStream() = default;

  std::uint64_t frameOffset(std::uint32_t index) const {
    return std::uint64_t{info_.data_offset} + std::uint64_t{index} * info_.block_align;
  }
  bool loopBackPending() const;
  std::uint64_t passEnd() const;  // exclusive output position where the current pass ends
  std::uint32_t writableNow() const;
  bool frameBuffered(std::uint32_t index) const;
  void gatherFrame(std::span<const std::uint8_t> ring, std::uint32_t index);
  void seekToLoopStart();

  At3Info info_;
  std::uint32_t buffer_addr_ = 0, buffer_bytes_ = 0;
  bool memory_mode_ = false;
  std::uint32_t frames_ = 0;       // complete frames the file (or the memory buffer) holds
  std::uint64_t file_end_ = 0;     // file offset after the last frame
  std::uint64_t valid_total_ = 0;  // samples of valid audio
  std::uint64_t out_end_ = 0;      // exclusive output position of the valid audio's end

  std::uint64_t buffered_end_ = 0;  // file offset of the next byte to be written
  std::uint32_t cursor_ = 0;        // next frame to decode
  std::uint64_t emit_from_ = 0;     // output before this position is discarded
  std::int32_t loop_num_ = 0;
  bool created_ = false, finished_ = false;
  // A loop seek discards the buffered bytes; a read requested earlier (stream_epoch_ differs from
  // info_epoch_) carries bytes from the old position.
  std::uint32_t stream_epoch_ = 0, info_epoch_ = 0;

  std::unique_ptr<AtracFrameDecoder> decoder_;
  std::vector<std::uint8_t> frame_bytes_;
  std::vector<float> frame_samples_;
  std::uint64_t decode_errors_ = 0, frames_decoded_ = 0;
};

}  // namespace dal
