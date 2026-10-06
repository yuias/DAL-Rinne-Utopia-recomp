// dal_game: loads the decrypted EBOOT.elf and runs it through the generated PPU code.

#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <string_view>

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

namespace {

struct Options {
  std::string game_dir;
  std::string elf;
  std::string hdd_dir;
  std::string sdata_cache;
  std::string vfs_config;
  std::string png_dump;
  std::string log_file;
  std::string dialog_answer;
  unsigned seconds = 0;
  bool strict = false;
  bool quiet_hle = false;
  bool load_only = false;
};

void printUsage() {
  std::fputs(
      "usage: dal_game [--game-dir DIR] [--vfs-config FILE] (at least one) [--elf PATH]\n"
      "                [--seconds N] [--strict] [--log FILE] [--hdd-dir DIR] [--sdata-cache DIR]\n"
      "                [--quiet-hle] [--load-only]\n"
      "                [--dialog-answer yes|no|ok] [--pad none|neutral]\n",
      stderr);
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
    } else if (arg == "--log") {
      if (!value(opt.log_file)) return false;
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

  // RPCS3 passes this VFS path as argv[0] (`Emu/System.cpp` `Emulator::Load`, "Fake arg").
  dal::bootMainThread(exe, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN");
  dal::runSupervisor(opt.seconds);
}
