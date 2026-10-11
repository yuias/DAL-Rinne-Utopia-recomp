// Tests of the dal_game option parser: derived values, validation messages, CLI11 behaviour and
// the global side effects of applyGameOptions.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

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

std::expected<dal::GameOptions, dal::OptionsExit> parse(const std::vector<std::string>& args) {
    return dal::parseGameOptions(args);
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
    TempDir() : path_(std::filesystem::temp_directory_path() / "dal_game_options_test") {
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

    CHECK(expectUsageError({}, "error: --game-dir or --vfs-config is required\n"));
    CHECK(expectUsageError({"--seconds", "10"}, "error: --game-dir or --vfs-config is required\n"));

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

}  // namespace

int main() {
    testGameOptionSpecs();
    testDerivedValues();
    testValidationMessages();
    testLastWins();
    testCli11Errors();
    testValuesStartingWithDash();
    testFlagsAndEqualsForm();
    testUtf8Path();
    testApplyGameOptions();
    std::printf("game options tests: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed != 0 ? 1 : 0;
}
