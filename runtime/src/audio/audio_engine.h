// The audio engine: one host thread that ticks every cellAudio period on steady_clock, asks a
// producer for one block per tick, and hands the block to the dump file and the output. The clock
// is the same for every output; a device output absorbs the difference between its own clock and
// this one in its block ring.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "audio/audio_output.h"

namespace dal {

// Periods the engine may produce beyond the first one when it wakes late. Zero: the game's mixer
// writes exactly one block per notify and needs the guest lock to do it, so two periods produced
// back to back under one lock hold would consume a block the mixer never rewrote. Late periods are
// skipped instead (silent, no producer call).
inline constexpr std::uint32_t kMaxCatchUpBlocks = 0;

// Fills one block (kAudioBlockSamples, interleaved stereo host float) for the current period. Runs
// on the engine thread and takes the guest lock itself. The block is zeroed before the call.
using AudioBlockProducer = std::function<void(std::span<float> block)>;

struct AudioEngineConfig {
  std::function<std::unique_ptr<AudioOutput>()> make_output;  // null: makeNullAudioOutput
  std::filesystem::path dump;                                  // empty: no dump
};

// Before boot. Replaces any earlier configuration; an engine that already started keeps its own.
void configureAudioEngine(AudioEngineConfig config);

// Starts the engine thread (idempotent). Returns at once: the output and the dump file are opened
// on the engine thread. A failed output open logs one warning and the engine continues with the
// null output; a failed dump open logs one warning and the engine continues without a dump. After
// requestAudioEngineStop() a new call resumes the same engine (same output and dump); the producer
// of the first call is kept.
void startAudioEngine(AudioBlockProducer producer);

// Stops producing. Never waits: the engine thread may itself be waiting for the guest lock that the
// caller holds. A producer call already under way, possibly still waiting for that lock, completes
// after this returns; no further call starts. The producer must therefore check its own state
// (cellAudio's) under the guest lock.
void requestAudioEngineStop();

struct AudioEngineStats {
  std::string output;  // "none", "wasapi", or "none (wasapi failed)"; empty before the first start
  // Blocks the producer filled, and periods skipped because the thread woke late. Each skipped
  // period was written to the dump and the output as silence, so blocks + skipped_blocks is the
  // number of periods elapsed.
  std::uint64_t blocks = 0;
  std::uint64_t non_silent_blocks = 0;
  std::uint64_t skipped_blocks = 0;
  std::chrono::microseconds max_producer_time{0};  // includes the wait for the guest lock
  AudioOutputStats output_stats;
};
AudioEngineStats audioEngineStats();  // any thread

// Patches the dump header and closes the file; later blocks are not written. For the stop path,
// which ends the process without running destructors. No-op without a dump.
void finishAudioDump();

// Test only: stops the engine, waits (bounded) until its thread has ended, finishes the dump and
// forgets the engine and its configuration, so the next test starts clean. `final_stats` receives
// the statistics taken after the thread ended. False on timeout.
bool stopAudioEngineForTest(AudioEngineStats* final_stats = nullptr);

struct BlockClockPlan {
  std::uint32_t produce = 0;  // blocks to ask the producer for
  std::uint32_t skipped = 0;  // periods to cover with silence instead
};

// The tick schedule, separate from the thread so it can be tested with made-up time points.
class BlockClock {
 public:
  using Clock = std::chrono::steady_clock;

  // The first period is due one kAudioBlockPeriod after `start`.
  explicit BlockClock(Clock::time_point start) : next_due_(start + kAudioBlockPeriod) {}

  // n periods are due at `now`: produce = min(n, 1 + kMaxCatchUpBlocks) and skipped = n - produce.
  // The schedule moves to the first period after `now`.
  BlockClockPlan due(Clock::time_point now);
  Clock::time_point nextDue() const { return next_due_; }

 private:
  Clock::time_point next_due_;
};

}  // namespace dal
