// Self-checking tests for the audio core: the output interface, the null output, the block ring and
// the WAV writer, reader and level statistics. No sound device is involved.
//
// With a directory argument the WAV files the tests write are kept there (for dal_audiotool
// wavstat); without one they go to a temporary directory that is removed at the end.

#include <algorithm>
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
#include <numbers>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "audio/audio_output.h"
#include "audio/wav_file.h"

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

  if (!keep) fs::remove_all(g_dir, ec);
  std::printf("audio core tests: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed != 0 ? 1 : 0;
}
