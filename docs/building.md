# Building

The [top-level README](../README.md) shows the one build command a player needs. This page lists
every parameter of the build script, the CMake variables behind them, and the commands for the
test runs.

## `tools/build.ps1`

```powershell
pwsh -NoProfile -File tools/build.ps1 [-Preset debug|release] [-Clean] [-Test]
     [-GeneratedDir <dir>] [-GameDir <dir>] [-EbootElf <path>] [-EbootBin <path>]
     [-Rpcs3CryptoDir <dir>] [-LlvmBin <dir>]
```

The script enters the Visual Studio developer shell itself unless it is already running in one,
then configures, builds, and optionally runs CTest.

| Parameter | Meaning |
| --- | --- |
| `-Preset debug\|release` | The CMake preset (default `debug`). Build output goes to `build/<preset>/`, executables to `build/<preset>/bin/`. |
| `-Clean` | Delete `build/<preset>/` before configuring. This also deletes the `.sdat` cache and the default hdd directory `build/<preset>/hdd` (game data and saves). |
| `-Test` | Run CTest after the build. Which tests run depends on the other parameters; see [Tests](tests.md). |
| `-GeneratedDir <dir>` | The directory written by `ps3tool.py emit --out <dir>`. Compiles it into `dal_ppu_generated` and builds `dal_game` and the link test. Without it only the runtime libraries, tools and unit tests are built. If the directory has no `sources.cmake`, configuration fails and prints the `emit` command to run. CMake variable `DAL_PPU_GENERATED_DIR`. |
| `-GameDir <dir>` | The game directory (the one that contains `PS3_GAME`). The build decrypts its `.sdat` files into `build/<preset>/sdata_cache` (see [SDAT files](sdat.md)), and the game tests run against it. With the generated tree it also writes `build/<preset>/bin/config.json` when there is none (see [below](#configjson-for-dal_game)). CMake variable `DAL_GAME_DIR`. |
| `-EbootElf <path>` | The decrypted executable for the Python tests that read the real ELF and for the game tests. Not needed to build or play. When `-EbootBin` is given and this is not, it defaults to `generated/inputs/EBOOT.elf`, the file that `-EbootBin` produces. The file must exist. CMake variable `DAL_EBOOT_ELF`. |
| `-EbootBin <path>` | The game's `PS3_GAME/USRDIR/EBOOT.BIN`. The build decrypts it into `generated/inputs/EBOOT.elf`; see [Decrypting `EBOOT.BIN`](#decrypting-ebootbin). The file must exist. CMake variable `DAL_EBOOT_BIN`. |
| `-Rpcs3CryptoDir <dir>` | The `rpcs3/Crypto` directory of a local RPCS3 checkout, used for the keys instead of the download (see [Keys](#keys)). The directory must exist. CMake variable `DAL_RPCS3_CRYPTO_DIR`. |
| `-LlvmBin <dir>` | Put this LLVM `bin` directory first on `PATH`. Defaults to the `DAL_LLVM_BIN` environment variable. Use it when the clang bundled with Visual Studio is too old or clang is not on `PATH`. |

Relative paths are resolved against the current directory. The parameters set CMake cache
variables, so a later build of the same preset keeps them until they are given again.

### `config.json` for `dal_game`

With `-GameDir`, after a successful build that produced `build/<preset>/bin/dal_game.exe` (so with
`-GeneratedDir`), `tools/write-game-config.ps1` writes `build/<preset>/bin/config.json`, which
`dal_game.exe` reads when it is started from that folder (see
[Configuration file](runtime-options.md#configuration-file)). The rules:

- The file is written only when the directory holds the game data (`<game-dir>/PS3_GAME/PARAM.SFO`
  exists). Otherwise the build prints `config.json not written: <game-dir> has no PS3_GAME/PARAM.SFO`.
- It holds `game_dir` (the full path of `-GameDir`) and `elf`: the full path of `-EbootElf` if it is
  given, otherwise of `generated/inputs/EBOOT.elf` if `-EbootBin` is given, otherwise the `DAL_EBOOT_ELF`
  value kept in the CMake cache from an earlier build. When all three are empty there is no `elf`. The build prints `wrote <path>`.
- An existing `config.json` is never modified, so your own edits survive later builds. The build
  prints `config.json left unchanged: ...` when the file cannot be parsed, when its `game_dir` is
  another directory than `-GameDir`, or when `-EbootElf` or `-EbootBin` is given and the file has no
  `elf`. In the last case the message gives the `"elf"` entry to add.
- `sdata_cache` and `hdd_dir` are not written: `dal_game` finds `build/<preset>/sdata_cache` and
  `build/<preset>/hdd` next to `bin` on its own, and a written path would point at a directory that
  `-Clean` deletes.

The script can also be run alone: `pwsh -NoProfile -File tools/write-game-config.ps1 -BinDir <dir>
-GameDir <dir> [-EbootElf <path>]`.

## CMake variables

These can also be passed to `cmake` directly (`cmake --preset release -D<name>=<value>`):

| Variable | Default | Meaning |
| --- | --- | --- |
| `DAL_PPU_GENERATED_DIR` | empty | Set by `-GeneratedDir`. |
| `DAL_GAME_DIR` | empty | Set by `-GameDir`. |
| `DAL_EBOOT_ELF` | empty | Set by `-EbootElf`. While it is empty and `DAL_EBOOT_BIN` is set, configuration sets it to `generated/inputs/EBOOT.elf`. |
| `DAL_EBOOT_BIN` | empty | Set by `-EbootBin`. |
| `DAL_RPCS3_CRYPTO_DIR` | empty | Set by `-Rpcs3CryptoDir`. |
| `DAL_FFMPEG_DIR` | empty | An extracted FFmpeg build to use instead of `generated/deps/ffmpeg/<pinned build>` (see [Movies](movies.md)). |
| `DAL_GAMEINPUT_DIR` | empty | An extracted `Microsoft.GameInput` package to use instead of `generated/deps/gameinput/<pinned version>` (see [Input](input.md#the-gameinput-package)). Configure needs the package from `tools/fetch-gameinput.ps1`. |
| `DAL_PPU_WERROR` | `ON` | Add `-Werror` for generated code and the code that mirrors it (header check, semantics harness, link test). Every such target is compiled with `-O1 -march=x86-64-v3 -ffp-contract=off -fpatchable-function-entry=8 -Wall -Wextra`, whatever the preset's optimization level. |

No tracked file contains a game path: they are command-line arguments, CMake cache variables, a
user-local `config.json` (`config/dal_game.example.json` shows the format with placeholders), or a
user-local VFS configuration (`config/vfs.example.json` does the same).

## Decrypting `EBOOT.BIN`

`dal_decrypt` turns the game's `EBOOT.BIN` into the decrypted `EBOOT.elf`, with no RPCS3 install.
It is configured by `-EbootBin`. Without it, configuration prints
`DAL: EBOOT.BIN decryption not configured`, and the build contains only the tool and its unit
tests.

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset release -EbootBin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN
```

The build target `dal_inputs` runs the tool and writes `generated/inputs/EBOOT.elf` (shared by both
presets, never into the game directory). Decryption runs at build time, not at configure time, and
again only when `EBOOT.BIN`, a key file or the tool has changed. The produced `EBOOT.elf` is the
default of `DAL_EBOOT_ELF`, so the Python real-executable tests and the game tests use it, and
`config.json` points at it (see [above](#configjson-for-dal_game)). The usual order is: build once
with `-EbootBin`, run `ps3tool.py emit generated/inputs/EBOOT.elf --out generated/ppu`, then build
again with `-GeneratedDir generated/ppu -GameDir <game-dir> -EbootBin <path>`.

### Keys

No key is stored in this repository. With `-EbootBin`, or with `-GameDir` and `-GeneratedDir` (for
the `.sdat` files, see [SDAT files](sdat.md)), configuration downloads `key_vault.h` and
`key_vault.cpp` from RPCS3 at commit `f9b8c209a3800c1865bcfeade611f65f5237138c` (`rpcs3/Crypto` in
<https://github.com/RPCS3/rpcs3>) into `generated/deps/rpcs3/<commit>/`. Each download is checked
against a SHA-256 pinned in `cmake/rpcs3_keys.cmake`, and a copy that is already there and matches is
not downloaded again:

| File | SHA-256 |
| --- | --- |
| `key_vault.h` | `d5d0cede74c7900bb23ca8d60066194648ed4b976eaee60315c0bcbed7dc2aaf` |
| `key_vault.cpp` | `a2947ff6b681258c198552ec6344e16685e2df649018206daa3f41a0a1aadbd7` |

`dal_decrypt` and `dal_sdat` read only the key constants from these files, as data. If the download
fails, configuration with `-EbootBin` stops with an error that names `DAL_RPCS3_CRYPTO_DIR` and
`-Rpcs3CryptoDir`; for the `.sdat` files alone it only warns (see [SDAT files](sdat.md#keys)). To
build offline, point `-Rpcs3CryptoDir` (`DAL_RPCS3_CRYPTO_DIR`) at the `rpcs3/Crypto` directory of a
local RPCS3 checkout; it must hold both files. Their hashes are not enforced: configuration prints a
line when one differs from the pin, and the output pin below catches a real problem.

### What is checked

- `EBOOT.BIN` must have the SHA-256 in the `eboot_bin` entry of `config/target.json`. A game-update
  `EBOOT.BIN` or another title is refused before decryption.
- The output: its size and SHA-256 must match the `eboot_elf` entry of `config/target.json`. On a
  mismatch the tool prints both values and writes nothing, and an existing output file stays as it
  was. A match is written as `EBOOT.elf.part` and then renamed.

NPDRM SELFs, debug SELFs and 32-bit (SPU) SELFs are refused with an error.

### The tool

`dal_decrypt` has two commands. The build runs `self` with the pins read from `config/target.json`;
running `dal_decrypt` without arguments prints the usage text and exits with 2.

```text
dal_decrypt self --keys DIR --in EBOOT.BIN --out EBOOT.elf --expect-size N --expect-sha256 HEX
                 [--expect-input-sha256 HEX]
dal_decrypt keys --keys DIR
```

`--keys` is the directory with `key_vault.h` and `key_vault.cpp`. `keys` prints how many keys of each
type were read, never the keys. A successful `self` prints
`EBOOT.elf: <size> bytes, sha256 <hex>: matches the pin`. Every error is one line that starts with
`error: `, and no key bytes appear in any output. The exit code is 0 on success, 1 on a failure and 2
on a usage error. The tests `decrypt_core` and `decrypt_eboot` are listed in [Tests](tests.md).

`dal_decrypt` (with its library and the unit tests `dal_decrypt_tests`) is always built. `dal_inputs`,
the target that writes `generated/inputs`, exists only with `-EbootBin`.

## What gets built

The runtime core (`dal_runtime_core`: `runtime/src/core`, `lv2`, `hle`, `input`, `rsx`, `audio`
without the WASAPI output, and the demuxer, player and ATRAC3plus file parser in `runtime/src/media`)
does not reference generated symbols and is always built, together with its unit tests. The renderer
(`dal_render`, `runtime/src/render`) is a separate library on top of it, so the core itself needs no
GPU. `dal_game` is built only with `-GeneratedDir`, because only `runtime/src/game_main.cpp` passes
the generated tables to the core. It checks the executable against the SHA-256 that the generated
code was emitted from.

## Test builds

Unit tests only (no game files needed):

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset debug -Clean -Test
```

Unit tests plus the Python tests that read the real executable:

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset release -EbootBin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN -Test
```

This also runs `decrypt_eboot`. With an `EBOOT.elf` that you already have, `-EbootElf
<path-to-EBOOT.elf>` replaces `-EbootBin`.

Generated code, the game runs (`game_boot`, `game_input`, `game_audio`) and the tests that need the
`.sdat` cache (`atrac_decode`) or the movies (`movie_decode`):

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset release -GeneratedDir generated/ppu -EbootBin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN -GameDir <game-dir> -Test
```

The full list of tests and the conditions under which each one is registered are in
[Tests](tests.md).

## Optional tools

- Ghidra 12.x and a JDK 21, for `ps3tool.py ghidra-export` only (see
  [Analysis tools](analysis-tools.md#ghidra-cross-check)). `recover` does not need Ghidra, because
  the Ghidra results it uses are committed.
- git and a sparse checkout of the RPCS3 sources, for `ps3tool.py nidgen` only (see
  [Analysis tools](analysis-tools.md#nid-and-syscall-name-tables)).
