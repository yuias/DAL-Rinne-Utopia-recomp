# DAL Rinne Utopia Recomp

Static recompilation of the PS3 title *デート・ア・ライブ 凜祢ユートピア* (BLJM61048) for
Windows, built from your own copy of the game.

This repository contains no game data (see [Licence and legal notice](#licence-and-legal-notice)). No executables, keys,
or decrypted binaries are included or accepted by version control; you supply your own
`EBOOT.elf`, and all tool output is written to ignored directories.

Current state: build scaffold, analysis tools for the PPU executable (ELF summary, SPU probe,
disassembler, Ghidra cross-check, NID and syscall name tables, function recovery), an emitter
that turns the recovered functions into C source, an SDAT decryption tool (`dal_sdat`), and a
runtime (`dal_game`) that loads the executable, runs it through the generated code on real guest
threads, serves a first set of lv2 system calls and HLE modules, and runs a fake RSX that
consumes the game's command buffer. With `--render vulkan` a renderer on Vulkan 1.3 draws what
the RSX commands describe, either into a window (`--window`) or into PNG files
(`--dump-frames`); a capture and replay tool (`dal_rsxreplay`) renders single recorded frames
without the game. The game boots, runs its frame loop at the 60 Hz vertical-blank rate, loads its
assets, plays its three intro movies (video only; FFmpeg decodes them) and reaches the title
screen. Audio output is not implemented. See [Current state](#current-state) and
[Not implemented](#not-implemented).

## Documentation

This README covers building and running the game. Development material is under
[`docs/`](docs/README.md): the analysis tools, function recovery, the C emitter, runtime
internals, the virtual file system, rendering, movies, tests, the repository layout, and a guide
for applying the method to other PS3 games.

## Prerequisites

- Visual Studio 2026 with the Windows SDK (provides the MSVC headers and libraries)
- PowerShell 7 and the `VSSetup` module (`Install-Module VSSetup -Scope CurrentUser`)
- clang and clang++ with lld (a standalone LLVM install; see `-LlvmBin` below)
- CMake 3.29 or newer
- git, with the submodules initialised: `git submodule update --init` (spdlog, libpng, zlib, yyjson,
  Vulkan-Headers, volk and VulkanMemoryAllocator under `externals/`; clone with
  `--recurse-submodules` to do it in one step)
- Ninja
- The Vulkan SDK, with the `VULKAN_SDK` environment variable set. The renderer compiles shaders at
  run time with the SDK's static shaderc, so CMake looks for `$VULKAN_SDK/Lib/shaderc_combined.lib`
  (release) and `$VULKAN_SDK/Lib/shaderc_combinedd.lib` (debug) and for
  `$VULKAN_SDK/Include/shaderc/shaderc.hpp`, and configuration fails when they are missing. The
  Vulkan headers and loader code come from the submodules, not from the SDK. The validation layer
  that `--vk-validation` enables is part of the SDK as well.
- To run the renderer: a Vulkan 1.3 device and driver with dynamic rendering, synchronization2 and
  shader demote-to-helper-invocation (see [Rendering](docs/rendering.md#rendering))
- FFmpeg for the movies: run `pwsh -NoProfile -File tools/fetch-ffmpeg.ps1` once before the
  first build. It downloads a pinned LGPL build (DLLs only), checks its SHA-256 and extracts it
  under `generated/deps/ffmpeg`; `-From <zip>` uses a local copy of the pinned zip instead.
  Configuration fails with this hint while FFmpeg is missing, and `tools/build.ps1` does not
  fetch it. See [Movies](docs/movies.md) and `THIRD_PARTY_NOTICES.md`.
- Python 3.10 or newer (the analysis tools use only the standard library)
- Optional, for `ghidra-export` only: Ghidra 12.x and a JDK 21 (see below). `recover` does
  not need Ghidra, because the Ghidra results it uses are committed.
- Optional, for `nidgen` only: git and a sparse checkout of the RPCS3 sources (see below)

## Obtaining `EBOOT.elf`

The tools work on the decrypted PPU executable, not on `EBOOT.BIN`.

1. Dump your own disc or digital copy and install it in RPCS3.
2. In RPCS3, use Utilities > Decrypt PS3 Binaries and select the game's `EBOOT.BIN`.
3. Take the resulting `EBOOT.elf` and place it anywhere you like. The `game/` directory in
   the repository root is git-ignored and works well for this.

`*.elf`, `*.self`, `*.sprx`, `*.prx`, `*.sdat`, and `*.edat` are also git-ignored.

## Building

```powershell
pwsh -NoProfile -File tools/build.ps1 [-Preset debug|release] [-Clean] [-Test] [-EbootElf <path>] [-PpuDir <dir>] [-GameDir <dir>] [-LlvmBin <dir>]
```

- `-Preset` selects the CMake preset (`debug` by default, or `release`). Build output goes
  to `build/<preset>/`.
- `-Clean` removes that preset's build directory before configuring.
- `-Test` runs CTest after the build: a runtime version check, a `musttail` support
  check, the contract header check (`ppu_header_check`), the semantics harness
  (`ppu_semantics`), the runtime core unit tests (`runtime_core`), the renderer tests
  (`render_core`, `vp_translator`, `fp_translator`; see [Rendering](docs/rendering.md#rendering)), the SDAT tests
  (`sdat_tests`), the movie tests (`media_core`, `media_ffmpeg`, and with `-GameDir` also
  `movie_decode`; see [Movies](docs/movies.md)), the Python unit tests, and,
  with `-PpuDir`, the link check of the generated code (`ppu_link`). With `-PpuDir`,
  `-EbootElf` and `-GameDir` together it also runs `game_boot`; see [Running the game](#running-the-game).
  When `DAL_RPCS3_CRYPTO_DIR` is set as well, it also runs `game_input`, which leaves the title
  screen with a pad script; see [Keyboard and pad scripts](#keyboard-and-pad-scripts). When a
  local capture and reference image exist it also runs `rsx_replay`; see [Capture and replay](docs/rendering.md#capture-and-replay).
  The full list is in [Tests](docs/tests.md).
- `-EbootElf <path>` passes the decrypted ELF to the tests so the checks against the real
  executable run instead of being skipped.
- `-PpuDir <dir>` compiles the C tree written by `emit --out <dir>` into the static library
  `dal_ppu_generated` and builds `dal_ppu_link_test`. It sets the CMake cache variable
  `DAL_PPU_GENERATED_DIR`. A relative path is resolved against the current directory. If the
  directory has no `sources.cmake`, configuration fails and prints the `emit` command to run.
  It also builds the executable `dal_game`.
- `-GameDir <dir>` sets the CMake cache variable `DAL_GAME_DIR`, the game directory (the one
  that contains `PS3_GAME`) that the `game_boot` test passes to `dal_game`. A relative path is
  resolved against the current directory.
- `-LlvmBin <dir>` puts a specific LLVM `bin` directory first on `PATH`. It defaults to the
  `DAL_LLVM_BIN` environment variable. Use it when the clang bundled with Visual Studio is
  too old or when clang is not on `PATH`.

The script enters the Visual Studio developer shell itself unless it is already running in
one, then configures, builds, and optionally tests.

Examples:

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset debug -Clean -Test
pwsh -NoProfile -File tools/build.ps1 -Preset release -Test -EbootElf game/EBOOT.elf
pwsh -NoProfile -File tools/build.ps1 -Preset release -PpuDir generated/ppu -Test
pwsh -NoProfile -File tools/build.ps1 -PpuDir generated/ppu -EbootElf game/EBOOT.elf -GameDir <game-dir> -Test
```

CMake options for generated code (also usable with `cmake` directly):

- `DAL_PPU_GENERATED_DIR` is the `emit` output directory; empty (the default) builds
  without generated code.
- `DAL_RPCS3_CRYPTO_DIR` (default empty) is the `rpcs3/Crypto` directory of a local RPCS3 source
  checkout. When it is set together with `DAL_GAME_DIR`, the build runs `dal_sdat` to decrypt
  the game's `.sdat` files into `build/<preset>/sdata_cache`, the `game_boot` test passes that
  directory to `dal_game`, and the `game_input` test is registered (see [SDAT files](#sdat-files)).
  It is a CMake cache variable, so pass it with
  `cmake -D` once (`cmake -S . -B build/<preset> -DDAL_RPCS3_CRYPTO_DIR=<dir>`); `tools/build.ps1`
  has no parameter for it and keeps the cached value.
- `DAL_PPU_WERROR` (default `ON`) adds `-Werror` for generated code and the code that
  mirrors it (header check, semantics harness, link test). Every such target is compiled with
  `-O1 -march=x86-64-v3 -ffp-contract=off -fpatchable-function-entry=8 -Wall -Wextra`,
  whatever the preset's optimization level.

## Running the game

`dal_game` loads the decrypted `EBOOT.elf` into a 4 GiB guest address space, sets up the main
PPU thread the way RPCS3 does and calls the entry function through the generated code. It is
built only when `DAL_PPU_GENERATED_DIR` is set (`-PpuDir`), because it links the generated
functions. The runtime core (`dal_runtime_core`, everything under `runtime/src/core`,
`runtime/src/lv2`, `runtime/src/hle`, `runtime/src/rsx` and the demuxer and player in `runtime/src/media`) does not reference generated symbols and is always
built, together with its unit tests (`runtime_core`). The renderer (`dal_render`, `runtime/src/render`)
is a separate library on top of it, so the core itself needs no GPU. Only `runtime/src/game_main.cpp`
passes the generated tables to the core. The executable is checked against the SHA-256 that the generated
code was emitted from.

```sh
dal_game [--game-dir DIR] [--vfs-config FILE] [--elf PATH] [--hdd-dir DIR] [--sdata-cache DIR]
         [--seconds N] [--strict] [--log FILE] [--log-level LEVEL] [--quiet-hle] [--dialog-answer yes|no|ok] [--pad none|neutral|keyboard] [--pad-script FILE]
         [--load-only] [--png-dump DIR]
         [--render none|vulkan] [--rsx-capture N[,M...]] [--dump-frames DIR] [--dump-frames-every K]
         [--shader-cache DIR] [--vk-validation] [--window]
```

| Option | Meaning |
| --- | --- |
| `--game-dir DIR` | The game directory, the one that contains `PS3_GAME`. It must exist. It is mounted read-only as `/dev_bdvd` and `/app_home` (`DIR/PS3_GAME/USRDIR`), `PS3_GAME/PARAM.SFO` is read from it, and it holds the default ELF location. Required unless `--vfs-config` mounts `/dev_bdvd` (the executable can also come from `--elf`). `DIR/PS3_GAME/USRDIR` need not exist. |
| `--vfs-config FILE` | A JSON file that describes the virtual file system. The options `--game-dir`, `--hdd-dir` and `--sdata-cache` override the matching parts of it. See [VFS configuration file](docs/virtual-file-system.md#vfs-configuration-file). |
| `--elf PATH` | The decrypted executable. Default: `PS3_GAME/USRDIR/EBOOT.elf` on `/dev_bdvd` (`DIR/PS3_GAME/USRDIR/EBOOT.elf`, or where the configuration puts it). |
| `--hdd-dir DIR` | Host directory behind `/dev_hdd0` and `/dev_hdd1` (`DIR/dev_hdd0`, `DIR/dev_hdd1`; created on demand). Without it the hdd devices do not resolve and the game cannot create its game data or save files. |
| `--sdata-cache DIR` | Directory of decrypted `.sdat` files, each at its path relative to the game directory, as written by `dal_sdat`. Without it `cellFsSdataOpen` fails. See [SDAT files](#sdat-files). |
| `--seconds N` | Watchdog: stop after `N` seconds. 0 (the default) disables it. |
| `--strict` | Treat a jump to an address that is not a recovered function as a fatal error (see [Dispatch](docs/runtime.md#dispatch)). |
| `--log FILE` | Also write every log line to `FILE`, which is truncated. |
| `--log-level trace\|debug\|info\|warn\|error` | Lowest level that is logged (default `info`). `debug` adds diagnostics such as the 10 s window statistics; `trace` is the most verbose. Debug and trace lines carry a `debug: ` / `trace: ` tag. |
| `--quiet-hle` | Do not log the first calls of default-handled imports. The counts in the census are unaffected. |
| `--dialog-answer yes\|no\|ok` | What an auto-answered Yes/No message dialog replies (default `yes`; `ok` is the same as `yes`). OK dialogs always answer OK. |
| `--pad none\|neutral\|keyboard` | Input source for port 0. `neutral` connects a standard pad that never changes, `none` connects no pad, `keyboard` makes the keyboard the pad (see [Keyboard and pad scripts](#keyboard-and-pad-scripts)). Default: `keyboard` with `--window` or `--pad-script`, otherwise `neutral`. `keyboard` without `--window` or `--pad-script` is a usage error. |
| `--pad-script FILE` | Press pad buttons at given times, for runs without a person at the keyboard. The file is read and checked at start-up; an error is a usage error that names the line. It cannot be combined with `--pad none` or `--pad neutral`. See [Keyboard and pad scripts](#keyboard-and-pad-scripts). |
| `--png-dump DIR` | Write every image `cellPngDec` decodes to `DIR/png_<n>.png` (8-bit RGBA, at most 64), to check what the guest was given. `DIR` is created. |
| `--load-only` | Load and verify the executable, print the load summary and exit with 0 without running any guest code. |
| `--render none\|vulkan` | Whether the RSX commands are drawn (default `none`, unless `--dump-frames` or `--window` is given, which imply `vulkan`). See [Rendering](docs/rendering.md#rendering). |
| `--window` | Show the frames in a window. See [Rendering](docs/rendering.md#rendering). |
| `--dump-frames DIR` | Write displayed frames as PNG files to `DIR`. See [Rendering](docs/rendering.md#rendering). |
| `--dump-frames-every K` | Dump every `K`-th flip instead of every one (default 1). |
| `--shader-cache DIR` | Directory of the SPIR-V cache (default `generated/shader_cache`). |
| `--vk-validation` | Enable the Vulkan validation layer, if it is installed. |
| `--rsx-capture N[,M...]` | Record the given frames (1-based flip numbers) as `.dalcap` files. See [Capture and replay](docs/rendering.md#capture-and-replay). |

The renderer options are a usage error (exit code 2) when the renderer is off. `--dump-frames` and
`--window` switch it on; `--dump-frames-every`, `--shader-cache` and `--vk-validation` need one of
them or `--render vulkan`; an explicit `--render none` conflicts with all of them.
`--rsx-capture` does not need the renderer.

No tracked file contains a game path: they are command-line arguments or live in a user-local
VFS configuration (`config/vfs.example.json` shows the format with placeholders). The build
wrapper takes them as `-EbootElf` and `-GameDir` for the tests (CMake cache variables
`DAL_EBOOT_ELF` and `DAL_GAME_DIR`).

### Exit codes and stop lines

Every run ends with one stop line on stderr and a census (below), then exits:

| Code | Meaning | Stop line |
| --- | --- | --- |
| 0 | The guest called `sys_process_exit` or `_sys_process_exit` (also `--load-only` after loading) | `dal: stop: process exit status N` |
| 2 | Usage error: bad option, neither `--game-dir` nor `--vfs-config`, game directory missing, log file not writable | `error: ...` (usage text) |
| 3 | Fatal error: unhandled instruction, trap, guest fault (access outside the committed guest memory), unknown dispatch target with `--strict`, guest address space cannot be reserved | `dal: stop: fatal <kind>: ...` |
| 4 | Load error: file missing, not a PPU executable, SHA-256 different from the one in the generated code, invalid VFS configuration, or no executable can be located | `error: ...` |
| 5 | The `--seconds` watchdog elapsed | `dal: stop: watchdog N s` |
| 6 | Stall: every live guest thread has been in an indefinite wait for at least 2 seconds | `dal: stop: stall (N threads blocked)` |
| 7 | All guest threads ended without a process exit | `dal: stop: no guest threads left` |

A fatal stop also prints the registers and the call trace (the most recent function entries) of
the faulting guest thread. The runtime never calls `abort`: the CRT and Windows error dialogs are
disabled, so a run always ends with an exit code, also when it is started from a script.

### SDAT files

The game reads several `.sdat` files (encrypted data containers) through `cellFsSdataOpen`. The
tool `dal_sdat` decrypts them ahead of time and `dal_game --sdata-cache` serves the plain
copies:

```sh
dal_sdat --keys <rpcs3-checkout>/rpcs3/Crypto --game-dir <game-dir> --out <cache-dir>
dal_game --game-dir <game-dir> --sdata-cache <cache-dir> ...
```

- `--keys` is `key_vault.h` (or `key_vault.cpp`) of a local RPCS3 source checkout, or the
  directory that holds it. The AES keys are read from it every time the tool runs and are never
  stored in this repository or in the build output. Without a checkout the tool cannot run, and
  the tests contain no key material.
- `--game-dir` is searched recursively for `*.sdat`; each file is written under `--out` at its
  path relative to the game directory. The tool verifies every block's AES-CMAC, so a wrong key
  or a damaged file is an error (exit 1). It uses Windows CNG for AES and is built together with
  the runtime (`dal_sdat`; its tests are the CTest entry `sdat_tests`).
- The container layout and the block compression follow RPCS3's `Crypto/unedat.cpp` and
  `Crypto/lz.cpp` (behaviour only; the code is independent).
- With the CMake variable `DAL_RPCS3_CRYPTO_DIR` set (see [Building](#building)), the build writes the cache
  to `build/<preset>/sdata_cache` and the `game_boot` test passes `--sdata-cache` to `dal_game`.
- When a file is missing from the cache, `cellFsSdataOpen` fails and the game shows its own error
  dialog (auto-answered, see [Headless policy](docs/runtime.md#headless-policy)).
- A `files` entry of the VFS configuration for the sdata path (for example
  `/dev_bdvd/PS3_GAME/USRDIR/Exi_DAL.sdat`) serves that host file instead of the cache copy.

### Using the renderer

```sh
dal_game --game-dir <game-dir> --sdata-cache <cache-dir> --hdd-dir <hdd-dir> --window
dal_game --game-dir <game-dir> --sdata-cache <cache-dir> --hdd-dir <hdd-dir> --seconds 30 --dump-frames out/frames --dump-frames-every 30
```

- `--window` opens a resizable window on its own thread (initial client size 1280x720) and shows
  every flipped frame. Closing the window stops the run with exit code 0 and the stop line
  `dal: stop: window closed`.
- `--dump-frames DIR` writes the displayed frame of every flip as `DIR/frame_<flip>.png` (flips
  counted from 1, six digits; RGB, 8 bits), with no window. `DIR` is created. A file that cannot
  be written is reported once and counted, and the run goes on. `--dump-frames-every K` writes
  every `K`-th flip only. The PNG files are encoded on a worker thread, so dumping does not slow
  the game down; when the worker falls more than four frames behind, a frame is skipped and
  counted (`dal: render: dump skipped (queue full) at flip N`, and `dumps skipped (queue full)`
  in the end-of-run `dal: render:` line). At the end of a run the pending frames are written
  within 2 s. Each file is written as `.png.tmp` and renamed, so an interrupted run never leaves a
  truncated `.png`.
- `--shader-cache DIR` sets the SPIR-V cache directory (default `generated/shader_cache`).
- `--vk-validation` turns on the Khronos validation layer. It needs the layer to be installed (it
  comes with the Vulkan SDK); otherwise `dal_game` logs a warning and goes on without it.
- The log says which Vulkan device was chosen and how long the device and renderer took to
  start. If no device meets the requirements below, or the window cannot be created, the RSX
  thread logs the reason and the game goes on without rendering.

Requirements on the device: Vulkan 1.3, and the dynamic rendering, synchronization2 and shader
demote-to-helper-invocation features (the translated shaders use `discard`, which glslang
compiles to a demote). A window also needs the Win32 surface and swapchain extensions, which any
Windows driver with Vulkan support provides.

### Keyboard and pad scripts

With `--window`, the keyboard is the pad on port 0 (`--pad keyboard` is the default then). The keys
are fixed and are physical key positions (scan codes), so the layout stays in place on AZERTY and
QWERTZ keyboards and the keys work while an IME is on (the game window turns the IME off):

| Keys | Pad |
| --- | --- |
| Arrow keys (not the numpad) | D-pad |
| `Z` / `X` / `S` / `D` | Cross / Circle / Square / Triangle |
| `W` / `E` / `R` / `U` | L1 / L2 / R1 / R2 |
| `1` / `3` (main row) | L3 / R3 |
| `V` / `C` | Start / Select |

- A pressed key is a fully pressed button (pressure 0xFF); the sticks stay centred, and there are no
  keys for them. Until a key is pressed the pad is the same as with `--pad neutral`.
- A press is reported to the game for at least 60 ms, so a quick tap is not lost between two of the
  game's pad reads. Key repeat is ignored. Left and Right, or Up and Down, held together cancel
  each other, as on a real D-pad.
- When the window loses keyboard focus, every key held on the keyboard is released. If keys do
  nothing, click the window: one that a background process opened may not have keyboard focus.
- The game confirms with Circle, which is the `X` key. Start and Cross do nothing on the title
  screen. Circle skips the notice screen and the intro movies (`if_logo.pam`, `ch_logo.pam`,
  `op.pam`), and it picks NEW GAME on the title screen. It does not skip `world.pam`, the 83 s movie
  that plays after NEW GAME.
- NEW GAME can write game data, so use a fresh `--hdd-dir` when you try it.

`--pad-script FILE` presses buttons without a person at the keyboard, with or without `--window`
(the script and the keyboard add up). It is a text file with one command per line; `#` starts a
comment, and blank lines are ignored:

```
<time> press   <buttons>
<time> release <buttons>
<time> tap     <buttons> [every <interval> until <time>]
```

- Times and intervals are decimal seconds from the start of the run (the `--seconds` clock, within a
  few milliseconds), for example `40` or `40.5`. Buttons are `select l3 r3 start up right down left
  l2 r2 l1 r1 triangle circle cross square`, joined with `+` for several at once (`circle+r1`).
- `tap` presses at `<time>` and releases 150 ms later. With `every <interval> until <time>` it
  repeats at `<time>`, `<time> + <interval>`, ... for as long as that is not after the `until`
  time. The interval must be longer than 150 ms.
- Events run in time order; at the same time, releases come before presses.
- A bad line is a usage error (exit code 2) that names the line, for example
  `line 7: unknown button "crosss"`.

`tools/pad_scripts/leave_title.txt` is the script of the `game_input` test. It taps `circle` every 2 s
from 4 s to 30 s, which skips whichever screen is showing, so the script does not depend on the exact
start-up time:

```sh
dal_game --game-dir <game-dir> --sdata-cache <cache-dir> --hdd-dir <hdd-dir> --seconds 40 \
         --pad-script tools/pad_scripts/leave_title.txt
```

Pad lines in the log (prefix `dal: pad:`):

| Line | Level | When |
| --- | --- | --- |
| `dal: pad: init max_connect N` | info | the game calls `cellPadInit` |
| `dal: pad: port N connected` / `disconnected` | info | the first time port `N` is reported in that state |
| `dal: pad: port N setting 0x2 (press on, sensor off)` | info | the game sets a port setting, once per value |
| `dal: pad: keyboard (window) (script NAME, N events)` | info | at start with `--pad keyboard`; each part appears only when it applies |
| `dal: pad: script T s: press\|release BUTTONS` | info | each script event as it is applied (prefix `[host]`) |
| `dal: pad: key 0x2c down -> cross` | debug | a mapped key changes state in the window (`--log-level debug`); unmapped keys and repeats are not logged |

The CTest `game_input` runs `leave_title.txt` for 40 s and passes when the log contains the line
where the main thread opens the event script (`Script/Event.sdat`), which a run without input
does not reach in that time. The run is stopped before `world.pam` ends. It is registered under the
same conditions as `game_boot` with the `.sdat` cache (`DAL_EBOOT_ELF`, `DAL_GAME_DIR` and
`DAL_RPCS3_CRYPTO_DIR` set), uses its own hdd directory `build/<preset>/game_hdd_input`, which the
setup test `game_input_hdd_clean` deletes before every run, and takes its seconds and milestone from
`cmake/game_input.cmake`. See [Runtime internals](docs/runtime.md#input).

### Current state

On the pinned executable, with the `.sdat` cache, the run loads, initializes thread-local storage
and starts the game's helper threads. It initializes GCM and the fake RSX, completes the
game-data handshake, loads its assets (the decrypted `.sdat` files and the texture files its
decode threads open), reads the pad, registers the trophy context, runs the save-data load (a
first boot finds no data, which the game accepts), and drives its frame loop with the vblank
handler running at 60 Hz. After the warning screen it plays its three intro movies
(`if_logo.pam`, `ch_logo.pam`, `op.pam`, about 7 s, 7 s and 95 s) at their 29.97 fps rate, then
shows the title screen, where it stays without input (a 165 s run ends at the watchdog, exit code 5).
With the keyboard or a pad script it leaves the title screen: Circle skips the notice screen and the
movies and confirms NEW GAME, after which the game loads its event script and plays `world.pam`
(see [Keyboard and pad scripts](#keyboard-and-pad-scripts)). The movie
audio is decoded but not heard, because `cellAudio` is still default-handled. With
`--render vulkan` the only entry in the `rsx unimplemented` census is the constant ZPASS report
value. Without the `.sdat` cache the game stops earlier, at its error dialog for the missing file;
without the FFmpeg DLLs the movies run their full length as black frames.

### Not implemented

- Graphics beyond the first scenes: only what the game's title screens use is drawn. Whatever
  the renderer meets and cannot handle is listed in the `rsx unimplemented` census (see
  [Rendering](docs/rendering.md#rendering)). Known gaps are the ZPASS report values (the constant 0 is written, no occlusion
  query runs) and user clip planes (the vertex translator does not write the clip distances).
  Without `--render` nothing is drawn, and the renderer needs a Vulkan 1.3 device.
- Audio: `cellAudio` and the mixer are default-handled, so no sound is produced, including the
  movie audio that is already decoded. `cellAtrac` only reports its work memory size so the title
  screen can start; nothing is decoded through it.
- Movies: only the `cellSail` calls this game makes are implemented (PAMF files with H.264 video
  and ATRAC3plus audio). Repeat mode is stored but not acted on.
- Input: the `neutral`, `none` and `keyboard` pad providers; no gamepad backend. The keyboard has
  a fixed mapping, no analog sticks, no vibration, and only port 0; there is no mouse or `cellKb`
  input.
- SPU and SPURS: SPU system calls only keep bookkeeping, no SPU code runs, SPURS workloads are
  not scheduled.
- The remaining imports (25 of 166, from `cellAudio` and `cellAtrac`) use the default handler;
  see the census of a run.
- SDAT decryption needs the keys from a local RPCS3 checkout at run time, so it only works on a
  machine that has one.
- Priority scheduling, lv2 interrupt threads, savestates.

## Licence and legal notice

The code in this repository is licensed under 0BSD; see `LICENSE`. Third-party components keep
their own licences; see `THIRD_PARTY_NOTICES.md`.

This is an unofficial fan project. It is not affiliated with, authorised by or endorsed by any
rights holder of the game or of the *デート・ア・ライブ* franchise, including:

- COMPILE HEART, the publisher, and STING, the developer, of *デート・ア・ライブ 凜祢ユートピア*
- 橘公司 and つなこ, the author and illustrator of the original *デート・ア・ライブ* light
  novels, and their publisher 富士見書房 (KADOKAWA)
- Sony Interactive Entertainment, for the PlayStation 3 platform

*デート・ア・ライブ*, *デート・ア・ライブ 凜祢ユートピア*, PlayStation and all related names and logos are
trademarks or registered trademarks of their respective owners. They are used here only to
identify the game and platform this project works with.

No game files, keys or other copyrighted game data are included in this repository, and none
will be provided. Please do not ask for them.
