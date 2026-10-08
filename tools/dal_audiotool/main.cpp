// dal_audiotool: audio utilities for the runtime. Subcommands:
//   wavstat FILE [--window S]   per-window levels of a WAV file (the audio dump, tool output)
// Exits 0 on success, 1 when the work fails, 2 on a usage error.

#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "audio/wav_file.h"

namespace {

struct Subcommand {
  std::string_view name;
  int (*run)(std::span<const std::string_view> args);
};

void printUsage() {
  std::fputs(
      "usage: dal_audiotool wavstat FILE [--window S]\n"
      "  wavstat   print RMS, peak and non-silent frame count per window of S seconds (default 1)\n",
      stderr);
}

void printLine(const std::string& line) {
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
}

bool parsePositive(std::string_view text, double& value) {
  const char* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  return ec == std::errc{} && ptr == end && value > 0 && std::isfinite(value);
}

int runWavstat(std::span<const std::string_view> args) {
  std::string_view file;
  double window = 1.0;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--window") {
      if (i + 1 >= args.size() || !parsePositive(args[++i], window)) {
        std::fputs("error: --window needs a positive number of seconds\n", stderr);
        return 2;
      }
    } else if (file.empty() && !args[i].starts_with("--")) {
      file = args[i];
    } else {
      std::fprintf(stderr, "error: unexpected argument %.*s\n", static_cast<int>(args[i].size()),
                   args[i].data());
      return 2;
    }
  }
  if (file.empty()) {
    std::fputs("error: wavstat needs a file\n", stderr);
    return 2;
  }

  const auto wav = dal::readWav(std::filesystem::path(file));
  if (!wav) {
    std::fprintf(stderr, "error: %s\n", wav.error().c_str());
    return 1;
  }

  const auto windows = dal::wavStats(wav->samples, wav->sample_rate, wav->channels, window);
  const double rate = wav->sample_rate;
  std::size_t non_silent_windows = 0;
  for (const auto& w : windows) {
    printLine(std::format("t {:.3f}-{:.3f} s: rms {:.1f} dBFS, peak {:.1f} dBFS, non-silent {} frames",
                          static_cast<double>(w.start_frame) / rate, static_cast<double>(w.end_frame) / rate,
                          w.rms_db, w.peak_db, w.non_silent_frames));
    if (w.non_silent_frames != 0) ++non_silent_windows;
  }
  printLine(std::format("wavstat: {:.3f} s, non-silent windows {} of {}",
                        static_cast<double>(wav->frames()) / rate, non_silent_windows, windows.size()));
  return 0;
}

constexpr Subcommand kSubcommands[] = {
    {"wavstat", runWavstat},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printUsage();
    return 2;
  }
  const std::string_view command = argv[1];
  std::vector<std::string_view> args(argv + 2, argv + argc);
  for (const auto& sub : kSubcommands) {
    if (sub.name == command) return sub.run(args);
  }
  std::fprintf(stderr, "error: unknown subcommand %s\n", argv[1]);
  printUsage();
  return 2;
}
