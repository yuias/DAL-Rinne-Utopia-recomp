// dal_audiotool: audio utilities for the runtime. Subcommands:
//   wavstat FILE [--window S]   per-window levels of a WAV file (the audio dump, tool output)
//   devices                     the active render devices and their mix formats
//   tone [--output none|wasapi] [--seconds N] [--hz F] [--db L]   a sine through an output
//   play FILE.wav [--output none|wasapi]                          a 48 kHz WAV through an output
//   at3scan FILE                the ATRAC3plus RIFF entries of an archive
// Exits 0 on success, 1 when the work fails, 2 on a usage error.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "audio/audio_output.h"
#include "audio/wasapi_output.h"
#include "audio/wav_file.h"
#include "core/sleeper.h"
#include "media/at3_file.h"

namespace {

struct Subcommand {
  std::string_view name;
  int (*run)(std::span<const std::string_view> args);
};

void printUsage() {
  std::fputs(
      "usage: dal_audiotool wavstat FILE [--window S]\n"
      "       dal_audiotool devices\n"
      "       dal_audiotool tone [--output none|wasapi] [--seconds N] [--hz F] [--db L]\n"
      "       dal_audiotool play FILE.wav [--output none|wasapi]\n"
      "       dal_audiotool at3scan FILE\n"
      "  wavstat   print RMS, peak and non-silent frame count per window of S seconds (default 1)\n"
      "  devices   list the active render devices with their mix format and device periods\n"
      "  tone      play a sine (default wasapi, 2 s, 440 Hz, -18 dBFS) and print the output statistics\n"
      "  play      play a 48 kHz mono or stereo WAV file and print the output statistics\n"
      "  at3scan   list the ATRAC3plus RIFF entries of an archive (offset, size, format, samples, delay, loop)\n",
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

int runDevices(std::span<const std::string_view> args) {
  if (!args.empty()) {
    std::fputs("error: devices takes no arguments\n", stderr);
    return 2;
  }
  const auto devices = dal::listRenderDevices();
  if (!devices) {
    std::fprintf(stderr, "error: %s\n", devices.error().c_str());
    return 1;
  }
  printLine(std::format("render devices: {}", devices->size()));
  for (const auto& device : *devices) {
    printLine(std::format("{} {}{}", device.is_default ? "*" : "-", device.name, device.is_default ? " (default)" : ""));
    printLine(std::format("    mix {} Hz, {} ch, {} bit {}; period default {:.1f} ms, min {:.1f} ms", device.mix_rate,
                          device.mix_channels, device.mix_bits, device.mix_float ? "float" : "integer",
                          device.default_period_ms, device.min_period_ms));
  }
  return devices->empty() ? 1 : 0;
}

using Clock = std::chrono::steady_clock;

bool parseNumber(std::string_view text, double& value) {
  const char* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  return ec == std::errc{} && ptr == end && std::isfinite(value);
}

// Takes the value of an option that must be one of the two output names.
bool parseOutputName(std::string_view text, std::string_view& output) {
  if (text != "none" && text != "wasapi") return false;
  output = text;
  return true;
}

std::unique_ptr<dal::AudioOutput> openOutput(std::string_view kind) {
  std::unique_ptr<dal::AudioOutput> output =
      kind == "wasapi" ? dal::makeWasapiOutput() : dal::makeNullAudioOutput();
  std::string error;
  if (!output->open(error)) {
    std::fprintf(stderr, "error: %.*s output: %s\n", static_cast<int>(kind.size()), kind.data(), error.c_str());
    return nullptr;
  }
  printLine(std::format("output: {} ({})", output->name(), output->description()));
  return output;
}

// Submits `blocks` blocks at the cellAudio rate, each due at an absolute time so that a late wake-up
// does not shift the schedule, then waits for the device to take them and prints the statistics.
int playBlocks(dal::AudioOutput& output, std::uint64_t blocks,
               const std::function<void(std::uint64_t block, std::span<float> samples)>& fill) {
  dal::Sleeper sleeper;
  dal::AudioBlock samples;
  const Clock::time_point start = Clock::now();
  for (std::uint64_t i = 0; i < blocks; ++i) {
    const Clock::time_point due =
        start + std::chrono::nanoseconds(static_cast<std::int64_t>(i * dal::kAudioBlockFrames * 1'000'000'000ULL /
                                                                    dal::kAudioSampleRate));
    sleeper.sleep(std::chrono::duration_cast<std::chrono::microseconds>(due - Clock::now()));
    fill(i, samples);
    output.submit(samples);
  }

  // frames_played counts frames handed to the device. The statistics are taken at that point: once
  // the last block is out, the device running dry is the end of the stream, not an underrun.
  const std::uint64_t wanted_frames = blocks * dal::kAudioBlockFrames;
  const Clock::time_point give_up = Clock::now() + std::chrono::seconds(2);
  while (output.stats().frames_played < wanted_frames && Clock::now() < give_up) {
    sleeper.sleep(std::chrono::milliseconds(2));
  }
  const dal::AudioOutputStats stats = output.stats();
  // Let the device play what it still holds before the output is closed.
  sleeper.sleep(std::chrono::milliseconds(200));

  printLine(std::format("stats: submitted {} blocks, dropped {}, underrun {} frames, played {} frames, device errors {}",
                        stats.blocks_submitted, stats.blocks_dropped, stats.underrun_frames, stats.frames_played,
                        stats.device_errors));
  return 0;
}

int runTone(std::span<const std::string_view> args) {
  std::string_view output_kind = "wasapi";
  double seconds = 2.0;
  double hz = 440.0;
  double db = -18.0;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view option = args[i];
    if (option != "--output" && option != "--seconds" && option != "--hz" && option != "--db") {
      std::fprintf(stderr, "error: unexpected argument %.*s\n", static_cast<int>(option.size()), option.data());
      return 2;
    }
    if (i + 1 >= args.size()) {
      std::fprintf(stderr, "error: %.*s needs a value\n", static_cast<int>(option.size()), option.data());
      return 2;
    }
    const std::string_view text = args[++i];
    bool ok = false;
    if (option == "--output") {
      ok = parseOutputName(text, output_kind);
    } else if (option == "--seconds") {
      ok = parsePositive(text, seconds);
    } else if (option == "--hz") {
      ok = parsePositive(text, hz) && hz < dal::kAudioSampleRate / 2.0;
    } else {
      ok = parseNumber(text, db) && db <= 0.0;
    }
    if (!ok) {
      std::fprintf(stderr, "error: bad value for %.*s: %.*s\n", static_cast<int>(option.size()), option.data(),
                   static_cast<int>(text.size()), text.data());
      return 2;
    }
  }

  auto output = openOutput(output_kind);
  if (!output) return 1;
  const float amplitude = static_cast<float>(std::pow(10.0, db / 20.0));
  printLine(std::format("tone: {} Hz, {} dBFS, {} s", hz, db, seconds));
  const auto blocks =
      static_cast<std::uint64_t>(std::ceil(seconds * dal::kAudioSampleRate / dal::kAudioBlockFrames));
  return playBlocks(*output, blocks, [&](std::uint64_t block, std::span<float> samples) {
    for (std::size_t frame = 0; frame < dal::kAudioBlockFrames; ++frame) {
      const double cycles = hz * static_cast<double>(block * dal::kAudioBlockFrames + frame) / dal::kAudioSampleRate;
      const float value = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * (cycles - std::floor(cycles))));
      samples[2 * frame] = value;
      samples[2 * frame + 1] = value;
    }
  });
}

int runPlay(std::span<const std::string_view> args) {
  std::string_view file;
  std::string_view output_kind = "wasapi";
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--output") {
      if (i + 1 >= args.size() || !parseOutputName(args[++i], output_kind)) {
        std::fputs("error: --output needs none or wasapi\n", stderr);
        return 2;
      }
    } else if (file.empty() && !args[i].starts_with("--")) {
      file = args[i];
    } else {
      std::fprintf(stderr, "error: unexpected argument %.*s\n", static_cast<int>(args[i].size()), args[i].data());
      return 2;
    }
  }
  if (file.empty()) {
    std::fputs("error: play needs a file\n", stderr);
    return 2;
  }

  const auto wav = dal::readWav(std::filesystem::path(file));
  if (!wav) {
    std::fprintf(stderr, "error: %s\n", wav.error().c_str());
    return 1;
  }
  if (wav->sample_rate != dal::kAudioSampleRate) {
    std::fprintf(stderr, "error: play needs a %u Hz file, this one has %u Hz\n", dal::kAudioSampleRate,
                 wav->sample_rate);
    return 1;
  }

  auto output = openOutput(output_kind);
  if (!output) return 1;
  const std::uint64_t frames = wav->frames();
  printLine(std::format("play: {:.3f} s, {} ch", static_cast<double>(frames) / dal::kAudioSampleRate, wav->channels));
  const auto blocks = (frames + dal::kAudioBlockFrames - 1) / dal::kAudioBlockFrames;
  return playBlocks(*output, blocks, [&](std::uint64_t block, std::span<float> samples) {
    for (std::size_t frame = 0; frame < dal::kAudioBlockFrames; ++frame) {
      const std::uint64_t index = block * dal::kAudioBlockFrames + frame;
      float left = 0.0f;
      float right = 0.0f;
      if (index < frames) {
        left = wav->samples[index * wav->channels];
        right = wav->channels > 1 ? wav->samples[index * wav->channels + 1] : left;
      }
      samples[2 * frame] = std::clamp(left, -1.0f, 1.0f);
      samples[2 * frame + 1] = std::clamp(right, -1.0f, 1.0f);
    }
  });
}

int runAt3scan(std::span<const std::string_view> args) {
  if (args.size() != 1 || args[0].starts_with("--")) {
    std::fputs("error: at3scan needs exactly one file\n", stderr);
    return 2;
  }
  std::ifstream stream(std::filesystem::path(args[0]), std::ios::binary | std::ios::ate);
  if (!stream) {
    std::fprintf(stderr, "error: cannot open %.*s\n", static_cast<int>(args[0].size()), args[0].data());
    return 1;
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stream.tellg()));
  stream.seekg(0);
  if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    std::fputs("error: read failed\n", stderr);
    return 1;
  }

  const auto entries = dal::scanRiffEntries(bytes);
  std::size_t parsed = 0, errors = 0, looped = 0, mono = 0, stereo = 0;
  std::size_t unaligned = 0, overrun = 0, loop_start_valid = 0, loop_end_is_valid_end = 0;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto [offset, size] = entries[index];
    // The entry's bytes, clamped to the archive so that a bad size cannot read past the end.
    const std::uint64_t available = std::min<std::uint64_t>(std::uint64_t{size} + 8, bytes.size() - offset);
    const auto info = dal::parseAt3Header(std::span<const std::uint8_t>(bytes).subspan(offset, available));
    if (!info) {
      ++errors;
      printLine(std::format("{} offset {} size {}: error: {}", index, offset, size, info.error()));
      continue;
    }
    ++parsed;
    (info->channels == 1 ? mono : stereo) += 1;

    std::string loop = "none";
    if (info->loop) {
      ++looped;
      loop = std::format("{}-{}", info->loop->start_sample, info->loop->end_sample);
      // The valid audio is [delay, delay + total); a loop start is somewhere inside it and the loop
      // end is normally its last sample.
      if (info->loop->start_sample >= info->delay_samples &&
          std::uint64_t{info->loop->start_sample} < std::uint64_t{info->delay_samples} + info->total_samples) {
        ++loop_start_valid;
      }
      if (std::uint64_t{info->loop->end_sample} + 1 == std::uint64_t{info->delay_samples} + info->total_samples) {
        ++loop_end_is_valid_end;
      }
    }
    std::string flags;
    if (info->data_size % info->block_align != 0) {
      ++unaligned;
      flags += " data-not-multiple-of-block_align";
    }
    if (std::uint64_t{info->data_offset} + info->data_size > available) {
      ++overrun;
      flags += " data-past-entry-end";
    }
    printLine(std::format("{} offset {} size {} ch {} rate {} block_align {} samples {} delay {} loop {}{}", index,
                          offset, size, info->channels, info->sample_rate, info->block_align, info->total_samples,
                          info->delay_samples, loop, flags));
  }
  printLine(std::format("at3scan: {} entries, {} parsed, {} errors; {} mono, {} stereo; {} with a loop", entries.size(),
                        parsed, errors, mono, stereo, looped));
  printLine(std::format("at3scan: loop start inside [delay, delay + samples) in {} of {}; loop end == delay + samples - 1 in {} of {}; "
                        "data size not a multiple of block_align in {}; data past the entry end in {}",
                        loop_start_valid, looped, loop_end_is_valid_end, looped, unaligned, overrun));
  return errors == 0 ? 0 : 1;
}

constexpr Subcommand kSubcommands[] = {
    {"wavstat", runWavstat},
    {"devices", runDevices},
    {"tone", runTone},
    {"play", runPlay},
    {"at3scan", runAt3scan},
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
