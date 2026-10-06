#include "core/log.h"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <unordered_set>

namespace dal {
namespace {

struct LogState {
  std::mutex mutex;
  GuestThreadIdFn thread_id_fn = nullptr;
  std::ofstream file;
  std::function<void(std::string_view)> sink;
  std::mutex once_mutex;
  std::unordered_set<std::string> once_keys;
};

// Leaked on purpose: log calls can run during static destruction and from fatal paths that end in
// _Exit.
LogState& state() {
  static LogState* s = new LogState;
  return *s;
}

std::string_view levelTag(LogLevel level) {
  switch (level) {
    case LogLevel::Warn:
      return "warn: ";
    case LogLevel::Error:
      return "error: ";
    case LogLevel::Info:
      break;
  }
  return "";
}

}  // namespace

void setGuestThreadIdProvider(GuestThreadIdFn fn) {
  LogState& s = state();
  std::lock_guard lock(s.mutex);
  s.thread_id_fn = fn;
}

bool setLogFile(const std::string& path) {
  LogState& s = state();
  std::lock_guard lock(s.mutex);
  if (s.file.is_open()) {
    s.file.close();
  }
  s.file.clear();
  if (path.empty()) {
    return true;
  }
  s.file.open(path, std::ios::out | std::ios::trunc);
  return s.file.is_open();
}

void setLogSink(std::function<void(std::string_view)> sink) {
  LogState& s = state();
  std::lock_guard lock(s.mutex);
  s.sink = std::move(sink);
}

void resetLogOnce() {
  LogState& s = state();
  std::lock_guard lock(s.once_mutex);
  s.once_keys.clear();
}

bool firstUse(std::string_view key) {
  LogState& s = state();
  std::lock_guard lock(s.once_mutex);
  return s.once_keys.emplace(key).second;
}

void logWrite(LogLevel level, std::string_view message) {
  LogState& s = state();
  std::lock_guard lock(s.mutex);

  std::string line;
  const std::optional<std::uint32_t> tid = s.thread_id_fn ? s.thread_id_fn() : std::nullopt;
  if (tid) {
    line = std::format("[t {:08x}] ", *tid);
  } else {
    line = "[host] ";
  }
  line += levelTag(level);
  line += message;

  if (s.sink) {
    s.sink(line);
    return;
  }
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), stderr);
  std::fflush(stderr);
  if (s.file.is_open()) {
    s.file.write(line.data(), static_cast<std::streamsize>(line.size()));
    s.file.flush();
  }
}

}  // namespace dal
