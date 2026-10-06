# DAL Rinne Utopia Recomp

Static recompilation of the PS3 title *Date A Live: Rinne Utopia* (BLJM61048) for
Windows, built from your own copy of the game.

This repository contains no game data. No executables, keys, or decrypted binaries are
included or accepted by version control; you supply your own `EBOOT.elf`, and all tool
output is written to ignored directories.

Current state: build scaffold, a small runtime stub, and analysis tools for the PPU
executable (ELF summary, SPU probe, disassembler). Recompilation itself is not implemented yet.

## Prerequisites

- Visual Studio 2026 with the Windows SDK (provides the MSVC headers and libraries)
- PowerShell 7 and the `VSSetup` module (`Install-Module VSSetup -Scope CurrentUser`)
- clang and clang++ with lld (a standalone LLVM install; see `-LlvmBin` below)
- CMake 3.29 or newer
- Ninja
- Python 3.10 or newer (the analysis tools use only the standard library)

## Obtaining `EBOOT.elf`

The tools work on the decrypted PPU executable, not on `EBOOT.BIN`.

1. Dump your own disc or digital copy and install it in RPCS3.
2. In RPCS3, use Utilities > Decrypt PS3 Binaries and select the game's `EBOOT.BIN`.
3. Take the resulting `EBOOT.elf` and place it anywhere you like. The `game/` directory in
   the repository root is git-ignored and works well for this.

`*.elf`, `*.self`, `*.sprx`, `*.prx`, `*.sdat`, and `*.edat` are also git-ignored.

## Building

```powershell
pwsh -NoProfile -File tools/build.ps1 [-Preset debug|release] [-Clean] [-Test] [-EbootElf <path>] [-LlvmBin <dir>]
```

- `-Preset` selects the CMake preset (`debug` by default, or `release`). Build output goes
  to `build/<preset>/`.
- `-Clean` removes that preset's build directory before configuring.
- `-Test` runs CTest after the build: a runtime version check, a `musttail` support
  check, and the Python unit tests.
- `-EbootElf <path>` passes the decrypted ELF to the tests so the checks against the real
  executable run instead of being skipped.
- `-LlvmBin <dir>` puts a specific LLVM `bin` directory first on `PATH`. It defaults to the
  `DAL_LLVM_BIN` environment variable. Use it when the clang bundled with Visual Studio is
  too old or when clang is not on `PATH`.

The script enters the Visual Studio developer shell itself unless it is already running in
one, then configures, builds, and optionally tests.

Examples:

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset debug -Clean -Test
pwsh -NoProfile -File tools/build.ps1 -Preset release -Test -EbootElf game/EBOOT.elf
```

## Analysis tools

`tools/ps3tool.py` has three subcommands. Each takes the path to the decrypted ELF.

```sh
python tools/ps3tool.py info   <elf> [-o OUTPUT] [--target-config FILE] [--no-verify]
python tools/ps3tool.py probe  <elf> [-o OUTPUT]
python tools/ps3tool.py disasm <elf> (--start ADDR (--count N | --end ADDR) | --entry [--count N] | --stats [--start ADDR --end ADDR] [--json OUT]) [--raw]
```

- `info` checks the file against the pinned SHA-256 and writes a JSON summary (segments,
  sections, imports, exports) to `generated/analysis/eboot_info.json`. It refuses encrypted
  input. `--no-verify` skips the hash check.
- `probe` searches for embedded SPU images and SPU-related imports, counts lv2 syscall
  sites and numbers, and writes `generated/analysis/probe.json`.
- `disasm` prints PPU disassembly for an address range (`--start` with `--count` or
  `--end`) or from the entry function (`--entry`). `--stats` decodes every word of the
  executable segments and reports decode coverage. `--raw` disables simplified mnemonics.

Run `python tools/ps3tool.py <command> --help` for the full option list.

Python unit tests use synthetic ELF files built in memory. Tests that need the real
executable are skipped unless `DAL_EBOOT_ELF` points at it:

```sh
python -m unittest discover -s tools/tests -t tools -v
DAL_EBOOT_ELF=game/EBOOT.elf python -m unittest discover -s tools/tests -t tools -v
```

## Target pin

`config/target.json` records the title ID and the size and SHA-256 of both the encrypted
`EBOOT.bin` and the decrypted `EBOOT.elf` that the tools were written against. It never
contains paths. `ps3tool.py info` compares the input with the `eboot_elf` entry so analysis
results are only trusted for the expected build.

## Layout

| Path | Contents |
| --- | --- |
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/` | Build configuration (clang, Ninja, `debug` and `release` presets) |
| `runtime/` | Runtime library and executable stub (`dal_runtime`) |
| `tools/build.ps1` | Windows build wrapper |
| `tools/ps3tool.py` | Command-line entry point for the analysis tools |
| `tools/ps3recomp/` | ELF/PRX parsing, PPU decoder and formatter, probe |
| `tools/tests/` | Unit tests |
| `config/target.json` | Pinned size and hashes of the target executable |
| `generated/` | Tool output (git-ignored) |
| `build/` | Build output (git-ignored) |
| `game/` | Suggested place for your own `EBOOT.elf` (git-ignored) |
