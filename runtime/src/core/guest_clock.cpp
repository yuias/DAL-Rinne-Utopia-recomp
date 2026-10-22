// The guest's clock: the host monotonic counter, run guestSpeed() times as fast while fast-forward
// is active. Everything the game can time itself by (systemTimeUs, the timebase) reads it.

#include "core/guest_clock.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "core/log.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace {

#if defined(_WIN32)
std::int64_t queryCounter() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::int64_t counterFrequency() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}
#else
// Nanoseconds, so the frequency is a constant. On Apple, CLOCK_UPTIME_RAW is the clock that does not
// count time asleep, like QueryPerformanceCounter does not on Windows.
std::int64_t queryCounter() {
#if defined(__APPLE__)
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
}

std::int64_t counterFrequency() { return 1'000'000'000; }
#endif

// Initialised during static initialisation, so it is the process start for all practical purposes.
const std::int64_t g_start_counter = queryCounter();

std::uint64_t elapsedCounter() {
    const std::int64_t now = queryCounter();
    return now > g_start_counter ? static_cast<std::uint64_t>(now - g_start_counter) : 0;
}

// The published segment is a seqlock: one writer at a time (g_mutex), any number of lock-free readers.
std::atomic<std::uint64_t> g_sequence{0};
std::atomic<std::uint64_t> g_host_origin{0};
std::atomic<std::uint64_t> g_guest_origin{0};
std::atomic<std::uint32_t> g_segment_speed{1};
// Mirrors g_segment_speed for guestSpeed(), which needs no consistent segment.
std::atomic<std::uint32_t> g_speed{1};

std::mutex g_mutex;
dal::FastForwardState g_state;  // guarded by g_mutex

// Recomputes the effective speed from g_state and, when it changed, publishes the continuing segment.
// Returns the new speed, or 0 when it did not change. Called with g_mutex held.
std::uint32_t publishLocked() {
    const std::uint32_t new_speed = g_state.active() ? g_state.speed : 1;
    if (new_speed == g_speed.load(std::memory_order_relaxed)) return 0;

    const std::uint64_t sequence = g_sequence.load(std::memory_order_relaxed);
    g_sequence.store(sequence + 1, std::memory_order_relaxed);
    // The host sample must come after the odd sequence is visible: a reader that validates against the
    // old sequence then sampled its host ticks no later than this one, so the old segment was still
    // right for it and guest time does not step back when the speed drops. The reader's host sample is not
    // ordered before its reload by the C++ model; on x64 that leaves at most a few counter ticks.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const dal::GuestClockSegment current{g_host_origin.load(std::memory_order_relaxed),
                                         g_guest_origin.load(std::memory_order_relaxed),
                                         g_segment_speed.load(std::memory_order_relaxed)};
    const dal::GuestClockSegment next = current.rebased(elapsedCounter(), new_speed);
    g_host_origin.store(next.host_origin, std::memory_order_relaxed);
    g_guest_origin.store(next.guest_origin, std::memory_order_relaxed);
    g_segment_speed.store(next.speed, std::memory_order_relaxed);
    g_sequence.store(sequence + 2, std::memory_order_release);
    g_speed.store(new_speed, std::memory_order_relaxed);
    return new_speed;
}

void logSpeedChange(std::uint32_t new_speed) {
    if (new_speed != 0) dal::logInfo("dal: clock: guest speed {}x", new_speed);
}

}  // namespace

namespace dal {

std::uint64_t GuestClockSegment::guestAt(std::uint64_t host) const {
    return host <= host_origin ? guest_origin : guest_origin + (host - host_origin) * speed;
}

GuestClockSegment GuestClockSegment::rebased(std::uint64_t host, std::uint32_t new_speed) const {
    return GuestClockSegment{host, guestAt(host), new_speed};
}

std::uint64_t guestClockTicks() {
    for (;;) {
        const std::uint64_t sequence = g_sequence.load(std::memory_order_acquire);
        if (sequence & 1) {
            // A writer is mid-update; it may have been preempted, so do not spin out the quantum.
            std::this_thread::yield();
            continue;
        }
        const GuestClockSegment segment{g_host_origin.load(std::memory_order_relaxed),
                                        g_guest_origin.load(std::memory_order_relaxed),
                                        g_segment_speed.load(std::memory_order_relaxed)};
        // Sampled inside the validated window, so the segment was current when the host was read.
        const std::uint64_t host = elapsedCounter();
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_sequence.load(std::memory_order_relaxed) == sequence) return segment.guestAt(host);
    }
}

std::uint64_t guestClockFrequency() { return static_cast<std::uint64_t>(counterFrequency()); }

std::uint32_t guestSpeed() { return g_speed.load(std::memory_order_relaxed); }

std::chrono::microseconds hostDuration(std::chrono::microseconds guest) {
    const std::uint32_t speed = guestSpeed();
    if (speed <= 1 || guest.count() <= 0) return guest;
    const std::int64_t host_us = (guest.count() + speed - 1) / speed;
    return std::chrono::microseconds(std::max<std::int64_t>(host_us, 1));
}

FastForwardState fastForwardState() {
    std::lock_guard lock(g_mutex);
    return g_state;
}

void setFastForwardToggled(bool on) {
    std::uint32_t changed;
    {
        std::lock_guard lock(g_mutex);
        g_state.toggled = on;
        changed = publishLocked();
    }
    logSpeedChange(changed);
}

void setFastForwardHeld(bool held) {
    std::uint32_t changed;
    {
        std::lock_guard lock(g_mutex);
        g_state.held = held;
        changed = publishLocked();
    }
    logSpeedChange(changed);
}

void setFastForwardSpeed(std::uint32_t speed) {
    std::uint32_t changed;
    {
        std::lock_guard lock(g_mutex);
        g_state.speed = std::clamp(speed, kMinFastForwardSpeed, kMaxFastForwardSpeed);
        changed = publishLocked();
    }
    logSpeedChange(changed);
}

}  // namespace dal
