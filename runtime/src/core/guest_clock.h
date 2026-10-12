#pragma once

#include <chrono>
#include <cstdint>

namespace dal {

inline constexpr std::uint32_t kMinFastForwardSpeed = 2;
inline constexpr std::uint32_t kMaxFastForwardSpeed = 8;
inline constexpr std::uint32_t kDefaultFastForwardSpeed = 4;

// Host-to-guest tick mapping from the last speed change on. Pure, for tests.
struct GuestClockSegment {
    std::uint64_t host_origin = 0;   // host ticks at the change
    std::uint64_t guest_origin = 0;  // guest ticks at the change
    std::uint32_t speed = 1;
    // `host` below host_origin reads as host_origin (a reader that sampled the host clock early).
    std::uint64_t guestAt(std::uint64_t host) const;
    // The segment that continues this one at `host` with `new_speed`: guest_origin = guestAt(host).
    GuestClockSegment rebased(std::uint64_t host, std::uint32_t new_speed) const;
};

// Host performance-counter ticks since process start, advancing guestSpeed() times as fast as the
// host clock. Monotonic and continuous across speed changes. Lock-free; any thread.
std::uint64_t guestClockTicks();
std::uint64_t guestClockFrequency();  // ticks per second (the host counter frequency)

// Guest seconds per host second: 1, or FastForwardState::speed while fast-forward is active. Any thread.
std::uint32_t guestSpeed();

// A wait the guest asked for, in host time: guest / guestSpeed(), rounded up; a nonzero duration
// stays at least 1 us.
std::chrono::microseconds hostDuration(std::chrono::microseconds guest);

struct FastForwardState {
    bool toggled = false;  // overlay checkbox or --fast-forward
    bool held = false;     // hold key in the window
    std::uint32_t speed = kDefaultFastForwardSpeed;
    bool active() const { return toggled || held; }
};
FastForwardState fastForwardState();            // any thread
void setFastForwardToggled(bool on);            // any thread
void setFastForwardHeld(bool held);             // any thread
void setFastForwardSpeed(std::uint32_t speed);  // clamped to [kMinFastForwardSpeed, kMaxFastForwardSpeed]

}  // namespace dal
