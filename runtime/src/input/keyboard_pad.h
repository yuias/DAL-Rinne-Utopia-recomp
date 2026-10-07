// Keyboard as the pad on port 0: scan-code mapping, the lock-free button state shared with the
// window and script threads, and the PadProvider that reads it.
//
// Scan codes (physical key positions) rather than virtual keys: the mapping is a positional cluster
// of keys, which must stay in place on AZERTY/QWERTZ layouts and keep working while an IME is open.
// A scan code is the PC/AT set 1 make code, with 0xE000 added for extended keys.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "hle/hle.h"
#include "input/pad_buttons.h"

namespace dal {

// Only the dedicated (extended) arrow keys map; the numpad does not.
std::optional<PadButton> padButtonForScanCode(std::uint32_t scan_code);

enum class InputSource : std::uint8_t { Keyboard, Script, Count };

// Pad buttons held by host input. Written by the window thread (Keyboard) and the script thread
// (Script), read by poll on guest threads under the guest lock, so everything is a relaxed atomic:
// each mask bit and each press time is independent, and a stale read delays a change by one poll.
class KeyboardPadState {
 public:
  using Clock = std::chrono::steady_clock;
  // A press stays reported for at least this long, so a tap shorter than the game's polling
  // interval (about 33 ms at 30 fps; cellPadGetData reports only changes) is still seen.
  static constexpr std::chrono::milliseconds kMinPressVisible{60};

  KeyboardPadState();

  // Keyboard source. Unmapped scan codes and key repeats are ignored. Returns whether the scan
  // code is mapped.
  bool keyEvent(std::uint32_t scan_code, bool down, bool repeat, Clock::time_point now);
  void setButtons(InputSource source, PadButtons buttons, bool down, Clock::time_point now);
  // Releases every held button of `source` at once, without the minimum visible time (focus
  // lost). A tap of `source` already released keeps its remaining visible time.
  void releaseAll(InputSource source);

  PadButtons held() const;  // OR of all sources
  // held() plus the buttons pressed within kMinPressVisible before `now`. A real d-pad cannot
  // report opposite directions together: both held cancel each other; a held direction beats the
  // tail of a released one; two tails leave the more recent press.
  PadButtons reported(Clock::time_point now) const;

 private:
  std::array<std::atomic<PadButtons>, static_cast<std::size_t>(InputSource::Count)> held_{};
  // Press time of each button per source in steady-clock nanoseconds, kept apart so that
  // releaseAll of one source cannot drop the minimum visible time of another source's tap.
  // kNoPress (see the .cpp) when none is remembered.
  std::array<std::array<std::atomic<std::int64_t>, kPadButtonTotal>,
             static_cast<std::size_t>(InputSource::Count)>
      pressed_at_ns_;
  // poll() runs under the guest lock and must never block.
  static_assert(std::atomic<std::int64_t>::is_always_lock_free &&
                std::atomic<PadButtons>::is_always_lock_free);
};

// Port 0: padStateFromButtons(state->reported(clock())), always connected; other ports are empty.
// `clock` defaults to steady_clock::now and is injectable for tests.
std::unique_ptr<PadProvider> makeKeyboardPadProvider(
    std::shared_ptr<KeyboardPadState> state,
    std::function<KeyboardPadState::Clock::time_point()> clock = {});

}  // namespace dal
