// Command-line options of dal_game: parsing, validation and the values derived from them.

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "input/pad_script.h"

namespace dal {

// The device output --audio can select besides "none": WASAPI on Windows, CoreAudio on macOS.
#if defined(_WIN32)
inline constexpr std::string_view kHostAudioOutput = "wasapi";
#elif defined(__APPLE__)
inline constexpr std::string_view kHostAudioOutput = "coreaudio";
#else
#error "no host audio output for this platform"
#endif

enum class OptionKind : std::uint8_t { Path, String, Unsigned, Flag, FrameList };

// One dal_game option. `name` is the long name without "--" ("game-dir"); its config file key is
// the same with '-' replaced by '_' ("game_dir").
struct OptionSpec {
    std::string_view name;
    OptionKind kind;
};

// The 27 options in usage-text order; the single source for the CLI11 registration and the file
// schema. Excludes --config, --no-config and --help, which have no file key.
std::span<const OptionSpec> gameOptionSpecs();
std::string configKeyFor(std::string_view option_name);  // "game-dir" -> "game_dir"

// Parsed, merged and validated options. Pure data: no run-time objects.
struct GameOptions {
    std::filesystem::path game_dir, elf, hdd_dir, sdata_cache, vfs_config, png_dump, rsx_census, log_file, pad_script,
            audio_dump, dump_frames, shader_cache;  // empty: not given
    std::optional<LogLevel> log_level;              // nullopt: keep the default
    std::string dialog_answer;                      // empty: keep the default
    std::string pad;                                // settled: none|neutral|keyboard|gamepad|keyboard+gamepad
    std::string audio;                              // settled: none|kHostAudioOutput
    std::vector<PadScriptEvent> pad_script_events;  // loaded from pad_script
    unsigned seconds = 0;
    std::uint32_t dump_frames_every = 1;
    std::uint32_t fast_forward = 1;  // 1: off; otherwise the guest speed, 2..8
    std::vector<std::uint32_t> rsx_capture_frames;
    bool strict = false, quiet_hle = false, load_only = false, pad_probe = false, vk_validation = false,
         no_window = false;
    bool window = false, render_vulkan = false;  // derived
    std::filesystem::path config_file;           // the file that was applied (absolute); empty: none
    std::vector<std::string> warnings;           // config file warnings, logged by applyGameOptions
};

// Why parsing ended without options to run with.
struct OptionsExit {
    int exit_code = 0;         // 0 for --help, static_cast<int>(ExitCode::Usage) otherwise
    std::string message;       // complete stderr lines ("error: ...\n"); empty for --help
    bool print_usage = false;  // the usage text follows (stdout for --help, stderr otherwise)
};

// `args` excludes the program name and is UTF-8. Never prints, never changes global state. Reads
// the pad script and creates the --audio-dump file (as before, both are usage errors), and the
// config file: `config.json` in the working directory, else next to the executable, unless
// --config or --no-config says otherwise. `executable_dir` is that second location; empty means the
// directory of the running executable (tests pass their own).
std::expected<GameOptions, OptionsExit> parseGameOptions(std::span<const std::string> args,
                                                         const std::filesystem::path& executable_dir = {});

std::string_view gameOptionsUsage();  // the usage text, ends with '\n'

// Global side effects, in this order: setLogLevel (first, so nothing later logs at the wrong level),
// setLogFile (error -> "cannot open log file <path>"), setDialogAnswer, then the
// `dal: config: <file>` Info line and one Warn line per config file warning.
std::expected<void, std::string> applyGameOptions(const GameOptions& options);

}  // namespace dal
