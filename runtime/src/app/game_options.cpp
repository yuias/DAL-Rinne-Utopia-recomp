#include "app/game_options.h"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "app/config_file.h"
#include "audio/audio_output.h"
#include "audio/wav_file.h"
#include "core/fatal.h"
#include "core/guest_clock.h"
#include "hle/hle.h"

namespace dal {
namespace {

constexpr std::array<OptionSpec, 27> kOptionSpecs{{
        {"game-dir", OptionKind::Path},
        {"vfs-config", OptionKind::Path},
        {"elf", OptionKind::Path},
        {"seconds", OptionKind::Unsigned},
        {"strict", OptionKind::Flag},
        {"log", OptionKind::Path},
        {"hdd-dir", OptionKind::Path},
        {"sdata-cache", OptionKind::Path},
        {"log-level", OptionKind::String},
        {"quiet-hle", OptionKind::Flag},
        {"load-only", OptionKind::Flag},
        {"png-dump", OptionKind::Path},
        {"dialog-answer", OptionKind::String},
        {"pad", OptionKind::String},
        {"pad-script", OptionKind::Path},
        {"pad-probe", OptionKind::Flag},
        {"audio", OptionKind::String},
        {"audio-dump", OptionKind::Path},
        {"render", OptionKind::String},
        {"rsx-capture", OptionKind::FrameList},
        {"rsx-census", OptionKind::Path},
        {"dump-frames", OptionKind::Path},
        {"dump-frames-every", OptionKind::Unsigned},
        {"shader-cache", OptionKind::Path},
        {"fast-forward", OptionKind::Unsigned},
        {"vk-validation", OptionKind::Flag},
        {"no-window", OptionKind::Flag},
}};

constexpr std::string_view kUsage =
        "usage: dal_game [--game-dir DIR] [--vfs-config FILE] (at least one) [--elf PATH]\n"
        "                [--seconds N] [--strict] [--log FILE] [--hdd-dir DIR] [--sdata-cache DIR]\n"
        "                [--log-level trace|debug|info|warn|error]\n"
        "                [--quiet-hle] [--load-only] [--png-dump DIR]\n"
        "                [--dialog-answer yes|no|ok] [--pad none|neutral|keyboard|gamepad|keyboard+gamepad]\n"
        "                [--pad-script FILE] [--pad-probe] [--audio none|wasapi] [--audio-dump FILE]\n"
        "                [--render none|vulkan] [--rsx-capture N[,M...]] [--rsx-census FILE]\n"
        "                [--dump-frames DIR] [--dump-frames-every K] [--shader-cache DIR]\n"
        "                [--fast-forward N] [--vk-validation] [--no-window]\n"
        "                [--config FILE | --no-config] [-h|--help]\n";

// The raw value of one option as CLI11 delivers it; the checks run afterwards, in table order.
struct Slot {
    OptionSpec spec;
    std::string text;                // Path, String, Unsigned
    bool flag = false;               // Flag
    std::vector<std::string> items;  // FrameList: one entry per occurrence
    CLI::Option* option = nullptr;
    bool from_file = false;  // the value came from the config file, not the command line

    bool given() const { return !option->empty(); }
};

bool padUsesKeyboard(std::string_view pad) { return pad == "keyboard" || pad == "keyboard+gamepad"; }
bool padUsesGamepad(std::string_view pad) { return pad == "gamepad" || pad == "keyboard+gamepad"; }

// Arguments are UTF-8; going through char8_t keeps non-ASCII paths out of the ANSI code page.
std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string utf8String(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::unexpected<OptionsExit> usageError(std::string_view text) {
    return std::unexpected(OptionsExit{static_cast<int>(ExitCode::Usage), std::format("error: {}\n", text), true});
}

// A config file problem is not a command-line mistake, so the usage text would not help.
std::unexpected<OptionsExit> configError(std::string_view text) {
    return std::unexpected(OptionsExit{static_cast<int>(ExitCode::Usage), std::format("error: {}\n", text), false});
}

// Not derived from a CLI11 type: CLI11 swallows FileError for the default file, and this one must reach
// parseGameOptions.
class ConfigFileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What the config file contributed; filled by JsonConfig while CLI11 applies the file.
struct ConfigState {
    std::filesystem::path path;  // absolute; empty: no file was applied
    std::vector<std::string> warnings;
    std::set<std::string, std::less<>> options;  // options whose value came from the file
};

// The flag's callback runs after the config file is read, so its results are looked at directly.
bool flagIsOn(const CLI::Option* flag) { return flag->count() > 0 && flag->as<bool>(); }

// Feeds config.json to CLI11 as config items, so a file value applies to exactly the options the
// command line did not give.
class JsonConfig : public CLI::Config {
public:
    JsonConfig(const CLI::App& app, const CLI::Option& no_config) : app_(app), no_config_(no_config) {}

    std::string to_config(const CLI::App*, bool, bool, std::string) const override { return {}; }

    std::vector<CLI::ConfigItem> from_config(std::istream&) const override {
        if (flagIsOn(&no_config_)) return {};
        const std::filesystem::path file = configPath(*app_.get_config_ptr());
        const std::string name = utf8String(file);
        // The stream CLI11 opened is in text mode, which folds CRLF and stops at Ctrl-Z; error offsets
        // must count the real bytes.
        std::ifstream in(file, std::ios::binary);
        if (!in) throw ConfigFileError(std::format("{}: cannot open the file", name));
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto contents = parseConfigFile(text, file.parent_path(), kOptionSpecs);
        if (!contents) throw ConfigFileError(std::format("{}: {}", name, contents.error()));

        state_.path = file;
        state_.warnings = std::move(contents->warnings);
        std::vector<CLI::ConfigItem> items;
        for (ConfigFileValue& value : contents->values) {
            const CLI::Option* option = app_.get_option_no_throw("--" + value.option);
            if (option && option->count() == 0) state_.options.insert(value.option);
            items.push_back(CLI::ConfigItem{.parents = {}, .name = value.option, .inputs = std::move(value.inputs)});
        }
        return items;
    }

    static std::filesystem::path configPath(const CLI::Option& config_option) {
        std::error_code ec;
        const std::filesystem::path path = pathFromUtf8(config_option.as<std::string>());
        const std::filesystem::path full = std::filesystem::absolute(path, ec);
        return ec ? path : full;
    }

    const ConfigState& state() const { return state_; }

private:
    const CLI::App& app_;
    const CLI::Option& no_config_;
    mutable ConfigState state_;
};

// A relative default name makes CLI11 look in the working directory; when that has no config.json,
// the one next to the executable is used instead. Only one file is ever read.
std::string defaultConfigName(const std::filesystem::path& executable_dir) {
    constexpr std::string_view kName = "config.json";
    std::error_code ec;
    if (std::filesystem::is_regular_file(kName, ec)) return std::string(kName);
    if (!executable_dir.empty()) {
        const std::filesystem::path beside = executable_dir / kName;
        if (std::filesystem::is_regular_file(beside, ec)) return utf8String(beside);
    }
    return std::string(kName);
}

bool parseFrameList(std::string_view list, std::vector<std::uint32_t>& frames) {
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view item = list.substr(0, comma);
        std::uint32_t frame = 0;
        const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), frame);
        if (item.empty() || ec != std::errc{} || end != item.data() + item.size() || frame == 0) return false;
        frames.push_back(frame);
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
        if (list.empty()) return false;  // trailing comma
    }
    return !frames.empty();
}

std::optional<LogLevel> parseLogLevel(std::string_view name) {
    if (name == "trace") return LogLevel::Trace;
    if (name == "debug") return LogLevel::Debug;
    if (name == "info") return LogLevel::Info;
    if (name == "warn") return LogLevel::Warn;
    if (name == "error") return LogLevel::Error;
    return std::nullopt;
}

// Whole-string unsigned parse, as the hand-written parser did.
template <typename T>
bool parseUnsigned(const std::string& text, T& value) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size();
}

// Checks the raw values in option-table order, then settles the defaults that depend on several
// options. A value is checked the same way wherever it came from.
std::expected<GameOptions, OptionsExit> validate(const std::vector<Slot>& slots, const ConfigState& config) {
    auto slot = [&](std::string_view name) -> const Slot& {
        return *std::ranges::find(slots, name, [](const Slot& s) { return s.spec.name; });
    };
    const std::string file_name = utf8String(config.path);
    // Names the source of a value in messages: the option for the command line, file and key otherwise.
    auto label = [&](const Slot& s) {
        return s.from_file ? std::format("{}: {}", file_name, configKeyFor(s.spec.name))
                           : std::format("--{}", s.spec.name);
    };
    // Like label, for messages that carry the offending path: the file case drops it (the file names it).
    auto labelWithValue = [&](const Slot& s) {
        return s.from_file ? label(s) : std::format("--{} {}", s.spec.name, s.text);
    };
    GameOptions o;
    std::optional<bool> render_flag;  // --render as given; the final mode is settled below
    bool dump_every_given = false;

    o.game_dir = pathFromUtf8(slot("game-dir").text);
    o.vfs_config = pathFromUtf8(slot("vfs-config").text);
    o.elf = pathFromUtf8(slot("elf").text);

    if (const Slot& s = slot("seconds"); s.given() && !parseUnsigned(s.text, o.seconds)) {
        return usageError(std::format("{} needs a non-negative integer", label(s)));
    }
    o.strict = slot("strict").flag;
    o.log_file = pathFromUtf8(slot("log").text);
    o.hdd_dir = pathFromUtf8(slot("hdd-dir").text);
    o.sdata_cache = pathFromUtf8(slot("sdata-cache").text);

    if (const Slot& s = slot("log-level"); s.given()) {
        o.log_level = parseLogLevel(s.text);
        if (!o.log_level) return usageError(std::format("{} needs trace, debug, info, warn or error", label(s)));
    }
    o.quiet_hle = slot("quiet-hle").flag;
    o.load_only = slot("load-only").flag;
    o.png_dump = pathFromUtf8(slot("png-dump").text);

    if (const Slot& s = slot("dialog-answer"); s.given()) {
        if (s.text != "yes" && s.text != "no" && s.text != "ok") {
            return usageError(std::format("{} needs yes, no or ok", label(s)));
        }
        o.dialog_answer = s.text;
    }
    if (const Slot& s = slot("pad"); s.given()) {
        if (s.text != "none" && s.text != "neutral" && s.text != "keyboard" && s.text != "gamepad" &&
            s.text != "keyboard+gamepad") {
            return usageError(std::format("{} needs none, neutral, keyboard, gamepad or keyboard+gamepad", label(s)));
        }
        o.pad = s.text;
    }
    if (const Slot& s = slot("pad-script"); s.given()) {
        o.pad_script = pathFromUtf8(s.text);
        auto events = loadPadScript(o.pad_script);
        if (!events) return usageError(std::format("{}: {}", labelWithValue(s), events.error()));
        o.pad_script_events = std::move(*events);
    }
    o.pad_probe = slot("pad-probe").flag;
    if (const Slot& s = slot("audio"); s.given()) {
        if (s.text != "none" && s.text != "wasapi") return usageError(std::format("{} needs none or wasapi", label(s)));
        o.audio = s.text;
    }
    o.audio_dump = pathFromUtf8(slot("audio-dump").text);
    if (const Slot& s = slot("render"); s.given()) {
        if (s.text != "none" && s.text != "vulkan") return usageError(std::format("{} needs none or vulkan", label(s)));
        render_flag = s.text == "vulkan";
    }
    for (const std::string& list : slot("rsx-capture").items) {
        if (!parseFrameList(list, o.rsx_capture_frames)) {
            return usageError(
                    std::format("{} needs frame numbers (1-based), e.g. 300,600", label(slot("rsx-capture"))));
        }
    }
    o.rsx_census = pathFromUtf8(slot("rsx-census").text);
    o.dump_frames = pathFromUtf8(slot("dump-frames").text);
    if (const Slot& s = slot("dump-frames-every"); s.given()) {
        if (!parseUnsigned(s.text, o.dump_frames_every) || o.dump_frames_every == 0) {
            return usageError(std::format("{} needs a positive integer", label(s)));
        }
        dump_every_given = true;
    }
    o.shader_cache = pathFromUtf8(slot("shader-cache").text);
    if (const Slot& s = slot("fast-forward"); s.given()) {
        if (!parseUnsigned(s.text, o.fast_forward) ||
            (o.fast_forward != 1 && (o.fast_forward < kMinFastForwardSpeed || o.fast_forward > kMaxFastForwardSpeed))) {
            return usageError(std::format("{} needs 1 (off) or a speed from {} to {}", label(s), kMinFastForwardSpeed,
                                          kMaxFastForwardSpeed));
        }
    }
    o.vk_validation = slot("vk-validation").flag;
    o.no_window = slot("no-window").flag;

    if (o.game_dir.empty() && o.vfs_config.empty()) {
        return usageError("--game-dir or --vfs-config is required (or \"game_dir\" or \"vfs_config\" in config.json)");
    }
    // Playing is the default, so the window is on unless the run asks for none: --no-window for
    // unattended runs, or an explicit "--render none", which leaves nothing to show.
    o.window = !o.no_window && render_flag.value_or(true);
    // --dump-frames and the window imply vulkan; an explicit "--render none" (anywhere on the line)
    // overrides that and then conflicts with it. The other options have no effect without a renderer.
    o.render_vulkan = render_flag.value_or(!o.dump_frames.empty() || o.window);
    const bool render_option = !o.dump_frames.empty() || dump_every_given || !o.shader_cache.empty() || o.vk_validation;
    if (render_option && !o.render_vulkan) {
        return usageError(
                "--dump-frames, --dump-frames-every, --shader-cache and --vk-validation need --render vulkan");
    }
    // GameInput delivers readings to the foreground window only, so unattended runs have nothing to probe.
    if (o.pad_probe && !o.window) return usageError("--pad-probe needs the window");
    // A window or a script is what makes a keyboard pad useful, so either one selects it by default.
    const bool pad_input = o.window || !o.pad_script.empty();
    if (o.pad.empty()) o.pad = o.window ? "keyboard+gamepad" : pad_input ? "keyboard" : "neutral";
    if (o.pad == "keyboard" && !pad_input) return usageError("--pad keyboard needs the window or --pad-script");
    // GameInput delivers readings to the foreground window only.
    if (padUsesGamepad(o.pad) && !o.window) return usageError(std::format("--pad {} needs the window", o.pad));
    if (!o.pad_script.empty() && !padUsesKeyboard(o.pad)) {
        return usageError("--pad-script needs --pad keyboard or keyboard+gamepad");
    }
    // A window means somebody is listening; unattended runs must not need a sound device.
    if (o.audio.empty()) o.audio = o.window ? "wasapi" : "none";
    if (!o.audio_dump.empty()) {
        // The engine creates the file again on its own thread; creating it here makes a bad path a
        // usage error instead of a warning in the middle of a run.
        const auto probe = WavWriter::create(o.audio_dump, kAudioSampleRate, kAudioOutputChannels);
        if (!probe) {
            return usageError(std::format("{}: {}", labelWithValue(slot("audio-dump")), probe.error()));
        }
    }
    o.config_file = config.path;
    o.warnings = config.warnings;
    return o;
}

}  // namespace

std::span<const OptionSpec> gameOptionSpecs() { return kOptionSpecs; }

std::string configKeyFor(std::string_view option_name) {
    std::string key(option_name);
    std::ranges::replace(key, '-', '_');
    return key;
}

std::string_view gameOptionsUsage() { return kUsage; }

std::expected<GameOptions, OptionsExit> parseGameOptions(std::span<const std::string> args,
                                                         const std::filesystem::path& executable_dir) {
    // The empty name keeps CLI11's messages free of a program-name prefix.
    CLI::App app{"", ""};
    // An argument starting with '/' is not a Windows-style option: it is a path or an unknown argument.
    app.allow_windows_style_options(false);
    // Set before any add_option: the defaults are copied when an option is created. Matches the
    // previous parser, where a repeated option replaced the earlier value.
    app.option_defaults()->multi_option_policy(CLI::MultiOptionPolicy::TakeLast);

    std::vector<Slot> slots;
    slots.reserve(kOptionSpecs.size());  // add_option keeps references into the vector
    for (const OptionSpec& spec : kOptionSpecs) {
        Slot& slot = slots.emplace_back();
        slot.spec = spec;
        const std::string flag_name = std::format("--{}", spec.name);
        switch (spec.kind) {
            case OptionKind::Flag: slot.option = app.add_flag(flag_name, slot.flag); break;
            case OptionKind::FrameList:
                // One value per occurrence, so "--rsx-capture 1 --rsx-capture 2" accumulates without
                // the option swallowing the next argument.
                slot.option = app.add_option(flag_name, slot.items)->expected(1)->allow_extra_args(false)->take_all();
                break;
            default: slot.option = app.add_option(flag_name, slot.text); break;
        }
    }

    // Not CLI11's excludes: the default file gives the --config option a count before the
    // requirements run, so excludes would fail whenever a config.json exists.
    CLI::Option* no_config = app.add_flag("--no-config")->configurable(false);
    const std::filesystem::path exe_dir = executable_dir.empty() ? executableDirectory() : executable_dir;
    CLI::Option* config_option =
            app.set_config("--config", defaultConfigName(exe_dir), "Read this file instead of config.json");
    auto config = std::make_shared<JsonConfig>(app, *no_config);
    app.config_formatter(config);
    // A key the reader did not filter out is a bug; the command line has its own extras check.
    app.allow_config_extras(CLI::ConfigExtrasMode::Error);

    // The config option has a count even when only the default file is used; parse_order tells
    // whether it was on the command line.
    auto configGiven = [&] { return std::ranges::find(app.parse_order(), config_option) != app.parse_order().end(); };
    auto conflict = [&] { return configGiven() && flagIsOn(no_config); };

    // App::parse(vector) takes the arguments last first.
    std::vector<std::string> reversed(args.rbegin(), args.rend());
    try {
        app.parse(reversed);
    } catch (const ConfigFileError& e) {
        return configError(e.what());
    } catch (const CLI::CallForHelp&) {
        return std::unexpected(OptionsExit{.exit_code = 0, .message = {}, .print_usage = true});
    } catch (const CLI::FileError&) {
        // The explicit file is missing; a missing --config next to --no-config is the user's mistake.
        if (conflict()) return usageError("--no-config excludes --config");
        return configError(std::format("{}: cannot open the file", utf8String(JsonConfig::configPath(*config_option))));
    } catch (const CLI::ParseError& e) {
        return usageError(e.what());
    }
    if (conflict()) return usageError("--no-config excludes --config");
    if (configGiven() && config_option->as<std::string>().empty()) return usageError("--config needs a file name");

    const ConfigState& applied = config->state();
    for (Slot& slot : slots) slot.from_file = applied.options.contains(slot.spec.name);
    auto options = validate(slots, applied);
    if (!options && !applied.path.empty()) {
        // A message that names the file already points there; the others may stem from its values.
        const std::string file = utf8String(applied.path);
        if (!options.error().message.starts_with(std::format("error: {}: ", file))) {
            options.error().message += std::format("note: settings were also read from {}\n", file);
        }
    }
    return options;
}

std::expected<void, std::string> applyGameOptions(const GameOptions& options) {
    // First, so that nothing later in startup logs at the wrong level.
    if (options.log_level) setLogLevel(*options.log_level);
    if (!options.log_file.empty() && !setLogFile(options.log_file.string())) {
        return std::unexpected(std::format("cannot open log file {}", options.log_file.string()));
    }
    if (!options.dialog_answer.empty() && !setDialogAnswer(options.dialog_answer)) {
        return std::unexpected("--dialog-answer needs yes, no or ok");
    }
    if (!options.config_file.empty()) {
        const std::string file = utf8String(options.config_file);
        logInfo("dal: config: {}", file);
        for (const std::string& warning : options.warnings) logWarn("dal: config: {}: {}", file, warning);
    }
    return {};
}

}  // namespace dal
