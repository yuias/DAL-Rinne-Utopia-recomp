// Self-checking tests for the audio core: the output interface, the null output, the block ring, the
// WAV writer, reader and level statistics, and the engine with its block clock. No sound device is
// involved.
//
// With a directory argument the WAV files the tests write are kept there (for dal_audiotool
// wavstat); without one they go to a temporary directory that is removed at the end.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/audio_output.h"
#include "audio/wav_file.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "dal/ppu_context.h"
#include "hle/cell_audio_ports.h"
#include "hle/hle.h"
#include "lv2/sys_sync.h"

namespace {

namespace fs = std::filesystem;

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

constexpr double kInf = std::numeric_limits<double>::infinity();

bool isNear(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

fs::path g_dir;

// Frame n of a ramp: left n, right -n. Exact in float up to 2^24 frames.
void fillRamp(std::span<float> block, std::uint64_t first_frame) {
  for (std::size_t i = 0; i < block.size() / 2; ++i) {
    const float value = static_cast<float>(first_frame + i);
    block[2 * i] = value;
    block[2 * i + 1] = -value;
  }
}

bool isRamp(std::span<const float> frames, std::uint64_t first_frame) {
  for (std::size_t i = 0; i < frames.size() / 2; ++i) {
    const float value = static_cast<float>(first_frame + i);
    if (frames[2 * i] != value || frames[2 * i + 1] != -value) return false;
  }
  return true;
}

// 1 kHz sine at 48 kHz (a whole number of periods per second). The phase offset keeps every sample
// away from zero, so all frames count as non-silent.
float toneSample(std::uint64_t frame, float amplitude) {
  return amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 *
                                                     static_cast<double>(frame) / 48000.0 + 0.3));
}

bool pushRamp(dal::AudioBlockRing& ring, std::uint64_t first_frame) {
  dal::AudioBlock block;
  fillRamp(block, first_frame);
  return ring.push(block);
}

void testRingPushPop() {
  dal::AudioBlockRing ring(4);
  CHECK(ring.queuedFrames() == 0);

  std::vector<float> out(2 * 1024);
  CHECK(ring.pop(out, 100) == 0);

  for (std::uint64_t b = 0; b < 4; ++b) CHECK(pushRamp(ring, b * dal::kAudioBlockFrames));
  CHECK(ring.queuedFrames() == 4 * dal::kAudioBlockFrames);
  // Full: the block is refused and nothing changes.
  CHECK(!pushRamp(ring, 4 * dal::kAudioBlockFrames));
  CHECK(ring.queuedFrames() == 4 * dal::kAudioBlockFrames);

  // Partial reads cross block boundaries and continue where the last one stopped.
  std::uint64_t next = 0;
  const std::size_t chunks[] = {100, 1, 155, 256, 300, 212};
  for (const std::size_t chunk : chunks) {
    CHECK(ring.pop(out, chunk) == chunk);
    CHECK(isRamp(std::span<const float>(out).first(chunk * 2), next));
    next += chunk;
  }
  CHECK(next == 1024);
  CHECK(ring.queuedFrames() == 0);
  CHECK(ring.pop(out, 10) == 0);

  // A wrong-sized block is refused.
  std::vector<float> small(dal::kAudioBlockSamples - 2);
  CHECK(!ring.push(small));
  CHECK(ring.queuedFrames() == 0);

  // The destination bounds the read as well as the frame count.
  CHECK(pushRamp(ring, 1024));
  CHECK(ring.pop(std::span<float>(out).first(20), 256) == 10);
  CHECK(ring.queuedFrames() == dal::kAudioBlockFrames - 10);
  CHECK(ring.pop(out, 0) == 0);

  // A capacity of zero still holds one block.
  dal::AudioBlockRing tiny(0);
  CHECK(pushRamp(tiny, 0));
  CHECK(!pushRamp(tiny, 256));
}

void testRingWrap() {
  dal::AudioBlockRing ring(3);
  std::vector<float> out(2 * 1024);
  std::uint64_t pushed = 0;
  std::uint64_t popped = 0;
  bool ok = true;

  // Keep the ring between one and three blocks full while reads and writes drift against each other
  // across many wraps.
  for (int round = 0; round < 200; ++round) {
    while (pushRamp(ring, pushed)) pushed += dal::kAudioBlockFrames;
    const std::size_t want = 37 + static_cast<std::size_t>(round * 11 % 400);
    const std::size_t got = ring.pop(out, want);
    ok = ok && got == std::min<std::size_t>(want, pushed - popped);
    ok = ok && isRamp(std::span<const float>(out).first(got * 2), popped);
    popped += got;
    ok = ok && ring.queuedFrames() == pushed - popped;
  }
  CHECK(ok);
  CHECK(pushed > 20 * 3 * dal::kAudioBlockFrames);
}

void testRingDrop() {
  dal::AudioBlockRing ring(4);
  std::vector<float> out(2 * 1024);

  // Dropping from an empty ring does nothing.
  ring.dropOldestBlock();
  CHECK(ring.queuedFrames() == 0);
  CHECK(ring.pop(out, 100) == 0);
  CHECK(pushRamp(ring, 0));
  CHECK(ring.queuedFrames() == dal::kAudioBlockFrames);

  // The drop discards the oldest block; the count reflects it before the consumer runs.
  CHECK(pushRamp(ring, 256));
  CHECK(pushRamp(ring, 512));
  ring.dropOldestBlock();
  CHECK(ring.queuedFrames() == 2 * dal::kAudioBlockFrames);
  CHECK(ring.pop(out, 256) == 256);
  CHECK(isRamp(std::span<const float>(out).first(512), 256));
  CHECK(ring.queuedFrames() == dal::kAudioBlockFrames);

  // A drop with less than a block queued discards what is there; with a block half read it removes
  // the rest of the data it covers.
  CHECK(ring.pop(out, 100) == 100);
  CHECK(isRamp(std::span<const float>(out).first(200), 512));
  ring.dropOldestBlock();
  CHECK(ring.queuedFrames() == 0);
  CHECK(ring.pop(out, 100) == 0);

  // A drop requested on a full ring frees space once the consumer has run.
  std::uint64_t next = 768;
  for (int i = 0; i < 4; ++i, next += dal::kAudioBlockFrames) CHECK(pushRamp(ring, next));
  CHECK(!pushRamp(ring, next));
  ring.dropOldestBlock();
  ring.dropOldestBlock();
  CHECK(ring.queuedFrames() == 2 * dal::kAudioBlockFrames);
  CHECK(ring.pop(out, 1) == 1);
  CHECK(isRamp(std::span<const float>(out).first(2), 768 + 512));
  CHECK(pushRamp(ring, next));
  CHECK(ring.queuedFrames() == 2 * dal::kAudioBlockFrames - 1 + dal::kAudioBlockFrames);
}

void testRingThreaded() {
  constexpr std::uint64_t kBlocks = 3000;
  constexpr std::uint64_t kFrames = kBlocks * dal::kAudioBlockFrames;
  dal::AudioBlockRing ring(8);

  std::thread producer([&ring] {
    dal::AudioBlock block;
    for (std::uint64_t b = 0; b < kBlocks; ++b) {
      fillRamp(block, b * dal::kAudioBlockFrames);
      while (!ring.push(block)) std::this_thread::yield();
    }
  });

  bool ok = true;
  std::uint64_t consumed = 0;
  std::vector<float> out(2 * 300);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (consumed < kFrames && std::chrono::steady_clock::now() < deadline) {
    const std::size_t want = 1 + static_cast<std::size_t>(consumed * 7 % 300);
    const std::size_t got = ring.pop(out, want);
    ok = ok && isRamp(std::span<const float>(out).first(got * 2), consumed);
    consumed += got;
    if (got == 0) std::this_thread::yield();
  }
  producer.join();
  CHECK(ok);
  CHECK(consumed == kFrames);
  CHECK(ring.queuedFrames() == 0);
}

void testNullOutput() {
  const auto output = dal::makeNullAudioOutput();
  CHECK(output->name() == "none");
  CHECK(!output->description().empty());

  std::string error;
  CHECK(output->open(error));
  CHECK(error.empty());

  dal::AudioOutputStats stats = output->stats();
  CHECK(stats.blocks_submitted == 0 && stats.frames_played == 0);

  dal::AudioBlock block{};
  for (int i = 0; i < 5; ++i) output->submit(block);
  stats = output->stats();
  CHECK(stats.blocks_submitted == 5);
  CHECK(stats.frames_played == 5 * dal::kAudioBlockFrames);
  CHECK(stats.blocks_dropped == 0 && stats.underrun_frames == 0 && stats.device_errors == 0);

  // The block period is the block length at the sample rate, to the nanosecond.
  CHECK(dal::kAudioBlockPeriod.count() == 5'333'333);
  CHECK(dal::kAudioBlockSamples == 512);
}

std::vector<std::uint8_t> makePcm16Wav(std::uint16_t channels, std::uint32_t rate,
                                       std::span<const std::int16_t> samples, bool long_fmt,
                                       bool extra_chunk, std::uint32_t data_size_override = 0) {
  std::vector<std::uint8_t> bytes;
  auto put = [&bytes](const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    bytes.insert(bytes.end(), p, p + size);
  };
  auto put32 = [&](std::uint32_t v) { put(&v, 4); };
  auto put16 = [&](std::uint16_t v) { put(&v, 2); };

  put("RIFF", 4);
  put32(0);  // patched below; the reader ignores it
  put("WAVE", 4);
  if (extra_chunk) {
    // Odd size: the chunk is followed by a pad byte the reader must skip.
    put("LIST", 4);
    put32(5);
    put("abcde", 5);
    put("\0", 1);
  }
  put("fmt ", 4);
  put32(long_fmt ? 18 : 16);
  put16(1);
  put16(channels);
  put32(rate);
  put32(rate * channels * 2);
  put16(static_cast<std::uint16_t>(channels * 2));
  put16(16);
  if (long_fmt) put16(0);
  put("data", 4);
  put32(data_size_override != 0 ? data_size_override
                                : static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t)));
  put(samples.data(), samples.size() * sizeof(std::int16_t));
  const std::uint32_t riff = static_cast<std::uint32_t>(bytes.size() - 8);
  std::memcpy(bytes.data() + 4, &riff, 4);
  return bytes;
}

std::vector<std::uint8_t> readFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void testWavFloatRoundTrip() {
  const fs::path path = g_dir / "roundtrip.wav";
  auto created = dal::WavWriter::create(path, 48000, 2);
  CHECK(created.has_value());
  if (!created) return;
  auto& writer = *created;

  // 30000 frames in uneven pieces, including a piece that is not a multiple of the block size.
  std::vector<float> all(2 * 30000);
  for (std::size_t i = 0; i < all.size(); ++i) {
    all[i] = std::sin(static_cast<float>(i) * 0.01f) * 0.8f;
  }
  const std::size_t pieces[] = {256, 1, 4999, 12345, 30000 - 256 - 1 - 4999 - 12345};
  std::size_t offset = 0;
  bool wrote = true;
  for (const std::size_t frames : pieces) {
    wrote = writer->write(std::span<const float>(all).subspan(offset * 2, frames * 2)) && wrote;
    offset += frames;
  }
  CHECK(wrote);
  CHECK(writer->framesWritten() == 30000);
  CHECK(writer->finish());

  const auto wav = dal::readWav(path);
  CHECK(wav.has_value());
  if (wav) {
    CHECK(wav->sample_rate == 48000 && wav->channels == 2);
    CHECK(wav->frames() == 30000);
    CHECK(wav->samples == all);
  }
  CHECK(readFile(path).size() == 58 + 30000 * 8);

  // finish() is idempotent; writes after it are refused and leave the file alone.
  CHECK(writer->finish());
  CHECK(!writer->write(std::span<const float>(all).first(512)));
  CHECK(writer->framesWritten() == 30000);
  CHECK(readFile(path).size() == 58 + 30000 * 8);

  // The writer's header follows the layout other tools expect for float WAV.
  const auto bytes = readFile(path);
  CHECK(std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WAVE", 4) == 0);
  std::uint16_t format = 0;
  std::uint32_t riff_size = 0;
  std::uint32_t data_size = 0;
  std::memcpy(&riff_size, bytes.data() + 4, 4);
  std::memcpy(&format, bytes.data() + 20, 2);
  std::memcpy(&data_size, bytes.data() + 54, 4);
  CHECK(format == 3);
  CHECK(riff_size == bytes.size() - 8);
  CHECK(data_size == 30000 * 8);
}

void testWavHeaderValidWithoutFinish() {
  const fs::path path = g_dir / "unfinished.wav";
  auto created = dal::WavWriter::create(path, 48000, 2);
  CHECK(created.has_value());
  if (!created) return;
  auto& writer = *created;

  // A file that was never written to is a valid empty WAV.
  auto empty = dal::readWav(path);
  CHECK(empty.has_value() && empty->frames() == 0 && empty->channels == 2 && empty->sample_rate == 48000);

  std::uint64_t frame = 0;
  auto writeFrames = [&](std::uint64_t count) {
    std::vector<float> block(2 * count);
    fillRamp(block, frame);
    frame += count;
    return writer->write(block);
  };

  // Before the first patch the header still describes an empty file.
  CHECK(writeFrames(40000));
  auto early = dal::readWav(path);
  CHECK(early.has_value() && early->frames() == 0);

  // After the patch at 48000 frames the file holds exactly that many, whatever was written since
  // sits only in the stream's buffer (what a std::_Exit leaves behind).
  bool ok = true;
  for (int i = 0; i < 100 && ok; ++i) ok = writeFrames(256);
  CHECK(ok);
  CHECK(writer->framesWritten() == 40000 + 100 * 256);
  auto cut = dal::readWav(path);
  CHECK(cut.has_value());
  if (cut) {
    CHECK(cut->frames() == 48000);
    CHECK(isRamp(cut->samples, 0));
  }

  CHECK(writeFrames(48000));
  cut = dal::readWav(path);
  CHECK(cut.has_value() && cut->frames() == 96000 && isRamp(cut->samples, 0));

  CHECK(writer->finish());
  const auto done = dal::readWav(path);
  CHECK(done.has_value() && done->frames() == 40000 + 100 * 256 + 48000 && isRamp(done->samples, 0));
}

void testWavWriterErrors() {
  CHECK(!dal::WavWriter::create(g_dir / "no_such_dir" / "x.wav", 48000, 2).has_value());
  CHECK(!dal::WavWriter::create(g_dir / "bad_format.wav", 0, 2).has_value());
  CHECK(!dal::WavWriter::create(g_dir / "bad_format.wav", 48000, 0).has_value());

  // Samples that are not whole frames are refused without being written.
  auto created = dal::WavWriter::create(g_dir / "odd.wav", 48000, 2);
  CHECK(created.has_value());
  if (!created) return;
  const float three[3] = {0.1f, 0.2f, 0.3f};
  CHECK(!(*created)->write(three));
  CHECK((*created)->framesWritten() == 0);
  CHECK((*created)->write(std::span<const float>(three).first(2)));
  CHECK((*created)->framesWritten() == 1);

  // The destructor finishes the file.
  created->reset();
  const auto wav = dal::readWav(g_dir / "odd.wav");
  CHECK(wav.has_value() && wav->frames() == 1 && wav->samples[0] == 0.1f && wav->samples[1] == 0.2f);

  // A mono writer counts frames, not samples.
  auto mono = dal::WavWriter::create(g_dir / "mono.wav", 44100, 1);
  CHECK(mono.has_value());
  if (!mono) return;
  CHECK((*mono)->write(three));
  CHECK((*mono)->framesWritten() == 3);
  CHECK((*mono)->finish());
  const auto mono_wav = dal::readWav(g_dir / "mono.wav");
  CHECK(mono_wav.has_value() && mono_wav->channels == 1 && mono_wav->sample_rate == 44100 &&
        mono_wav->frames() == 3);
}

void testWavReadPcm16() {
  const std::int16_t stereo[] = {0, 16384, -16384, -32768, 32767, 1};

  for (const bool long_fmt : {false, true}) {
    for (const bool extra : {false, true}) {
      const auto bytes = makePcm16Wav(2, 44100, stereo, long_fmt, extra);
      const auto wav = dal::parseWav(bytes);
      CHECK(wav.has_value());
      if (!wav) continue;
      CHECK(wav->sample_rate == 44100 && wav->channels == 2 && wav->frames() == 3);
      CHECK(wav->samples.size() == 6);
      CHECK(wav->samples[0] == 0.0f && wav->samples[1] == 0.5f && wav->samples[2] == -0.5f);
      CHECK(wav->samples[3] == -1.0f && wav->samples[5] == 1.0f / 32768.0f);
      CHECK(isNear(wav->samples[4], 32767.0 / 32768.0, 1e-9));
    }
  }

  const auto mono = dal::parseWav(makePcm16Wav(1, 22050, stereo, false, false));
  CHECK(mono.has_value() && mono->channels == 1 && mono->frames() == 6 && mono->sample_rate == 22050);

  // A data size past the end of the buffer (a file cut off mid-write) reads what is present; a
  // trailing partial frame is dropped.
  const auto cut = dal::parseWav(makePcm16Wav(2, 48000, stereo, false, false, 1000));
  CHECK(cut.has_value() && cut->frames() == 3);
  const std::int16_t odd[] = {1, 2, 3, 4, 5};
  const auto partial = dal::parseWav(makePcm16Wav(2, 48000, odd, false, false));
  CHECK(partial.has_value() && partial->frames() == 2);
}

void testWavReadRejects() {
  const std::int16_t samples[] = {1, 2, 3, 4};
  CHECK(!dal::parseWav({}).has_value());
  CHECK(!dal::readWav(g_dir / "does_not_exist.wav").has_value());

  auto bytes = makePcm16Wav(2, 48000, samples, false, false);
  auto damaged = bytes;
  damaged[0] = 'X';
  CHECK(!dal::parseWav(damaged).has_value());
  damaged = bytes;
  damaged[8] = 'X';
  CHECK(!dal::parseWav(damaged).has_value());

  // fmt cut off, and no data chunk.
  CHECK(!dal::parseWav(std::span<const std::uint8_t>(bytes).first(20)).has_value());
  CHECK(!dal::parseWav(std::span<const std::uint8_t>(bytes).first(36)).has_value());

  // Unsupported sample formats and channel counts: 8-bit PCM, 24-bit PCM, float64, 3 channels.
  // The fmt payload starts at offset 20; bits per sample sits 14 bytes into it.
  auto with = [&](std::size_t offset, std::uint16_t value) {
    auto copy = bytes;
    std::memcpy(copy.data() + offset, &value, 2);
    return copy;
  };
  CHECK(!dal::parseWav(with(20 + 14, 8)).has_value());
  CHECK(!dal::parseWav(with(20 + 14, 24)).has_value());
  CHECK(!dal::parseWav(with(20, 3)).has_value());   // float tag with 16-bit samples
  CHECK(!dal::parseWav(with(20, 0xFFFE)).has_value());
  CHECK(!dal::parseWav(with(20 + 2, 3)).has_value());
  CHECK(!dal::parseWav(with(20 + 2, 0)).has_value());
}

void testWavStatsTone() {
  // One second of a 0.5-amplitude sine: RMS is 0.5 / sqrt(2), the peak is just under 0.5.
  std::vector<float> tone(2 * 48000);
  for (std::uint64_t i = 0; i < 48000; ++i) {
    tone[2 * i] = tone[2 * i + 1] = toneSample(i, 0.5f);
  }
  auto stats = dal::wavStats(tone, 48000, 2, 1.0);
  CHECK(stats.size() == 1);
  if (stats.size() == 1) {
    CHECK(stats[0].start_frame == 0 && stats[0].end_frame == 48000);
    CHECK(isNear(stats[0].rms_db, 20 * std::log10(0.5 / std::numbers::sqrt2), 0.01));
    CHECK(isNear(stats[0].peak_db, 20 * std::log10(0.5), 0.05));
    CHECK(stats[0].non_silent_frames == 48000);
  }

  // The same signal on the left channel only: RMS is taken over both channels, the peak is unchanged.
  std::vector<float> left_only = tone;
  for (std::size_t i = 1; i < left_only.size(); i += 2) left_only[i] = 0;
  stats = dal::wavStats(left_only, 48000, 2, 1.0);
  CHECK(stats.size() == 1);
  if (stats.size() == 1) {
    CHECK(isNear(stats[0].rms_db, 20 * std::log10(0.25), 0.01));
    CHECK(isNear(stats[0].peak_db, 20 * std::log10(0.5), 0.05));
    CHECK(stats[0].non_silent_frames == 48000);
  }

  // Windows of 0.25 s: four equal windows.
  stats = dal::wavStats(tone, 48000, 2, 0.25);
  CHECK(stats.size() == 4);
  for (std::size_t i = 0; i < stats.size(); ++i) {
    CHECK(stats[i].start_frame == i * 12000 && stats[i].end_frame == (i + 1) * 12000);
    CHECK(isNear(stats[i].rms_db, 20 * std::log10(0.5 / std::numbers::sqrt2), 0.01));
  }

  // A mono signal is one channel per frame.
  std::vector<float> mono(48000);
  for (std::uint64_t i = 0; i < mono.size(); ++i) mono[i] = toneSample(i, 1.0f);
  stats = dal::wavStats(mono, 48000, 1, 1.0);
  CHECK(stats.size() == 1 && stats[0].non_silent_frames == 48000 &&
        isNear(stats[0].rms_db, 20 * std::log10(1.0 / std::numbers::sqrt2), 0.01));
}

void testWavStatsSilence() {
  std::vector<float> silence(2 * 48000 + 2 * 1000);
  auto stats = dal::wavStats(silence, 48000, 2, 1.0);
  CHECK(stats.size() == 2);
  if (stats.size() == 2) {
    // The last window covers the remainder.
    CHECK(stats[1].start_frame == 48000 && stats[1].end_frame == 49000);
    for (const auto& w : stats) {
      CHECK(w.rms_db == -kInf && w.peak_db == -kInf && w.non_silent_frames == 0);
    }
  }

  // The threshold is 1e-4 on any channel: at it is silent, above it is not.
  std::vector<float> faint(2 * 100);
  for (std::size_t i = 0; i < 10; ++i) faint[2 * i] = 1e-4f;           // at the threshold
  for (std::size_t i = 10; i < 30; ++i) faint[2 * i + 1] = 2e-4f;      // right channel only
  for (std::size_t i = 30; i < 35; ++i) faint[2 * i] = -3e-4f;         // negative counts too
  stats = dal::wavStats(faint, 48000, 2, 1.0);
  CHECK(stats.size() == 1 && stats[0].non_silent_frames == 25);
  CHECK(stats.size() == 1 && isNear(stats[0].peak_db, 20 * std::log10(3e-4), 0.01));

  // Degenerate input produces no windows.
  CHECK(dal::wavStats({}, 48000, 2, 1.0).empty());
  CHECK(dal::wavStats(silence, 0, 2, 1.0).empty());
  CHECK(dal::wavStats(silence, 48000, 0, 1.0).empty());
  CHECK(dal::wavStats(silence, 48000, 2, 0.0).empty());
  CHECK(dal::wavStats(silence, 48000, 2, -1.0).empty());
  CHECK(dal::wavStats(silence, 48000, 2, 1e-9).empty());
  CHECK(dal::wavStats(silence, 48000, 2, 1e30).size() == 1);
}

// The file dal_audiotool wavstat is checked against by hand (and by anyone reading the docs):
//   0-1 s    stereo tone, amplitude 0.5
//   1-2 s    stereo tone, amplitude 0.05
//   2-3 s    silence
//   3-3.5 s  tone, amplitude 0.5, left channel only
void testWavStatsFile() {
  const fs::path path = g_dir / "wavstat_sample.wav";
  auto created = dal::WavWriter::create(path, 48000, 2);
  CHECK(created.has_value());
  if (!created) return;

  std::vector<float> all(2 * 168000);
  for (std::uint64_t i = 0; i < 168000; ++i) {
    float left = 0;
    float right = 0;
    if (i < 48000) {
      left = right = toneSample(i, 0.5f);
    } else if (i < 96000) {
      left = right = toneSample(i, 0.05f);
    } else if (i >= 144000) {
      left = toneSample(i, 0.5f);
    }
    all[2 * i] = left;
    all[2 * i + 1] = right;
  }
  // Written in cellAudio-sized blocks, like the engine will.
  bool wrote = true;
  for (std::size_t frame = 0; frame < 168000; frame += dal::kAudioBlockFrames) {
    const std::size_t frames = std::min<std::size_t>(dal::kAudioBlockFrames, 168000 - frame);
    wrote = (*created)->write(std::span<const float>(all).subspan(frame * 2, frames * 2)) && wrote;
  }
  CHECK(wrote);
  CHECK((*created)->finish());

  const auto wav = dal::readWav(path);
  CHECK(wav.has_value() && wav->frames() == 168000 && wav->samples == all);
  if (!wav) return;
  const auto stats = dal::wavStats(wav->samples, wav->sample_rate, wav->channels, 1.0);
  CHECK(stats.size() == 4);
  if (stats.size() != 4) return;
  CHECK(isNear(stats[0].rms_db, -9.03, 0.01) && isNear(stats[0].peak_db, -6.02, 0.05) &&
        stats[0].non_silent_frames == 48000);
  CHECK(isNear(stats[1].rms_db, -29.03, 0.01) && isNear(stats[1].peak_db, -26.02, 0.05) &&
        stats[1].non_silent_frames == 48000);
  CHECK(stats[2].rms_db == -kInf && stats[2].peak_db == -kInf && stats[2].non_silent_frames == 0);
  CHECK(stats[3].end_frame - stats[3].start_frame == 24000);
  CHECK(isNear(stats[3].rms_db, -12.04, 0.01) && isNear(stats[3].peak_db, -6.02, 0.05) &&
        stats[3].non_silent_frames == 24000);
}

void testBlockClock() {
  using Clock = dal::BlockClock::Clock;
  using namespace std::chrono_literals;
  const Clock::time_point start{};
  const auto period = dal::kAudioBlockPeriod;
  CHECK(dal::kMaxCatchUpBlocks == 0);

  dal::BlockClock clock(start);
  CHECK(clock.nextDue() == start + period);

  // Before the first period is due nothing happens and the schedule stays.
  auto plan = clock.due(start + period - 1ns);
  CHECK(plan.produce == 0 && plan.skipped == 0 && clock.nextDue() == start + period);

  // On time, then a little late: one block each, no skips.
  plan = clock.due(start + period);
  CHECK(plan.produce == 1 && plan.skipped == 0 && clock.nextDue() == start + 2 * period);
  plan = clock.due(start + 2 * period + 3ms);
  CHECK(plan.produce == 1 && plan.skipped == 0 && clock.nextDue() == start + 3 * period);

  // Woken with three periods due (3p, 4p, 5p): one block, two skipped, and the schedule moves to the
  // first period after now.
  Clock::time_point now = start + 5 * period + 1ms;
  plan = clock.due(now);
  CHECK(plan.produce == 1 && plan.skipped == 2);
  CHECK(clock.nextDue() == start + 6 * period && clock.nextDue() > now);

  // Ten due.
  now = clock.nextDue() + 9 * period + 100us;
  const Clock::time_point before = clock.nextDue();
  plan = clock.due(now);
  CHECK(plan.produce == 1 && plan.skipped == 9);
  CHECK(clock.nextDue() == before + 10 * period && clock.nextDue() > now && clock.nextDue() - now <= period);

  // Just short of the next period: nothing due.
  plan = clock.due(clock.nextDue() - 1ns);
  CHECK(plan.produce == 0 && plan.skipped == 0);

  // Exactly on time for a long run: one block per period, nothing skipped.
  dal::BlockClock steady(start);
  std::uint64_t produced = 0;
  std::uint64_t skipped = 0;
  for (int i = 1; i <= 1000; ++i) {
    plan = steady.due(start + i * period);
    produced += plan.produce;
    skipped += plan.skipped;
  }
  CHECK(produced == 1000 && skipped == 0 && steady.nextDue() == start + 1001 * period);

  // A pause of an hour is one block and a large count of skipped periods.
  dal::BlockClock paused(start);
  now = start + 1h;
  plan = paused.due(now);
  const std::uint64_t due = 1 + (now - (start + period)) / period;
  CHECK(plan.produce == 1 && plan.skipped == due - 1);
  CHECK(paused.nextDue() > now && paused.nextDue() - now <= period);
}

// Everything a RecordingOutput receives, readable by the test after the engine has stopped.
struct Recording {
  std::mutex mutex;
  std::vector<float> samples;  // every submitted block back to back
  int opens = 0;
  std::chrono::milliseconds open_delay{0};  // how long open() takes, to model a slow device

  std::vector<float> take() {
    std::lock_guard<std::mutex> lock(mutex);
    return samples;
  }
};

class RecordingOutput final : public dal::AudioOutput {
 public:
  RecordingOutput(std::shared_ptr<Recording> recording, bool open_ok)
      : recording_(std::move(recording)), open_ok_(open_ok) {}

  std::string_view name() const override { return "fake"; }
  bool open(std::string& error) override {
    if (recording_->open_delay.count() > 0) std::this_thread::sleep_for(recording_->open_delay);
    std::lock_guard<std::mutex> lock(recording_->mutex);
    ++recording_->opens;
    if (!open_ok_) error = "device gone";
    return open_ok_;
  }
  void submit(std::span<const float> block) override {
    std::lock_guard<std::mutex> lock(recording_->mutex);
    recording_->samples.insert(recording_->samples.end(), block.begin(), block.end());
    submitted_.fetch_add(1);
  }
  std::string description() const override { return "recording"; }
  dal::AudioOutputStats stats() const override {
    dal::AudioOutputStats stats;
    stats.blocks_submitted = submitted_.load();
    return stats;
  }

 private:
  std::shared_ptr<Recording> recording_;
  bool open_ok_;
  std::atomic<std::uint64_t> submitted_{0};
};

dal::AudioEngineConfig engineConfig(const std::shared_ptr<Recording>& recording, bool open_ok,
                                    fs::path dump) {
  dal::AudioEngineConfig config;
  config.make_output = [recording, open_ok] { return std::make_unique<RecordingOutput>(recording, open_ok); };
  config.dump = std::move(dump);
  return config;
}

// A producer that numbers its calls: call c fills block frames c * 256 + 1 ... as a ramp, so every
// produced block is non-silent and says which call made it. Optionally every fourth call leaves the
// block silent, and one call can stall to make the engine wake late.
struct ProducerState {
  std::atomic<std::uint64_t> calls{0};
  bool silent_every_fourth = false;
  std::uint64_t stall_call = ~std::uint64_t{0};
  std::chrono::milliseconds stall{0};
};

dal::AudioBlockProducer makeProducer(const std::shared_ptr<ProducerState>& state) {
  return [state](std::span<float> block) {
    const std::uint64_t call = state->calls.fetch_add(1);
    if (call == state->stall_call) std::this_thread::sleep_for(state->stall);
    if (state->silent_every_fourth && call % 4 == 3) return;
    fillRamp(block, call * dal::kAudioBlockFrames + 1);
  };
}

template <typename Predicate>
bool waitFor(Predicate done, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

struct RecordedBlocks {
  std::size_t total = 0;
  std::size_t silent = 0;
  std::vector<std::uint64_t> tags;  // producer call of each non-silent block, in order
  bool ramps_intact = true;
};

RecordedBlocks analyse(std::span<const float> samples) {
  RecordedBlocks result;
  result.total = samples.size() / dal::kAudioBlockSamples;
  for (std::size_t b = 0; b < result.total; ++b) {
    const auto block = samples.subspan(b * dal::kAudioBlockSamples, dal::kAudioBlockSamples);
    if (std::all_of(block.begin(), block.end(), [](float s) { return s == 0.0f; })) {
      ++result.silent;
      continue;
    }
    const auto tag = static_cast<std::uint64_t>((static_cast<double>(block[0]) - 1.0) / dal::kAudioBlockFrames);
    result.tags.push_back(tag);
    result.ramps_intact = result.ramps_intact && isRamp(block, tag * dal::kAudioBlockFrames + 1);
  }
  return result;
}

double periodsToMs(std::uint64_t periods) {
  return static_cast<double>(periods) * static_cast<double>(dal::kAudioBlockPeriod.count()) / 1e6;
}

void testEngineBlocksAndDump() {
  using Clock = std::chrono::steady_clock;
  const fs::path dump_path = g_dir / "engine_dump.wav";
  const auto recording = std::make_shared<Recording>();
  const auto state = std::make_shared<ProducerState>();
  state->silent_every_fourth = true;
  const auto other = std::make_shared<ProducerState>();

  dal::configureAudioEngine(engineConfig(recording, true, dump_path));
  const Clock::time_point t0 = Clock::now();
  dal::startAudioEngine(makeProducer(state));
  dal::startAudioEngine(makeProducer(other));  // already running: ignored
  CHECK(waitFor([&] { return state->calls.load() >= 40; }));
  const Clock::time_point t_stop = Clock::now();

  const dal::AudioEngineStats running = dal::audioEngineStats();
  CHECK(running.output == "fake");

  dal::AudioEngineStats stats;
  CHECK(dal::stopAudioEngineForTest(&stats));
  const Clock::time_point t1 = Clock::now();
  CHECK(other->calls.load() == 0);

  const std::uint64_t calls = state->calls.load();
  CHECK(stats.output == "fake");
  CHECK(stats.blocks == calls);
  const std::vector<float> samples = recording->take();
  const RecordedBlocks recorded = analyse(samples);
  CHECK(recorded.total == stats.blocks + stats.skipped_blocks);
  CHECK(stats.output_stats.blocks_submitted == recorded.total);
  CHECK(recording->opens == 1);

  // Every fourth produced block is silent; the rest arrive in call order, unchanged.
  std::vector<std::uint64_t> expected_tags;
  for (std::uint64_t c = 0; c < calls; ++c) {
    if (c % 4 != 3) expected_tags.push_back(c);
  }
  CHECK(recorded.tags == expected_tags);
  CHECK(recorded.ramps_intact);
  CHECK(stats.non_silent_blocks == expected_tags.size());
  CHECK(recorded.silent == calls / 4 + stats.skipped_blocks);

  // The dump holds exactly what the output received.
  const auto wav = dal::readWav(dump_path);
  CHECK(wav.has_value());
  if (wav) {
    CHECK(wav->sample_rate == dal::kAudioSampleRate && wav->channels == dal::kAudioOutputChannels);
    CHECK(wav->samples == samples);
  }

  // The periods that elapsed are all accounted for as blocks or skips.
  const double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double until_stop_ms = std::chrono::duration<double, std::milli>(t_stop - t0).count();
  CHECK(periodsToMs(recorded.total) <= elapsed_ms);
  CHECK(periodsToMs(recorded.total) >= until_stop_ms - 80.0);

  // The engine is gone: the next test starts from a clean slate.
  CHECK(dal::audioEngineStats().blocks == 0 && dal::audioEngineStats().output.empty());
}

void testEngineSkipsLatePeriods() {
  const fs::path dump_path = g_dir / "engine_skip.wav";
  const auto recording = std::make_shared<Recording>();
  const auto state = std::make_shared<ProducerState>();
  state->stall_call = 5;
  state->stall = std::chrono::milliseconds(25);  // 4.7 periods

  dal::configureAudioEngine(engineConfig(recording, true, dump_path));
  dal::startAudioEngine(makeProducer(state));
  CHECK(waitFor([&] { return state->calls.load() >= 20; }));
  dal::AudioEngineStats stats;
  CHECK(dal::stopAudioEngineForTest(&stats));

  const std::uint64_t calls = state->calls.load();
  const std::vector<float> samples = recording->take();
  const RecordedBlocks recorded = analyse(samples);

  // The producer ran once per produced block, never for a skipped period; every skipped period is a
  // silent block in the output and the dump.
  CHECK(stats.blocks == calls);
  CHECK(stats.skipped_blocks >= 3);
  CHECK(stats.max_producer_time >= std::chrono::milliseconds(20));
  CHECK(recorded.total == stats.blocks + stats.skipped_blocks);
  CHECK(recorded.silent == stats.skipped_blocks);
  CHECK(stats.non_silent_blocks == calls);
  CHECK(recorded.ramps_intact);
  bool contiguous = recorded.tags.size() == calls;
  for (std::size_t i = 0; i < recorded.tags.size(); ++i) contiguous = contiguous && recorded.tags[i] == i;
  CHECK(contiguous);

  // The silence for the missed periods sits between the stalled block (call 5) and the next one.
  std::size_t run = 0;
  std::size_t index_of_five = 0;
  for (std::size_t b = 0; b < recorded.total; ++b) {
    if (samples[b * dal::kAudioBlockSamples] == 5.0f * dal::kAudioBlockFrames + 1.0f) index_of_five = b;
  }
  for (std::size_t b = index_of_five + 1; b < recorded.total; ++b) {
    const float first = samples[b * dal::kAudioBlockSamples];
    if (first != 0.0f) break;
    ++run;
  }
  CHECK(run >= 3);

  const auto wav = dal::readWav(dump_path);
  CHECK(wav.has_value() && wav->samples == samples);
  CHECK(wav.has_value() && wav->frames() == recorded.total * dal::kAudioBlockFrames);
}

void testEngineStopDoesNotWait() {
  using Clock = std::chrono::steady_clock;
  const auto recording = std::make_shared<Recording>();
  // Shared with the producer, which can outlive this function if the engine fails to stop.
  struct Gate {
    std::mutex mutex;  // stands in for the guest lock
    std::atomic<int> calls{0};
  };
  const auto gate_state = std::make_shared<Gate>();
  gate_state->mutex.lock();
  std::mutex& gate = gate_state->mutex;
  std::atomic<int>& calls = gate_state->calls;

  dal::configureAudioEngine(engineConfig(recording, true, {}));
  dal::startAudioEngine([gate_state](std::span<float>) {
    gate_state->calls.fetch_add(1);
    std::lock_guard<std::mutex> hold(gate_state->mutex);
  });
  CHECK(waitFor([&] { return calls.load() >= 1; }));

  // The producer is stuck on the gate; neither stopping nor reading the statistics waits for it.
  const Clock::time_point before = Clock::now();
  dal::requestAudioEngineStop();
  const dal::AudioEngineStats blocked = dal::audioEngineStats();
  const auto took = Clock::now() - before;
  CHECK(took < std::chrono::milliseconds(100));
  CHECK(blocked.blocks == 0);

  gate.unlock();
  dal::AudioEngineStats stats;
  CHECK(dal::stopAudioEngineForTest(&stats));
  // The thread ended at its next tick without calling the producer again.
  CHECK(calls.load() == 1);
  CHECK(stats.blocks == 1);
}

void testEngineOutputOpenFails() {
  std::mutex lines_mutex;
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    lines.emplace_back(line);
  });

  const auto recording = std::make_shared<Recording>();
  const auto state = std::make_shared<ProducerState>();
  // The dump cannot be created either: the engine reports it once and carries on without it.
  dal::configureAudioEngine(engineConfig(recording, false, g_dir / "no_such_dir" / "x.wav"));
  dal::startAudioEngine(makeProducer(state));
  CHECK(waitFor([&] { return state->calls.load() >= 5; }));
  dal::AudioEngineStats stats;
  CHECK(dal::stopAudioEngineForTest(&stats));
  dal::setLogSink({});

  CHECK(stats.output == "none (fake failed)");
  CHECK(stats.blocks >= 5);
  CHECK(stats.output_stats.blocks_submitted == stats.blocks + stats.skipped_blocks);
  CHECK(recording->opens == 1 && recording->take().empty());

  auto count = [&](std::string_view needle) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    return std::count_if(lines.begin(), lines.end(),
                         [&](const std::string& line) { return line.find(needle) != std::string::npos; });
  };
  CHECK(count("dal: audio: fake: device gone; continuing without sound") == 1);
  CHECK(count("continuing without sound") == 1);
  CHECK(count("dal: audio: output none") == 1);
  CHECK(count("continuing without a dump") == 1);
}

void testEngineFinishDumpDuringOpen() {
  const fs::path dump_path = g_dir / "engine_early_finish.wav";
  std::error_code ec;
  fs::remove(dump_path, ec);

  const auto recording = std::make_shared<Recording>();
  recording->open_delay = std::chrono::milliseconds(200);
  const auto state = std::make_shared<ProducerState>();
  dal::configureAudioEngine(engineConfig(recording, true, dump_path));
  dal::startAudioEngine(makeProducer(state));

  // The engine thread is still inside the slow open(): there is no dump to finish yet. The finish
  // request must not be lost; the engine then creates no dump file.
  dal::finishAudioDump();
  CHECK(waitFor([&] { return state->calls.load() >= 3; }));
  dal::AudioEngineStats stats;
  CHECK(dal::stopAudioEngineForTest(&stats));
  CHECK(stats.blocks >= 3);
  CHECK(!recording->take().empty());
  CHECK(!fs::exists(dump_path));
}

void testEngineResumesAfterStop() {
  const fs::path dump_path = g_dir / "engine_resume.wav";
  for (const bool wait_for_exit : {false, true}) {
    const auto recording = std::make_shared<Recording>();
    const auto state = std::make_shared<ProducerState>();
    dal::configureAudioEngine(engineConfig(recording, true, dump_path));
    dal::startAudioEngine(makeProducer(state));
    CHECK(waitFor([&] { return state->calls.load() >= 5; }));

    // A start right after a stop request cancels it; one after the thread has ended starts a new
    // thread. Either way production goes on with the same output and dump.
    dal::requestAudioEngineStop();
    if (wait_for_exit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const std::uint64_t settled = state->calls.load();
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      CHECK(state->calls.load() == settled);
    }
    const std::uint64_t before = state->calls.load();
    dal::startAudioEngine(makeProducer(std::make_shared<ProducerState>()));
    CHECK(waitFor([&] { return state->calls.load() >= before + 10; }));

    dal::AudioEngineStats stats;
    CHECK(dal::stopAudioEngineForTest(&stats));
    const std::vector<float> samples = recording->take();
    const RecordedBlocks recorded = analyse(samples);
    CHECK(recording->opens == 1);
    CHECK(stats.blocks == state->calls.load());
    CHECK(recorded.total == stats.blocks + stats.skipped_blocks);
    const auto wav = dal::readWav(dump_path);
    CHECK(wav.has_value() && wav->samples == samples);
  }
}

// ---- cellAudio on synthetic guest memory ----

constexpr std::uint32_t kAudioNotInit = 0x80310703;
constexpr std::uint32_t kAudioAlreadyInit = 0x80310701;
constexpr std::uint32_t kAudioParam = 0x80310704;
constexpr std::uint32_t kAudioPortFull = 0x80310705;
constexpr std::uint32_t kAudioAlreadyRun = 0x80310706;
constexpr std::uint32_t kAudioPortNotOpen = 0x80310707;
constexpr std::uint32_t kAudioPortNotRun = 0x80310708;
constexpr std::uint32_t kAudioTransEvent = 0x80310709;
constexpr std::uint32_t kAudioTagNotFound = 0x8031070f;
constexpr std::uint64_t kNotifyKey = 0x8000cafe02460300;  // the game mixer's first notify queue key

// One simulated guest thread that holds the guest lock, with guest memory reserved and the
// cellAudio state, the event objects and the audio engine reset. Release the lock before the engine
// is stopped: its producer may be waiting for it.
class AudioGuest {
 public:
  AudioGuest() : caller_(0x01000000, dal::GuestThreadParams{}, 0, 0) {
    dal::setLogSink([](std::string_view) {});
    dal::resetSyncObjects();
    dal::resetCellAudio();
    dal::configureAudioEngine({});
    CHECK(dal::process().memory.reserve());
    base_ = dal::process().memory.base();
    scratch_ = dal::process().main.allocate(0x10000, 0x10000);
    CHECK(scratch_ != 0);
    dal::guestLock().lock();
  }
  ~AudioGuest() {
    dal::guestLock().unlock();
    CHECK(dal::stopAudioEngineForTest());
    dal::resetCellAudio();
    dal::setLogSink({});
  }

  std::uint8_t* base() const { return base_; }
  // Guest address of scratch byte `offset`.
  std::uint32_t at(std::uint32_t offset) const { return scratch_ + offset; }

  // Calls the handler of a cellAudio import and returns its result as the guest sees it in r3.
  std::uint32_t call(std::string_view name, std::uint64_t r3 = 0, std::uint64_t r4 = 0, std::uint64_t r5 = 0) {
    for (const dal::HleEntry& entry : dal::cellAudioHandlers()) {
      if (name != entry.name) continue;
      DalPpuContext* ctx = &caller_.context();
      ctx->gpr[3] = r3;
      ctx->gpr[4] = r4;
      ctx->gpr[5] = r5;
      entry.handler(ctx, base_);
      return static_cast<std::uint32_t>(ctx->gpr[3]);
    }
    CHECK(false);
    return 0;
  }

  // cellAudioInit, then stops the engine it started, so that only runPeriod drives periods: a real
  // producer call that sneaks in while a timed receive gives up the guest lock would add a notify.
  // A plain stop request is not enough, because one queued producer call still completes.
  std::uint32_t initWithoutEngine() {
    const std::uint32_t result = call("cellAudioInit");
    dal::guestLock().unlock();
    CHECK(dal::stopAudioEngineForTest());
    dal::guestLock().lock();
    return result;
  }

  // Writes a CellAudioPortParam at scratch `offset` and opens a port with it.
  std::uint32_t open(std::uint32_t offset, std::uint64_t channels, std::uint64_t blocks, std::uint64_t attr,
                     float level, std::uint32_t port_out) {
    dalWrite64(base_, at(offset), channels);
    dalWrite64(base_, at(offset + 8), blocks);
    dalWrite64(base_, at(offset + 0x10), attr);
    dalWrite32(base_, at(offset + 0x18), std::bit_cast<std::uint32_t>(level));
    return call("cellAudioPortOpen", at(offset), port_out);
  }

  // Creates an event queue with `key` (PPU queue, FIFO, depth 2) through the syscall; returns its id.
  std::uint32_t createQueue(std::uint64_t key) {
    dalWrite32(base_, at(0x400), 1);
    dalWrite32(base_, at(0x404), 1);
    const std::uint64_t result = syscall(128, at(0x410), at(0x400), key, 2);
    CHECK(result == 0);
    return dalRead32(base_, at(0x410));
  }

  // The result of `sys_event_queue_receive(id, buffer, 1 ms)`; the event words come back in r4..r7.
  std::uint64_t receive(std::uint32_t id, std::array<std::uint64_t, 4>& event) {
    const std::uint64_t result = syscall(130, id, at(0x440), 1000);
    DalPpuContext* ctx = &caller_.context();
    event = {ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->gpr[7]};
    return result;
  }

  std::uint64_t syscall(std::uint32_t number, std::uint64_t r3, std::uint64_t r4 = 0, std::uint64_t r5 = 0,
                        std::uint64_t r6 = 0) {
    DalPpuContext* ctx = &caller_.context();
    ctx->gpr[11] = number;
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[6] = r6;
    dalSyscall(ctx, base_, 0x10000);
    return ctx->gpr[3];
  }

  // The 32-bit result of the last syscall as a signed lv2 error.
  static std::uint64_t err(std::uint32_t code) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(code)));
  }

 private:
  dal::GuestThread caller_;
  std::uint8_t* base_ = nullptr;
  std::uint32_t scratch_ = 0;
};

void writeFloat(std::uint8_t* base, std::uint32_t addr, float value) {
  dalWrite32(base, addr, std::bit_cast<std::uint32_t>(value));
}

// Runs one period and returns the stereo block.
dal::AudioBlock runPeriod(AudioGuest& guest, std::uint64_t now_us) {
  dal::AudioBlock out{};
  dal::cellAudioPorts().producePeriod(guest.base(), now_us, out);
  return out;
}

void testCellAudioStateErrors() {
  AudioGuest guest;
  // Everything but Init needs Init first.
  for (const char* name : {"cellAudioQuit", "cellAudioPortOpen", "cellAudioGetPortConfig", "cellAudioPortStart",
                           "cellAudioPortStop", "cellAudioPortClose", "cellAudioGetPortBlockTag",
                           "cellAudioGetPortTimestamp", "cellAudioSetNotifyEventQueue",
                           "cellAudioRemoveNotifyEventQueue"}) {
    CHECK(guest.call(name, guest.at(0), guest.at(0x40), guest.at(0x80)) == kAudioNotInit);
  }
  CHECK(guest.initWithoutEngine() == 0);
  CHECK(guest.call("cellAudioInit") == kAudioAlreadyInit);
  CHECK(guest.call("cellAudioQuit") == 0);
  CHECK(guest.call("cellAudioQuit") == kAudioNotInit);
  CHECK(dal::cellAudioHandlers().size() == 11);
}

void testCellAudioPortOpenAndConfig() {
  AudioGuest guest;
  std::uint8_t* base = guest.base();
  CHECK(guest.initWithoutEngine() == 0);

  // The game's parameters.
  CHECK(guest.open(0x00, 8, 8, 0, 1.0f, guest.at(0x60)) == 0);
  CHECK(dalRead32(base, guest.at(0x60)) == 0);
  CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
  const std::uint32_t cell = dalRead32(base, guest.at(0x80));
  const std::uint32_t memory = dalRead32(base, guest.at(0x80 + 0x1c));
  CHECK(cell != 0 && cell % 16 == 0 && dal::process().memory.isCommitted(cell, 8));
  CHECK(dalRead64(base, cell) == 0);
  CHECK(memory != 0 && memory % 0x10000 == 0 && dal::process().memory.isCommitted(memory, 0x10000));
  CHECK(dalRead32(base, guest.at(0x80 + 4)) == 1);  // opened
  CHECK(dalRead64(base, guest.at(0x80 + 8)) == 8);
  CHECK(dalRead64(base, guest.at(0x80 + 0x10)) == 8);
  CHECK(dalRead32(base, guest.at(0x80 + 0x18)) == 0x10000);

  // Start zeroes the port memory and marks the port running.
  dalWrite32(base, memory + 0x100, 0x3f800000);
  CHECK(guest.call("cellAudioPortStart", 0) == 0);
  CHECK(dalRead32(base, memory + 0x100) == 0);
  CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
  CHECK(dalRead32(base, guest.at(0x80 + 4)) == 2);
  CHECK(guest.call("cellAudioPortStart", 0) == kAudioAlreadyRun);

  // A second port gets the next number and its own cell and memory.
  CHECK(guest.open(0x00, 2, 4, 0, 1.0f, guest.at(0x60)) == 0);
  CHECK(dalRead32(base, guest.at(0x60)) == 1);
  CHECK(guest.call("cellAudioGetPortConfig", 1, guest.at(0xc0)) == 0);
  CHECK(dalRead32(base, guest.at(0xc0)) == cell + 16);
  CHECK(dalRead32(base, guest.at(0xc0 + 0x1c)) != memory);
  CHECK(dalRead32(base, guest.at(0xc0 + 0x18)) == 2 * 4 * 1024);

  // Stop, start again; closing works from any open state; the number is reused.
  CHECK(guest.call("cellAudioPortStop", 1) == kAudioPortNotRun);
  CHECK(guest.call("cellAudioPortStop", 0) == 0);
  CHECK(guest.call("cellAudioPortStop", 0) == kAudioPortNotRun);
  CHECK(guest.call("cellAudioPortStart", 0) == 0);
  CHECK(guest.call("cellAudioPortClose", 0) == 0);
  CHECK(guest.call("cellAudioPortClose", 0) == kAudioPortNotOpen);
  CHECK(guest.call("cellAudioPortStart", 0) == kAudioPortNotOpen);
  CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
  CHECK(dalRead32(base, guest.at(0x80 + 4)) == 0x1010);
  CHECK(guest.open(0x00, 8, 8, 0, 1.0f, guest.at(0x60)) == 0);
  CHECK(dalRead32(base, guest.at(0x60)) == 0);

  // Argument checks.
  CHECK(guest.call("cellAudioGetPortConfig", 8, guest.at(0x80)) == kAudioParam);
  CHECK(guest.call("cellAudioGetPortConfig", 0, 0) == kAudioParam);
  CHECK(guest.call("cellAudioPortStart", 8) == kAudioParam);
  CHECK(guest.call("cellAudioPortStop", 8) == kAudioParam);
  CHECK(guest.call("cellAudioPortClose", 8) == kAudioParam);
  CHECK(guest.call("cellAudioPortOpen", 0, guest.at(0x60)) == kAudioParam);
  CHECK(guest.call("cellAudioPortOpen", guest.at(0), 0) == kAudioParam);
  CHECK(guest.open(0x00, 4, 8, 0, 1.0f, guest.at(0x60)) == kAudioParam);
  CHECK(guest.open(0x00, 2, 3, 0, 1.0f, guest.at(0x60)) == kAudioParam);
  CHECK(guest.open(0x00, 2, 64, 0, 1.0f, guest.at(0x60)) == kAudioParam);

  // Eight ports exist; a ninth open fails.
  for (int i = 0; i < 6; ++i) CHECK(guest.open(0x00, 2, 2, 0, 1.0f, guest.at(0x60)) == 0);
  CHECK(guest.open(0x00, 2, 2, 0, 1.0f, guest.at(0x60)) == kAudioPortFull);

  CHECK(guest.call("cellAudioQuit") == 0);
}

void testCellAudioTagsAndTimestamps() {
  AudioGuest guest;
  std::uint8_t* base = guest.base();
  CHECK(guest.initWithoutEngine() == 0);

  // Periods before any port is open still count, so tags do not start at 0.
  constexpr std::uint64_t kStart = 1'000'000;
  for (std::uint64_t i = 0; i < 5; ++i) runPeriod(guest, kStart + i * 5333);
  CHECK(dal::cellAudioPorts().stats().periods == 5);

  CHECK(guest.open(0x00, 8, 8, 0, 1.0f, guest.at(0x60)) == 0);
  CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
  const std::uint32_t cell = dalRead32(base, guest.at(0x80));
  CHECK(guest.call("cellAudioPortStart", 0) == 0);

  const std::uint64_t t5 = kStart + 5 * 5333;
  const std::uint32_t tag_out = guest.at(0x100);
  const std::uint32_t stamp_out = guest.at(0x108);
  CHECK(guest.call("cellAudioGetPortBlockTag", 0, 0, tag_out) == 0);
  CHECK(dalRead64(base, tag_out) == 5);

  // Period k publishes the next block with tag k; the cell holds the block the library mixes next.
  for (std::uint64_t i = 0; i < 12; ++i) {
    runPeriod(guest, t5 + i * 5333);
    const std::uint64_t index = (i + 1) % 8;
    CHECK(dalRead64(base, cell) == index);
    CHECK(guest.call("cellAudioGetPortBlockTag", 0, index, tag_out) == 0);
    CHECK(dalRead64(base, tag_out) == 5 + i);
    CHECK(guest.call("cellAudioGetPortBlockTag", 0, (index + 1) % 8, tag_out) == 0);
    if ((index + 1) % 8 > index) CHECK(dalRead64(base, tag_out) == 5 + i + 1);
  }
  // 12 periods since open: the latest period is number 16 at t5 + 11 periods.
  const std::uint64_t latest_time = t5 + 11 * 5333;
  dalWrite64(base, stamp_out, 0xdeadbeef);
  CHECK(guest.call("cellAudioGetPortTimestamp", 0, 16, stamp_out) == 0);
  CHECK(dalRead64(base, stamp_out) == latest_time);
  CHECK(guest.call("cellAudioGetPortTimestamp", 0, 13, stamp_out) == 0);
  CHECK(dalRead64(base, stamp_out) == latest_time - 3 * 5333);
  // No history: tag 0 answers too, extrapolated back at one period per tag.
  CHECK(guest.call("cellAudioGetPortTimestamp", 0, 0, stamp_out) == 0);
  CHECK(dalRead64(base, stamp_out) == latest_time - 16 * 5333);
  CHECK(guest.call("cellAudioGetPortTimestamp", 0, 15, 0) == 0);  // null output: nothing written

  // A tag that was never issued: error, output untouched.
  dalWrite64(base, stamp_out, 0xdeadbeef);
  CHECK(guest.call("cellAudioGetPortTimestamp", 0, 17, stamp_out) == kAudioTagNotFound);
  CHECK(dalRead64(base, stamp_out) == 0xdeadbeef);

  // Stopping freezes the counters but they still answer.
  CHECK(guest.call("cellAudioPortStop", 0) == 0);
  runPeriod(guest, t5 + 20 * 5333);
  CHECK(guest.call("cellAudioGetPortTimestamp", 0, 16, stamp_out) == 0);
  CHECK(dalRead64(base, stamp_out) == latest_time);
  CHECK(guest.call("cellAudioGetPortTimestamp", 0, 17, stamp_out) == kAudioTagNotFound);

  // Argument checks: block number, port number, closed port, unwritten output.
  CHECK(guest.call("cellAudioGetPortBlockTag", 0, 8, tag_out) == kAudioParam);
  CHECK(guest.call("cellAudioGetPortBlockTag", 8, 0, tag_out) == kAudioParam);
  CHECK(guest.call("cellAudioGetPortBlockTag", 1, 0, tag_out) == kAudioPortNotOpen);
  CHECK(guest.call("cellAudioGetPortTimestamp", 1, 0, stamp_out) == kAudioPortNotOpen);
  CHECK(guest.call("cellAudioGetPortTimestamp", 8, 0, stamp_out) == kAudioParam);
  CHECK(guest.call("cellAudioGetPortBlockTag", 0, 0, 0) == 0);
}

void testCellAudioMixing() {
  AudioGuest guest;
  std::uint8_t* base = guest.base();
  CHECK(guest.initWithoutEngine() == 0);

  // 8 channels: a ramp on all of them. Only the front pair reaches the output; the rest is counted.
  CHECK(guest.open(0x00, 8, 8, 0, 0.25f, guest.at(0x60)) == 0);  // attr 0: the level argument is ignored
  CHECK(guest.call("cellAudioGetPortConfig", 0, guest.at(0x80)) == 0);
  const std::uint32_t cell = dalRead32(base, guest.at(0x80));
  const std::uint32_t memory = dalRead32(base, guest.at(0x80 + 0x1c));

  // Opened but not started: contributes nothing, the cell stays.
  writeFloat(base, memory, 0.5f);
  CHECK(std::ranges::all_of(runPeriod(guest, 1000), [](float s) { return s == 0.0f; }));
  CHECK(dalRead64(base, cell) == 0);

  CHECK(guest.call("cellAudioPortStart", 0) == 0);
  for (std::uint32_t frame = 0; frame < 256; ++frame) {
    for (std::uint32_t channel = 0; channel < 8; ++channel) {
      const float value = channel == 0 ? static_cast<float>(frame) / 512.0f
                          : channel == 1 ? -static_cast<float>(frame) / 1024.0f
                                         : 0.25f * static_cast<float>(channel);
      writeFloat(base, memory + (frame * 8 + channel) * 4, value);
    }
  }
  const dal::AudioBlock out = runPeriod(guest, 2000);
  bool front_intact = true;
  for (std::uint32_t frame = 0; frame < 256; ++frame) {
    front_intact = front_intact && out[2 * frame] == static_cast<float>(frame) / 512.0f &&
                   out[2 * frame + 1] == -static_cast<float>(frame) / 1024.0f;
  }
  CHECK(front_intact);

  // The consumed block is zeroed and the index advanced as a big-endian u64.
  bool zeroed = true;
  for (std::uint32_t offset = 0; offset < 0x2000; offset += 4) zeroed = zeroed && dalRead32(base, memory + offset) == 0;
  CHECK(zeroed);
  CHECK(base[cell + 7] == 1 && dalRead64(base, cell) == 1);

  const dal::CellAudioStats stats = dal::cellAudioPorts().stats();
  CHECK(stats.nonzero_samples[0] == 255 && stats.nonzero_samples[1] == 255);
  for (std::size_t channel = 2; channel < 8; ++channel) CHECK(stats.nonzero_samples[channel] == 256);

  // Block 1 was never written: the next period is silent and the index moves on.
  CHECK(std::ranges::all_of(runPeriod(guest, 3000), [](float s) { return s == 0.0f; }));
  CHECK(dalRead64(base, cell) == 2);
  CHECK(guest.call("cellAudioPortClose", 0) == 0);

  // 2 channels: scaled by the initial level only when the attribute asks for it; ports add up.
  CHECK(guest.open(0x00, 2, 2, 0x1000, 0.5f, guest.at(0x60)) == 0);
  const std::uint32_t scaled = dalRead32(base, guest.at(0x60));
  CHECK(guest.open(0x00, 2, 4, 0, 0.25f, guest.at(0x60)) == 0);
  const std::uint32_t plain = dalRead32(base, guest.at(0x60));
  CHECK(guest.call("cellAudioGetPortConfig", scaled, guest.at(0x80)) == 0);
  const std::uint32_t scaled_memory = dalRead32(base, guest.at(0x80 + 0x1c));
  CHECK(guest.call("cellAudioGetPortConfig", plain, guest.at(0xc0)) == 0);
  const std::uint32_t plain_memory = dalRead32(base, guest.at(0xc0 + 0x1c));
  CHECK(guest.call("cellAudioPortStart", scaled) == 0);
  CHECK(guest.call("cellAudioPortStart", plain) == 0);
  for (std::uint32_t frame = 0; frame < 256; ++frame) {
    writeFloat(base, scaled_memory + frame * 8, 0.25f);
    writeFloat(base, scaled_memory + frame * 8 + 4, 0.5f);
    writeFloat(base, plain_memory + frame * 8, 0.125f);
    writeFloat(base, plain_memory + frame * 8 + 4, 0.0625f);
  }
  // Frame 255 overdrives: 3.0 * 0.5 + 0.125 clamps to 1.
  writeFloat(base, scaled_memory + 255 * 8, 3.0f);
  writeFloat(base, scaled_memory + 255 * 8 + 4, -3.0f);
  const dal::AudioBlock mixed = runPeriod(guest, 4000);
  CHECK(mixed[0] == 0.25f && mixed[1] == 0.3125f);
  CHECK(mixed[2 * 255] == 1.0f && mixed[2 * 255 + 1] == -1.0f);
  CHECK(dalRead64(base, dalRead32(base, guest.at(0x80))) == 1);
  CHECK(dalRead64(base, dalRead32(base, guest.at(0xc0))) == 1);
}

void testCellAudioNotify() {
  AudioGuest guest;
  std::uint8_t* base = guest.base();
  (void)base;
  CHECK(guest.initWithoutEngine() == 0);
  const std::uint32_t queue = guest.createQueue(kNotifyKey);
  CHECK(queue != 0);

  // Unknown keys, the local key and a second registration of the same queue fail.
  CHECK(guest.call("cellAudioSetNotifyEventQueue", kNotifyKey + 1) == kAudioTransEvent);
  CHECK(guest.call("cellAudioSetNotifyEventQueue", 0) == kAudioTransEvent);
  CHECK(guest.call("cellAudioRemoveNotifyEventQueue", kNotifyKey) == kAudioTransEvent);
  CHECK(guest.call("cellAudioSetNotifyEventQueue", kNotifyKey) == 0);
  CHECK(guest.call("cellAudioSetNotifyEventQueue", kNotifyKey) == kAudioTransEvent);

  // One event per period, even with no port started: source, then three zeros.
  std::array<std::uint64_t, 4> event{};
  runPeriod(guest, 1000);
  CHECK(guest.receive(queue, event) == 0);
  CHECK(event[0] == 0x1'0e00'0000 && event[1] == 0 && event[2] == 0 && event[3] == 0);

  // The queue holds two events; the third is refused and only counted.
  runPeriod(guest, 2000);
  runPeriod(guest, 3000);
  runPeriod(guest, 4000);
  const dal::CellAudioStats stats = dal::cellAudioPorts().stats();
  CHECK(stats.notifies_sent == 3 && stats.notifies_failed == 1);
  CHECK(guest.receive(queue, event) == 0);
  CHECK(guest.receive(queue, event) == 0);
  CHECK(guest.receive(queue, event) == AudioGuest::err(0x8001000B));  // the fourth event never existed

  // After Remove nothing more arrives.
  CHECK(guest.call("cellAudioRemoveNotifyEventQueue", kNotifyKey) == 0);
  CHECK(guest.call("cellAudioRemoveNotifyEventQueue", kNotifyKey) == kAudioTransEvent);
  runPeriod(guest, 5000);
  CHECK(guest.receive(queue, event) == AudioGuest::err(0x8001000B));
  CHECK(dal::cellAudioPorts().stats().notifies_sent == 3);

  // A destroyed queue only costs failed notifies, and its registration goes at the next Set.
  CHECK(guest.call("cellAudioSetNotifyEventQueue", kNotifyKey) == 0);
  CHECK(guest.syscall(129, queue, 0) == 0);
  runPeriod(guest, 6000);
  CHECK(dal::cellAudioPorts().stats().notifies_failed == 2);
  const std::uint32_t second = guest.createQueue(kNotifyKey + 1);
  CHECK(guest.call("cellAudioSetNotifyEventQueue", kNotifyKey + 1) == 0);
  runPeriod(guest, 7000);
  CHECK(dal::cellAudioPorts().stats().notifies_failed == 2);
  CHECK(dal::cellAudioPorts().stats().notifies_sent == 4);
  CHECK(guest.receive(second, event) == 0);
  // The third successful registration since reset has its own source.
  CHECK(event[0] == 0x1'0e00'0000 + 2 * 0x100);
}

void testCellAudioEngine() {
  AudioGuest guest;
  CHECK(guest.call("cellAudioInit") == 0);
  const auto periods = [] { return dal::cellAudioPorts().stats().periods; };

  // Init starts the engine, which calls the producer once the guest lock is free.
  dal::guestLock().unlock();
  CHECK(waitFor([&] { return periods() >= 5; }));
  dal::guestLock().lock();

  // Quit returns while the engine may be waiting for the lock we hold, and the engine stops
  // calling the producer (at most the call that was already under way completes).
  CHECK(guest.call("cellAudioQuit") == 0);
  dal::guestLock().unlock();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const std::uint64_t settled_calls = dal::audioEngineStats().blocks;
  const std::uint64_t settled = periods();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(dal::audioEngineStats().blocks == settled_calls);
  CHECK(periods() == settled);
  dal::guestLock().lock();

  // A second Init resumes the same engine.
  CHECK(guest.call("cellAudioInit") == 0);
  dal::guestLock().unlock();
  CHECK(waitFor([&] { return periods() >= settled + 5; }));
  dal::guestLock().lock();
}

}  // namespace

int main(int argc, char** argv) {
  const bool keep = argc > 1;
  std::error_code ec;
  if (keep) {
    g_dir = argv[1];
  } else {
    g_dir = fs::temp_directory_path() /
            std::format("dal_audio_core_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  }
  fs::create_directories(g_dir, ec);
  if (ec) {
    std::fprintf(stderr, "cannot create %s\n", g_dir.string().c_str());
    return 1;
  }

  testRingPushPop();
  testRingWrap();
  testRingDrop();
  testRingThreaded();
  testNullOutput();
  testWavFloatRoundTrip();
  testWavHeaderValidWithoutFinish();
  testWavWriterErrors();
  testWavReadPcm16();
  testWavReadRejects();
  testWavStatsTone();
  testWavStatsSilence();
  testWavStatsFile();
  testBlockClock();
  testEngineBlocksAndDump();
  testEngineSkipsLatePeriods();
  testEngineStopDoesNotWait();
  testEngineOutputOpenFails();
  testEngineFinishDumpDuringOpen();
  testEngineResumesAfterStop();
  testCellAudioStateErrors();
  testCellAudioPortOpenAndConfig();
  testCellAudioTagsAndTimestamps();
  testCellAudioMixing();
  testCellAudioNotify();
  testCellAudioEngine();

  if (!keep) fs::remove_all(g_dir, ec);
  std::printf("audio core tests: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed != 0 ? 1 : 0;
}
