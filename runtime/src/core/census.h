#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dal {

// PS3 timebase frequency; dalMftb and sys_time_get_timebase_frequency report it.
inline constexpr std::uint64_t kTimebaseFrequency = 79800000;

// Microseconds since the runtime started (a monotonic host clock).
std::uint64_t systemTimeUs();

// Run statistics printed on every stop path. Counters are updated under the guest lock; the
// census keeps its own mutex as well so that the supervisor can print without holding it.

// Counts one syscall. `name` may be null (printed as "?"). `implemented` is false for calls that
// went through the default handler. The first call of each number logs "dal: first sc N name".
void censusSyscall(std::uint32_t number, const char* name, bool implemented);

// Counts one HLE import call; the first call logs "dal: first hle module::name".
void censusHle(const char* module, const char* name, bool implemented);

// Counts a dynamic transfer to an address with no generated function. Returns true the first
// time this (target, site) pair is seen.
bool censusUnknownTarget(std::uint32_t target, std::uint32_t site);

struct ThreadCensusEntry {
  enum class State { Running, Blocked, Exited };
  std::uint32_t id = 0;
  std::string name;
  State state = State::Running;
  std::string blocked_in;  // meaningful for State::Blocked
};

// Returns every guest thread ever created, in creation order. Registered by the thread layer so
// the census does not depend on it.
using ThreadCensusFn = std::vector<ThreadCensusEntry> (*)();
void setThreadCensusProvider(ThreadCensusFn fn);

// The census as text, one line per element, no trailing newline. Entries are sorted by count
// (descending) and then by name. In the syscall and HLE headers "default-handled" is the number of
// distinct entries that used the default handler.
std::string formatCensus();

// Logs formatCensus() line by line.
void printCensus();

// Registers printCensus as the census printer of the stop path.
void installCensus();

// Forgets all counters. Intended for tests.
void resetCensus();

}  // namespace dal
