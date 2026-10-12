# Date A Live: Rinne Utopia / recompiled

Static recompilation of the PS3 title *デート・ア・ライブ 凜祢ユートピア* (BLJM61048) for
Windows, built from your own copy of the game.

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

## Prerequisites

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

## Game files

Dump your own disc or digital copy. The directory that contains `PS3_GAME` is the game directory
(`<game-dir>` below).

The tools work on the decrypted PPU executable, `EBOOT.elf`, not on `EBOOT.BIN`. The build produces
it from `<game-dir>/PS3_GAME/USRDIR/EBOOT.BIN` (step 3 below); RPCS3 is not needed. The supported
executable has the SHA-256 `8b4362daac30cad2e597fdd30e6540a3226da88760dbded4b7c14890f8a88348`;
the build and `dal_game` refuse any other.

## Building

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

### Gamepad

A connected Xbox-family gamepad or DualShock 4 (second model) works in the window together with the
keyboard; it needs the GameInput runtime from the build step above. Quit Steam, or turn off its
PlayStation controller support, before using a DualShock 4. The button mapping, the dead zone, Steam
and the `gameinput unavailable` warning are in [Input](docs/input.md).

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
