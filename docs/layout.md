# Repository layout

## Layout

| Path | Contents |
| --- | --- |
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/` | Build configuration (clang, Ninja, `debug` and `release` presets) |
| `runtime/` | Runtime executable stub (`dal_runtime`), stub runtime for generated code (`dal_ppu_stub`), the runtime core and `dal_game` (`runtime/src/core`, `lv2`, `hle`, `input`, `rsx`, `media`, `audio`, `app`, `game_main.cpp`; see [Runtime internals](runtime.md) and [`dal_game` options](runtime-options.md)) and runtime-side tests |
| `runtime/src/app/` | `dal_game` option handling (`dal_game_options`): the option table, command-line parsing with CLI11, the `config.json` reader, validation and derived values, and the usage text; see [`dal_game` options](runtime-options.md) |
| `tools/dal_sdat/` | `dal_sdat`, the SDAT decryption tool, and its tests; see [SDAT files](sdat.md) |
| `tools/dal_decrypt/` | `dal_decrypt`: decrypts `EBOOT.BIN` into `EBOOT.elf`, with its synthetic unit tests; see [Decrypting `EBOOT.BIN`](building.md#decrypting-ebootbin) |
| `cmake/Decrypt.cmake`, `cmake/rpcs3_keys.cmake` | The `dal_decrypt` targets, the `dal_inputs` target that writes `generated/inputs` and the tests `decrypt_core` and `decrypt_eboot`; and the pin of the two RPCS3 key files that `dal_decrypt` and `dal_sdat` read, and their download |
| `externals/spdlog` | spdlog submodule used by the runtime logger |
| `externals/libpng`, `externals/zlib` | libpng (v1.6.59) and zlib (v1.3.2) submodules, the PNG codec behind `cellPngDec` |
| `externals/yyjson` | yyjson submodule (0.13.0, built as a static library), used to parse the VFS configuration and `config.json` |
| `externals/CLI11` | CLI11 submodule (2.7.2, header-only), used by `dal_game` option parsing |
| `externals/Vulkan-Headers`, `externals/volk`, `externals/VulkanMemoryAllocator` | Vulkan headers, the run-time loader and the memory allocator of the renderer |
| `externals/imgui` | Dear ImGui submodule (v1.91.9b, built as a static library from the core files and the Win32 and Vulkan backends, no demo window), used only by the debug overlay |
| `runtime/src/render/` | The renderer (`dal_render`): Vulkan context, render targets, textures, pipelines, window and swapchain, the hook that lets the overlay draw into the swapchain image, vertex program and fragment program translators, shader compiler and cache, PNG output; see [Rendering](rendering.md#rendering) |
| `runtime/src/media/` | Movie and ATRAC3plus support: the PAMF header parser and MPEG-PS demuxer, the host movie player, the AT3 file parser and the ATRAC frame-decoder interface (all in the runtime core), and the FFmpeg loader with the movie and ATRAC3plus decoders (`dal_media`); see [Movies](movies.md) and [Audio](audio.md#cellatrac) |
| `tools/dal_moviedump/` | `dal_moviedump`, which decodes a `.pam` file without the game; see [Movies](movies.md#dal_moviedump) |
| `tools/fetch-ffmpeg.ps1`, `cmake/ffmpeg.cmake`, `cmake/Media.cmake`, `cmake/movie_decode.cmake` | FFmpeg fetch script and pin, the media targets, and the pin file of the `movie_decode` test |
| `tools/dal_rsxreplay/` | `dal_rsxreplay`, which replays an RSX capture without the game; see [Capture and replay](rendering.md#capture-and-replay) |
| `cmake/Render.cmake`, `cmake/rsx_replay.cmake` | Renderer targets and tests, and the pin file of the `rsx_replay` test |
| `runtime/src/overlay/`, `cmake/Overlay.cmake` | The debug overlay (`dal_overlay`): the numbers it shows (no ImGui), the panels, the connections to the runtime and the ImGui backends that draw it over the presented image, plus the `imgui` and `dal_overlay_tests` targets and the `overlay_core` test; see [Debug overlay](runtime.md#debug-overlay) |
| `cmake/game_boot.cmake` | Pin file of the `game_boot` test: arguments and milestones |
| `runtime/src/input/` | Pad buttons, the host pad state and provider, the pad script player, and the gamepad code (backend interface, GameInput backend `dal_input_gameinput`, mapping, selection, hub, probe); see [Input](runtime.md#input) and [Gamepads](input.md) |
| `tools/dal_padtool/` | `dal_padtool`: GameInput devices, live readings and rumble; see [`dal_padtool`](input.md#dal_padtool) |
| `tools/fetch-gameinput.ps1`, `cmake/gameinput.cmake`, `cmake/Input.cmake` | GameInput package fetch script and pin, and the input targets and tests (`input_core`, `input_gameinput`) |
| `runtime/src/audio/` | Audio: the output interface, the null output, the audio engine with its block clock, WAV files (in the runtime core), and the WASAPI output (`dal_audio_wasapi`); see [Audio](audio.md) |
| `tools/dal_audiotool/` | `dal_audiotool`: WAV statistics, device list, tone and WAV playback, ATRAC3plus archive scan and decode; see [`dal_audiotool`](audio.md#dal_audiotool) |
| `cmake/Audio.cmake`, `cmake/atrac_decode.cmake`, `cmake/game_audio.cmake` | Audio targets and tests (`audio_core`, `audio_wasapi`), and the pin files of the `atrac_decode` and `game_audio` tests |
| `tools/pad_scripts/`, `cmake/game_input.cmake` | Pad scripts (`leave_title.txt` is used by the `game_input` test) and the pin file of that test; see [Pad script](runtime.md#pad-script) |
| `tools/build.ps1` | Windows build wrapper |
| `tools/write-game-config.ps1` | Writes `build/<preset>/bin/config.json` for `dal_game` after a build with `-GameDir`; see [`config.json` for `dal_game`](building.md#configjson-for-dal_game) |
| `tools/ps3tool.py` | Command-line entry point for the analysis tools |
| `tools/ps3recomp/` | ELF/PRX parsing, PPU decoder and formatter, probe, Ghidra export, diff and summary, function recovery (seeds, partition, jump tables, coverage, syscall resolution), name table generator |
| `tools/ghidra/` | Java `GhidraScript`s run by `ghidra-export`, and `CellLangCheck.java`, the script behind `ghidra-cell-lang verify` |
| `tools/ghidra/CellPPU/` | Sources of the `CellPPU` Ghidra language module; `ghidra-cell-lang build` stages and compiles them, see [Files](ghidra-cell-language.md#files) |
| `tools/install-ghidra-cell.ps1` | Optional installer of the built module into a Ghidra settings or install directory; see [The install script](ghidra-cell-language.md#the-install-script) |
| `tools/ps3recomp/emit*.py` | The `emit` command: function DB loader and census, per-instruction handlers by group, output writer and report |
| `tools/tests/` | Unit tests |
| `tools/tests/ppu_semantics/` | Semantics harness cases and the C generator; see [Semantics harness](emitter.md#semantics-harness) |
| `runtime/include/dal/` | Contract header and inline helpers for generated code, and `ppu_override.h`, the override and hook handler interface |
| `runtime/src/game/overrides/` | Game-specific override and hook handlers (`*.cpp`, compiled into `dal_game`); the directory is absent until a game needs one; see [Function overrides and hooks](overrides.md#game-handlers) |
| `config/target.json` | Pinned size and hashes of the target executable (`EBOOT.BIN` and `EBOOT.elf`) |
| `config/overrides.json` | Guest functions replaced or hooked by native handlers at emit time (no entries by default); see [Function overrides and hooks](overrides.md#configuration-file) |
| `config/vfs.example.json` | Example VFS configuration for `dal_game --vfs-config`, placeholder paths only; see [VFS configuration file](virtual-file-system.md#vfs-configuration-file) |
| `config/dal_game.example.json` | Example `config.json` for `dal_game`, placeholder paths only; see [Configuration file](runtime-options.md#configuration-file) |
| `config/nids.json`, `config/lv2_syscalls.json` | FNID and lv2 syscall name tables generated from RPCS3 sources |
| `config/ghidra_summary.json`, `config/extra_seeds.json`, `config/functions.json` | Ghidra summary, extra function starts and the recovered function database; see [Function recovery](function-recovery.md#function-recovery) |
| `docs/` | Development documentation |
| `LICENSE`, `THIRD_PARTY_NOTICES.md` | Licence of this project (0BSD) and notices of third-party components (FFmpeg, GameInput, CLI11, Dear ImGui) |
| `generated/` | Tool output, including `generated/ppu` from `emit` (git-ignored) |
| `generated/inputs/` | The decrypted `EBOOT.elf` written by the build with `-EbootBin` (git-ignored) |
| `generated/deps/rpcs3/` | The downloaded RPCS3 key files `key_vault.h` and `key_vault.cpp`, one directory per commit (git-ignored) |
| `build/` | Build output (git-ignored) |
| `game/` | Suggested place for your own `EBOOT.elf` (git-ignored) |
