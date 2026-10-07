#include "media/ffmpeg_api.h"

#include <array>
#include <format>
#include <mutex>
#include <string_view>
#include <system_error>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" {
#include <libswresample/version_major.h>
}

#include "core/log.h"

namespace dal {

namespace {

// Everything this project loads comes from one directory, so FFmpeg dependencies of the DLLs
// (avutil, swresample) are found there too (DLL_LOAD_DIR) and a copy on PATH or in the current
// directory is never used.
constexpr DWORD kLoadFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;

struct Modules {
  HMODULE avutil = nullptr;
  HMODULE avcodec = nullptr;
  HMODULE swscale = nullptr;
};

using FormatLineFn = decltype(&::av_log_format_line2);

// Set before the log callback is installed and never changed while it can run.
FormatLineFn g_format_line = nullptr;

void ffmpegLogCallback(void* ptr, int level, const char* fmt, va_list vl) {
  // Warnings and errors from FFmpeg are frequent and mostly harmless for a decoder that skips bad
  // units, so even those stay at debug.
  const LogLevel mapped = level <= AV_LOG_WARNING ? LogLevel::Debug : LogLevel::Trace;
  if (!logEnabled(mapped) || g_format_line == nullptr) return;

  // Local on purpose: FFmpeg logs from decoder threads and a shared prefix state would race.
  int print_prefix = 1;
  std::array<char, 1024> line;
  g_format_line(ptr, level, fmt, vl, line.data(), static_cast<int>(line.size()), &print_prefix);
  std::string_view text(line.data());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
  if (text.empty()) return;
  logWrite(mapped, std::format("ffmpeg: {}", text));
}

std::string lastErrorText() {
  return std::system_category().message(static_cast<int>(GetLastError()));
}

std::string joinNames(const std::vector<std::string>& names) {
  std::string joined;
  for (const std::string& name : names) {
    if (!joined.empty()) joined += ", ";
    joined += name;
  }
  return joined;
}

std::filesystem::path executableDirectory() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size()) {
      buffer.resize(length);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::expected<FfmpegApi, std::string> loadFrom(const std::filesystem::path& dir) {
  std::error_code ec;
  const std::filesystem::path absolute_dir = std::filesystem::absolute(dir, ec);
  if (ec || absolute_dir.empty()) {
    return std::unexpected(std::format("cannot resolve the FFmpeg directory '{}'", dir.string()));
  }

  // Load order puts the dependencies first. swresample is only a static import of avcodec.
  const std::array<std::string, 4> files = {
      std::format("avutil-{}.dll", LIBAVUTIL_VERSION_MAJOR),
      std::format("swresample-{}.dll", LIBSWRESAMPLE_VERSION_MAJOR),
      std::format("swscale-{}.dll", LIBSWSCALE_VERSION_MAJOR),
      std::format("avcodec-{}.dll", LIBAVCODEC_VERSION_MAJOR),
  };

  std::vector<std::string> missing_files;
  for (const std::string& file : files) {
    if (!std::filesystem::is_regular_file(absolute_dir / file, ec)) missing_files.push_back(file);
  }
  if (!missing_files.empty()) {
    return std::unexpected(
        std::format("missing {} in {}", joinNames(missing_files), absolute_dir.generic_string()));
  }

  std::array<HMODULE, 4> handles{};
  for (std::size_t i = 0; i < files.size(); ++i) {
    // The directory must be part of an absolute path for DLL_LOAD_DIR to take effect.
    handles[i] = LoadLibraryExW((absolute_dir / files[i]).c_str(), nullptr, kLoadFlags);
    if (handles[i] == nullptr) {
      return std::unexpected(std::format("cannot load {} from {}: {}", files[i],
                                         absolute_dir.generic_string(), lastErrorText()));
    }
  }
  const Modules modules{.avutil = handles[0], .avcodec = handles[3], .swscale = handles[2]};

  FfmpegApi api;
  std::vector<std::string> missing_symbols;
#define DAL_FFMPEG_RESOLVE(library, name)                                                   \
  api.name = reinterpret_cast<decltype(api.name)>(                                          \
      reinterpret_cast<void*>(GetProcAddress(modules.library, #name)));                     \
  if (api.name == nullptr) missing_symbols.emplace_back(#name);
  DAL_FFMPEG_FUNCTIONS(DAL_FFMPEG_RESOLVE)
#undef DAL_FFMPEG_RESOLVE
  if (!missing_symbols.empty()) {
    return std::unexpected(std::format("missing functions: {}", joinNames(missing_symbols)));
  }

  // The struct layouts in the headers must match the loaded libraries.
  struct VersionCheck {
    const char* name;
    unsigned loaded;
    unsigned expected_major;
  };
  const std::array<VersionCheck, 3> checks = {{
      {"avutil", api.avutil_version(), LIBAVUTIL_VERSION_MAJOR},
      {"avcodec", api.avcodec_version(), LIBAVCODEC_VERSION_MAJOR},
      {"swscale", api.swscale_version(), LIBSWSCALE_VERSION_MAJOR},
  }};
  for (const VersionCheck& check : checks) {
    if ((check.loaded >> 16) != check.expected_major) {
      return std::unexpected(std::format("{} major version {} loaded, {} expected", check.name,
                                         check.loaded >> 16, check.expected_major));
    }
  }

  g_format_line = api.av_log_format_line2;
  api.av_log_set_callback(ffmpegLogCallback);
  // The callback filters by our own level; FFmpeg's threshold only bounds what it formats.
  api.av_log_set_level(AV_LOG_VERBOSE);

  logDebug("ffmpeg: avutil {}.{}.{}, avcodec {}.{}.{}, swscale {}.{}.{} from {}",
           checks[0].loaded >> 16, (checks[0].loaded >> 8) & 0xff, checks[0].loaded & 0xff,
           checks[1].loaded >> 16, (checks[1].loaded >> 8) & 0xff, checks[1].loaded & 0xff,
           checks[2].loaded >> 16, (checks[2].loaded >> 8) & 0xff, checks[2].loaded & 0xff,
           absolute_dir.generic_string());
  return api;
}

}  // namespace

std::expected<FfmpegApi, std::string> loadFfmpeg(const std::filesystem::path& dir) {
  // Serialized because the log bridge state is process-wide.
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  return loadFrom(dir);
}

const std::expected<FfmpegApi, std::string>& defaultFfmpeg() {
  static const std::expected<FfmpegApi, std::string> result = [] {
    const std::filesystem::path dir = executableDirectory();
    if (dir.empty()) {
      return std::expected<FfmpegApi, std::string>(
          std::unexpected(std::string("cannot determine the executable directory")));
    }
    return loadFfmpeg(dir);
  }();
  return result;
}

}  // namespace dal
