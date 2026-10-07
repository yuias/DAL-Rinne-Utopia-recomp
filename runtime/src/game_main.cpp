// dal_game: loads the decrypted EBOOT.elf and runs it through the generated PPU code.

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <string_view>
#include <vector>

#include "core/boot.h"
#include "core/census.h"
#include "core/elf_loader.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/param_sfo.h"
#include "core/threads.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lv2/vfs.h"
#include "lv2/vfs_config.h"
#include "render/vk_context.h"
#include "render/vk_renderer.h"
#include "render/vk_window.h"
#include "rsx/rsx_thread.h"

namespace {

struct Options {
  std::string game_dir;
  std::string elf;
  std::string hdd_dir;
  std::string sdata_cache;
  std::string vfs_config;
  std::string png_dump;
  std::string rsx_census;
  std::string log_file;
  std::string dialog_answer;
  unsigned seconds = 0;
  bool strict = false;
  bool quiet_hle = false;
  bool load_only = false;
  bool render_vulkan = false;
  std::optional<bool> render_flag;  // --render as given; the final mode is settled after all arguments
  bool dump_every_given = false;
  bool vk_validation = false;
  bool window = false;
  std::string dump_frames;
  std::uint32_t dump_frames_every = 1;
  std::string shader_cache;
  std::vector<std::uint32_t> rsx_capture_frames;
};

// The Vulkan device and the renderer for the RSX thread. Built and destroyed on that thread; the
// counters are published for the census, which reads them from another one.
class VulkanExecutor final : public dal::RsxExecutor {
 public:
  // `presenter` may be null (no window).
  VulkanExecutor(std::unique_ptr<dal::VkContext> context, std::unique_ptr<dal::Presenter> presenter,
                 const dal::RenderOptions& options)
      : context_(std::move(context)),
        presenter_(std::move(presenter)),
        renderer_(std::make_unique<dal::RsxRenderer>(*context_, options)) {
    renderer_->setPresenter(presenter_.get());
  }

  ~VulkanExecutor() override {
    // The renderer and the swapchain go before the device they were created from.
    renderer_.reset();
    presenter_.reset();
    context_.reset();
  }

  bool valid() const { return renderer_->valid(); }
  const std::string& error() const { return renderer_->error(); }
  const dal::VkContext& context() const { return *context_; }

  void execute(std::span<dal::RsxCommand> commands) override {
    if (!renderer_->valid()) return;
    renderer_->execute(commands);
    const dal::RendererStats& stats = renderer_->stats();
    draws_.store(stats.draws, std::memory_order_relaxed);
    skipped_.store(stats.draws_skipped, std::memory_order_relaxed);
    clears_.store(stats.clears, std::memory_order_relaxed);
    flips_.store(stats.flips, std::memory_order_relaxed);
    dumped_.store(stats.dumped, std::memory_order_relaxed);
    if (!renderer_->valid()) {
      failed_.store(true, std::memory_order_relaxed);
      dal::logError("dal: render: renderer stopped: {}", renderer_->error());
    }
  }

  dal::RsxRenderCounts counts() const override {
    dal::RsxRenderCounts counts;
    counts.draws = draws_.load(std::memory_order_relaxed);
    counts.draws_skipped = skipped_.load(std::memory_order_relaxed);
    counts.clears = clears_.load(std::memory_order_relaxed);
    counts.flips = flips_.load(std::memory_order_relaxed);
    counts.frames_dumped = dumped_.load(std::memory_order_relaxed);
    counts.validation = context_->validationEnabled();
    counts.validation_errors = context_->validationErrorCount();
    counts.failed = failed_.load(std::memory_order_relaxed);
    return counts;
  }

 private:
  std::unique_ptr<dal::VkContext> context_;
  std::unique_ptr<dal::Presenter> presenter_;
  std::unique_ptr<dal::RsxRenderer> renderer_;
  std::atomic<std::uint64_t> draws_{0}, skipped_{0}, clears_{0}, flips_{0}, dumped_{0};
  std::atomic<bool> failed_{false};
};

// Runs on the RSX thread; a null result makes the consumer continue without rendering.
std::unique_ptr<dal::RsxExecutor> createVulkanExecutor(const Options& opt, std::string& error) {
  const auto start = std::chrono::steady_clock::now();
  dal::VkContextOptions context_options;
  context_options.validation = opt.vk_validation;
  context_options.window = opt.window;
  std::unique_ptr<dal::VkContext> context = dal::createVkContext(context_options, error);
  if (!context) return nullptr;
  const std::uint32_t api = context->deviceProperties().apiVersion;
  dal::logInfo("dal: render: vulkan device {} (api {}.{}.{}, d24s8 {})", context->deviceName(),
               VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
               context->hasD24S8() ? "yes" : "no");
  if (opt.vk_validation && !context->validationEnabled()) {
    dal::logWarn("dal: render: validation layer requested but not installed");
  }

  dal::RenderOptions render_options;
  render_options.dump_dir = opt.dump_frames;
  render_options.dump_every = opt.dump_frames_every;
  render_options.shader_cache = opt.shader_cache.empty() ? std::filesystem::path("generated") / "shader_cache"
                                                         : std::filesystem::path(opt.shader_cache);
  std::unique_ptr<dal::Presenter> presenter;
  if (opt.window) {
    dal::PresenterOptions presenter_options;
    presenter_options.frame_slots = dal::RsxRenderer::kFramesInFlight;
    presenter_options.on_close = [] { dal::stopProcess(dal::ExitCode::ProcessExit, "dal: stop: window closed"); };
    presenter = dal::Presenter::create(*context, std::move(presenter_options), error);
    if (!presenter) {
      error = "window: " + error;
      return nullptr;
    }
  }
  auto executor = std::make_unique<VulkanExecutor>(std::move(context), std::move(presenter), render_options);
  if (!executor->valid()) {
    error = executor->error();
    return nullptr;
  }
  const auto init_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  dal::logInfo("dal: render: device and renderer ready in {} ms", init_ms);
  if (!render_options.dump_dir.empty()) {
    dal::logInfo("dal: render: dumping every {} flip(s) to {}", render_options.dump_every,
                 render_options.dump_dir.generic_string());
  }
  return executor;
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

void printUsage() {
  std::fputs(
      "usage: dal_game [--game-dir DIR] [--vfs-config FILE] (at least one) [--elf PATH]\n"
      "                [--seconds N] [--strict] [--log FILE] [--hdd-dir DIR] [--sdata-cache DIR]\n"
      "                [--log-level trace|debug|info|warn|error]\n"
      "                [--quiet-hle] [--load-only]\n"
      "                [--dialog-answer yes|no|ok] [--pad none|neutral]\n"
      "                [--render none|vulkan] [--rsx-capture N[,M...]]\n"
      "                [--dump-frames DIR] [--dump-frames-every K] [--shader-cache DIR]\n"
      "                [--vk-validation] [--window]\n",
      stderr);
}

std::optional<dal::LogLevel> parseLogLevel(std::string_view name) {
  if (name == "trace") return dal::LogLevel::Trace;
  if (name == "debug") return dal::LogLevel::Debug;
  if (name == "info") return dal::LogLevel::Info;
  if (name == "warn") return dal::LogLevel::Warn;
  if (name == "error") return dal::LogLevel::Error;
  return std::nullopt;
}

bool parseArgs(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; i++) {
    const std::string_view arg = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s needs a value\n", argv[i]);
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (arg == "--game-dir") {
      if (!value(opt.game_dir)) return false;
    } else if (arg == "--elf") {
      if (!value(opt.elf)) return false;
    } else if (arg == "--hdd-dir") {
      if (!value(opt.hdd_dir)) return false;
    } else if (arg == "--sdata-cache") {
      if (!value(opt.sdata_cache)) return false;
    } else if (arg == "--vfs-config") {
      if (!value(opt.vfs_config)) return false;
    } else if (arg == "--png-dump") {
      if (!value(opt.png_dump)) return false;
    } else if (arg == "--rsx-census") {
      if (!value(opt.rsx_census)) return false;
    } else if (arg == "--rsx-capture") {
      std::string list;
      if (!value(list) || !parseFrameList(list, opt.rsx_capture_frames)) {
        std::fprintf(stderr, "error: --rsx-capture needs frame numbers (1-based), e.g. 300,600\n");
        return false;
      }
    } else if (arg == "--log") {
      if (!value(opt.log_file)) return false;
    } else if (arg == "--log-level") {
      std::string name;
      if (!value(name)) return false;
      const std::optional<dal::LogLevel> level = parseLogLevel(name);
      if (!level) {
        std::fprintf(stderr, "error: --log-level needs trace, debug, info, warn or error\n");
        return false;
      }
      // Applied here so that nothing later in startup logs at the wrong level.
      dal::setLogLevel(*level);
    } else if (arg == "--dialog-answer") {
      if (!value(opt.dialog_answer)) return false;
      if (!dal::setDialogAnswer(opt.dialog_answer)) {
        std::fprintf(stderr, "error: --dialog-answer needs yes, no or ok\n");
        return false;
      }
    } else if (arg == "--pad") {
      std::string name;
      if (!value(name)) return false;
      if (!dal::setPadBackend(name)) {
        std::fprintf(stderr, "error: --pad needs none or neutral\n");
        return false;
      }
    } else if (arg == "--render") {
      std::string name;
      if (!value(name)) return false;
      if (name != "none" && name != "vulkan") {
        std::fprintf(stderr, "error: --render needs none or vulkan\n");
        return false;
      }
      opt.render_flag = name == "vulkan";
    } else if (arg == "--dump-frames") {
      if (!value(opt.dump_frames)) return false;
    } else if (arg == "--dump-frames-every") {
      std::string text;
      if (!value(text)) return false;
      const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), opt.dump_frames_every);
      if (ec != std::errc{} || end != text.data() + text.size() || opt.dump_frames_every == 0) {
        std::fprintf(stderr, "error: --dump-frames-every needs a positive integer\n");
        return false;
      }
      opt.dump_every_given = true;
    } else if (arg == "--shader-cache") {
      if (!value(opt.shader_cache)) return false;
    } else if (arg == "--vk-validation") {
      opt.vk_validation = true;
    } else if (arg == "--window") {
      opt.window = true;
    } else if (arg == "--seconds") {
      std::string text;
      if (!value(text)) return false;
      const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), opt.seconds);
      if (ec != std::errc{} || end != text.data() + text.size()) {
        std::fprintf(stderr, "error: --seconds needs a non-negative integer\n");
        return false;
      }
    } else if (arg == "--strict") {
      opt.strict = true;
    } else if (arg == "--quiet-hle") {
      opt.quiet_hle = true;
    } else if (arg == "--load-only") {
      opt.load_only = true;
    } else {
      std::fprintf(stderr, "error: unknown argument %s\n", argv[i]);
      return false;
    }
  }
  if (opt.game_dir.empty() && opt.vfs_config.empty()) {
    std::fputs("error: --game-dir or --vfs-config is required\n", stderr);
    return false;
  }
  // --dump-frames and --window imply vulkan; an explicit "--render none" (anywhere on the line) overrides
  // that and then conflicts with it. The other options have no effect without a renderer.
  opt.render_vulkan = opt.render_flag.value_or(!opt.dump_frames.empty() || opt.window);
  const bool render_option = !opt.dump_frames.empty() || opt.dump_every_given || !opt.shader_cache.empty() ||
                             opt.vk_validation || opt.window;
  if (render_option && !opt.render_vulkan) {
    std::fputs(
        "error: --dump-frames, --dump-frames-every, --shader-cache, --vk-validation and --window need --render vulkan\n",
        stderr);
    return false;
  }
  return true;
}

// Video and audio out report what the game's PARAM.SFO allows. A missing or broken file keeps the
// defaults of gameParamSfo().
void loadGameParamSfo() {
  bool read_only = false;
  const auto sfo_path = dal::resolveGuestPath("/dev_bdvd/PS3_GAME/PARAM.SFO", read_only);
  if (!sfo_path) {
    dal::logWarn("dal: sfo: /dev_bdvd is not mounted; using the defaults");
    return;
  }
  auto sfo = dal::readParamSfoFile(*sfo_path);
  if (!sfo) {
    dal::logWarn("dal: sfo: {}; using the defaults", sfo.error());
    return;
  }
  const auto title_id = sfo->strings.find("TITLE_ID");
  const auto resolution = sfo->ints.find("RESOLUTION");
  const auto sound_format = sfo->ints.find("SOUND_FORMAT");
  dal::logInfo("dal: sfo: TITLE_ID {} RESOLUTION {:#x} SOUND_FORMAT {:#x}",
               title_id == sfo->strings.end() ? "?" : title_id->second,
               resolution == sfo->ints.end() ? 0x1u : resolution->second,
               sound_format == sfo->ints.end() ? 0x1u : sound_format->second);
  dal::setGameParamSfo(std::move(*sfo));
}

}  // namespace

int main(int argc, char** argv) {
  dal::installProcessSettings();
  dal::installCensus();

  Options opt;
  if (!parseArgs(argc, argv, opt)) {
    printUsage();
    return static_cast<int>(dal::ExitCode::Usage);
  }

  std::error_code ec;
  dal::VfsCliOptions cli;
  if (!opt.game_dir.empty()) {
    const std::filesystem::path game_dir(opt.game_dir);
    if (!std::filesystem::is_directory(game_dir, ec)) {
      std::fprintf(stderr, "error: game directory %s does not exist\n", opt.game_dir.c_str());
      return static_cast<int>(dal::ExitCode::Usage);
    }
    cli.game_dir = game_dir;
  }
  if (!opt.hdd_dir.empty()) {
    cli.hdd_dir = std::filesystem::path(opt.hdd_dir);
  }
  if (!opt.sdata_cache.empty()) {
    cli.sdata_cache = std::filesystem::path(opt.sdata_cache);
  }

  if (!opt.log_file.empty() && !dal::setLogFile(opt.log_file)) {
    std::fprintf(stderr, "error: cannot open log file %s\n", opt.log_file.c_str());
    return static_cast<int>(dal::ExitCode::Usage);
  }

  // Precedence: command-line options, then the configuration file, then the defaults.
  std::optional<dal::VfsConfigFile> vfs_file;
  if (!opt.vfs_config.empty()) {
    auto loaded = dal::loadVfsConfig(std::filesystem::path(opt.vfs_config));
    if (!loaded) {
      dal::logError("vfs config: {}", loaded.error());
      return static_cast<int>(dal::ExitCode::LoadError);
    }
    for (const std::string& warning : loaded->warnings) {
      dal::logWarn("vfs config: {}", warning);
    }
    vfs_file = std::move(*loaded);
  }
  auto layout = dal::composeVfsLayout(vfs_file ? &*vfs_file : nullptr, cli);
  if (!layout) {
    dal::logError("vfs: {}", layout.error());
    return static_cast<int>(dal::ExitCode::LoadError);
  }
  dal::configureVfs(std::move(*layout));

  std::filesystem::path elf_path(opt.elf);
  if (opt.elf.empty()) {
    bool read_only = false;
    const auto default_elf = dal::resolveGuestPath("/dev_bdvd/PS3_GAME/USRDIR/EBOOT.elf", read_only);
    if (!default_elf) {
      dal::logError("no executable: pass --elf or --game-dir, or mount /dev_bdvd in the VFS config");
      return static_cast<int>(dal::ExitCode::LoadError);
    }
    elf_path = *default_elf;
  }

  if (!opt.png_dump.empty()) {
    std::filesystem::create_directories(opt.png_dump, ec);
    dal::setPngDumpDirectory(std::filesystem::path(opt.png_dump));
  }

  loadGameParamSfo();
  if (!opt.rsx_census.empty()) dal::setRsxArgCensusFile(opt.rsx_census);
  if (!opt.rsx_capture_frames.empty()) {
    dal::setRsxCaptureFrames(opt.rsx_capture_frames, std::filesystem::path("generated") / "rsx" / "captures");
  }

  dal::installFaultHandler();
  dal::Process& process = dal::process();
  process.strict = opt.strict;
  if (!process.memory.reserve()) {
    dal::logError("cannot reserve the guest address space");
    return static_cast<int>(dal::ExitCode::Fatal);
  }

  const auto loaded = dal::loadExecutable(elf_path);
  if (!loaded) {
    dal::logError("load failed: {}", loaded.error());
    return static_cast<int>(dal::ExitCode::LoadError);
  }
  const dal::LoadedExecutable& exe = *loaded;

  const std::string_view expected_sha256 = kDalPpuExecutableSha256;
  if (exe.sha256 != expected_sha256) {
    dal::logError("executable hash {} does not match the generated code ({})", exe.sha256,
                  expected_sha256);
    return static_cast<int>(dal::ExitCode::LoadError);
  }

  dalRegisterFuncs(kDalPpuFuncMappings, kDalPpuFuncMappingCount);
  dal::setQuietHle(opt.quiet_hle);
  dal::installImports(kDalPpuImports, kDalPpuImportCount);

  dal::logInfo(
      "dal: loaded {}: entry {:#010x} toc {:#010x} opd {:#010x} tls {:#010x}/{}/{:#x} sdk {:#010x} "
      "prio {} stacksize {:#x} malloc {:#x} ppc_seg {:#x} image end {:#010x}",
      elf_path.filename().string(), exe.entry_func, exe.entry_toc, exe.entry_opd, exe.tls_vaddr,
      exe.tls_filesz, exe.tls_memsz, exe.sdk_version, exe.primary_prio, exe.primary_stacksize,
      exe.malloc_pagesize, exe.ppc_seg, exe.image_end);
  dal::logInfo("dal: sha256 {}", exe.sha256);

  if (opt.load_only) {
    return static_cast<int>(dal::ExitCode::ProcessExit);
  }

  // The device is created on the RSX thread when it starts, so it belongs to that thread.
  if (opt.render_vulkan) {
    dal::setRsxExecutorFactory(
        [opt](std::string& error) { return createVulkanExecutor(opt, error); });
  }

  // RPCS3 passes this VFS path as argv[0] (`Emu/System.cpp` `Emulator::Load`, "Fake arg").
  dal::bootMainThread(exe, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN");
  dal::runSupervisor(opt.seconds);
}
