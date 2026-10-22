# Runtime internals

## Logging

Log lines go to stderr, and to `--log FILE` when given, one line each. The logger is a thin layer
over spdlog (the `externals/spdlog` submodule, built as a static library with `std::format`
instead of the bundled fmt); spdlog types do not appear in the runtime's own headers. A line is
prefixed with `[host]` or with `[t XXXXXXXX]`, the id of the guest thread that wrote it, followed
by `warn: ` or `error: ` for those levels. Output from the guest itself (`sys_tty_write`) goes to
stdout, unprefixed.

## Census

On every stop the runtime prints what the game did, sorted by count and then by name:

```
census: syscalls 18 calls, 14 distinct, 2 default-handled
  sc 141 sys_timer_usleep: 1 [impl]
  sc 169 sys_spu_initialize: 1 [default]
census: hle 118 calls, 21 distinct, 10 default-handled
  sysPrxForUser::sys_lwmutex_lock: 29 [impl]
  cellSysmodule::cellSysmoduleLoadModule: 15 [default]
census: unknown targets 0 distinct
census: rsx 383050 headers, 264 distinct methods, 836 flips, rendered 2761 draws, skipped 0
  interrupt handler calls: vblank 1492, flip 0, user 836
  method 0x324: 7912
  method 0xfeac GCM_FLIP_COMMAND: 836
census: rsx unimplemented 1 kinds
  state zpass report constant: 1673
census: threads 5 created, 5 live
  0x01000000 main_thread: running
  0x01000001 _SPU_printf_server: blocked in sys_event_queue_receive
  0x01000005 _gcm_intr_thread: blocked in gcm interrupt wait [service]
```

`[impl]` marks a call that an implemented handler served and `[default]` one that the default
handler took (see below). An unknown target is printed as `0x<target> from 0x<site>: count`. A default-handled import is
followed by `ptr r3,r5,...`, the argument registers that held a pointer to committed guest memory
(an out-pointer candidate, see [Default handlers](#default-handlers)). The `rsx` section is described under [Fake RSX](rendering.md#fake-rsx);
the `, rendered ... draws, skipped ...` part appears when the renderer runs, and the
`rsx unimplemented` section under [Rendering](rendering.md#rendering).
After a run that played movies, a `census: sail <n> movies` section repeats the per-movie
summary lines (see [Movies](movies.md#log-lines-and-census)). After a run in which the game
initialised `cellAudio`, a `census: audio` section gives the audio engine, mixer input and
`cellAtrac` counters (see [Audio](audio.md#census)). When the generated code has override or hook
entries, a `census: overrides <n> entries, <total> calls` section follows, with one line per entry
sorted by call count and then address (see [Function overrides and hooks](overrides.md#log-lines-and-census)):

```
census: overrides 2 entries, 5 calls
  0x0001c000 override skipSpuWait: 5
  0x0001a2b0 hook dalHookLogCall "frameStep": 0
```
Thread states are `running`, `blocked in <what>` or `exited`. The first call of each syscall and
each import also logs `dal: first sc <number> <name>` or `dal: first hle <module>::<name>` while
the game runs; these lines are the progress markers that `game_boot` checks.

## Memory map

The guest space is one reservation of 4 GiB plus a guard area, big-endian, accessed only through
the `dalRead*` and `dalWrite*` helpers. Pages are committed on demand by the allocators below. An
access to an uncommitted page raises a fault that the runtime turns into a fatal stop with the
guest address and the access kind. The operating system calls sit behind `runtime/src/core/host_memory.h`:
on Windows the space is reserved and committed with `VirtualAlloc` and faults arrive through a
vectored exception handler; on macOS it is `mmap` and `mprotect` with a `SIGSEGV` and `SIGBUS` handler
that runs on an alternate stack. The guest keeps 4 KiB logical pages, but the host page of an Apple
Silicon Mac is 16 KiB, so a host page stays accessible while any of its logical pages is committed
(see "Page size and guard pages" under [Running and debugging on a Mac](building.md#running-and-debugging-on-a-mac)).

| Range | Use |
| --- | --- |
| `0x00010000..0x10000000` | Main region: the ELF image (committed at its segment addresses), per-thread TLS blocks and the heap handlers' allocations. 64 KiB granularity. |
| `0x10000000..0x20000000` | RSX context area, fixed by the console memory map: the default command buffer context (`0x10000000`), the DMA control and `put`/`get`/`ref` registers (`0x10100000`, `0x10100040`), the driver info block (`0x10200000`) and the reports and semaphores (`0x10300000`). The first 4 MiB are committed when the game initializes GCM. |
| `0x20000000..0x30000000` | User memory with 1 MiB pages (`sys_memory_allocate` with flags 0x400 or 0). |
| `0x30000000..0x40000000` | User memory with 64 KiB pages (`sys_memory_allocate` with flags 0x200). |
| `0xC0000000..0xCF900000` | RSX local memory (video memory), committed when the game initializes GCM. |
| `0xD0000000..0xE0000000` | Thread stacks, 4 KiB pages, with an uncommitted 4 KiB guard page on each side of every stack (on macOS a guard that shares a 16 KiB host page with a live allocation stays accessible). |

The top 64 KiB, `0xFFFF0000..0xFFFFFFFF`, are never committed: they hold the entry keys of host
functions, the function descriptors that the HLE layer hands to guest code (for example a callback
the runtime itself provides). Guest code can call or start a thread on such a descriptor, and the
dispatcher resolves the key to the host handler.

The primary stack size from the executable's process parameters is clamped as RPCS3 does (the
game's 0x8000 becomes 0x10000). The argument block (`argv`, `envp`, strings) sits at the top of
the stack. `sys_memory_get_user_memory_size` reports a 0xD500000 container. The timebase runs at
79.8 MHz, scaled from the host monotonic counter (`QueryPerformanceCounter` on Windows,
`CLOCK_UPTIME_RAW` on macOS).

## Dispatch

`dalRegisterFuncs` builds a flat table indexed by guest address from the generated function
mappings, so a computed jump or an indirect call costs one lookup. A target that is not in the
table is an unknown target: the first occurrence of each (target, call site) pair is logged with
the call trace and counted in the census. With `--strict` it is a fatal stop. Without it the
dispatch returns, which acts like a callee that returned immediately, so the run goes on and the
census lists every such target.

A function that the overrides file names has a wrapper in the table under the function's own
address, so computed jumps, indirect calls, callbacks and thread entries reach the handler; the
original body is not in the table. Right after registering the functions, `dal_game` checks the
generated override table against the dispatch table (a wrong table is a fatal stop) and logs
`dal: overrides: none`, or `dal: overrides: <n> entries (override <o>, hook <h>)` followed by one
`dal: overrides: <kind> 0x<address> <handler>[ "<label>"]` line per entry. The hook handler
`dalHookLogCall` logs its first 8 calls per entry as
`dal: hook 0x<address>[ "<label>"] call <n>: r3=... r4=... r5=... r6=... lr=...`. See
[Function overrides and hooks](overrides.md).

## Threads

Each guest thread is a host thread (16 MiB host stack; `_beginthreadex` on Windows, a detached
pthread with a 64 KiB host guard region on macOS) with its own context. All guest code runs
under one global guest lock, a FIFO ticket lock, so guest threads never run in parallel. A thread
gives up the lock only in `dalYield` (called from loop hooks, when another thread is waiting), in
a blocking wait and when it exits. Blocking system calls (mutex, condition variable, event flag,
event queue, sleep, join) wait on host condition variables with the guest lock, and record what
they wait for for the census and for stall detection.

Priority scheduling is deferred, with one exception. Runnable threads get the lock in arrival order,
and `sys_ppu_thread_set_priority` and `sys_ppu_thread_get_priority` only store and return the value.
The exception is the creation rule of `sys_ppu_thread_create`: a new thread whose priority number is
lower than the stored priority of its creator (lower number means higher precedence on lv2) runs
before the creator continues. `startGuestThreadFirst` starts the host thread, waits until it has
queued for the guest lock (at most 5 ms), and yields the lock to it. The new thread then runs until
it blocks, exits, yields or uses up its preemption budget; the creator's budget is reset afterwards.
A thread with an equal or higher number, an interrupt thread, and the threads the runtime itself
creates are started as before. The game depends on it: its audio mixer thread (priority 90) is created
by the main thread (1005) and has to store its notify queue key before main registers it with
`cellAudio`. The rule is logged at debug level, and a new thread that did not queue within 5 ms
logs `dal: thread: ... not queued within 5 ms; creator continues` at info level. There is no other
priority behaviour: no priority-ordered hand-off, no wake order by priority, no preemption, and no
rescheduling when a priority changes.

On Windows, `dal_game` holds a 1 ms system timer resolution for the whole run (`timeBeginPeriod`,
`runtime/src/core/timer_resolution.cpp`). Timed waits (`sys_timer_usleep`, lock and queue time-outs)
are host condition variable waits, and Windows rounds them to its timer tick (15.6 ms by default),
so a 1 ms guest sleep would take 15 ms. The game's decode loops poll with 1 ms sleeps. The request
ends with the process; a refusal is logged as a warning and the default tick stays. macOS has no
coarse tick to raise, so `timer_resolution_posix.cpp` holds nothing and the request counts as granted
(see [Audio](audio.md#engine-and-block-clock)).

The RSX thread and the audio engine are host threads that take the same guest lock. The RSX thread
therefore pumps the command buffer in slices of 128 words and leaves the lock once a hold has lasted
300 us, so that the game's mixer can run between the slices (see [Audio](audio.md#guest-lock)). It
also leaves the lock after each draw end and copies the draw's guest bytes without it; the game only
reuses that memory after a later label, report or flip, which the RSX thread writes after the copy.

Thread ids start at `0x01000000`. lv2 interrupt threads are not implemented; the one interrupt
thread the game needs is the GCM interrupt thread described under [Fake RSX](rendering.md#fake-rsx).

A service thread is a guest thread that the runtime starts on the game's behalf and that waits
for work from the host (the GCM interrupt thread, and `_libsail-control`, which delivers the
cellSail notify events and allocator calls; see [Movies](movies.md)). The census marks it `[service]`, and the
stall detector and the check for "no guest threads left" ignore it, so a game whose own threads
are all blocked still ends with a stall stop.

The host main thread is the supervisor. After boot it checks every 100 ms for the watchdog, a
stall and the absence of live guest threads.

## Implemented system calls

Everything else goes to the default handler. Behaviour follows the RPCS3 handler of the same
name; struct layouts and error codes are restated from its headers.

| Group | Calls |
| --- | --- |
| Process | `_sys_process_exit` (22) |
| TTY | `sys_tty_write` (403); `sys_tty_read` (402) returns `CELL_EIO` |
| Time | `sys_timer_usleep` (141), `sys_timer_sleep` (142), `sys_time_get_timebase_frequency` (147) |
| Memory | `sys_memory_allocate` (348), `sys_memory_free` (349), `sys_memory_get_user_memory_size` (352) |
| Mutex and condition variable | `sys_mutex_create`, `_destroy`, `_lock`, `_trylock`, `_unlock` (100-104); `sys_cond_create`, `_destroy`, `_wait`, `_signal`, `_signal_all` (105-109) |
| Event flag | `sys_event_flag_create` (82), `_destroy` (83), `_wait` (85), `_set` (87), `_clear` (118), `_cancel` (132), `_get` (139) |
| Event queue and port | `sys_event_queue_create` (128), `_destroy` (129), `_receive` (130), `_drain` (133); `sys_event_port_create` (134), `_destroy` (135), `_connect_local` (136), `_disconnect` (137), `_send` (138). A queue created with a non-zero IPC key can be found by that key (the host looks it up and sends to it; the `cellAudio` notify uses this); a second live queue with the same key fails with `CELL_EEXIST`, key 0 makes a local queue, and destroying the queue forgets the key. IPC ports (`sys_event_port_connect_ipc`) are not supported. |
| SPU | `sys_spu_initialize` (169), `sys_spu_thread_write_ls` (181), `_read_ls` (182), `_write_spu_mb` (190), `_connect_event` (191), `_disconnect_event` (192), `sys_spu_thread_group_connect_event_all_threads` (251), `_disconnect_event_all_threads` (252). Only the bookkeeping is kept; no SPU code runs. |
| File system | `sys_fs_open` (801), `_read` (802), `_write` (803), `_close` (804), `_opendir` (805), `_readdir` (806), `_closedir` (807), `_stat` (808), `_fstat` (809), `_mkdir` (811), `_unlink` (814), `_lseek` (818); host files are opened through `runtime/src/core/host_file.h` (`CreateFileW` on Windows, `open` on macOS; Windows opens with every share mode, so the guest can unlink or reopen a file it still holds open); see [Virtual file system](virtual-file-system.md#virtual-file-system) |
| Thread | `_sys_ppu_thread_exit` (41), `sys_ppu_thread_yield` (43), `sys_ppu_thread_join` (44), `sys_ppu_thread_set_priority` (47), `sys_ppu_thread_get_priority` (48); the creation rule of `sys_ppu_thread_create` is under [Threads](#threads) |

## Implemented imports

Imports are matched to handlers by module and FNID. All 166 of the executable's imports have a
handler (the log says `dal: hle: 166 imports, 166 with handlers`); the default handler below still
serves any import without one. Behaviour follows the RPCS3 function of the same name
(`Emu/Cell/Modules/`); each handler file lists its deviations in its header comment. `cellAudio` and
`cellAtrac` were implemented from a written description of RPCS3's behaviour, not from its code (see
[Audio](audio.md#clean-room)).

| Module | Imports |
| --- | --- |
| `sysPrxForUser` (all 23 imports) | `sys_initialize_tls` (per-thread TLS blocks laid out as RPCS3 does), `sys_ppu_thread_create`, `_get_id`, `_once`, `_exit`; `sys_lwmutex_create`, `_destroy`, `_lock`, `_trylock`, `_unlock`; `_sys_heap_create_heap`, `_malloc`, `_memalign`, `_free`, `_delete_heap`; `sys_time_get_system_time`, `sys_process_exit`; `_sys_process_atexitspawn`, `_sys_process_at_Exitspawn`, `sys_prx_exitspawn_with_level`, `sys_prx_register_library`, `_sys_spu_printf_initialize`, `_sys_spu_printf_finalize` (the last six return `CELL_OK` and do nothing else) |
| `cellSysmodule` | `cellSysmoduleLoadModule`, `cellSysmoduleUnloadModule`: every module id succeeds, because the HLE functions are always available |
| `cellSpurs` | `cellSpursInitializeWithAttribute2` and its attribute helper, `cellSpursGetSpuThreadGroupId`, `cellSpursFinalize`: a SPURS instance can be created and queried, but no SPU thread, workload or helper thread exists |
| `cellGcmSys` | 26 imports: initialization (`_cellGcmInitBody`), configuration, IO mapping (`cellGcmMapMainMemory`, `cellGcmAddressToOffset`, `cellGcmUnmapEaIoAddress`), display buffers, flip mode and flip status, the vblank, flip and user handlers, tiles and zcull, the control register and the default command buffer callback; see [Fake RSX](rendering.md#fake-rsx) |
| `cellSysutil` | the callback pump (`cellSysutilRegisterCallback`, `_UnregisterCallback`, `_CheckCallback`), BGM playback status, `cellVideoOutConfigure`, `_GetState`, `_GetResolutionAvailability`, `cellAudioOutGetState`, the message dialog (`cellMsgDialogOpen2`, `_Close`, progress bar reset and increment) and save data (`cellSaveDataAutoLoad2`, `AutoSave2`, `ListLoad2`, `ListSave2`, `Delete2`) |
| `cellGame` | `cellGameDataCheck`, `cellGameCreateGameData`, `cellGameContentPermit`, `cellGameContentErrorDialog` |
| `sys_fs` | `cellFsOpen`, `cellFsSdataOpen`, `cellFsReadWithOffset`, `cellFsWriteWithOffset`, `cellFsClose`, `cellFsFstat`, `cellFsStat`, `cellFsMkdir`, `cellFsUnlink` |
| `sys_io` | `cellPadInit`, `cellPadEnd`, `cellPadSetPortSetting`, `cellPadGetData`, `cellPadGetInfo2`, `cellPadSetActDirect` |
| `cellPngDec` | `cellPngDecCreate`, `cellPngDecDestroy`, `cellPngDecExtOpen`, `cellPngDecExtReadHeader`, `cellPngDecExtSetParameter`, `cellPngDecExtDecodeData`, `cellPngDecClose`; see [PNG decoding](#png-decoding) |
| `cellSail` | the 27 imports the game uses: the memory allocator, player initialise, boot, open, start and finalize, descriptors and their stream database, and the graphics and sound adapters (format, `GetFrame`, `UpdateAvSync`); see [Movies](movies.md) |
| `cellPamf` | `cellPamfReaderGetNumberOfSpecificStreams` on the descriptor's stream database |
| `cellAudio` | all 11 imports the game uses: `Init`, `Quit`, `PortOpen`, `PortStart`, `PortStop`, `PortClose`, `GetPortConfig`, `GetPortBlockTag`, `GetPortTimestamp`, `SetNotifyEventQueue`, `RemoveNotifyEventQueue`; the ports and the 8-block ring live in guest memory and a host clock drives the game's mixer; see [Audio](audio.md) |
| `cellAtrac` | all 15 imports the game uses: `SetDataAndGetMemSize`, `CreateDecoder`, `DeleteDecoder`, `Decode`, the stream calls (`GetStreamDataInfo`, `AddStreamData`, `GetRemainFrame`), the loop and info calls and the second-buffer calls; ATRAC3plus is decoded through FFmpeg; see [Audio](audio.md#cellatrac) |
| `sceNp`, `sceNpTrophy` | `sceNpInit`, `sceNpTerm`, `sceNpDrmIsAvailable`; the 10 trophy imports (`sceNpTrophyInit` to `sceNpTrophyUnlockTrophy`) |

Video and audio out report what the game's `PARAM.SFO` allows (the `RESOLUTION` and
`SOUND_FORMAT` attributes); a missing or unreadable file keeps the defaults.

## Headless policy

RPCS3 opens dialogs and waits for the user in several of these modules. `dal_game` has no user
interface, so each module follows a documented policy that never blocks on a person:

- **Message dialogs** (`cellMsgDialogOpen2`): the type and the text are logged. A dialog with
  buttons answers itself after 200 ms: OK for an OK dialog, and the `--dialog-answer` value
  (default yes) for a Yes/No dialog. A dialog without buttons stays open until
  `cellMsgDialogClose`. The answer is delivered through the cellSysutil callback queue, so the
  guest sees it from `cellSysutilCheckCallback`, as on the console.
- **Save data**: a save lives in `/dev_hdd0/home/00000001/savedata/<dir name>/`. Error dialogs
  are logged and their error code is returned, without a dialog object. A save list is never
  shown; the entry is the one the game's list callback highlights, with RPCS3's fallbacks, and
  without a choice the call returns `CELL_CANCEL`. `cellSaveDataDelete2` returns `CELL_CANCEL`.
  Secure files are stored in plain text.
- **Trophies**: no trophy pack is installed from the network and no notification is shown. The
  pack is read for its trophy list, and the unlock state is kept as a text file in the trophy
  directory under `/dev_hdd0/home/00000001`. The registration status callbacks run inside
  `sceNpTrophyRegisterContext` instead of being spread over two seconds.
- **cellSysutil callbacks**: they run on the guest thread that calls `cellSysutilCheckCallback`,
  from a queue that the modules above feed. `CELL_SYSUTIL_REQUEST_EXITGAME` is not acted on.
- **cellGame**: the content directories are created on the host through the VFS, under the
  `--hdd-dir` directory.
- **Audio**: the output is `none` with `--no-window` unless `--audio wasapi` asks for the device,
  so unattended runs need no sound device; the game's mixer and the `cellAudio` clock run either way
  (see [Audio](audio.md#options)). A device or decoder problem degrades to silence with one warning.
- **Input**: see `--pad` and [Input](#input). The pad on port 0 comes from a provider
  (`runtime/src/hle/hle.h`): none, neutral, or the host pad state that the keyboard, a pad script and
  gamepads write. Runs with `--no-window` never load a gamepad backend.

## Input

`cellPad` asks a `PadProvider` for the state of a port on every `cellPadGetData` call and reports
the changes to the game. `--pad` picks the provider: `none` (no pad), `neutral` (a standard pad on
port 0 that never changes), `keyboard`, `gamepad` or `keyboard+gamepad`. The last three are set up in
`runtime/src/game_main.cpp` with `setPadProvider`, because they need the window and the script or
gamepad thread; `setPadBackend` accepts only `none` and `neutral`. The option defaults are in
[`dal_game` options](runtime-options.md#window-input-and-sound) and the key table in the
[README](../README.md#keyboard); the script format is [below](#pad-script). Gamepads (mapping,
selection, Steam, the GameInput runtime, log lines and tools) are described in [Input](input.md).

| Part | File | Role |
| --- | --- | --- |
| Buttons | `runtime/src/input/pad_buttons.*` | The 16 buttons as a bit mask, `PadAnalog` (sticks and trigger pressure), `padStateFromButtons` and `padStateFromInput` (digital words, pressure words at 0xFF for pressed buttons or the analog trigger value, sticks, everything else as the neutral pad) and the button names used by scripts |
| State and provider | `runtime/src/input/keyboard_pad.*` | The scan-code table, `KeyboardPadState` (the state of every host source: keyboard, script, gamepad) and the provider for port 0 (always connected, or connected only while a gamepad is in use with `--pad gamepad`; other ports empty) |
| Script | `runtime/src/input/pad_script.*` | The script parser and the player thread |
| Gamepad | `runtime/src/input/gamepad_*`, `gameinput_backend.*`, `gamecontroller_backend.*` | Backend interface, GameInput backend (Windows), GameController backend (macOS), mapping, selection, the gamepad thread and `--pad-probe`; see [Input](input.md#design) |
| Window | `runtime/src/render/vk_window.*`, `vk_window_win32.cpp`, `vk_window_macos.mm` | Key and focus messages from the window thread (on macOS, from the main thread's event loop); on Windows it turns the IME off for the window |
| Overlay | `runtime/src/overlay/debug_overlay.*` | The `F1` toggle, the `Tab` fast-forward hold and the message hook that decides which key presses the [debug overlay](#debug-overlay) withholds from the pad |

The renderer library does not depend on `input/` or on the overlay: the window reports raw scan codes
and, through `PresenterOptions::on_message`, raw window messages through callbacks, and `game_main.cpp`
connects them to the state and to the overlay.

### Threading

`poll` runs on guest threads under the guest lock, so it must never block or take a lock that the
window thread, the script thread or the gamepad thread holds. `KeyboardPadState` is therefore
lock-free: one atomic mask of held buttons and one atomic press time per button for each source
(`Keyboard`, written by the window thread, `Script`, written by the script thread, and `Gamepad`,
written by the gamepad thread), one atomic with the sticks and trigger pressure of the gamepad, one
for its connection flag and one for the latest vibration request. `poll` only reads them. All
accesses are relaxed, because each bit and each time is independent, and a stale read delays a
change by one poll. The script and gamepad threads are detached and own a `shared_ptr` to the state;
nothing joins them, because the process ends with `std::_Exit`.

### Key handling

- Keys are identified by scan code (PC/AT set 1 make code), not by virtual key, so the mapping is a
  positional cluster on any layout and works while an IME is open. On Windows the scan code is
  `lParam` bits 16 to 23, plus `0xE000` for an extended key, and a zero scan code (some injected
  input) falls back to `MapVirtualKeyW`. On macOS the Cocoa window turns the key's virtual key code
  into the scan code (see [Input](input.md#keyboard-and-the-overlay-on-macos)). Only the dedicated
  arrow keys map; the numpad does not.
- Key repeats are dropped, so they do not refresh a press time.
- A press stays in the reported state for at least `KeyboardPadState::kMinPressVisible` (60 ms)
  after it started, also when the key is already up. The game reads the pad about every 33 ms and
  `cellPadGetData` reports only changes, so a shorter tap could fall between two reads.
- Both opposite D-pad directions held cancel each other. A held direction beats the 60 ms tail of a
  released opposite one; with two tails the later press wins.
- A loss of window focus (`WM_KILLFOCUS` on Windows, the window losing key status on macOS)
  releases every `Keyboard` button at once (a key-up is not delivered to a window without focus).
  Script buttons are a separate source and stay; the gamepad thread releases its own source when the
  process loses the foreground window. On Windows, `Alt` combinations still reach the default window
  procedure, so `Alt+F4` closes the window; on macOS `Cmd+Q` is handled by AppKit and ends the run.
- `F1` (scan code `0x3B`) is not in the pad map: it toggles the [debug overlay](#debug-overlay) and
  never reaches the pad, with any `--pad` value. While the overlay is shown and ImGui wants the
  keyboard (a text box is active), key presses (`WM_KEYDOWN`, `WM_SYSKEYDOWN` on Windows) are withheld from the
  pad. Key-ups are always delivered, so a key that went down before the overlay took the keyboard is
  still released.
- `Tab` (scan code `0x0F`) is not in the pad map either: holding it fast-forwards the game (see
  [Debug overlay](#debug-overlay)). The first key-down starts the hold, repeats are ignored, and the
  key-up ends it. It works with the overlay hidden, is withheld from the pad and from ImGui (so Tab
  does not move between the overlay's controls), and a loss of window focus ends the hold too, because the
  key-up does not arrive in a window without focus.
- The press mode that the game sets with `cellPadSetPortSetting` decides whether `cellPad` copies
  the pressure words to the game; the game under test turns it on (`dal: pad: port 0 setting 0x2`).

### Pad script

A script is a text file of timed commands, one per line, read by `loadPadScript` while the options
are parsed. `#` starts a comment, blank lines are ignored, tokens are separated by spaces or tabs:

```
<time> press   <buttons>
<time> release <buttons>
<time> tap     <buttons> [every <interval> until <time>]
```

- `<time>` and `<interval>` are decimal seconds from the start of the script, with millisecond
  precision (more decimals are rounded). `<buttons>` are the names `select l3 r3 start up right down
  left l2 r2 l1 r1 triangle circle cross square`, joined with `+`.
- `tap` is a press at `<time>` and a release `kPadScriptTapHold` (150 ms) later, which is longer
  than the minimum press time. `every <interval> until <time>` repeats it at `<time> + k *
  <interval>` while that is not after the `until` time; `<interval>` must be longer than the hold.
  A script has at most 100000 events and times of at most 1000000 s.
- The events are sorted by time (stable); at the same time, releases come before presses.
- An error names the line (`line 7: unknown button "crosss"`), is printed to stderr and ends the
  start-up with exit code 2.
- The clock starts right before the main guest thread is booted, so it matches `--seconds` within
  a few milliseconds. The player thread sleeps until each event, applies it to the `Script` source
  and logs it.

Example (`tools/pad_scripts/leave_title.txt`, which the `game_input` test uses):

```
# Skip whatever screen is showing every 2 s, from 4 s to 30 s.
4 tap circle every 2 until 30
```

A script that holds a direction while it taps a button:

```
10   press   down
10.5 tap     cross+r1
12   release down
```

### Log lines

All pad lines start with `dal: pad:`; the script lines carry the `[host]` prefix. The lines of
gamepads (`gameinput`, `steam`, `device`, `gamepad`, `probe`) are in [Input](input.md#log-lines).

| Line | Level | Source |
| --- | --- | --- |
| `dal: pad: init max_connect N` | info | `cellPadInit` |
| `dal: pad: port N connected` or `disconnected` | info | the first change of a port to that state |
| `dal: pad: port N setting 0xS (press on\|off, sensor on\|off)` | info | `cellPadSetPortSetting`, once per port and value |
| `dal: pad: <pad>` followed by ` (window)` and ` (script <file name>, <n> events)` as they apply | info | start-up with `--pad keyboard`, `gamepad` or `keyboard+gamepad` (`<pad>` is the value) |
| `dal: pad: script <s>.<ms> s: press\|release <buttons>` | info | the player thread, one line per event |
| `dal: pad: key 0x<scan code> down\|up -> <button>` | debug | a mapped key, window thread; unmapped keys and repeats are not logged |

### Scripted runs and tests

Pressing buttons can make the game write save or system data under `--hdd-dir`, so a scripted run
uses its own directory and deletes it first (`rm -rf build/release/game_hdd_input`), never the
directory of a normal run or of `game_boot`.

The `game_input` test (`cmake/PpuCodegen.cmake`, pins in `cmake/game_input.cmake`) is registered
under the same conditions as `game_boot` with the `.sdat` cache: `DAL_EBOOT_ELF` and `DAL_GAME_DIR`
must be set, and the build must have the `.sdat` keys. The setup test `game_input_hdd_clean` deletes
`build/<preset>/game_hdd_input`, and `game_input` (fixture `game_input_hdd`) then runs
`dal_game --no-window --pad-script tools/pad_scripts/leave_title.txt --hdd-dir build/<preset>/game_hdd_input
--seconds 40` with a time limit of 100 s. It passes when the log contains the main-thread line

```
[t 01000000] dal: fs: sdata-open "/dev_bdvd/PS3_GAME/USRDIR/Script/Event.sdat" ok
```

which the game writes about 20 s into a headless run after Circle has skipped the notice screen
and the movies and confirmed NEW GAME. Without input the game is still in the intro movies at that
time. The run stops at 40 s, before `world.pam` (83 s, not skippable with Circle) ends. The pad
code itself is covered by `runtime_core` (`testPadButtons`, `testKeyboardPadScanCodes`,
`testKeyboardPadState`, `testKeyboardPadProvider`, `testKeyboardPadThreads`, `testPadScriptParse`,
`testPadScriptErrors`, `testPadScriptPlay`), the gamepad source of the state by `runtime_core`
(`testPadStateFromInput`, `testGamepadSourceState`, `testGamepadPadProvider`, `testGamepadCellPad`),
the rest of the gamepad code by `input_core` and `input_gameinput` (see [Tests](tests.md)), and the
key message conversion by `render_core` (`testKeyMessages`).

Scripts live in `tools/pad_scripts/`.

## Debug overlay

With the window open, `F1` shows or hides a [Dear ImGui](https://github.com/ocornut/imgui) window
titled `dal` over the presented image, at the top left. It has four collapsible sections:

| Section | Contents |
| --- | --- |
| Stats | The flip number (the `frame_<N>.png` and `--rsx-capture N` numbering), flips per second over the last second, draws and skipped draws of the last flip, the total of skipped draws and a graph of the last 240 flip intervals. Below, the audio counters of the engine and `cellAudio`: the output, the longest producer time against the 5.333 ms block period and its margin, blocks, periods, failed notifies, skipped periods (with a rate), underrun frames (with a rate), dropped blocks, the clock correction in ppm and the mean output fill against its target. Before `cellAudioInit` the audio part says `n/a (audio not started)` |
| Settings | `Fast-forward` (checkbox), `Speed` (slider, 2x to 8x, default 4x), `Sound while fast-forwarding` (checkbox, off by default), the line `Hold Tab to fast-forward. Guest speed Nx` (N is 1 while fast-forward is off) and `Log level` (the lowest level that is logged; the same filter as `--log-level`) |
| Capture | `Capture next flip` and `Dump next flip as PNG`, each with the state of its last request, and the dump counts (written, failed, skipped) |
| RSX unimplemented | The live `rsx unimplemented` counts (see [Rendering](rendering.md#rendering)) as a table of count and kind, with a text filter, and the number of kinds dropped because the table was full |

Fast-forward makes the guest run `Speed` times as fast as real time while `Fast-forward` is ticked or
`Tab` is held in the window. It is the same state as the `--fast-forward N` option, which is the only
way to use it without a window. The device output is muted while it is active unless `Sound while
fast-forwarding` is on; `--audio-dump` always receives the real sound. A control whose connection to
the runtime is missing is shown disabled.

`Capture next flip` records the first whole frame that starts after the click, which is typically
flip N+2 when the click happens while flip N is displayed, and writes it as
`generated/rsx/captures/frame_<N>.dalcap` like `--rsx-capture`. The button is disabled while a
request is pending or recording, and when no frame builder runs. The panel shows `pending`,
`recording frame N`, then `frame N -> <path>` or `frame N failed: <reason>`.
`Dump next flip as PNG` writes the next completed flip as `frame_<N>.png` into the `--dump-frames`
directory, or `generated/frames` without that option; a flip that `--dump-frames-every` dumps anyway
is written once.

The overlay is drawn on the swapchain image after the letterboxed blit, so it is never part of
`--dump-frames` files, requested dumps, `readDisplay` or captures (see
[Rendering](rendering.md#rendering)). It redraws only when the game flips: during a stall without
flips (a load, for example) it freezes and takes no input. Without the window (`--no-window`,
`--render none`) there is no overlay, and `F1` and `Tab` do nothing.

Design notes:

- The code is the `dal_overlay` library in `runtime/src/overlay/`; `dal_render` does not depend on
  it. `overlay_model.*` holds the numbers (no ImGui), `overlay_panels.*` the ImGui window,
  `overlay_sources.*` the connections to the runtime (audio engine and `cellAudio` counters, the
  `rsx unimplemented` snapshot, the fast-forward state and its switches, the sound switch, the capture
  request and status, the log level) and `debug_overlay.*` the Vulkan backend and the keys. The calls into the window system sit behind
  `overlay_platform.h`, implemented by `overlay_platform_win32.cpp` (Win32 backend) and
  `overlay_platform_macos.mm` (`imgui_impl_osx`, reached through `imgui_osx_bridge.mm`).
  The overlay only calls thread-safe providers and never touches guest memory.
- The ImGui backends start at the first draw after the first show, not at start-up, on the RSX thread:
  the log gets `dal: overlay: ready (imgui 1.91.9b, scale N)` (info) and the font upload costs a
  one-time hitch. `scale` is the DPI scale of the window at that moment and is not updated later. If
  the setup fails, one `dal: overlay: ... failed, overlay disabled` warning is logged and the game
  goes on without the overlay: nothing is drawn, `F1` is still withheld from the pad and `Tab` still
  fast-forwards. A change of the swapchain's surface format
  after start-up skips the overlay for that image and logs `dal: overlay: surface format changed,
  overlay skipped for that image` once.
- One global ImGui context is guarded by a mutex. The window thread feeds it the mouse and key
  messages (on Windows, the Win32 backend calls `SetCapture` and `GetKeyState`, which only work on the
  thread that owns the window); the RSX thread holds the mutex while it builds and records a frame,
  and never across a swapchain call. On Windows `ImGui_ImplWin32_NewFrame` is not used, because its
  key workaround reads `GetKeyState` on the calling thread. On macOS the window passes each event to
  `imgui_impl_osx` under the mutex instead of the backend's own event monitor, and a focus change
  reaches ImGui as an explicit focus event. Hiding the overlay sends ImGui a focus-lost event so that held
  keys and buttons are dropped, and showing it again sends a focus-gained event: ImGui 1.91.9b keeps
  clearing its input while it believes the application has no focus.
- The overlay window has no `imgui.ini`, no docking, no gamepad navigation and does not change the
  mouse cursor.

## Guest time and fast-forward

Fast-forward is a debug feature: the guest runs N times faster than real time, N from 2 to 8. It is
switched on by `--fast-forward N` (the only way without a window), by the `Fast-forward` checkbox and
`Speed` slider of the [debug overlay](#debug-overlay), and by holding `Tab` in the window (losing the
window focus ends the hold). The state is `toggled` (option or checkbox), `held` (the key) and the
speed (default 4); the guest speed is the speed while either is set and 1 otherwise. Each change logs
`dal: clock: guest speed Nx` (`1x` when the guest returns to real time). With fast-forward off every
path below is the same as without the feature.

Turning off only the vblank would not be enough: the game also measures time itself, movies end by
their clock, timed waits are host waits, and with a window the swapchain's `FIFO` present holds the RSX
thread at the display rate. So the guest speed applies to everything the game can observe
(`runtime/src/core/guest_clock.*`):

| What | Behaviour at speed N |
| --- | --- |
| Guest time: `systemTimeUs()` and the timebase counter (`dalMftb`, the `mftb` reads; the timebase frequency is unchanged), so `sys_time_get_system_time`, the RSX report and last-flip timestamps, the `cellAudio` period timestamps and the `cellSail` log times | Advance N times as fast as the host clock. The clock is continuous and never goes back when the speed changes; it is lock-free and readable from any thread (a sequence-locked segment of host start, guest start and speed) |
| Timed guest waits: `sys_timer_usleep` and `sys_timer_sleep`, and the timeouts of mutexes, lightweight mutexes, condition variables, event flags and event queues | The host wait is the guest duration divided by N, rounded up and at least 1 us for a nonzero duration. A wait already in progress is not shortened by a speed change |
| `cellMsgDialogClose` delay | Divided by N like a timed wait |
| VBlank | The period is 16667 us of guest time, so 16667 us / N on the host. The game's frame loop waits on the vblank handler, so it speeds up with it |
| Movie clock | 90 kHz ticks of guest time, so movies advance and end at N times real time (see [Movies](movies.md#hle-boundary)) |
| Window presentation | While N > 1 the swapchain uses the `MAILBOX` present mode (else `IMMEDIATE`, else `FIFO`), which does not wait for the display's refresh, and the renderer does not wait for a free swapchain image: a flip with no image free is rendered and submitted without a present and counted as `skipped` in the `dal: render: window:` line. At speed 1 it is `FIFO` again. Each switch recreates the swapchain once and logs `dal: render: window: present mode <mode>`. The overlay is drawn only on presented flips |
| Audio engine | Stays on host time (below) |

Stay on host time: the `--seconds` watchdog, pad scripts, the stall detector, the auto-answer delay of
message dialogs, the `cellPad` sensor interval, the keyboard's 60 ms minimum press time, the bounds the
runtime sets itself (the `cellSail` preroll, the 5 s stops of its service threads) and the audio
engine's block clock.

The audio engine keeps producing one `cellAudio` period every 5.333 ms of host time, so the game's
mixer and sound thread run at real time. The device output gets silence while the guest speed is above
1, unless `Sound while fast-forwarding` is on (then it plays at normal pitch and is out of step with
the picture); the `--audio-dump` file and the engine's block counts always see the mixed sound. The
`cellAudio` timestamps are guest time, so they advance N times per period. See
[Audio](audio.md#engine-and-block-clock).

Consequences:

- The census rates (`--rsx-census` per-second arrays) and the `cellSail` `movie window` lines are per
  guest second, because they are computed from guest time. A run of `--seconds S` lasts S host seconds,
  which is N times S guest seconds.
- The real speed-up can be below N. Periodic host waits use a timer that sleeps in steps of about
  1 ms, and a short guest sleep takes at least about 1 ms of host time. The log line `dal: gcm: the
  vblank handler is behind, ticks are being coalesced` is logged once per run and also appears at 1x
  during boot, so it does not mark that limit by itself.
- Measured over 60 s with `--no-window`: 60.0 vblanks per second at 1x, 239.9 at 4x and 478.8 at 8x
  (4.00x and 7.98x). In the window over 40 s: 59 per second at 1x and about 470 at 8x. Before the
  window switched to `MAILBOX`, the `FIFO` swapchain held the RSX thread at the display refresh
  through the frame-slot fence wait, and windowed 8x reached only 280 per second; the `fence wait`
  figures in the `dal: render: window:` debug line show such a wait.
- Movie frames are dropped when the frame loop falls behind the movie clock: at 8x without a window
  about 6% of `op.pam`'s frames.

## PNG decoding

`cellPngDec` decodes with libpng's classic API (`externals/libpng` at v1.6.59, built as a static
library together with zlib v1.3.2 from `externals/zlib`; nothing is installed or downloaded).
libpng reports errors by `longjmp`, so every call that can fail runs in a function that owns the
`setjmp` and has no locals with destructors. The handle and stream objects are allocated through
the game's malloc and free callbacks, as on the console. A buffer or file source is read into one
host buffer; with a `cbCtrlStrm` callback the runtime calls it for more data until the IEND chunk
is complete and then decodes the whole image. Output RGBA, ARGB, RGB, gray and gray-alpha come
from the stored format (palette, tRNS and sub-8-bit samples expand, 16-bit samples drop to the
high byte for 8-bit output, Adam7 images are de-interlaced); rows are written
`outputBytesPerLine` apart, top to bottom or bottom to top. Not implemented: RGB to grayscale, partial output through `cbCtrlDisp`, the
`cellPngDecGet*` chunk accessors. Each stream's source, header and output format are logged with
the `dal: pngdec:` prefix.

## Default handlers

A system call without a handler, and an import without one, return 0 (`CELL_OK`) in `r3` and
change nothing else. The first call of each distinct syscall is logged as
`dal: default sc <number> <name>(r3..r6)`, and the first 8 calls of each import as
`dal: default hle <module>::<name>(r3..r8)` (none with `--quiet-hle`); later calls are only
counted. Returning success is a probe, not an implementation: guest code that depends on an
output parameter reads whatever was in memory, so a default-handled call can lead to a later
fault. The census shows which calls were default-handled, which is the work list for the next
handlers. For each default-handled import the runtime also logs the argument registers that look
like out-pointers (a nonzero 32-bit value that addresses committed memory outside the code), as
`dal: default hle <module>::<name>: possible out-pointer r<N>=<address>`, and the census repeats the
register list as `ptr r<N>,...`. These are hints for writing the handler, not facts: a register
left over from an earlier call can look like a pointer too.
