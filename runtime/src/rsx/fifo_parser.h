#pragma once

// Pure decoder of the RSX command FIFO: no threads, no guest memory. The consumer feeds it a word
// reader and a register-write sink. Header kinds and masks follow RPCS3 `Emu/RSX/gcm_enums.h`
// (`RSX_METHOD_*`) and `Emu/RSX/RSXFIFO.cpp` (`FIFO_control::read`, `thread::run_FIFO`).

#include <cstdint>
#include <string>
#include <string_view>

namespace dal {

// Header word layout. Methods are byte offsets of the register (RPCS3 stores them >> 2).
inline constexpr std::uint32_t kFifoOldJumpMask = 0xE0000003;
inline constexpr std::uint32_t kFifoOldJump = 0x20000000;
inline constexpr std::uint32_t kFifoOldJumpOffsetMask = 0x1FFFFFFC;
inline constexpr std::uint32_t kFifoNewJumpMask = 0xE0000003;
inline constexpr std::uint32_t kFifoNewJump = 0x00000001;
inline constexpr std::uint32_t kFifoNewJumpOffsetMask = 0xFFFFFFFC;
inline constexpr std::uint32_t kFifoCallMask = 0x00000003;
inline constexpr std::uint32_t kFifoCall = 0x00000002;
inline constexpr std::uint32_t kFifoCallOffsetMask = 0x1FFFFFFC;
inline constexpr std::uint32_t kFifoReturnMask = 0xFFFF0003;
inline constexpr std::uint32_t kFifoReturn = 0x00020000;
// Any of these bits set means "not a plain method header": flow control or malformed.
inline constexpr std::uint32_t kFifoNonMethodMask = 0xA0030003;
inline constexpr std::uint32_t kFifoNonIncrementMask = 0xE0030003;
inline constexpr std::uint32_t kFifoNonIncrement = 0x40000000;
inline constexpr std::uint32_t kFifoMethodMask = 0x0000FFFC;
inline constexpr std::uint32_t kFifoCountShift = 18;
inline constexpr std::uint32_t kFifoCountMask = 0x7FF;

// Method registers the consumer interprets (RPCS3 `gcm_enums.h`, shifted back to byte offsets).
inline constexpr std::uint32_t kMethodNv406eSetReference = 0x0050;
inline constexpr std::uint32_t kMethodNv406eSetContextDmaSemaphore = 0x0060;
inline constexpr std::uint32_t kMethodNv406eSemaphoreOffset = 0x0064;
inline constexpr std::uint32_t kMethodNv406eSemaphoreAcquire = 0x0068;
inline constexpr std::uint32_t kMethodNv406eSemaphoreRelease = 0x006C;
inline constexpr std::uint32_t kMethodNv4097SetContextDmaSemaphore = 0x01A4;
inline constexpr std::uint32_t kMethodNv4097SetContextDmaReport = 0x01A8;
inline constexpr std::uint32_t kMethodNv4097GetReport = 0x1800;
inline constexpr std::uint32_t kMethodNv4097SetSemaphoreOffset = 0x1D6C;
inline constexpr std::uint32_t kMethodNv4097BackEndWriteSemaphoreRelease = 0x1D70;
inline constexpr std::uint32_t kMethodNv4097TextureReadSemaphoreRelease = 0x1D74;
inline constexpr std::uint32_t kMethodGcmFlipHead = 0xE920;  // two registers, head 0 and 1
inline constexpr std::uint32_t kMethodGcmSetUserCommand = 0xEB00;  // two registers
inline constexpr std::uint32_t kMethodGcmFlipCommand = 0xFEAC;

class RsxSink {
 public:
  virtual ~RsxSink() = default;
  // Reads the big-endian word at an IO offset; false when the page is unmapped.
  virtual bool readIo(std::uint32_t io, std::uint32_t& word) = 0;
  // One register write; `method` is a byte offset. false = "cannot complete now" (semaphore
  // acquire not satisfied, flip waiting for VBlank): the parser keeps the write and retries it on
  // the next run.
  virtual bool write(std::uint32_t method, std::uint32_t value) = 0;
};

enum class FifoStatus {
  Idle,     // get == put, or spinning on a jump to itself
  Waiting,  // a write returned false
  Budget,   // max_words consumed with work left
  Fault,    // malformed header or unmapped read; sticky until reset()
};

class FifoParser {
 public:
  // Consumes words from `get` toward `put` (both IO offsets; the low two bits of put are ignored).
  // `get` always names the next unread word, so it already points past a write that returned false:
  // that write is kept inside the parser. After Fault `get` names the offending word.
  FifoStatus run(std::uint32_t& get, std::uint32_t put, std::uint32_t max_words, RsxSink& sink);

  std::string_view lastFault() const { return fault_; }
  // Method headers with at least one argument, for the census.
  std::uint64_t headersParsed() const { return headers_; }
  // Forgets a pending method, the call slot and the fault.
  void reset();
  // Where the guest-visible get register may point after a run that ended with `get`: the header
  // of an unfinished command, so the guest never sees a command as consumed before it completed.
  std::uint32_t commandGet(std::uint32_t get) const { return (remaining_ != 0 || pending_) ? command_get_ : get; }

 private:
  FifoStatus fail(std::string message);

  // Arguments of the current method still to be read, and the register the next one goes to.
  std::uint32_t remaining_ = 0;
  std::uint32_t method_ = 0;
  std::uint32_t command_get_ = 0;  // IO offset of the current command's header
  bool increment_ = true;
  // A write the sink refused, retried first on the next run.
  bool pending_ = false;
  std::uint32_t pending_method_ = 0;
  std::uint32_t pending_value_ = 0;
  // Only one call level exists on the hardware.
  bool in_call_ = false;
  std::uint32_t return_get_ = 0;
  std::uint64_t headers_ = 0;
  std::string fault_;
  bool faulted_ = false;
};

}  // namespace dal
