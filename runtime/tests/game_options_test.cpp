// Tests of the dal_game option parser: derived values, validation messages, CLI11 behaviour and
// the global side effects of applyGameOptions.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "app/config_file.h"
#include "app/game_options.h"
#include "core/fatal.h"
#include "core/log.h"
#include "hle/hle.h"

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            g_passed++;                                                            \
        } else {                                                                   \
            g_failed++;                                                            \
            std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
        }                                                                          \
    } while (0)

constexpr int kUsageExit = static_cast<int>(dal::ExitCode::Usage);

std::string utf8Of(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

// A directory that does not exist, so the executable-directory lookup finds nothing in the plain tests.
// The working directory is an empty scratch directory for the whole run (see main), for the same reason.
const std::filesystem::path kNoExecutableDir = std::filesystem::temp_directory_path() / "dal_game_options_noexe";

std::expected<dal::GameOptions, dal::OptionsExit> parse(const std::vector<std::string>& args) {
    return dal::parseGameOptions(args, kNoExecutableDir);
}

// Checks one failing command line: exit code 2, the exact stderr text and the usage text.
bool expectUsageError(const std::vector<std::string>& args, std::string_view message) {
    const auto result = parse(args);
    if (result) {
        std::fprintf(stderr, "expected an error for:");
        for (const std::string& arg : args) std::fprintf(stderr, " %s", arg.c_str());
        std::fprintf(stderr, "\n");
        return false;
    }
    const bool ok =
            result.error().exit_code == kUsageExit && result.error().message == message && result.error().print_usage;
    if (!ok) {
        std::fprintf(stderr, "unexpected result: code %d, usage %d, message \"%s\"\n  expected \"%.*s\"\n",
                     result.error().exit_code, result.error().print_usage ? 1 : 0, result.error().message.c_str(),
                     static_cast<int>(message.size()), message.data());
    }
    return ok;
}

// A scratch directory for pad scripts and the audio-dump probe.
class TempDir {
public:
    explicit TempDir(const char* name = "dal_game_options_test")
        : path_(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    std::string write(std::string_view name, std::string_view text) const {
        const std::filesystem::path file = path_ / name;
        std::ofstream(file, std::ios::binary) << text;
        return file.string();
    }

private:
    std::filesystem::path path_;
};

// Changes the working directory for a scope; declare it after the TempDir it points into, so the
// directory is left before it is removed.
class ScopedCwd {
public:
    explicit ScopedCwd(const std::filesystem::path& dir) : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(dir);
    }
    ~ScopedCwd() {
        std::error_code ec;
        std::filesystem::current_path(previous_, ec);
    }
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;

private:
    std::filesystem::path previous_;
};

const std::string kRequired =
        "error: --game-dir or --vfs-config is required (or \"game_dir\" or \"vfs_config\" in config.json)\n";

void testGameOptionSpecs() {
    const std::span<const dal::OptionSpec> specs = dal::gameOptionSpecs();
    CHECK(specs.size() == 26);
    CHECK(specs.front().name == "game-dir" && specs.back().name == "no-window");
    CHECK(dal::configKeyFor("game-dir") == "game_dir");
    CHECK(dal::configKeyFor("dump-frames-every") == "dump_frames_every");
    CHECK(dal::configKeyFor("seconds") == "seconds");
    const std::string_view usage = dal::gameOptionsUsage();
    CHECK(usage.starts_with("usage: dal_game "));
    CHECK(usage.ends_with("\n"));
    CHECK(usage.find("[-h|--help]") != std::string_view::npos);
    CHECK(usage.find("[--config FILE | --no-config]") != std::string_view::npos);
    for (const dal::OptionSpec& spec : specs) {
        CHECK(usage.find("--" + std::string(spec.name)) != std::string_view::npos);
    }
}

void testDerivedValues() {
    {
        const auto result = parse({"--game-dir", "g"});
        CHECK(result.has_value());
        if (result) {
            CHECK(result->game_dir == std::filesystem::path("g"));
            CHECK(result->window && result->render_vulkan);
            CHECK(result->pad == "keyboard+gamepad");
            CHECK(result->audio == "wasapi");
            CHECK(result->seconds == 0 && result->dump_frames_every == 1);
            CHECK(!result->log_level && result->dialog_answer.empty() && result->warnings.empty());
        }
    }
    {
        const auto result = parse({"--game-dir", "g", "--no-window"});
        CHECK(result.has_value());
        if (result) {
            CHECK(result->no_window && !result->window && !result->render_vulkan);
            CHECK(result->pad == "neutral");
            CHECK(result->audio == "none");
        }
    }
    {
        const auto result = parse({"--game-dir", "g", "--render", "none"});
        CHECK(result.has_value());
        if (result) CHECK(!result->window && !result->render_vulkan);
    }
    {
        const auto result = parse({"--game-dir", "g", "--no-window", "--render", "vulkan"});
        CHECK(result.has_value());
        if (result) CHECK(!result->window && result->render_vulkan);
    }
    {
        const auto result = parse({"--game-dir", "g", "--no-window", "--dump-frames", "d"});
        CHECK(result.has_value());
        if (result) {
            CHECK(!result->window && result->render_vulkan);
            CHECK(result->dump_frames == std::filesystem::path("d"));
        }
    }
    {
        const TempDir dir;
        const std::string script = dir.write("pad.txt", "0.1 tap cross\n0.5 press start\n");
        const auto result = parse({"--game-dir", "g", "--no-window", "--pad-script", script});
        CHECK(result.has_value());
        if (result) {
            CHECK(result->pad == "keyboard");
            CHECK(!result->window);
            CHECK(result->pad_script_events.size() == 3);  // a tap is a press and a release
        }
    }
    {
        const auto result = parse({"--vfs-config", "v"});
        CHECK(result.has_value());
        if (result) {
            CHECK(result->game_dir.empty());
            CHECK(result->vfs_config == std::filesystem::path("v"));
        }
    }
    {
        // Every kind of value in one line. The audio-dump probe creates its file, so it goes to the scratch directory.
        const TempDir dir;
        const std::string wav = (dir.path() / "dump.wav").string();
        const auto result = parse({"--game-dir",
                                   "g",
                                   "--elf",
                                   "e",
                                   "--hdd-dir",
                                   "h",
                                   "--sdata-cache",
                                   "s",
                                   "--png-dump",
                                   "p",
                                   "--rsx-census",
                                   "c",
                                   "--log",
                                   "l",
                                   "--audio-dump",
                                   wav,
                                   "--shader-cache",
                                   "sc",
                                   "--dump-frames",
                                   "df",
                                   "--dump-frames-every",
                                   "7",
                                   "--seconds",
                                   "9",
                                   "--log-level",
                                   "debug",
                                   "--dialog-answer",
                                   "no",
                                   "--pad",
                                   "none",
                                   "--audio",
                                   "none",
                                   "--render",
                                   "vulkan",
                                   "--rsx-capture",
                                   "300,600",
                                   "--strict",
                                   "--quiet-hle",
                                   "--load-only",
                                   "--vk-validation"});
        CHECK(result.has_value());
        if (result) {
            CHECK(result->elf == std::filesystem::path("e") && result->hdd_dir == std::filesystem::path("h"));
            CHECK(result->sdata_cache == std::filesystem::path("s") && result->png_dump == std::filesystem::path("p"));
            CHECK(result->rsx_census == std::filesystem::path("c") && result->log_file == std::filesystem::path("l"));
            CHECK(result->shader_cache == std::filesystem::path("sc"));
            CHECK(result->audio_dump == std::filesystem::path(wav));
            CHECK(result->dump_frames == std::filesystem::path("df") && result->dump_frames_every == 7);
            CHECK(result->seconds == 9);
            CHECK(result->log_level && *result->log_level == dal::LogLevel::Debug);
            CHECK(result->dialog_answer == "no" && result->pad == "none" && result->audio == "none");
            CHECK(result->rsx_capture_frames == (std::vector<std::uint32_t>{300, 600}));
            CHECK(result->strict && result->quiet_hle && result->load_only && result->vk_validation);
            CHECK(!result->pad_probe && !result->no_window);
        }
    }
}

void testValidationMessages() {
    const TempDir dir;
    const std::string script = dir.write("pad.txt", "0.1 tap cross\n");
    const std::string bad_script = dir.write("bad.txt", "0.1 tap cross\n0.2 frobnicate cross\n");
    const std::filesystem::path missing_wav = dir.path() / "missing" / "x.wav";

    CHECK(expectUsageError({}, kRequired));
    CHECK(expectUsageError({"--seconds", "10"}, kRequired));

    CHECK(expectUsageError({"--game-dir", "g", "--log-level", "loud"},
                           "error: --log-level needs trace, debug, info, warn or error\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--dialog-answer", "maybe"},
                           "error: --dialog-answer needs yes, no or ok\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--pad", "bogus"},
                           "error: --pad needs none, neutral, keyboard, gamepad or keyboard+gamepad\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--audio", "bogus"}, "error: --audio needs none or wasapi\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--render", "bogus"}, "error: --render needs none or vulkan\n"));

    CHECK(expectUsageError({"--game-dir", "g", "--seconds=-1"}, "error: --seconds needs a non-negative integer\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--seconds", "-1"}, "error: --seconds needs a non-negative integer\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--seconds", "abc"}, "error: --seconds needs a non-negative integer\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--seconds", "5x"}, "error: --seconds needs a non-negative integer\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--dump-frames-every", "0", "--dump-frames", "d"},
                           "error: --dump-frames-every needs a positive integer\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--dump-frames-every", "abc"},
                           "error: --dump-frames-every needs a positive integer\n"));

    const std::string_view frames = "error: --rsx-capture needs frame numbers (1-based), e.g. 300,600\n";
    CHECK(expectUsageError({"--game-dir", "g", "--rsx-capture", "0"}, frames));
    CHECK(expectUsageError({"--game-dir", "g", "--rsx-capture", "1,"}, frames));
    CHECK(expectUsageError({"--game-dir", "g", "--rsx-capture", ",1"}, frames));
    CHECK(expectUsageError({"--game-dir", "g", "--rsx-capture", "1,,2"}, frames));
    CHECK(expectUsageError({"--game-dir", "g", "--rsx-capture", "x"}, frames));

    const std::string_view render_needed =
            "error: --dump-frames, --dump-frames-every, --shader-cache and --vk-validation need --render vulkan\n";
    CHECK(expectUsageError({"--game-dir", "g", "--render", "none", "--dump-frames", "d"}, render_needed));
    CHECK(expectUsageError({"--game-dir", "g", "--no-window", "--shader-cache", "s"}, render_needed));
    CHECK(expectUsageError({"--game-dir", "g", "--no-window", "--vk-validation"}, render_needed));
    CHECK(expectUsageError({"--game-dir", "g", "--no-window", "--dump-frames-every", "2"}, render_needed));

    CHECK(expectUsageError({"--game-dir", "g", "--pad-probe", "--no-window"}, "error: --pad-probe needs the window\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--pad", "keyboard", "--no-window"},
                           "error: --pad keyboard needs the window or --pad-script\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--pad", "gamepad", "--no-window"},
                           "error: --pad gamepad needs the window\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--pad", "keyboard+gamepad", "--no-window", "--pad-script", script},
                           "error: --pad keyboard+gamepad needs the window\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--pad-script", script, "--pad", "neutral"},
                           "error: --pad-script needs --pad keyboard or keyboard+gamepad\n"));

    CHECK(expectUsageError({"--game-dir", "g", "--pad-script", bad_script},
                           "error: --pad-script " + bad_script + ": line 2: unknown command \"frobnicate\"\n"));
    {
        const auto result = parse({"--game-dir", "g", "--pad-script", (dir.path() / "nothing.txt").string()});
        CHECK(!result.has_value());
        if (!result) {
            CHECK(result.error().exit_code == kUsageExit && result.error().print_usage);
            CHECK(result.error().message.starts_with("error: --pad-script "));
            CHECK(result.error().message.find("cannot open") != std::string::npos);
        }
    }
    {
        const auto result = parse({"--game-dir", "g", "--audio-dump", missing_wav.string()});
        CHECK(!result.has_value());
        if (!result) {
            CHECK(result.error().exit_code == kUsageExit && result.error().print_usage);
            CHECK(result.error().message.starts_with("error: --audio-dump " + missing_wav.string() + ": "));
        }
    }
    {
        // A usable path is probed and accepted; the probe leaves the file behind.
        const auto result = parse({"--game-dir", "g", "--audio-dump", (dir.path() / "ok.wav").string()});
        CHECK(result.has_value());
    }
}

void testLastWins() {
    {
        const auto result = parse({"--game-dir", "g", "--seconds", "5", "--seconds", "10"});
        CHECK(result.has_value());
        if (result) CHECK(result->seconds == 10);
    }
    {
        const auto result = parse({"--game-dir", "a", "--game-dir", "b"});
        CHECK(result.has_value());
        if (result) CHECK(result->game_dir == std::filesystem::path("b"));
    }
    {
        const auto result = parse({"--game-dir", "g", "--render", "none", "--render", "vulkan"});
        CHECK(result.has_value());
        if (result) CHECK(result->window && result->render_vulkan);
    }
    {
        // A bad earlier value is replaced by a later good one, as before.
        const auto result = parse({"--game-dir", "g", "--pad", "bogus", "--pad", "none"});
        CHECK(result.has_value());
        if (result) CHECK(result->pad == "none");
    }
    {
        // --rsx-capture accumulates, one value per occurrence.
        const auto result = parse({"--game-dir", "g", "--rsx-capture", "1", "--rsx-capture", "2,3"});
        CHECK(result.has_value());
        if (result) CHECK(result->rsx_capture_frames == (std::vector<std::uint32_t>{1, 2, 3}));
    }
    {
        // The value after --rsx-capture is not swallowed into the list.
        const auto result = parse({"--rsx-capture", "5", "--game-dir", "g"});
        CHECK(result.has_value());
        if (result) {
            CHECK(result->rsx_capture_frames == (std::vector<std::uint32_t>{5}));
            CHECK(result->game_dir == std::filesystem::path("g"));
        }
    }
}

void testCli11Errors() {
    CHECK(expectUsageError({"--game-dir", "g", "--bogus"},
                           "error: The following argument was not expected: --bogus\n"));
    CHECK(expectUsageError({"--game-dir", "g", "extra"}, "error: The following argument was not expected: extra\n"));
    CHECK(expectUsageError({"--game-dir"}, "error: --game-dir: 1 required TEXT missing\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--seconds"}, "error: --seconds: 1 required TEXT missing\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--strict=bogus"}, "error: Could not convert: --strict = bogus\n"));
    // Windows-style options are off, so a slash-prefixed word is an unknown argument like any other.
    CHECK(expectUsageError({"--game-dir", "g", "/strict"},
                           "error: The following argument was not expected: /strict\n"));

    for (const char* flag : {"--help", "-h"}) {
        const auto result = parse({flag});
        CHECK(!result.has_value());
        if (!result) {
            CHECK(result.error().exit_code == 0);
            CHECK(result.error().message.empty());
            CHECK(result.error().print_usage);
        }
    }
    {
        // --help wins over a missing required option.
        const auto result = parse({"--game-dir", "g", "--help"});
        CHECK(!result.has_value() && result.error().exit_code == 0);
    }
}

void testValuesStartingWithDash() {
    // A separate value is taken whatever it starts with, as before.
    const auto log = parse({"--game-dir", "g", "--log", "-x.txt"});
    CHECK(log.has_value());
    if (log) CHECK(log->log_file == std::filesystem::path("-x.txt"));

    const auto slash = parse({"--game-dir", "/abs/game", "--elf", "/abs/EBOOT.elf"});
    CHECK(slash.has_value());
    if (slash) {
        CHECK(slash->game_dir == std::filesystem::path("/abs/game"));
        CHECK(slash->elf == std::filesystem::path("/abs/EBOOT.elf"));
    }
}

void testFlagsAndEqualsForm() {
    {
        const auto result = parse({"--game-dir=g", "--seconds=4", "--log-level=trace"});
        CHECK(result.has_value());
        if (result) {
            CHECK(result->game_dir == std::filesystem::path("g"));
            CHECK(result->seconds == 4);
            CHECK(result->log_level && *result->log_level == dal::LogLevel::Trace);
        }
    }
    {
        const auto result = parse({"--game-dir", "g", "--strict", "--strict=false"});
        CHECK(result.has_value());
        if (result) CHECK(!result->strict);
    }
    {
        const auto result = parse({"--game-dir", "g", "--strict=false", "--strict"});
        CHECK(result.has_value());
        if (result) CHECK(result->strict);
    }
    {
        const auto result = parse({"--game-dir", "g", "--no-window=true", "--load-only=true"});
        CHECK(result.has_value());
        if (result) CHECK(result->no_window && result->load_only && !result->window);
    }
    {
        const auto result = parse({"--game-dir", "g", "--no-window", "--no-window=false"});
        CHECK(result.has_value());
        if (result) CHECK(!result->no_window && result->window);
    }
}

void testUtf8Path() {
    // Arguments are UTF-8; the path must carry the same code points, not the ANSI code page's view.
    const std::u8string name = u8"日本語_éü";
    const std::string arg(name.begin(), name.end());
    const auto result = parse({"--game-dir", arg, "--hdd-dir=" + arg});
    CHECK(result.has_value());
    if (result) {
        CHECK(result->game_dir == std::filesystem::path(name));
        CHECK(result->game_dir.u8string() == name);
        CHECK(result->hdd_dir == std::filesystem::path(name));
    }
}

// Config file reader. `base` stands in for the directory of the file.
const std::filesystem::path kConfigBase = std::filesystem::temp_directory_path() / "dal_config_base";

std::filesystem::path pathOfInput(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// Checks that `text` fails with exactly `message`.
bool expectConfigError(std::string_view text, std::string_view message) {
    const auto result = dal::parseConfigFile(text, kConfigBase, dal::gameOptionSpecs());
    if (result) {
        std::fprintf(stderr, "expected a config error for: %.*s\n", static_cast<int>(text.size()), text.data());
        return false;
    }
    if (result.error() != message) {
        std::fprintf(stderr, "unexpected config error: \"%s\"\n  expected \"%.*s\"\n", result.error().c_str(),
                     static_cast<int>(message.size()), message.data());
        return false;
    }
    return true;
}

void testConfigFileKinds() {
    const auto result = dal::parseConfigFile(R"({
  "game_dir": "game",
  "log_level": "debug",
  "seconds": 12,
  "no_window": false,
  "strict": true,
  "rsx_capture": [300, 600]
})",
                                             kConfigBase, dal::gameOptionSpecs());
    CHECK(result.has_value());
    if (!result) return;
    CHECK(result->warnings.empty());
    CHECK(result->values.size() == 6);
    if (result->values.size() != 6) return;
    // Document order, one input each, option name and key spelled as designed.
    const auto& values = result->values;
    CHECK(values[0].option == "game-dir" && values[0].key == "game_dir" && values[0].inputs.size() == 1);
    CHECK(pathOfInput(values[0].inputs[0]) == kConfigBase / "game");
    CHECK(values[1].option == "log-level" && values[1].inputs == std::vector<std::string>{"debug"});
    CHECK(values[2].option == "seconds" && values[2].inputs == std::vector<std::string>{"12"});
    CHECK(values[3].option == "no-window" && values[3].key == "no_window");
    CHECK(values[3].inputs == std::vector<std::string>{"false"});
    CHECK(values[4].inputs == std::vector<std::string>{"true"});
    CHECK(values[5].option == "rsx-capture" && values[5].inputs == std::vector<std::string>{"300,600"});

    const auto empty = dal::parseConfigFile("{}", kConfigBase, dal::gameOptionSpecs());
    CHECK(empty.has_value() && empty->values.empty() && empty->warnings.empty());
    const auto bom = dal::parseConfigFile("\xEF\xBB\xBF{\"seconds\": 1}", kConfigBase, dal::gameOptionSpecs());
    CHECK(bom.has_value() && bom->values.size() == 1);
    const auto big = dal::parseConfigFile("{\"seconds\": 18446744073709551615}", kConfigBase, dal::gameOptionSpecs());
    CHECK(big.has_value() && big->values.size() == 1 && big->values[0].inputs[0] == "18446744073709551615");
}

void testConfigFilePaths() {
    const std::filesystem::path absolute = std::filesystem::temp_directory_path() / "abs" / "dir";
    // Backslashes of a Windows path are JSON escapes, so the text is built from the generic form.
    const std::u8string generic = absolute.generic_u8string();
    const std::string text = "{\"game_dir\": \"" + std::string(generic.begin(), generic.end()) +
                             "\", \"hdd_dir\": \"saves/sub\", \"elf\": \"../x/./y.elf\", \"log\": \"a/../b\"}";
    const auto result = dal::parseConfigFile(text, kConfigBase, dal::gameOptionSpecs());
    CHECK(result.has_value());
    if (!result || result->values.size() != 4) return;
    CHECK(pathOfInput(result->values[0].inputs[0]) == absolute.lexically_normal());
    CHECK(pathOfInput(result->values[1].inputs[0]) == kConfigBase / "saves" / "sub");
    CHECK(pathOfInput(result->values[2].inputs[0]) == kConfigBase.parent_path() / "x" / "y.elf");
    CHECK(pathOfInput(result->values[3].inputs[0]) == kConfigBase / "b");

    // A path with code points outside the ANSI code page survives the conversion both ways.
    const std::u8string name = u8"日本語_éü";
    const auto utf8 = dal::parseConfigFile("{\"hdd_dir\": \"" + std::string(name.begin(), name.end()) + "\"}",
                                           kConfigBase, dal::gameOptionSpecs());
    CHECK(utf8.has_value());
    if (utf8 && utf8->values.size() == 1) {
        const std::filesystem::path expected = kConfigBase / std::filesystem::path(name);
        CHECK(pathOfInput(utf8->values[0].inputs[0]) == expected);
        const std::u8string expected_utf8 = expected.lexically_normal().u8string();
        CHECK(utf8->values[0].inputs[0] == std::string(expected_utf8.begin(), expected_utf8.end()));
    }
}

void testConfigFileErrors() {
    CHECK(expectConfigError("{\"a\":1,\"a\":2}", "duplicate key \"a\" in an object"));
    CHECK(expectConfigError("{\"x\": {\"b\": 1, \"b\": 2}}", "duplicate key \"b\" in an object"));
    CHECK(expectConfigError("{\"seconds\": 1, \"seconds\": 2}", "duplicate key \"seconds\" in an object"));
    CHECK(expectConfigError("[1, 2]", "the top level must be an object"));
    CHECK(expectConfigError("\"game_dir\"", "the top level must be an object"));
    CHECK(expectConfigError("null", "the top level must be an object"));

    CHECK(expectConfigError("{\"game_dir\": 5}", "\"game_dir\": expected a path string, got number"));
    CHECK(expectConfigError("{\"game_dir\": [\"a\"]}", "\"game_dir\": expected a path string, got array"));
    CHECK(expectConfigError("{\"elf\": \"\"}", "\"elf\": the path is empty"));
    CHECK(expectConfigError("{\"pad\": 1}", "\"pad\": expected a string, got number"));
    CHECK(expectConfigError("{\"pad\": true}", "\"pad\": expected a string, got boolean"));
    CHECK(expectConfigError("{\"seconds\": \"5\"}", "\"seconds\": expected a non-negative integer, got string"));
    CHECK(expectConfigError("{\"seconds\": -1}", "\"seconds\": expected a non-negative integer, got number"));
    CHECK(expectConfigError("{\"seconds\": 1.5}", "\"seconds\": expected a non-negative integer, got number"));
    CHECK(expectConfigError("{\"dump_frames_every\": {}}",
                            "\"dump_frames_every\": expected a non-negative integer, got object"));
    CHECK(expectConfigError("{\"strict\": \"true\"}", "\"strict\": expected true or false, got string"));
    CHECK(expectConfigError("{\"no_window\": 1}", "\"no_window\": expected true or false, got number"));
    CHECK(expectConfigError("{\"rsx_capture\": 300}",
                            "\"rsx_capture\": expected a string or an array of positive integers, got number"));
    CHECK(expectConfigError("{\"rsx_capture\": [1, \"2\"]}",
                            "\"rsx_capture\": expected a string or an array of positive integers, got string"));
    CHECK(expectConfigError("{\"rsx_capture\": [1, -2]}",
                            "\"rsx_capture\": expected a string or an array of positive integers, got number"));
    CHECK(expectConfigError("{\"rsx_capture\": []}",
                            "\"rsx_capture\": expected a string or an array of positive integers, got array"));

    // null is refused for every kind of option.
    for (const char* key : {"game_dir", "pad", "seconds", "strict", "rsx_capture"}) {
        CHECK(expectConfigError(
                std::string("{\"") + key + "\": null}",
                std::string("\"") + key + "\": null is not a value; remove the key to use the default"));
    }

    // The first bad value in document order is the one reported.
    CHECK(expectConfigError("{\"strict\": 1, \"pad\": 2}", "\"strict\": expected true or false, got number"));

    // yyjson reports positions for syntax errors only; the line is counted over CRLF text too.
    {
        const auto result = dal::parseConfigFile("{\r\n  \"seconds\": 1,\r\n  \"pad\": ]\r\n}", kConfigBase,
                                                 dal::gameOptionSpecs());
        CHECK(!result.has_value());
        if (!result) {
            CHECK(result.error().starts_with("invalid JSON: "));
            CHECK(result.error().ends_with(" at line 3, column 10 (byte 29)"));
        }
    }
    {
        // No comments and no trailing commas.
        const auto comment = dal::parseConfigFile("{\n// c\n}", kConfigBase, dal::gameOptionSpecs());
        CHECK(!comment && comment.error().starts_with("invalid JSON: ") &&
              comment.error().find("at line 2, column 1 (byte 2)") != std::string::npos);
        const auto comma = dal::parseConfigFile("{\"seconds\": 1,}", kConfigBase, dal::gameOptionSpecs());
        CHECK(!comma && comma.error().starts_with("invalid JSON: "));
        const auto truncated = dal::parseConfigFile("{", kConfigBase, dal::gameOptionSpecs());
        CHECK(!truncated && truncated.error().starts_with("invalid JSON: "));
        const auto none = dal::parseConfigFile("", kConfigBase, dal::gameOptionSpecs());
        CHECK(!none && none.error().starts_with("invalid JSON: "));
        // yyjson rejects invalid UTF-8 itself, so a path never reaches the path conversion with it.
        const auto invalid = dal::parseConfigFile("{\"elf\": \"\xFF\"}", kConfigBase, dal::gameOptionSpecs());
        CHECK(!invalid && invalid.error().starts_with("invalid JSON: "));
    }
}

void testConfigFileUnknownKeys() {
    const auto result = dal::parseConfigFile(
            R"({"config": "x.json", "seconds": 3, "no_config": true, "help": true, "future": null, "game-dir": "g"})",
            kConfigBase, dal::gameOptionSpecs());
    CHECK(result.has_value());
    if (!result) return;
    // Unknown keys are not checked, so a null or a wrong type does not fail the file.
    CHECK(result->values.size() == 1 && result->values[0].option == "seconds");
    CHECK(result->warnings ==
          (std::vector<std::string>{"unknown key \"config\" ignored", "unknown key \"no_config\" ignored",
                                    "unknown key \"help\" ignored", "unknown key \"future\" ignored",
                                    "unknown key \"game-dir\" ignored"}));
}

void testConfigFileFrameList() {
    const auto array = dal::parseConfigFile(R"({"rsx_capture": [5]})", kConfigBase, dal::gameOptionSpecs());
    CHECK(array.has_value() && array->values.size() == 1 && array->values[0].inputs == std::vector<std::string>{"5"});
    const auto many = dal::parseConfigFile(R"({"rsx_capture": [1, 20, 300]})", kConfigBase, dal::gameOptionSpecs());
    CHECK(many.has_value() && many->values.size() == 1 &&
          many->values[0].inputs == std::vector<std::string>{"1,20,300"});
    // The string form is the command-line form and is checked later, like the command line.
    const auto text = dal::parseConfigFile(R"({"rsx_capture": "300,600"})", kConfigBase, dal::gameOptionSpecs());
    CHECK(text.has_value() && text->values.size() == 1 &&
          text->values[0].inputs == std::vector<std::string>{"300,600"});
    const auto bad_text = dal::parseConfigFile(R"({"rsx_capture": "1,,x"})", kConfigBase, dal::gameOptionSpecs());
    CHECK(bad_text.has_value() && bad_text->values[0].inputs == std::vector<std::string>{"1,,x"});
    // Zero is an unsigned integer here; the frame number check (1-based) is the validation phase's.
    const auto zero = dal::parseConfigFile(R"({"rsx_capture": [0]})", kConfigBase, dal::gameOptionSpecs());
    CHECK(zero.has_value() && zero->values[0].inputs == std::vector<std::string>{"0"});
}

void testConfigFileAcceptsEveryKey() {
    std::string text = "{";
    for (const dal::OptionSpec& spec : dal::gameOptionSpecs()) {
        const char* value = "\"v\"";
        switch (spec.kind) {
            case dal::OptionKind::Path:
            case dal::OptionKind::String: value = "\"v\""; break;
            case dal::OptionKind::Unsigned: value = "7"; break;
            case dal::OptionKind::Flag: value = "true"; break;
            case dal::OptionKind::FrameList: value = "[2]"; break;
        }
        if (text.size() > 1) text += ", ";
        text += "\"" + dal::configKeyFor(spec.name) + "\": " + value;
    }
    text += "}";
    const auto result = dal::parseConfigFile(text, kConfigBase, dal::gameOptionSpecs());
    CHECK(result.has_value());
    if (!result) return;
    CHECK(result->warnings.empty());
    CHECK(result->values.size() == dal::gameOptionSpecs().size());
    for (std::size_t i = 0; i < result->values.size() && i < dal::gameOptionSpecs().size(); ++i) {
        CHECK(result->values[i].option == dal::gameOptionSpecs()[i].name);
        CHECK(result->values[i].key == dal::configKeyFor(result->values[i].option));
        CHECK(result->values[i].inputs.size() == 1);
    }
}

// The tracked example must stay a valid file, so users can copy it.
void testConfigFileExample() {
    const std::filesystem::path file = std::filesystem::path(DAL_SOURCE_DIR) / "config" / "dal_game.example.json";
    std::ifstream in(file, std::ios::binary);
    CHECK(in.good());
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!text.empty());
    const auto result = dal::parseConfigFile(text, file.parent_path(), dal::gameOptionSpecs());
    CHECK(result.has_value());
    if (!result) {
        std::fprintf(stderr, "example file error: %s\n", result.error().c_str());
        return;
    }
    CHECK(result->warnings.empty());
    CHECK(!result->values.empty());
}

void testApplyGameOptions() {
    CHECK(!dal::logEnabled(dal::LogLevel::Debug));
    {
        dal::GameOptions options;
        options.log_level = dal::LogLevel::Debug;
        CHECK(dal::applyGameOptions(options).has_value());
        CHECK(dal::logEnabled(dal::LogLevel::Debug));
        options.log_level = dal::LogLevel::Error;
        CHECK(dal::applyGameOptions(options).has_value());
        CHECK(!dal::logEnabled(dal::LogLevel::Warn) && dal::logEnabled(dal::LogLevel::Error));
    }
    dal::setLogLevel(dal::LogLevel::Info);

    // The option parser's own list (yes, no, ok) is exactly what setDialogAnswer accepts.
    for (const char* answer : {"yes", "no", "ok"}) {
        const auto parsed = parse({"--game-dir", "g", "--dialog-answer", answer});
        CHECK(parsed.has_value());
        CHECK(dal::setDialogAnswer(answer));
        if (parsed) {
            CHECK(parsed->dialog_answer == answer);
            CHECK(dal::applyGameOptions(*parsed).has_value());
        }
    }
    for (const char* answer : {"maybe", "YES", ""}) {
        CHECK(!dal::setDialogAnswer(answer));
    }
    dal::resetCellMsgDialog();

    // Nothing to apply: the log level stays.
    CHECK(dal::applyGameOptions(dal::GameOptions{}).has_value());
    CHECK(dal::logEnabled(dal::LogLevel::Info) && !dal::logEnabled(dal::LogLevel::Debug));
}

// ---- config.json lookup and merging ----

// Checks one failing parse that is not a command-line mistake: exit code 2, the exact text, no usage.
bool expectConfigFailure(const std::expected<dal::GameOptions, dal::OptionsExit>& result, std::string_view message) {
    if (result) {
        std::fprintf(stderr, "expected a config error\n");
        return false;
    }
    const bool ok =
            result.error().exit_code == kUsageExit && result.error().message == message && !result.error().print_usage;
    if (!ok) {
        std::fprintf(stderr, "unexpected result: code %d, usage %d, message \"%s\"\n  expected \"%.*s\"\n",
                     result.error().exit_code, result.error().print_usage ? 1 : 0, result.error().message.c_str(),
                     static_cast<int>(message.size()), message.data());
    }
    return ok;
}

std::filesystem::path normal(const std::filesystem::path& path) { return path.lexically_normal(); }

void testConfigLookupNoFile() {
    const TempDir dir("dal_game_options_cfg");
    const ScopedCwd cwd(dir.path());
    const auto result = parse({"--game-dir", "g"});
    CHECK(result.has_value());
    if (result) CHECK(result->config_file.empty() && result->warnings.empty() && result->seconds == 0);
    // A directory named config.json is not a file and is skipped.
    std::filesystem::create_directories(dir.path() / "config.json");
    CHECK(parse({"--game-dir", "g"}).has_value());
}

void testConfigLookupWorkingDirectory() {
    const TempDir dir("dal_game_options_cfg");
    dir.write("config.json", R"({"game_dir": "gd", "seconds": 10, "log_level": "debug"})");
    const ScopedCwd cwd(dir.path());
    const auto result = parse({});
    CHECK(result.has_value());
    if (!result) return;
    CHECK(normal(result->config_file) == normal(std::filesystem::absolute("config.json")));
    CHECK(normal(result->game_dir) == normal(std::filesystem::current_path() / "gd"));
    CHECK(result->seconds == 10 && result->log_level && *result->log_level == dal::LogLevel::Debug);
    CHECK(result->warnings.empty());
}

void testConfigLookupExecutableDirectory() {
    const TempDir dir("dal_game_options_cfg");
    const std::filesystem::path work = dir.path() / "work";
    const std::filesystem::path exe = dir.path() / "exe";
    std::filesystem::create_directories(work);
    std::filesystem::create_directories(exe);
    dir.write("exe/config.json", R"({"game_dir": "from_exe", "seconds": 2})");
    const ScopedCwd cwd(work);
    {
        // No file in the working directory: the one next to the executable is used.
        const auto result = dal::parseGameOptions({}, exe);
        CHECK(result.has_value());
        if (result) {
            CHECK(normal(result->config_file) == normal(exe / "config.json"));
            CHECK(normal(result->game_dir) == normal(exe / "from_exe") && result->seconds == 2);
        }
    }
    dir.write("work/config.json", R"({"game_dir": "from_cwd"})");
    {
        // Both exist: only the working directory's is read, nothing is merged.
        const auto result = dal::parseGameOptions({}, exe);
        CHECK(result.has_value());
        if (result) {
            CHECK(normal(result->game_dir) == normal(std::filesystem::current_path() / "from_cwd"));
            CHECK(result->seconds == 0);
        }
    }
    {
        // An explicit --config replaces both lookups.
        const std::vector<std::string> args{"--config", dir.write("other.json", R"({"game_dir": "from_other"})")};
        const auto result = dal::parseGameOptions(args, exe);
        CHECK(result.has_value());
        if (result) CHECK(normal(result->game_dir) == normal(dir.path() / "from_other") && result->seconds == 0);
    }
}

void testConfigExplicitFile() {
    const TempDir dir("dal_game_options_cfg");
    dir.write("config.json", R"({"game_dir": "default_file", "seconds": 1})");
    dir.write("other.json", R"({"game_dir": "other_file", "hdd_dir": "saves"})");
    const ScopedCwd cwd(dir.path());
    for (const char* arg : {"--config=other.json"}) {
        const auto result = parse({arg});
        CHECK(result.has_value());
        if (!result) continue;
        CHECK(normal(result->config_file) == normal(std::filesystem::absolute("other.json")));
        CHECK(normal(result->game_dir) == normal(std::filesystem::current_path() / "other_file"));
        CHECK(result->seconds == 0);  // the default file is not read as well
    }
    // The last --config wins.
    const auto last = parse({"--config", "config.json", "--config", "other.json"});
    CHECK(last.has_value() && last->seconds == 0 && last->hdd_dir.filename() == "saves");

    CHECK(expectConfigFailure(parse({"--game-dir", "g", "--config", "nope.json"}),
                              "error: " + utf8Of(std::filesystem::absolute("nope.json")) + ": cannot open the file\n"));
    // A directory is not a readable file either.
    std::filesystem::create_directories(dir.path() / "subdir");
    CHECK(expectConfigFailure(parse({"--game-dir", "g", "--config", "subdir"}),
                              "error: " + utf8Of(std::filesystem::absolute("subdir")) + ": cannot open the file\n"));
    CHECK(expectUsageError({"--game-dir", "g", "--config", ""}, "error: --config needs a file name\n"));
}

void testConfigNoConfig() {
    const TempDir dir("dal_game_options_cfg");
    dir.write("config.json", "{");
    dir.write("other.json", R"({"game_dir": "o"})");
    const ScopedCwd cwd(dir.path());
    // --no-config reads nothing, so a broken file does not matter.
    const auto skipped = parse({"--game-dir", "g", "--no-config"});
    CHECK(skipped.has_value() && skipped->config_file.empty());
    // --no-config=false is the same as leaving it out.
    const auto read = parse({"--game-dir", "g", "--no-config=false"});
    CHECK(!read.has_value());
    if (!read)
        CHECK(read.error().message.starts_with("error: " + utf8Of(std::filesystem::absolute("config.json")) +
                                               ": invalid JSON: "));

    const std::string excludes = "error: --no-config excludes --config\n";
    CHECK(expectUsageError({"--game-dir", "g", "--config", "other.json", "--no-config"}, excludes));
    CHECK(expectUsageError({"--game-dir", "g", "--no-config", "--config", "other.json"}, excludes));
    // The conflict is reported even when the explicit file is missing.
    CHECK(expectUsageError({"--game-dir", "g", "--no-config", "--config", "nope.json"}, excludes));
}

void testConfigPrecedence() {
    const TempDir dir("dal_game_options_cfg");
    dir.write(
            "config.json",
            R"({"game_dir": "g", "seconds": 10, "no_window": true, "strict": true, "rsx_capture": [7], "pad": "none"})");
    const ScopedCwd cwd(dir.path());
    {
        const auto result = parse({});
        CHECK(result.has_value());
        if (result) {
            // The file beats the defaults.
            CHECK(result->seconds == 10 && result->no_window && !result->window && result->strict);
            CHECK(result->pad == "none" && result->rsx_capture_frames == (std::vector<std::uint32_t>{7}));
        }
    }
    {
        // The command line beats the file, for a value, a flag and a list.
        const auto result = parse({"--seconds", "5", "--no-window=false", "--strict=false", "--rsx-capture", "1,2"});
        CHECK(result.has_value());
        if (result) {
            CHECK(result->seconds == 5 && !result->no_window && result->window && !result->strict);
            CHECK(result->rsx_capture_frames == (std::vector<std::uint32_t>{1, 2}));
            CHECK(result->pad == "none");  // untouched keys still come from the file
        }
    }
    {
        const auto result = parse({"--game-dir", "cli_game"});
        CHECK(result.has_value() && result->game_dir == std::filesystem::path("cli_game"));
    }
}

void testConfigRelativePaths() {
    const TempDir dir("dal_game_options_cfg");
    std::filesystem::create_directories(dir.path() / "sub");
    dir.write("sub/c.json", R"({"game_dir": "../game", "hdd_dir": "saves"})");
    const ScopedCwd cwd(dir.path());
    const auto result = parse({"--config", "sub/c.json"});
    CHECK(result.has_value());
    if (!result) return;
    // Relative to the file's directory, not to the working directory.
    CHECK(normal(result->hdd_dir) == normal(std::filesystem::current_path() / "sub" / "saves"));
    CHECK(normal(result->game_dir) == normal(std::filesystem::current_path() / "game"));
}

void testConfigValueErrors() {
    const TempDir dir("dal_game_options_cfg");
    const ScopedCwd cwd(dir.path());
    const std::string file = utf8Of(std::filesystem::absolute("config.json"));
    const std::string prefix = "error: " + file + ": ";

    // A bad value from the file names the file and the key; it is a validation error, so usage follows.
    dir.write("config.json", R"({"game_dir": "g", "pad": "bogus"})");
    CHECK(expectUsageError({}, prefix + "pad needs none, neutral, keyboard, gamepad or keyboard+gamepad\n"));
    dir.write("config.json", R"({"game_dir": "g", "seconds": 99999999999})");
    CHECK(expectUsageError({}, prefix + "seconds needs a non-negative integer\n"));
    dir.write("config.json", R"({"game_dir": "g", "dump_frames_every": 0})");
    CHECK(expectUsageError({}, prefix + "dump_frames_every needs a positive integer\n"));
    dir.write("config.json", R"({"game_dir": "g", "rsx_capture": "1,,2"})");
    CHECK(expectUsageError({}, prefix + "rsx_capture needs frame numbers (1-based), e.g. 300,600\n"));
    dir.write("config.json", R"({"game_dir": "g", "pad_script": "missing_script.txt"})");
    {
        const auto result = parse({});
        CHECK(!result.has_value());
        if (!result) {
            CHECK(result.error().exit_code == kUsageExit && result.error().print_usage);
            CHECK(result.error().message.starts_with(prefix + "pad_script: "));
            CHECK(result.error().message.find("note:") == std::string::npos);
        }
    }
    // The same mistake on the command line keeps today's text, plus the note that a file was read too.
    dir.write("config.json", R"({"game_dir": "g"})");
    CHECK(expectUsageError({"--pad", "bogus"},
                           "error: --pad needs none, neutral, keyboard, gamepad or keyboard+gamepad\n"
                           "note: settings were also read from " +
                                   file + "\n"));
}

void testConfigConflictNote() {
    const TempDir dir("dal_game_options_cfg");
    const ScopedCwd cwd(dir.path());
    const std::string file = utf8Of(std::filesystem::absolute("config.json"));
    dir.write("config.json", R"({"game_dir": "g", "render": "none"})");
    CHECK(expectUsageError({"--dump-frames", "d"},
                           "error: --dump-frames, --dump-frames-every, --shader-cache and --vk-validation need "
                           "--render vulkan\nnote: settings were also read from " +
                                   file + "\n"));
    // Without a game directory in the file, the required-option message mentions the keys.
    dir.write("config.json", R"({"seconds": 1})");
    CHECK(expectUsageError({}, kRequired + "note: settings were also read from " + file + "\n"));
    // "vfs_config" alone satisfies the check.
    dir.write("config.json", R"({"vfs_config": "v.json"})");
    const auto result = parse({});
    CHECK(result.has_value());
    if (result) CHECK(normal(result->vfs_config) == normal(std::filesystem::current_path() / "v.json"));
}

void testConfigFileFailures() {
    const TempDir dir("dal_game_options_cfg");
    const ScopedCwd cwd(dir.path());
    const std::string file = utf8Of(std::filesystem::absolute("config.json"));
    dir.write("config.json", "{\r\n  \"seconds\": 1,\r\n  \"pad\": ]\r\n}");
    {
        const auto result = parse({"--game-dir", "g"});
        CHECK(!result.has_value());
        if (!result) {
            CHECK(result.error().exit_code == kUsageExit && !result.error().print_usage);
            CHECK(result.error().message.starts_with("error: " + file + ": invalid JSON: "));
            // The offset counts the CRLF bytes of the file.
            CHECK(result.error().message.ends_with(" at line 3, column 10 (byte 29)\n"));
        }
    }
    dir.write("config.json", R"({"seconds": "5"})");
    CHECK(expectConfigFailure(parse({"--game-dir", "g"}),
                              "error: " + file + ": \"seconds\": expected a non-negative integer, got string\n"));
    // A file that is broken wins over an unknown argument, which CLI11 would report later.
    dir.write("config.json", "{");
    {
        const auto result = parse({"--game-dir", "g", "--bogus"});
        CHECK(!result.has_value());
        if (!result) {
            CHECK(result.error().exit_code == kUsageExit && !result.error().print_usage);
            CHECK(result.error().message.starts_with("error: " + file + ": invalid JSON: "));
        }
    }
    // --help is handled before the file is read.
    const auto help = parse({"--help"});
    CHECK(!help.has_value() && help.error().exit_code == 0 && help.error().print_usage);
    // A missing value is reported before the file is read, too.
    CHECK(expectUsageError({"--game-dir"}, "error: --game-dir: 1 required TEXT missing\n"));
}

void testConfigUnknownKeyWarning() {
    const TempDir dir("dal_game_options_cfg");
    dir.write("config.json", R"({"game_dir": "g", "future_option": 1, "config": "x.json"})");
    const ScopedCwd cwd(dir.path());
    const auto result = parse({});
    CHECK(result.has_value());
    if (result) {
        CHECK(result->warnings ==
              (std::vector<std::string>{"unknown key \"future_option\" ignored", "unknown key \"config\" ignored"}));
    }
}

void testApplyGameOptionsConfigLog() {
    const TempDir dir("dal_game_options_cfg");
    dir.write("config.json", R"({"game_dir": "g", "future_option": 1})");
    const ScopedCwd cwd(dir.path());
    const auto parsed = parse({});
    CHECK(parsed.has_value());
    if (!parsed) return;
    std::vector<std::string> lines;
    dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    const auto applied = dal::applyGameOptions(*parsed);
    dal::setLogSink({});
    CHECK(applied.has_value());
    const std::string file = utf8Of(std::filesystem::absolute("config.json"));
    CHECK(lines.size() == 2);
    if (lines.size() == 2) {
        CHECK(lines[0].ends_with("dal: config: " + file) && lines[0].find("warn") == std::string::npos);
        CHECK(lines[1].find("warn: dal: config: " + file + ": unknown key \"future_option\" ignored") !=
              std::string::npos);
    }
    // Without a file nothing is logged.
    lines.clear();
    dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
    CHECK(dal::applyGameOptions(dal::GameOptions{}).has_value());
    dal::setLogSink({});
    CHECK(lines.empty());
}

}  // namespace

int main() {
    // An empty working directory for the plain tests; the config tests make their own.
    const TempDir isolated("dal_game_options_cwd");
    const ScopedCwd cwd(isolated.path());
    testGameOptionSpecs();
    testDerivedValues();
    testValidationMessages();
    testLastWins();
    testCli11Errors();
    testValuesStartingWithDash();
    testFlagsAndEqualsForm();
    testUtf8Path();
    testConfigFileKinds();
    testConfigFilePaths();
    testConfigFileErrors();
    testConfigFileUnknownKeys();
    testConfigFileFrameList();
    testConfigFileAcceptsEveryKey();
    testConfigFileExample();
    testApplyGameOptions();
    testConfigLookupNoFile();
    testConfigLookupWorkingDirectory();
    testConfigLookupExecutableDirectory();
    testConfigExplicitFile();
    testConfigNoConfig();
    testConfigPrecedence();
    testConfigRelativePaths();
    testConfigValueErrors();
    testConfigConflictNote();
    testConfigFileFailures();
    testConfigUnknownKeyWarning();
    testApplyGameOptionsConfigLog();
    std::printf("game options tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
