# Date A Live: Rinne Utopia / recompiled

Static recompilation of the PS3 title *デート・ア・ライブ 凜祢ユートピア* (BLJM61048) for
Windows and for macOS on Apple Silicon, built from your own copy of the game.

This repository contains no game data (see [Licence and legal notice](#licence-and-legal-notice)).
No executables, keys, or decrypted binaries are included or accepted by version control. You
supply your own `EBOOT.BIN`; the build decrypts it into the ignored `generated/inputs/`, and the keys
it needs are downloaded at configure time, not stored (see [Building](#building)). All tool output is
written to ignored directories.

Current state: the game boots, plays its intro movies with sound and reaches the title screen.
NEW GAME plays the opening movie and then the first story scenes, with their pictures, music and
the message window (speaker name and dialogue text). It can be played with the keyboard, an
Xbox-family gamepad or a DualShock 4. How far beyond these first scenes it works has not been
checked. See [Status](docs/status.md).

The macOS build runs the same code. It shows the game in a Cocoa window, draws with Vulkan on top of
Metal, plays sound through CoreAudio and reads gamepads with GameController.framework. Sections
that differ by system are marked Windows or macOS; the others apply to both.

## Prerequisites

### Windows

- Visual Studio 2026 with the Windows SDK (provides the MSVC headers and libraries)
- PowerShell 7 and the `VSSetup` module (`Install-Module VSSetup -Scope CurrentUser`)
- clang and clang++ with lld (a standalone LLVM install, on `PATH`)
- CMake 3.29 or newer
- Ninja
- git, with the submodules initialised: `git submodule update --init` (clone with
  `--recurse-submodules` to do it in one step)
- The Vulkan SDK, with the `VULKAN_SDK` environment variable set
- A Vulkan 1.3 device and driver
- Python 3.10 or newer (the tools use only the standard library)
- Internet access for the first build that decrypts the game's files: two RPCS3 source files that
  contain the keys are downloaded (see [Building](docs/building.md#keys) for an offline
  alternative)

### macOS

- A Mac with Apple Silicon (arm64) and the Xcode Command Line Tools (`xcode-select --install`).
  Intel Macs are not supported.
- [Homebrew](https://brew.sh), with these packages: `brew install llvm cmake ninja python ffmpeg`.
  The build uses Homebrew's clang (found with `brew --prefix llvm`); Apple's clang is refused when
  CMake configures. Another LLVM install can be named with `--llvm-prefix` (see
  [Building](docs/building.md#toolsbuildsh)). CMake 3.29 or newer is needed.
- Python 3.10 or newer as `python3`. The `python3` that macOS provides (`/usr/bin/python3`) can be
  3.9; Homebrew's `python` is newer. Check `python3 --version` before running `ps3tool.py`.
- The LunarG Vulkan SDK for macOS, installed with its "System Global Installation" component. It
  provides the Vulkan loader, the shader compiler library and the two Metal drivers (KosmicKrisp and
  MoltenVK) under `/usr/local`. If `VULKAN_SDK` is exported (the SDK's `macOS` directory), configure
  takes the shader compiler from there; otherwise it uses the copy in `/usr/local`.
- git, with the submodules initialised: `git submodule update --init` (clone with
  `--recurse-submodules` to do it in one step)
- Internet access for the first build that decrypts the game's files, as on Windows.
- No FFmpeg download and no GameInput runtime: FFmpeg comes from Homebrew and gamepads need nothing
  to install.

## Game files

Dump your own disc or digital copy. The directory that contains `PS3_GAME` is the game directory
(`<game-dir>` below).

The tools work on the decrypted PPU executable, `EBOOT.elf`, not on `EBOOT.BIN`. The build produces
it from `<game-dir>/PS3_GAME/USRDIR/EBOOT.BIN` (step 3 below, or step 1 of the macOS build);
RPCS3 is not needed. The supported
executable has the SHA-256 `8b4362daac30cad2e597fdd30e6540a3226da88760dbded4b7c14890f8a88348`;
the build and `dal_game` refuse any other.

## Building

### Windows

Run these from the repository root:

1. Fetch FFmpeg, which decodes the movies and the game's sound, and the GameInput package, which
   provides the gamepad API. This is needed once.

   ```powershell
   pwsh -NoProfile -File tools/fetch-ffmpeg.ps1
   pwsh -NoProfile -File tools/fetch-gameinput.ps1
   ```

2. Install the GameInput runtime, which reads the gamepads. This is needed once and requires
   administrator rights; without it gamepads are disabled and the keyboard still works.
   `<version>` is the directory under `generated/deps/gameinput`.

   ```powershell
   msiexec /i generated\deps\gameinput\<version>\redist\GameInputRedist.msi
   ```

3. Decrypt the executable. This configures, downloads the key files, builds `dal_decrypt` and runs
   it:

   ```powershell
   pwsh -NoProfile -File tools/build.ps1 -Preset release -EbootBin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN
   ```

   The output is `generated/inputs/EBOOT.elf`. It is checked against the size and SHA-256 in
   `config/target.json`; a file that does not match is refused and nothing is written. The build
   log shows `matches the pin`.

4. Recompile the executable into C source under `generated/ppu`:

   ```powershell
   python tools/ps3tool.py emit generated/inputs/EBOOT.elf --out generated/ppu
   ```

5. Build:

   ```powershell
   pwsh -NoProfile -File tools/build.ps1 -Preset release -GeneratedDir generated/ppu -GameDir <game-dir> -EbootBin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN
   ```

The build writes `dal_game` to `build/release/bin/` and decrypts the game's `.sdat` files into
`build/release/sdata_cache`. The keys for both decryptions are read from RPCS3's `key_vault.h` and
`key_vault.cpp`, which the first build downloads from a pinned RPCS3 commit (see
[Building](docs/building.md#decrypting-ebootbin) and [SDAT files](docs/sdat.md)).

Every build parameter, the CMake variables and the test builds are described in
[Building](docs/building.md).

### macOS

Run these from the repository root. `tools/build.sh` is the counterpart of `tools/build.ps1`; its
preset `macos-release` is the optimised build (`macos-debug`, the default, is slower; use
`macos-release` to play).

1. Decrypt the executable. This configures, downloads the key files, builds `dal_decrypt` and runs
   it:

   ```sh
   tools/build.sh --preset macos-release --eboot-bin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN
   ```

   The output is `generated/inputs/EBOOT.elf`, checked against `config/target.json` as on Windows.
   The first line of the output is the version of the clang that is used, which must be Homebrew's.

2. Recompile the executable into C source under `generated/ppu`. Use `python3`, and make sure it is
   3.10 or newer:

   ```sh
   python3 tools/ps3tool.py emit generated/inputs/EBOOT.elf --out generated/ppu
   ```

3. Build:

   ```sh
   tools/build.sh --preset macos-release --generated-dir generated/ppu --game-dir <game-dir> --eboot-bin <game-dir>/PS3_GAME/USRDIR/EBOOT.BIN
   ```

The build writes `dal_game` to `build/macos-release/bin/` and decrypts the game's `.sdat` files into
`build/macos-release/sdata_cache`. FFmpeg is not copied: `dal_game` loads Homebrew's libraries from
where they are installed (see [Movies](docs/movies.md#ffmpeg-on-macos)). Presets, the options of
`tools/build.sh`, the Vulkan drivers and debugging are in [Building](docs/building.md#building-on-macos).

## Running the game

```powershell
build/release/bin/dal_game.exe --game-dir <game-dir> --elf generated/inputs/EBOOT.elf
```

`--elf` names the decrypted executable. Without it `dal_game` looks for
`<game-dir>/PS3_GAME/USRDIR/EBOOT.elf`; copying the produced file there works too.

Closing the window ends the run. Game data and saves are written to `build/release/hdd`;
`tools/build.ps1 -Clean` deletes them together with the rest of `build/release`.

The build with `-GameDir` also writes `build/release/bin/config.json` (with `game_dir`, and `elf` if
you gave `-EbootBin` or `-EbootElf` in this or an earlier build of the preset) when that file does not exist yet. `dal_game.exe` reads a `config.json` from
the working directory or, when there is none, from its own folder. So `dal_game.exe` needs no
options when you start it from `build/release/bin` (for example by double-click, or from a shortcut
whose "Start in" is that folder). To use a file elsewhere, pass `--config <file>`; `--no-config`
reads none. The command line wins over the file. The file format is described in
[`dal_game` options](docs/runtime-options.md#configuration-file), and `config/dal_game.example.json`
is an example.

Every option of `dal_game` is described in [`dal_game` options](docs/runtime-options.md).

### Running on macOS

```sh
build/macos-release/bin/dal_game --game-dir <game-dir> --elf generated/inputs/EBOOT.elf
```

The options, `--elf`, `config.json` and the `hdd` directory work as on Windows, with the executable
called `dal_game` (no `.exe`): `dal_game` reads a `config.json` from the working directory or, when
there is none, from the folder of the executable, and `tools/build.sh --game-dir` writes one to
`build/macos-release/bin/` under the same rules as `tools/build.ps1 -GameDir`. Closing the window
and `Cmd+Q` both end the run. Start the game from a session where you are logged in: the window
needs the window server, so a run over SSH works only with `--no-window`.

### Keyboard

The keyboard is the pad on port 0. The keys are physical key positions, so the layout stays in
place on AZERTY and QWERTZ keyboards:

| Keys | Pad |
| --- | --- |
| Arrow keys (not the numpad) | D-pad |
| `Z` / `X` / `S` / `D` | Cross / Circle / Square / Triangle |
| `W` / `E` / `R` / `U` | L1 / L2 / R1 / R2 |
| `1` / `3` (main row) | L3 / R3 |
| `V` / `C` | Start / Select |

The game confirms with Circle (the `X` key), which also skips the notice screen and the intro
movies. If keys do nothing, click the window to give it keyboard focus.

On macOS the same physical key positions are used. Key presses that include `Cmd` are system
shortcuts, such as `Cmd+Q`, and are never pad input. Keys that macOS reports without a PC scan code (`Fn`, media keys, the Japanese `Eisu`
and `Kana` keys) do nothing.

Two keys are not pad buttons: `F1` shows or hides the debug overlay, and holding `Tab` runs the game
faster (see [Debug overlay](docs/runtime.md#debug-overlay)).

### Gamepad

A connected Xbox-family gamepad or DualShock 4 (second model) works in the window together with the
keyboard; it needs the GameInput runtime from the build step above. Quit Steam, or turn off its
PlayStation controller support, before using a DualShock 4. The button mapping, the dead zone, Steam
and the `gameinput unavailable` warning are in [Input](docs/input.md).

On macOS gamepads are read through GameController.framework, which needs nothing to install: a
controller that macOS reports with an extended gamepad profile works in the window together with the
keyboard, with the same button mapping, and vibrates through Core Haptics when it supports that.
Input reaches the game only while its window is the active one. The Steam and
GameInput notes above apply to Windows only. See [Input](docs/input.md#gamepads-on-macos).

## Documentation

Development material is under [`docs/`](docs/README.md): build and run parameters, tests, the
analysis tools, function recovery, the C emitter, runtime internals, the virtual file system,
rendering, movies, audio, gamepad input, the repository layout, and a guide for applying the method
to other PS3 games.

## Licence and legal notice

The code in this repository is licensed under 0BSD; see [LICENSE](LICENSE). Third-party components keep
their own licences; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

This is an unofficial project. It is not affiliated with, authorised by or endorsed by any rights
holder of the game.

*デート・ア・ライブ*, *デート・ア・ライブ 凜祢ユートピア* and all related names and logos are
trademarks or registered trademarks of their respective owners. They are used here only to
identify the game and platform this project works with.

No game files, keys or other copyrighted game data are included in this repository, and none
will be provided. Please do not ask for them.
