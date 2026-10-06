// dal_game: loads the decrypted EBOOT.elf and runs it through the generated PPU code.

#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <string_view>

#include "core/boot.h"
#include "core/census.h"
#include "core/elf_loader.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"

namespace {

struct Options {
  std::string game_dir;
  std::string elf;
  std::string log_file;
  unsigned seconds = 0;
  bool strict = false;
  bool quiet_hle = false;
  bool load_only = false;
};

void printUsage() {
  std::fputs(
      "usage: dal_game --game-dir DIR [--elf PATH] [--seconds N] [--strict] [--log FILE]\n"
      "                [--quiet-hle] [--load-only]\n",
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
    } else if (arg == "--log") {
      if (!value(opt.log_file)) return false;
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
  if (opt.game_dir.empty()) {
    std::fputs("error: --game-dir is required\n", stderr);
    return false;
  }
  return true;
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
  const std::filesystem::path game_dir(opt.game_dir);
  if (!std::filesystem::is_directory(game_dir, ec)) {
    std::fprintf(stderr, "error: game directory %s does not exist\n", opt.game_dir.c_str());
    return static_cast<int>(dal::ExitCode::Usage);
  }
  const std::filesystem::path elf_path =
      opt.elf.empty() ? game_dir / "PS3_GAME" / "USRDIR" / "EBOOT.elf"
                      : std::filesystem::path(opt.elf);

  if (!opt.log_file.empty() && !dal::setLogFile(opt.log_file)) {
    std::fprintf(stderr, "error: cannot open log file %s\n", opt.log_file.c_str());
    return static_cast<int>(dal::ExitCode::Usage);
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

  // RPCS3 passes this VFS path as argv[0] (`Emu/System.cpp` `Emulator::Load`, "Fake arg").
  dal::bootMainThread(exe, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN");
  dal::runSupervisor(opt.seconds);
}
