#include "app/game_options.h"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <memory>
#include <system_error>
#include <utility>

#include "audio/audio_output.h"
#include "audio/wav_file.h"
#include "core/fatal.h"
#include "hle/hle.h"

namespace dal {
namespace {

constexpr std::array<OptionSpec, 26> kOptionSpecs{{
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
        "                [--vk-validation] [--no-window] [-h|--help]\n";

// The raw value of one option as CLI11 delivers it; the checks run afterwards, in table order.
struct Slot {
    OptionSpec spec;
    std::string text;                // Path, String, Unsigned
    bool flag = false;               // Flag
    std::vector<std::string> items;  // FrameList: one entry per occurrence
    CLI::Option* option = nullptr;

    bool given() const { return !option->empty(); }
};

bool padUsesKeyboard(std::string_view pad) { return pad == "keyboard" || pad == "keyboard+gamepad"; }
bool padUsesGamepad(std::string_view pad) { return pad == "gamepad" || pad == "keyboard+gamepad"; }

// Arguments are UTF-8; going through char8_t keeps non-ASCII paths out of the ANSI code page.
std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::unexpected<OptionsExit> usageError(std::string_view text) {
    return std::unexpected(OptionsExit{static_cast<int>(ExitCode::Usage), std::format("error: {}\n", text), true});
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
std::expected<GameOptions, OptionsExit> validate(const std::vector<Slot>& slots) {
    auto slot = [&](std::string_view name) -> const Slot& {
        return *std::ranges::find(slots, name, [](const Slot& s) { return s.spec.name; });
    };
    GameOptions o;
    std::optional<bool> render_flag;  // --render as given; the final mode is settled below
    bool dump_every_given = false;

    o.game_dir = pathFromUtf8(slot("game-dir").text);
    o.vfs_config = pathFromUtf8(slot("vfs-config").text);
    o.elf = pathFromUtf8(slot("elf").text);

    if (const Slot& s = slot("seconds"); s.given() && !parseUnsigned(s.text, o.seconds)) {
        return usageError("--seconds needs a non-negative integer");
    }
    o.strict = slot("strict").flag;
    o.log_file = pathFromUtf8(slot("log").text);
    o.hdd_dir = pathFromUtf8(slot("hdd-dir").text);
    o.sdata_cache = pathFromUtf8(slot("sdata-cache").text);

    if (const Slot& s = slot("log-level"); s.given()) {
        o.log_level = parseLogLevel(s.text);
        if (!o.log_level) return usageError("--log-level needs trace, debug, info, warn or error");
    }
    o.quiet_hle = slot("quiet-hle").flag;
    o.load_only = slot("load-only").flag;
    o.png_dump = pathFromUtf8(slot("png-dump").text);

    if (const Slot& s = slot("dialog-answer"); s.given()) {
        if (s.text != "yes" && s.text != "no" && s.text != "ok") {
            return usageError("--dialog-answer needs yes, no or ok");
        }
        o.dialog_answer = s.text;
    }
    if (const Slot& s = slot("pad"); s.given()) {
        if (s.text != "none" && s.text != "neutral" && s.text != "keyboard" && s.text != "gamepad" &&
            s.text != "keyboard+gamepad") {
            return usageError("--pad needs none, neutral, keyboard, gamepad or keyboard+gamepad");
        }
        o.pad = s.text;
    }
    if (const Slot& s = slot("pad-script"); s.given()) {
        o.pad_script = pathFromUtf8(s.text);
        auto events = loadPadScript(o.pad_script);
        if (!events) return usageError(std::format("--pad-script {}: {}", s.text, events.error()));
        o.pad_script_events = std::move(*events);
    }
    o.pad_probe = slot("pad-probe").flag;
    if (const Slot& s = slot("audio"); s.given()) {
        if (s.text != "none" && s.text != "wasapi") return usageError("--audio needs none or wasapi");
        o.audio = s.text;
    }
    o.audio_dump = pathFromUtf8(slot("audio-dump").text);
    if (const Slot& s = slot("render"); s.given()) {
        if (s.text != "none" && s.text != "vulkan") return usageError("--render needs none or vulkan");
        render_flag = s.text == "vulkan";
    }
    for (const std::string& list : slot("rsx-capture").items) {
        if (!parseFrameList(list, o.rsx_capture_frames)) {
            return usageError("--rsx-capture needs frame numbers (1-based), e.g. 300,600");
        }
    }
    o.rsx_census = pathFromUtf8(slot("rsx-census").text);
    o.dump_frames = pathFromUtf8(slot("dump-frames").text);
    if (const Slot& s = slot("dump-frames-every"); s.given()) {
        if (!parseUnsigned(s.text, o.dump_frames_every) || o.dump_frames_every == 0) {
            return usageError("--dump-frames-every needs a positive integer");
        }
        dump_every_given = true;
    }
    o.shader_cache = pathFromUtf8(slot("shader-cache").text);
    o.vk_validation = slot("vk-validation").flag;
    o.no_window = slot("no-window").flag;

    if (o.game_dir.empty() && o.vfs_config.empty()) {
        return usageError("--game-dir or --vfs-config is required");
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
            return usageError(std::format("--audio-dump {}: {}", slot("audio-dump").text, probe.error()));
        }
    }
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

std::expected<GameOptions, OptionsExit> parseGameOptions(std::span<const std::string> args) {
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

    // App::parse(vector) takes the arguments last first.
    std::vector<std::string> reversed(args.rbegin(), args.rend());
    try {
        app.parse(reversed);
    } catch (const CLI::CallForHelp&) {
        return std::unexpected(OptionsExit{.exit_code = 0, .message = {}, .print_usage = true});
    } catch (const CLI::ParseError& e) {
        return usageError(e.what());
    }
    return validate(slots);
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
    return {};
}

}  // namespace dal
