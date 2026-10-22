# Tests

## CTest targets

`tools/build.ps1 -Test` (Windows) and `tools/build.sh --test` (macOS) run every registered CTest
target; `ctest --preset <preset>` does the same after a build. Targets that need the game or local
files are registered only when those are configured, and configuration prints why one is not. Targets
marked Windows only or macOS only exist on that system only; the others are registered everywhere.
Targets that need a device or a window exit with code 77, which CTest reports as skipped, when the
machine has none.

| Target | What it checks | Needs |
| --- | --- | --- |
| `runtime_version`, `runtime_musttail` | The runtime stub starts, and the compiler supports `musttail` | - |
| `tools_unittest` | The Python unit tests (below). CTest passes `DAL_EBOOT_ELF` to them, so they include the real-executable tests when `-EbootElf` or `-EbootBin` is given | - |
| `ppu_header_check`, `ppu_semantics` | Contract header and the semantics harness (see [Emitting C](emitter.md)). The header check includes hand-written wrappers in the shape `emit` generates for a hook and an override, with `dalCountCall` and an empty table. The harness has the override and hook cases (see [Semantics harness](emitter.md#semantics-harness)): every way into an overridden function reaches its wrapper, an override can run the original, a hook runs first and can rewrite an argument, a hook does not grow the host stack in a tail-call cycle, and the call counters are exact | - |
| `ppu_link` | The generated code links with the stub runtime. It also checks the generated override table: addresses strictly ascending, dispatch reaches the wrapper and not the original body, and every entry has a counter and a handler name. Its ok line gives the number of overrides | `-GeneratedDir` |
| `runtime_core` | Runtime core unit tests (memory, threads, lv2, HLE, VFS, fake RSX). `testCellPadIntercept` covers the pad interception of system dialogs: an intercepted `cellPadGetData` reports no change and leaves the game's data alone, `cellPadGetInfo2` sets the intercepted bit, `pollHostPad` reads the provider without touching what the game was told, and the release phase after a dialog reports one neutral change and then waits until the dialog buttons are up. `testCellSaveDataDialog` runs the list functions of `cellSaveData` with a scripted dialog driver (the new-data row first or last, the row a list opens on, the row the Auto policy takes, the confirmation texts, No then Yes, a confirmation that cannot be shown ending the call with `CELL_SAVEDATA_ERROR_INTERNAL`, back ending it with `CELL_CANCEL`) and the error messages of the callbacks (one OK message per error, the headless and the `error dialog:` log lines); `testDecodePngRgba8` covers the PNG to RGBA8 helper behind the list icons (colour types, bit depths, interlacing, truncated data and the side limit). `testOverrides` covers the start-up check of the override table, its log lines, the `census: overrides` section and the four generic handlers. On a host whose page is larger than the 4 KiB logical page (Apple Silicon: 16 KiB) `testLogicalPagesSharingAHostPage` checks that a host page stays committed while one of its logical pages is, and that a logical page committed in a kept host page is zero-filled | - |
| `game_options` | The `dal_game` option parser (`runtime/src/app`), with no game files: derived values, every validation message, last-wins and CLI11 errors, `--name=value` and `--flag=false`, UTF-8 paths, the configuration file reader (every kind, paths, every error text, unknown keys), the lookup (working directory, executable directory, `--config`, `--no-config`), precedence, error notes and the `dal: config:` log line, and that `config/dal_game.example.json` parses without errors or warnings. `testSaveDialogOptions` covers `--save-dialog` and `--ui-font`: the default of the dialog mode from the window, the pad and a pad script, explicit values, the usage errors of `window` and of a bad value (on the command line and in the configuration file), and a relative `ui_font` in the file. The tests run in scratch working directories, so a `config.json` of yours does not affect them | - |
| `save_dialog` | The save data dialog without a window or a game (see [Save data dialog](runtime.md#save-data-dialog)). The model runs on an injected clock, so nothing sleeps: input edges, d-pad and stick auto-repeat, L1/R1 jumps, the circle/cross assignment, the fade-in and fade-out, scrolling, clamping and the results of a list, a Yes/No and an OK dialog. The text: sizes, dates, confirmation and error texts. The Auto and Cancel drivers (the Auto driver must log nothing), the board and the driver registry. The interactive driver runs on a host thread that holds the guest lock, against a keyboard pad provider: a list answered by pad input, a message answered by pad input, a dialog with no drawer or whose drawer fails during the wait answering as cancelled, a refused `DRAWING_BEGIN`, `cellMsgDialogOpen2` answering BUSY while a dialog is open, and a confirmation, a message and a list while the game's own message dialog is open. Every wait has a deadline, so a broken driver fails the test instead of hanging it | - |
| `sdat_tests` | SDAT decryption and decompression on synthetic data | - |
| `crypto_core` | The host crypto library `dal_crypto` (CNG on Windows, CommonCrypto on macOS) against published vectors: AES-128 and AES-256 (FIPS-197), CBC (SP 800-38A), AES-CMAC (RFC 4493), HMAC-SHA1 (RFC 2202), and SHA-256 of `abc`, of the empty input and of 1 MiB of zeros; the AES-CMAC and SHA-256 inputs include ones longer than an internal chunk of a provider | - |
| `decrypt_core` | `dal_decrypt` on synthetic data: crypto known answers, key file parsing, SCE containers, SELF reassembly and the command line | - |
| `decrypt_eboot` | `dal_decrypt self` on the real `EBOOT.BIN`; the output must match the pin of `EBOOT.elf` (the file is written under `build/<preset>/decrypt_test`) | `-EbootBin` |
| `render_core`, `vp_translator`, `fp_translator` | Renderer and shader translators (see [Rendering](rendering.md)). `render_core` also checks the idle present rule (`idlePresentDue`: nothing within 100 ms of the last real flip, then every 33 ms, and a new flip restarts the wait) and that `presentIdle` does nothing without a presenter or an overlay and never counts as a flip (see [Overlays](rendering.md#overlays)) | a Vulkan 1.3 device for the rendering parts |
| `render_window` | macOS only. Opens a real Cocoa window with the validation layer on, presents three frames to it and checks the window handle and that the layer reports no error (see [Rendering](rendering.md#vulkan-on-macos)). It runs in `main`, because the window belongs to the main thread. Exit code 77 (skipped) when there is no window-server session (`no GUI session`). Runs serially, because it opens a window | a logged-in session and a Vulkan 1.3 device |
| `rsx_replay` | Replays a local capture and compares it with a reference image | a local capture (see [Capture and replay](rendering.md#capture-and-replay)) |
| `media_core` | PAMF parser and demuxer, the movie player and the cellSail HLE on synthetic data, with fake codecs; with `DAL_GAME_DIR` set it also demuxes every `.pam` file of the game | - |
| `media_ffmpeg` | The FFmpeg loader, the H.264 and ATRAC3plus decoders (the movie decoder and the raw-frame decoder behind `cellAtrac`) and the colour and PCM conversion; with `DAL_GAME_DIR` set it also plays `ch_logo.pam` through the movie player with a fast clock | the fetched FFmpeg DLLs (Windows), Homebrew's FFmpeg (macOS) |
| `movie_decode` | `dal_moviedump` on the first 60 frames of `ch_logo.pam`: frame count, YUV and RGB hashes of three frames and the hash of the whole decoded audio, pinned in `cmake/movie_decode.cmake` (the audio hash has a separate value for macOS) | `-GameDir` or `--game-dir` (see [Movies](movies.md#tests)) |
| `audio_core` | Audio on synthetic data, with no device and no FFmpeg: the block ring, null output, WAV writer and reader, `wavStats`, `BlockClock` and its correction, the clock controller against a fake device, the engine with fake producers and outputs, the `cellAudio` state and handlers on synthetic guest memory, the AT3 parser, and the `cellAtrac` stream model and handlers with a fake decoder | - |
| `audio_wasapi` | Windows only. The WASAPI output: device loss and reopen, priming, high-water drops and the fill level on fake streams, then (when a render device exists) opening the default device, playing silence and a real device loss and reopen. Exit code 77 (skipped) when the machine has no render device. Runs serially, because it measures timing | a render device for the real-device part |
| `audio_coreaudio` | macOS only. The CoreAudio output and the device-thread code it shares with WASAPI (`dal_audio_stream`): a failing open is reported, priming, high-water drops, device loss and reopen on fake streams, then (when a default output device exists) opening the real one, playing a second of silence and checking the fill level, the frames played, the underrun and that no device error occurred. Exit code 77 (skipped) when everything else passed but the machine has no output device. Runs serially, because it measures timing | an output device for the real-device part |
| `overlay_core` | The debug overlay (see [Debug overlay](runtime.md#debug-overlay)): the flip statistics (flips per second, the frame-time ring, draw and skip deltas), the refresh of the providers (audio, `rsx unimplemented`, fast-forward state, sound switch, log level, capture status) into the state, the panels drawn headless on a private ImGui context with full and empty sources, and the runtime providers (all set; fast-forward toggle, hold and speed, sound switch, `rsx unimplemented` counts and log level round-trip; no audio engine or RSX thread runs, so no audio and no capture). With a Vulkan device it also checks the `F1` toggle and key gate and the `Tab` hold (a non-repeat down and up, withheld from the pad, released by `WM_KILLFOCUS`), then records the overlay over an offscreen image twice (title bar lit, far corner untouched), a second overlay on the same device, and, on Windows only, feeds a Win32 mouse move through a hidden window; it passes when the validation layer reports no error. For the save data dialog (see [Overlays](rendering.md#overlays)) it covers, without a device, the overlay stack (every child sees every flip, a request from any child survives the merge, `record` runs only the children that want to draw, `wantsDraw` and `wantsIdlePresent` of the children), the shared ImGui lock, the font candidate order and validation, and the code points and glyph ranges an atlas needs for a dialog; with a device it also checks that a debug overlay keeps another user's current ImGui context and that destroying one inside a scope leaves no freed context current, then records the save dialog overlay over an offscreen image and reads the pixels back (a message, the list with its legend and scrollbar, atlas rebuilds only for new glyphs, fades, the icon textures with their cap of 32 and their release, and the drawer state on the board). Without a device it prints `overlay tests: vulkan: skipped (<reason>)` and still passes | a Vulkan 1.3 device for the key and recording parts |
| `input_core` | Gamepad code behind the backend interface, with a fake backend and no GameInput or GameController: value, reading and device formatting; the `--pad-probe` logic (a line per change, one `no gamepad reading` per device, devices coming and going, the `SteamVirtualGamepadInfo` lines); the button table, dead zone, stick bytes, trigger threshold, merging of pads and motor conversion; Steam virtual path parsing, `SDL_GAMECONTROLLER_IGNORE_DEVICES` parsing and the selection rule (recorded runs, a virtual pad, the ignore list, no Steam); the hub (use of a pad, removal, two pads, a virtual pad and its physical pad, the focus gate, vibration, the provider following plugging) and the gamepad thread with a fake backend (unavailable backend, feeding the pad, start lines, probe alone); and the table that turns macOS key codes into PC scan codes (`testMacKeyCodes`, run on every platform) | - |
| `input_gameinput` | Windows only. The GameInput backend: the Steam context, then (with a GameInput runtime of API version 3) the device list, unique non-zero ids, a stable device generation, and `read`, `readRaw` and `setRumble` on unknown ids. No pad has to be connected. Exit code 77 (skipped) with the reason when no such runtime is installed | a GameInput runtime for all but the Steam context part (see [Input](input.md#the-gameinput-runtime-and-the-gameinput-unavailable-warning)) |
| `input_gamecontroller` | macOS only. The GameController.framework backend: the Steam context (environment variables only) and the behaviour for unknown device ids need no controller. The rest needs one: the program waits up to 3 s for the framework to report a controller, then checks the device list (unique non-zero ids, a gamepad among them), that readings are in range and exist exactly for devices with the gamepad kind, and that the device generation stays stable. For each controller with haptics it also drives the low-frequency and the high-frequency motor for 0.7 s each, so that a person holding it can feel them. It exits with code 77 (skipped) if everything else passed but no controller arrived | a connected controller for the device part |
| `atrac_decode` | `dal_audiotool at3` on the title BGM (entry 19 of the decrypted `Bgm.sdat`): frame count, sample count and the SHA-256 of the decoded PCM, pinned in `cmake/atrac_decode.cmake` (the PCM hash has a separate value for macOS) | the sdata cache (registered when it exists or can be built), the FFmpeg libraries |
| `game_boot` | The game reaches a milestone in a 20 s run (below) | `-GeneratedDir`, `-GameDir` and `-EbootElf` or `-EbootBin` |
| `game_input_hdd_clean`, `game_input` | A pad script leaves the title screen and the game reaches a milestone in a 40 s run (below) | as `game_boot`, plus the `.sdat` keys ([SDAT files](sdat.md#keys)) |
| `game_audio_hdd_clean`, `game_audio` | The same run as `game_input` with `--audio none --audio-dump`: the `atrac` line of the audio census reports decoded frames with audible content (below) | as `game_input` |

The game-file parts of `media_core` and `media_ffmpeg` read the `DAL_GAME_DIR` environment
variable; `tools/build.ps1` and `tools/build.sh` do not set it, so export it before running CTest to
include them. The pins of `movie_decode` hold for the pinned FFmpeg build; another build may round
the RGB conversion or the audio decoder differently, which needs new pins (the YUV hashes stay
valid).

The PCM hashes of `movie_decode` and `atrac_decode` for macOS are separate pins, taken with
Homebrew's FFmpeg on Apple Silicon, whose float code rounds differently from the Windows build
(the frame and sample counts and the YUV and RGB hashes are shared). They were identical over repeated runs, but
another Homebrew FFmpeg version or CPU may need new ones, in `cmake/movie_decode.cmake` and
`cmake/atrac_decode.cmake`.

The font loading check of `overlay_core` uses `fonts/BIZUDPGothic-Regular.ttf` next to the test executable, which the
`dal_ui_fonts` target copies (see [Fallback UI font](building.md#fallback-ui-font)); without it the check prints
`overlay tests: ui font: BIZ UDPGothic check skipped (<path> not found)` and goes on. A second check, that a Latin-only
font is refused, runs on Windows only (`arial.ttf` from the Windows fonts directory). The dialog is drawn on an
offscreen image, not on a swapchain, so the checks read pixels back from it. To look at the list layout, set the
environment variable `DAL_OVERLAY_TEST_LIST_PNG` to a file path before running `overlay_core`; the test then writes its
render of a seven-row list there as a PNG and prints `overlay tests: list render saved to <path>`. Nothing else
produces a picture of the dialog, because the overlays are not part of `--dump-frames` files or captures (see
[Overlays](rendering.md#overlays)).

## The `game_boot` test

CTest registers `game_boot` only when `DAL_PPU_GENERATED_DIR`, `DAL_EBOOT_ELF` and
`DAL_GAME_DIR` are all set (`-GeneratedDir`, `-EbootElf`, `-GameDir`); otherwise configuration prints
that it is not registered. `-EbootBin` also sets `DAL_EBOOT_ELF`, to the produced
`generated/inputs/EBOOT.elf`, unless `-EbootElf` is given. It runs `dal_game --game-dir <dir> --elf <elf>` with the arguments
from the pin file (the run keeps its game data in `build/<preset>/game_hdd`), with a 60 s time
limit, and passes when the output matches the milestone regular expression. The exit code is not
checked: the run ends at the 20 s watchdog or at a stop somewhere after the milestone, and a
stop before the milestone leaves the pattern unmatched.

The pin file `cmake/game_boot.cmake` holds the arguments and two milestones:

```cmake
set(DAL_GAME_BOOT_ARGS "--no-config --no-window --seconds 20 --hdd-dir \"${CMAKE_BINARY_DIR}/game_hdd\"")
set(DAL_GAME_BOOT_MILESTONE "\\[t 01000000\\] dal: first hle sceNpTrophy::sceNpTrophyGetTrophyUnlockState")
set(DAL_GAME_BOOT_MILESTONE_NO_SDATA "\\[t 01000000\\] dal: first hle cellSysutil::cellMsgDialogOpen2")
```

A milestone is a `dal: first ...` line written by the main thread (thread id `01000000`), because
a line from another thread can appear or not depending on scheduling. The first one applies when
the build has the `.sdat` keys (the test also passes `--sdata-cache`), the second when it has not and
the game stops at its error dialog for the missing `.sdat` file. When a change lets the game get
further, run `dal_game --no-config --game-dir <dir> --no-window --seconds 20 --hdd-dir <empty-dir>` three times, take the
last main-thread `dal: first ...` line that appears in every run, and set the matching milestone.
Update only that file; the test then guards against regressions.

All three game tests (`game_boot`, `game_input`, `game_audio`) pass `--no-config`. CTest runs them in
`build/<preset>/`, so without it a `config.json` there, or one next to `dal_game.exe` (`dal_game` on macOS), would
change their options, and a broken one would fail them with exit code 2. (The `config.json` that
`tools/build.ps1 -GameDir` or `tools/build.sh --game-dir` writes sits in `build/<preset>/bin/`.) The runs are
headless, so the save data dialog is in the Auto policy: the `dal: savedata:` and `dal: msgdialog:` lines are those the
game logged before the dialog existed, and the one new start-up line is `dal: savedata dialog: auto`.

## The `game_input` test

CTest registers `game_input` under the same conditions as `game_boot` when the build also has the
`.sdat` keys, because the game needs its decrypted `.sdat` files to get past the title screen. It
runs `dal_game` with `--sdata-cache`, `--pad-script tools/pad_scripts/leave_title.txt` and its own
hdd directory `build/<preset>/game_hdd_input`, which the setup test `game_input_hdd_clean` deletes
before every run, so that save data written after NEW GAME never reaches `game_boot` or the next
run. The pin file `cmake/game_input.cmake` holds the run length (`DAL_GAME_INPUT_SECONDS`, 40) and
the milestone (`DAL_GAME_INPUT_MILESTONE`): the main thread opening `Script/Event.sdat`, which a
run without input does not reach in that time. The time limit is the run length plus 60 s. The
script and the pad log lines are described in
[Pad script](runtime.md#pad-script) and [Log lines](runtime.md#log-lines). The runs are headless
(`--no-window`), so they never load a gamepad backend (GameInput or GameController) and their logs
have no `dal: pad: gameinput` line.

## The `game_audio` test

CTest registers `game_audio` under the same conditions as `game_input`. It runs the same pad script
for 40 s (`DAL_GAME_AUDIO_SECONDS` in `cmake/game_audio.cmake`) with `--audio none` and
`--audio-dump build/<preset>/game_audio.wav`, and its own hdd directory `build/<preset>/game_hdd_audio`,
which the setup test `game_audio_hdd_clean` deletes before every run. The time limit is 100 s. It
passes when the stop census has an `atrac` line with at least one non-silent decoded frame
(`DAL_GAME_AUDIO_ATRAC_PATTERN`): the title music and the confirm sound effect are both
ATRAC3plus, so it fails if the FFmpeg libraries are missing, if the `cellAudio` mixer is not driven, or if
`cellAtrac` stops decoding. The WAV file is not inspected by the test; `dal_audiotool wavstat` reads
it (see [Audio](audio.md#dal_audiotool)).

`atrac_decode` takes the archive, the entry index and the expected counts from
`cmake/atrac_decode.cmake`. The hash of the decoded PCM holds for the pinned FFmpeg build; another
build or CPU feature set may round the float output differently and needs a new hash (the counts
stay).

Other audio code is covered by `runtime_core` (`testEventQueueIpcKeys`,
`testPpuThreadCreatePriority`, `testTimerResolution`) and `media_ffmpeg` (`testAtracFrameDecoder`,
`testAtracResetWithGameFile`, which is skipped unless `Se.sdat` is in the sdata cache of the build directory).

## Python unit tests

Python unit tests use synthetic ELF files built in memory. Tests that need the real
executable are skipped unless `DAL_EBOOT_ELF` points at it. The overrides file has its own tests
(`test_emit_overrides.py`): every validation message and exit code, and the rendered texts; the
`emit` command with `--overrides` and `--no-overrides` on a synthetic ELF is in
`test_emit_output.py`. The Ghidra comparison tests
additionally need `DAL_GHIDRA_JSON`, the path of an export written by `ghidra-export --cell`, and
are skipped without it. The tests never run Ghidra themselves, apart from the opt-in live tests below. The tests of `recover` and
`emit` that use the real executable also need `DAL_EBOOT_ELF`; the `emit` tests assert the
counts above and that no word is unhandled.

The tests of the Cell PPU Ghidra language (see [Ghidra language for the Cell PPU](ghidra-cell-language.md))
run without Ghidra, on fake Ghidra trees in temporary directories:

- `test_ghidra_cell.py`: the module sources in `tools/ghidra/CellPPU/` against the decoder
  (encodings and the language definition), `ghidra-cell-lang build` with a fake SLEIGH compiler,
  the settings-directory candidates and the external-module environment used by `--staged`.
- `test_ghidra_cell_verify.py`: the request, the result evaluation and the exit codes of
  `ghidra-cell-lang verify` with a fake headless run.
- `test_install_ghidra_cell.py`: `tools/install-ghidra-cell.ps1` against fake trees. It needs `pwsh`
  (PowerShell 7) on `PATH` and is skipped without it.
- `test_install_ghidra_cell_sh.py`: `tools/install-ghidra-cell.sh` against the same kind of fake
  trees, with the same checks and isolation of `HOME`, `XDG_CONFIG_HOME` and `GHIDRA_INSTALL_DIR`
  (`--yes` and answers on standard input replace `-Confirm:$false`).

The POSIX shell scripts have tests of their own, run in a temporary directory with fake trees and
no network: `test_write_game_config_sh.py` (`tools/write-game-config.sh`), `test_fetch_ffmpeg_sh.py`
(`tools/fetch-ffmpeg.sh`: usage errors, a zip with the wrong hash, an up-to-date marker, a zip with
an unsafe entry; `DAL_FETCH_FFMPEG_ROOT` points the script at a copy of the pin file, for the tests
only) and `test_install_ghidra_cell_sh.py` above. They are skipped unless the system is POSIX with an
`sh`. Run them also with `PATH=/usr/bin:/bin`, to see that the scripts use only the system tools
and not GNU replacements that Homebrew may put first.

One live test in each of `test_ghidra_cell.py` and `test_ghidra_cell_verify.py` runs the real
`ghidra-cell-lang build` and `verify` (staged module, then the stock baseline). They run only with
`DAL_GHIDRA_LIVE=1` and `GHIDRA_INSTALL_DIR` set to a Ghidra 12.x install, and are skipped otherwise.
They write only to temporary directories.

```sh
python -m unittest discover -s tools/tests -t tools -v
DAL_EBOOT_ELF=game/EBOOT.elf python -m unittest discover -s tools/tests -t tools -v
DAL_EBOOT_ELF=game/EBOOT.elf DAL_GHIDRA_JSON=generated/ghidra/eboot_ghidra_cell.json python -m unittest discover -s tools/tests -t tools -v
```

On macOS use `python3` (3.10 or newer) in these commands.
