# `dal_game` options

The [top-level README](../README.md) shows the command for playing. This page lists every option of
`dal_game`, the [configuration file](#configuration-file) that can set them, its exit codes, and the
options for unattended and diagnostic runs.

```sh
dal_game [--game-dir DIR] [--vfs-config FILE] [--elf PATH] [--hdd-dir DIR] [--sdata-cache DIR]
         [--seconds N] [--strict] [--log FILE] [--log-level LEVEL] [--quiet-hle] [--dialog-answer yes|no|ok]
         [--pad none|neutral|keyboard|gamepad|keyboard+gamepad] [--pad-script FILE] [--pad-probe]
         [--audio none|wasapi] [--audio-dump FILE]
         [--load-only] [--png-dump DIR]
         [--render none|vulkan] [--rsx-capture N[,M...]] [--rsx-census FILE]
         [--dump-frames DIR] [--dump-frames-every K]
         [--shader-cache DIR] [--fast-forward N] [--vk-validation] [--no-window]
         [--config FILE | --no-config] [-h|--help]
```

`--audio` takes `none` or the system's device output: `wasapi` on Windows, `coreaudio` on macOS. The
usage text of `dal_game` shows the one of the system it runs on; the block above shows the Windows
one.

A value is given as `--name value` or `--name=value`. Flags take no value; `--flag=false` switches a
flag off (see [precedence](#precedence)). Arguments are read as UTF-8, so paths with characters
outside the ANSI code page work. When an option is repeated, the last one wins, except
`--rsx-capture`, whose values add up.

`dal_game` loads the decrypted `EBOOT.elf` into a 4 GiB guest address space, sets up the main PPU
thread the way RPCS3 does and calls the entry function through the generated code.

## Options

### Game files

| Option | Meaning |
| --- | --- |
| `--game-dir DIR` | The game directory, the one that contains `PS3_GAME`. It must exist. It is mounted read-only as `/dev_bdvd` and `/app_home` (`DIR/PS3_GAME/USRDIR`), `PS3_GAME/PARAM.SFO` is read from it, and it holds the default ELF location. Required (on the command line or in the configuration file) unless `--vfs-config` mounts `/dev_bdvd` (the executable can also come from `--elf`). `DIR/PS3_GAME/USRDIR` need not exist. |
| `--elf PATH` | The decrypted executable. Default: `PS3_GAME/USRDIR/EBOOT.elf` on `/dev_bdvd` (`DIR/PS3_GAME/USRDIR/EBOOT.elf`, or where the configuration puts it). |
| `--hdd-dir DIR` | Host directory behind `/dev_hdd0` and `/dev_hdd1` (`DIR/dev_hdd0`, `DIR/dev_hdd1`; created on demand), where the game keeps its game data and saves. Default: `build/<preset>/hdd`, next to the `bin` directory of the executable. |
| `--sdata-cache DIR` | Directory of decrypted `.sdat` files, each at its path relative to the game directory, as written by `dal_sdat`. Default: `build/<preset>/sdata_cache` if it exists, which is where the build writes it. Without a cache `cellFsSdataOpen` fails. See [SDAT files](sdat.md). |
| `--vfs-config FILE` | A JSON file that describes the virtual file system. The options above override the matching parts of it. See [VFS configuration file](virtual-file-system.md#vfs-configuration-file). |

### Window, input and sound

| Option | Meaning |
| --- | --- |
| `--no-window` | Run without a window, for unattended runs and tests. By default `dal_game` shows the frames in a window (which implies `--render vulkan`), and closing it stops the run with exit code 0 (on macOS `Cmd+Q` does the same). An explicit `--render none` also turns the window off. Without the window no gamepad backend is loaded (GameInput on Windows, GameController.framework on macOS), so these runs need no gamepad or GameInput runtime. On macOS the process also never connects to the window server, so such a run works over SSH. |
| `--pad none\|neutral\|keyboard\|gamepad\|keyboard+gamepad` | Input source for port 0. `neutral` connects a standard pad that never changes, `none` connects no pad, `keyboard` makes the keyboard the pad, `gamepad` the connected gamepads, and `keyboard+gamepad` both at once. With `gamepad`, port 0 is connected only while a gamepad is in use; otherwise it is always connected. Default: `keyboard+gamepad` with the window; `keyboard` with `--no-window` and `--pad-script`; otherwise `neutral`. `keyboard` without the window or `--pad-script` is a usage error, and so are `gamepad` and `keyboard+gamepad` with `--no-window`: unattended runs never load a gamepad backend. See [Input](runtime.md#input) and [Gamepads](input.md#gamepads). |
| `--pad-script FILE` | Press pad buttons at given times, for runs without a person at the keyboard. The file is read and checked at start-up; an error is a usage error that names the line. It needs `--pad keyboard` or `keyboard+gamepad` (the default with the window) and cannot be combined with `none`, `neutral` or `gamepad`. See [Pad script](runtime.md#pad-script). |
| `--pad-probe` | Diagnostic for gamepads: logs every game controller that GameInput (Windows) or GameController.framework (macOS) reports, every change of their readings and what the process knows about Steam, as `dal: pad: probe ...` lines. Needs the window (a usage error with `--no-window`) and is independent of `--pad`; it never changes the pad. See [`--pad-probe`](input.md#--pad-probe). |
| `--audio none\|wasapi` (Windows), `--audio none\|coreaudio` (macOS) | Where the game's sound goes. `wasapi` plays it through the default Windows audio device and `coreaudio` through the default macOS output device; `none` plays nothing, but the game's mixer and the audio clock run the same. Default: the system's device output with the window, `none` with `--no-window`, so unattended runs and tests need no sound device. The other system's name is a usage error (`--audio needs none or coreaudio` on macOS). See [Audio](audio.md#options). |
| `--audio-dump FILE` | Write the mixed sound to `FILE` as a WAV file (48 kHz, stereo, 32-bit float), with either output. A file that cannot be created is a usage error. Its header is updated every second, so a run stopped by `--seconds` or by closing the window leaves a usable file. `dal_audiotool wavstat FILE` prints its levels per second (see [`dal_audiotool`](audio.md#dal_audiotool)). |
| `--dialog-answer yes\|no\|ok` | What an auto-answered Yes/No message dialog replies (default `yes`; `ok` is the same as `yes`). OK dialogs always answer OK. |

### Rendering

| Option | Meaning |
| --- | --- |
| `--render none\|vulkan` | Whether the RSX commands are drawn (default `vulkan` with the window or `--dump-frames`, otherwise `none`). See [Rendering](rendering.md#rendering). |
| `--dump-frames DIR` | Write the displayed frame of every flip as `DIR/frame_<flip>.png` (flips counted from 1, six digits; RGB, 8 bits), with or without the window. `DIR` is created. |
| `--dump-frames-every K` | Dump every `K`-th flip instead of every one (default 1). |
| `--shader-cache DIR` | Directory of the SPIR-V cache (default `generated/shader_cache`). |
| `--vk-validation` | Enable the Khronos validation layer. It comes with the Vulkan SDK; if it is not installed, `dal_game` logs a warning and goes on without it. |
| `--rsx-capture N[,M...]` | Record the given frames (1-based flip numbers) as `.dalcap` files. See [Capture and replay](rendering.md#capture-and-replay). |
| `--rsx-census FILE` | Count the arguments the game writes to the RSX registers (surface and texture formats, vertex layouts, primitive types, draw sizes, render state, flip and vblank rates) and write them to `FILE` as JSON when the run stops. The fragment and vertex programs the game used are written next to it as `generated/rsx/fp_<hash>.bin` and `vp_<hash>.bin` (relative to the working directory). The directories are created; the file is truncated. |

The renderer options are a usage error (exit code 2) when the renderer is off. The window and
`--dump-frames` switch it on; `--dump-frames-every`, `--shader-cache` and `--vk-validation` need one
of them or `--render vulkan`; an explicit `--render none` conflicts with all of them.
`--rsx-capture` and `--rsx-census` do not need the renderer.

Frame dumps are encoded on a worker thread, so dumping does not slow the game down. When the worker
falls more than four frames behind, a frame is skipped and counted (`dal: render: dump skipped
(queue full) at flip N`, and `dumps skipped (queue full)` in the end-of-run `dal: render:` line). At
the end of a run the pending frames are written within 2 s. Each file is written as `.png.tmp` and
renamed, so an interrupted run never leaves a truncated `.png`. A file that cannot be written is
reported once and counted, and the run goes on.

The log says which Vulkan device was chosen and how long the device and renderer took to start. If
no device meets the requirements (Vulkan 1.3 with dynamic rendering, synchronization2 and shader
demote-to-helper-invocation; a window also needs the platform's surface extension, Win32 or Metal,
and the swapchain extension), or the window cannot be created, the RSX thread logs the reason and the
game goes on without rendering. On macOS the Vulkan driver (KosmicKrisp or MoltenVK) is chosen with
the loader's environment variables, not with an option; see
[Vulkan on macOS](rendering.md#vulkan-on-macos).

### Diagnostics

| Option | Meaning |
| --- | --- |
| `--seconds N` | Watchdog: stop after `N` seconds. 0 (the default) disables it. |
| `--log FILE` | Also write every log line to `FILE`, which is truncated. |
| `--log-level trace\|debug\|info\|warn\|error` | Lowest level that is logged (default `info`). `debug` adds diagnostics such as the 10 s window statistics; `trace` is the most verbose. Debug and trace lines carry a `debug: ` / `trace: ` tag. |
| `--quiet-hle` | Do not log the first calls of default-handled imports. The counts in the census are unaffected. |
| `--fast-forward N` | Debug option: run the guest `N` times faster than real time. `N` is 1 (off, the default) or a speed from 2 to 8; anything else is a usage error (`--fast-forward needs 1 (off) or a speed from 2 to 8`). The game's clock, timed waits, vblank and movie clock follow the speed, and the sound to the audio device is muted unless the overlay's `Sound while fast-forwarding` is on (`--audio-dump` always receives it). `--seconds`, pad scripts and the stall detector stay in host time. The run logs `dal: clock: guest speed Nx`. This is the way to fast-forward without the window; with the window, holding `Tab` or the overlay's Settings do the same interactively (the option ticks the overlay's `Fast-forward` checkbox). See [Guest time and fast-forward](runtime.md#guest-time-and-fast-forward). |
| `--strict` | Treat a jump to an address that is not a recovered function as a fatal error (see [Dispatch](runtime.md#dispatch)). |
| `--load-only` | Load and verify the executable, print the load summary and exit with 0 without running any guest code. |
| `--png-dump DIR` | Write every image `cellPngDec` decodes to `DIR/png_<n>.png` (8-bit RGBA, at most 64), to check what the guest was given. `DIR` is created. |

### Help and configuration file

| Option | Meaning |
| --- | --- |
| `-h`, `--help` | Print the usage text on stdout and exit with 0. It works even when `config.json` is broken. |
| `--config FILE` | Read `FILE` instead of looking for `config.json`. A relative name is relative to the working directory. The file must exist and be readable, and the name must not be empty. See [Configuration file](#configuration-file). |
| `--no-config` | Read no configuration file. Cannot be combined with `--config` (`error: --no-config excludes --config`). The game tests pass it. |

These three have no key in the configuration file.

## Configuration file

Every option above can also be set in a JSON file, so that `dal_game.exe` starts without arguments
(double-click, or a shortcut with "Start in" set to the `bin` folder). On macOS the executable is
`dal_game`, which starts without arguments from a terminal in the `bin` folder.

### Lookup

1. `config.json` in the working directory, if it exists.
2. Otherwise `config.json` in the directory of `dal_game.exe` (`dal_game` on macOS).

Only one file is read; they are not merged. A missing file is not an error. `--config FILE` reads
`FILE` instead (the last `--config` counts), and then a missing or unreadable file is an error.
`--no-config` reads none. `tools/build.ps1 -GameDir <game-dir>` (Windows) or `tools/build.sh --game-dir <game-dir>` (macOS)
writes `build/<preset>/bin/config.json` when there is none (see
[Building](building.md#toolsbuildps1) and [Building on macOS](building.md#toolsbuildsh)); `/config.json` at the repository root is
git-ignored, for a file of your own.

The log tells which file was read, as `dal: config: <file>`.

### Format

Strict JSON, with an optional UTF-8 byte-order mark: no comments, no trailing commas. The top level is
an object, and each key may appear once. A key is the option name without the leading `--` and with `-`
replaced by `_` (`--game-dir` is `game_dir`). `null` is not a value; remove the key to get the default.

| Kind | Keys | JSON value |
| --- | --- | --- |
| Path | `game_dir`, `vfs_config`, `elf`, `log`, `hdd_dir`, `sdata_cache`, `png_dump`, `pad_script`, `audio_dump`, `rsx_census`, `dump_frames`, `shader_cache` | A non-empty string. A relative path is relative to the directory of the file, not to the working directory. |
| String | `log_level`, `dialog_answer`, `pad`, `audio`, `render` | A string with one of the values of the option. |
| Unsigned integer | `seconds`, `dump_frames_every`, `fast_forward` | A non-negative integer, with the range of the option (`dump_frames_every` must be positive; `fast_forward` is 1 or from 2 to 8). |
| Flag | `strict`, `quiet_hle`, `load_only`, `pad_probe`, `vk_validation`, `no_window` | `true` or `false`. |
| Frame list | `rsx_capture` | An array of positive integers (`[300, 600]`), or a string in the command-line form (`"300,600"`). |

A key that is not in the table (including `config`, `no_config` and `help`) is ignored with a warning,
`dal: config: <file>: unknown key "<key>" ignored`, so a newer file still starts an older build.
Values are checked like their command-line forms, so the derived defaults, the conflict checks and
the exit codes are the same.

`config/dal_game.example.json` is a tracked example with placeholder paths:

```json
{
  "game_dir": "/path/to/BLJM61048",
  "elf": "/path/to/EBOOT.elf",
  "hdd_dir": "saves",
  "log_level": "info",
  "pad": "keyboard+gamepad",
  "audio": "wasapi",
  "no_window": false
}
```

Here `hdd_dir` is `saves` next to the file. On macOS the `audio` value is `coreaudio` (or leave the key
out for the default); `wasapi` is a usage error there.

### Precedence

1. The command line.
2. `config.json`.
3. The VFS configuration named by `--vfs-config` (or `vfs_config`); the merged `game_dir`, `hdd_dir`
   and `sdata_cache` replace the matching parts of it (see [Virtual file system](virtual-file-system.md#vfs-configuration-file)).
4. The built-in defaults.

A value on the command line always wins over the same option in the file, including flags:
`"no_window": true` in the file is switched off with `--no-window=false`. Relative paths from the
command line stay relative to the working directory; those from the file are relative to the file.

### Errors

A configuration file error exits with code 2 and prints one line, **without** the usage text, because
the command line is not the mistake. The file is named by its full path:

| Message (after `error: <file>: `) | Cause |
| --- | --- |
| `cannot open the file` | `--config FILE` names a file that is missing or cannot be read. |
| `invalid JSON: <message> at line L, column C (byte B)` | A syntax error. |
| `duplicate key "k" in an object` | A key given twice. |
| `the top level must be an object` | The file is valid JSON but not an object. |
| `"k": expected a path string, got <type>`, `"k": the path is empty`, `"k": the path is not valid UTF-8` | A bad Path value. |
| `"k": expected a string, got <type>` | A bad String value. |
| `"k": expected a non-negative integer, got <type>` | A bad integer value. |
| `"k": expected true or false, got <type>` | A bad flag value. |
| `"k": expected a string or an array of positive integers, got <type>` | A bad `rsx_capture` value. |
| `"k": null is not a value; remove the key to use the default` | A `null`. |

`<type>` is `null`, `object`, `array`, `string`, `boolean` or `number`. A broken file is reported before
an unknown command-line argument.

A value that parses but is not valid is reported like its command-line form, with the file and key in
place of the option: `error: <file>: seconds needs a non-negative integer`, `error: <file>: pad needs
none, neutral, keyboard, gamepad or keyboard+gamepad`, `error: <file>: pad_script: <reason>`. A conflict between
options keeps its usual text (for example `--pad-probe needs the window`) and gets the line
`note: settings were also read from <file>`. When neither `--game-dir` nor `--vfs-config` is given,
the message is `error: --game-dir or --vfs-config is required (or "game_dir" or "vfs_config" in
config.json)`. These print the usage text as before.

### Differences from earlier versions

- `-h` and `--help` print the usage text and exit with 0 (before, they were unknown arguments).
- `--name=value` works for every option, and `--flag=false` and `--flag=true` for flags. `--name=`
  with an empty value takes the next argument as the value.
- Unknown arguments and missing values use the wording of the command-line library after `error: `, for
  example `error: The following argument was not expected: --bogus` and `error: --log: 1 required TEXT
  missing`.
- When several options are wrong, the first one in the usage order is reported, not the first one on the
  command line.
- Arguments are UTF-8.
- `--rsx-capture` can be repeated; the frames of all occurrences are recorded.

## Exit codes and stop lines

Every run ends with one stop line on stderr and a census (see [Census](runtime.md#census)), then
exits:

| Code | Meaning | Stop line |
| --- | --- | --- |
| 0 | The guest called `sys_process_exit` or `_sys_process_exit`, the window was closed (on macOS also by `Cmd+Q`), or `--load-only` finished loading | `dal: stop: process exit status N`, `dal: stop: window closed` |
| 2 | Usage error: bad option, neither `--game-dir` nor `--vfs-config`, game directory missing, log file or audio dump file not writable, configuration file missing (`--config`) or invalid | `error: ...` (usage text; none for a configuration file error) |
| 3 | Fatal error: unhandled instruction, trap, guest fault (access outside the committed guest memory), unknown dispatch target with `--strict`, guest address space cannot be reserved; also the stops the game itself asks for (`cellGameContentErrorDialog`, a failed trophy registration) | `dal: stop: fatal <kind>: ...`, or a `dal: stop: ...` line naming the call |
| 4 | Load error: file missing, not a PPU executable, SHA-256 different from the one in the generated code, invalid VFS configuration, or no executable can be located | `error: ...` |
| 5 | The `--seconds` watchdog elapsed | `dal: stop: watchdog N s` |
| 6 | Stall: every live guest thread has been in an indefinite wait for at least 2 seconds | `dal: stop: stall (N threads blocked)` |
| 7 | All guest threads ended without a process exit | `dal: stop: no guest threads left` |

A fatal stop also prints the registers and the call trace (the most recent function entries) of
the faulting guest thread. The runtime never calls `abort`: the CRT and Windows error dialogs are
disabled, so a run always ends with an exit code, also when it is started from a script.

## Examples by purpose

A run that stops by itself at a time limit, with a log file (the three intro movies last about
109 s together, so a shorter limit ends the run during them):

```sh
dal_game --game-dir <game-dir> --no-window --seconds 165 --log run.log
```

Every 30th frame of the first 30 s as PNG files, without a window:

```sh
dal_game --game-dir <game-dir> --no-window --seconds 30 --dump-frames out/frames --dump-frames-every 30
```

Leaving the title screen without a person at the keyboard, with a fresh hdd directory (the script
is the one the `game_input` test uses):

```sh
dal_game --game-dir <game-dir> --no-window --hdd-dir <empty-dir> --seconds 40 --pad-script tools/pad_scripts/leave_title.txt
```

Playing with the settings of a file of your own (the file's `no_window` is switched off for this
run):

```sh
dal_game --config <settings-file> --no-window=false
```

Recording the game's sound to a file without playing it:

```sh
dal_game --game-dir <game-dir> --no-window --hdd-dir <empty-dir> --seconds 40 --pad-script tools/pad_scripts/leave_title.txt --audio-dump out/run.wav
```
