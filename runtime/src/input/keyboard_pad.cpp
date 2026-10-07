#include "input/keyboard_pad.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace dal {
namespace {

// A time of zero is a valid steady-clock reading in tests, so "no press" needs its own value.
constexpr std::int64_t kNoPress = std::numeric_limits<std::int64_t>::min();

constexpr std::size_t kSourceCount = static_cast<std::size_t>(InputSource::Count);

struct Axis {
  PadButton a;
  PadButton b;
};
constexpr Axis kAxes[] = {{PadButton::Up, PadButton::Down}, {PadButton::Left, PadButton::Right}};

std::int64_t toNanoseconds(KeyboardPadState::Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

class KeyboardPadProvider final : public PadProvider {
 public:
  KeyboardPadProvider(std::shared_ptr<KeyboardPadState> state,
                      std::function<KeyboardPadState::Clock::time_point()> clock)
      : state_(std::move(state)), clock_(std::move(clock)) {
    if (!clock_) clock_ = [] { return KeyboardPadState::Clock::now(); };
  }

  PadPortState poll(std::uint32_t port) override {
    if (port != 0) return {};
    return padStateFromButtons(state_->reported(clock_()));
  }

 private:
  std::shared_ptr<KeyboardPadState> state_;
  std::function<KeyboardPadState::Clock::time_point()> clock_;
};

}  // namespace

std::optional<PadButton> padButtonForScanCode(std::uint32_t scan_code) {
  switch (scan_code) {
    case 0xE048: return PadButton::Up;
    case 0xE050: return PadButton::Down;
    case 0xE04B: return PadButton::Left;
    case 0xE04D: return PadButton::Right;
    case 0x2C: return PadButton::Cross;     // Z
    case 0x2D: return PadButton::Circle;    // X
    case 0x1F: return PadButton::Square;    // S
    case 0x20: return PadButton::Triangle;  // D
    case 0x11: return PadButton::L1;        // W
    case 0x12: return PadButton::L2;        // E
    case 0x13: return PadButton::R1;        // R
    case 0x16: return PadButton::R2;        // U
    case 0x02: return PadButton::L3;        // 1
    case 0x04: return PadButton::R3;        // 3
    case 0x2F: return PadButton::Start;     // V
    case 0x2E: return PadButton::Select;    // C
    default: return std::nullopt;
  }
}

KeyboardPadState::KeyboardPadState() {
  for (auto& times : pressed_at_ns_) {
    for (auto& time : times) time.store(kNoPress, std::memory_order_relaxed);
  }
}

bool KeyboardPadState::keyEvent(std::uint32_t scan_code, bool down, bool repeat, Clock::time_point now) {
  const std::optional<PadButton> button = padButtonForScanCode(scan_code);
  if (!button) return false;
  const PadButtons bit = padButtonBit(*button);
  if (down) {
    // Level-based state: a repeat changes nothing, and must not refresh the press time.
    const bool already_down =
        (held_[static_cast<std::size_t>(InputSource::Keyboard)].load(std::memory_order_relaxed) & bit) != 0;
    if (!repeat && !already_down) setButtons(InputSource::Keyboard, bit, true, now);
  } else {
    setButtons(InputSource::Keyboard, bit, false, now);
  }
  return true;
}

void KeyboardPadState::setButtons(InputSource source, PadButtons buttons, bool down, Clock::time_point now) {
  std::atomic<PadButtons>& mask = held_[static_cast<std::size_t>(source)];
  if (!down) {
    mask.fetch_and(static_cast<PadButtons>(~buttons), std::memory_order_relaxed);
    return;
  }
  const std::int64_t now_ns = toNanoseconds(now);
  auto& times = pressed_at_ns_[static_cast<std::size_t>(source)];
  for (std::size_t i = 0; i < kPadButtonTotal; i++) {
    if ((buttons & padButtonBit(static_cast<PadButton>(i))) != 0) {
      times[i].store(now_ns, std::memory_order_relaxed);
    }
  }
  mask.fetch_or(buttons, std::memory_order_relaxed);
}

void KeyboardPadState::releaseAll(InputSource source) {
  const PadButtons released = held_[static_cast<std::size_t>(source)].exchange(0, std::memory_order_relaxed);
  auto& times = pressed_at_ns_[static_cast<std::size_t>(source)];
  for (std::size_t i = 0; i < kPadButtonTotal; i++) {
    if ((released & padButtonBit(static_cast<PadButton>(i))) != 0) {
      times[i].store(kNoPress, std::memory_order_relaxed);
    }
  }
}

PadButtons KeyboardPadState::held() const {
  PadButtons all = 0;
  for (std::size_t i = 0; i < kSourceCount; i++) all |= held_[i].load(std::memory_order_relaxed);
  return all;
}

PadButtons KeyboardPadState::reported(Clock::time_point now) const {
  const PadButtons held_now = held();
  PadButtons buttons = held_now;
  const std::int64_t now_ns = toNanoseconds(now);
  const std::int64_t visible_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(kMinPressVisible).count();
  std::array<std::int64_t, kPadButtonTotal> latest;
  for (std::size_t i = 0; i < kPadButtonTotal; i++) {
    latest[i] = kNoPress;
    for (const auto& times : pressed_at_ns_) {
      latest[i] = std::max(latest[i], times[i].load(std::memory_order_relaxed));
    }
    // A press stamped slightly after `now` (two threads, two clock reads) counts as recent.
    if (latest[i] != kNoPress && now_ns - latest[i] < visible_ns) {
      buttons |= padButtonBit(static_cast<PadButton>(i));
    }
  }
  // A real d-pad never reports opposite directions together. The tail of a released key must not
  // swallow a newly pressed opposite key (quick Up then Down in a menu).
  for (const Axis& axis : kAxes) {
    const PadButtons bit_a = padButtonBit(axis.a);
    const PadButtons bit_b = padButtonBit(axis.b);
    if ((buttons & (bit_a | bit_b)) != (bit_a | bit_b)) continue;
    const bool held_a = (held_now & bit_a) != 0;
    const bool held_b = (held_now & bit_b) != 0;
    if (held_a && held_b) {
      buttons &= static_cast<PadButtons>(~(bit_a | bit_b));
    } else if (held_a != held_b) {
      buttons &= static_cast<PadButtons>(~(held_a ? bit_b : bit_a));
    } else {
      const bool a_newer = latest[static_cast<std::size_t>(axis.a)] > latest[static_cast<std::size_t>(axis.b)];
      buttons &= static_cast<PadButtons>(~(a_newer ? bit_b : bit_a));
    }
  }
  return buttons;
}

std::unique_ptr<PadProvider> makeKeyboardPadProvider(
    std::shared_ptr<KeyboardPadState> state,
    std::function<KeyboardPadState::Clock::time_point()> clock) {
  return std::make_unique<KeyboardPadProvider>(std::move(state), std::move(clock));
}

}  // namespace dal
