#pragma once

// The fake RSX command consumer: a host thread that feeds the FIFO parser, executes the
// synchronisation methods (labels, reports, reference, flips, user commands) and counts every
// register write for the census. Nothing is drawn. Method semantics follow RPCS3
// `Emu/RSX/NV47/HW/nv406e.cpp`, `nv4097.cpp`, `nv47_sync.hpp`, `rsx_methods.cpp` (`flip_command`,
// `user_command`) and `RSXThread.cpp` (`get_address`, `handle_emu_flip`, `post_vblank_event`).

#include <atomic>
#include <cstdint>
#include <memory>

#include "core/census.h"
#include "rsx/fifo_parser.h"

namespace dal {

enum class RsxInterrupt { VBlank, Flip, User };

// Called by the RSX thread with the guest lock held. `arg` is the head (1) for VBlank and Flip and
// the cause for User. No-op while unset.
void setRsxInterruptHook(void (*hook)(RsxInterrupt kind, std::uint32_t arg));

// One VBlank every 1/60 s (RPCS3's default VBlank rate).
inline constexpr std::uint32_t kRsxVBlankPeriodUs = 16667;

// Register state and effects behind the parser. All members are used under the guest lock except
// census(), which only touches atomics.
class RsxConsumer final : public RsxSink {
 public:
  explicit RsxConsumer(std::uint8_t* base);

  bool readIo(std::uint32_t io, std::uint32_t& word) override;
  bool write(std::uint32_t method, std::uint32_t value) override;

  // Parses what the guest has put since the last call (at most `max_words` words), then publishes
  // `get` to the control register. After Fault nothing is consumed any more.
  FifoStatus pump(std::uint32_t max_words);

  // False when a pump would find nothing new for this raw (big-endian already decoded) put value.
  // Needs no lock: used by the host thread to decide whether to take it.
  bool hasWork(std::uint32_t put) const;

  // Counts a VBlank and reports it.
  void vblankTick();

  RsxCensus census() const;
  bool faulted() const { return faulted_; }
  std::uint32_t parserGet() const { return get_; }

 private:
  bool execute(std::uint32_t method, std::uint32_t value);
  bool acquire(std::uint32_t value);
  void releaseNv406e(std::uint32_t value);
  void releaseNv4097(std::uint32_t value, bool swizzle);
  void getReport(std::uint32_t value);
  bool flip(std::uint32_t buffer);
  void userCommand(std::uint32_t cause);
  void publishGet(std::uint32_t get);
  // Effective address of `offset` in the context DMA `location`; false when out of range or not
  // one of the areas the consumer serves.
  bool resolve(std::uint32_t offset, std::uint32_t location, std::uint32_t size, std::uint32_t& ea) const;
  void resetDmaRegisters();

  std::uint8_t* base_;
  FifoParser parser_;
  std::uint32_t get_ = 0;  // IO offset of the next unread word
  std::uint32_t published_get_ = 0;
  bool get_known_ = false;
  bool faulted_ = false;
  FifoStatus last_status_ = FifoStatus::Budget;
  std::uint32_t idle_put_ = 0xFFFFFFFF;  // masked put at the last Idle, never a valid one initially

  // Latched registers: semaphore offsets and context DMA objects.
  std::uint32_t offset_406e_ = 0x30, offset_4097_ = 0;
  std::uint32_t dma_406e_ = 0, dma_semaphore_ = 0, dma_report_ = 0;

  bool flip_armed_ = false;
  std::uint64_t flip_vblank_ = 0;
  std::uint64_t last_timestamp_ns_ = 0;

  std::atomic<std::uint64_t> headers_{0}, flips_{0}, user_commands_{0};
  std::unique_ptr<std::atomic<std::uint64_t>[]> method_counts_;  // by register offset >> 2
};

// Starts the consumer thread over the guest memory at `base` and registers its census section.
// Call once after the RSX state is initialised; later calls do nothing.
void startRsxThread(std::uint8_t* base);

}  // namespace dal
