# Function overrides and hooks

Generated code is not edited by hand: the next `emit` replaces it. To change what a guest function
does, or to watch it, list its entry address in `config/overrides.json`. `emit` then generates the
code so that every path into that function reaches a native (host C++) handler that you write.
There are two kinds of entry:

- **override**: the guest body is replaced by the handler. The handler may still run the original
  body (`site->original`), before, after or instead of its own code.
- **hook**: the handler runs at function entry, then the original body runs.

Both apply to game code only. Imports (the `imp_<address>` stubs) are replaced by HLE handlers keyed
by module and FNID (see [Implemented imports](runtime.md#implemented-imports)); an entry on an
import stub is an error.

Use an override for code that waits on hardware the runtime does not model, or that should run
natively (a busy loop on an SPU mailbox, a hand-written copy loop). Use a hook to observe calls or to
adjust an argument before the original runs. "Run something after the original" and "skip the
original sometimes" are overrides that call `site->original` themselves.

The change is made at emit time: after editing the file, run `emit` again and rebuild. There is no
run-time switch in `dal_game`.

## Configuration file

`config/overrides.json` is tracked and ships with no entries. Another file can be chosen with
`emit --overrides PATH`.

```json
{
  "schema": 1,
  "executable_sha256": "<64 hex digits of the decrypted EBOOT.elf>",
  "comment": "optional, ignored",
  "entries": [
    {
      "address": "0x0001a2b0",
      "kind": "hook",
      "handler": "dalHookLogCall",
      "label": "frameStep",
      "comment": "optional, ignored"
    },
    { "address": "0x0001c000", "kind": "override", "handler": "skipSpuWait" }
  ]
}
```

| Key | Where | Meaning |
| --- | --- | --- |
| `schema` | top level, required | Must be `1` |
| `executable_sha256` | top level, required | SHA-256 of the ELF the addresses belong to. `emit` refuses a file made for another build, because a wrong address is hard to diagnose |
| `entries` | top level, required | Array of entries; may be empty |
| `comment` | top level and entries, optional | A string, ignored |
| `address` | entry, required | The function entry: `0x` and 1 to 8 hex digits, either case. Functions are selected by address because the function database has no names |
| `kind` | entry, required | `"override"` or `"hook"` |
| `handler` | entry, required | The C symbol of the handler: a letter followed by up to 63 letters, digits or underscores. It must not start with `fn_`, `imp_`, `L_` or `case_` (names of generated symbols) and must not be a C keyword. One handler can serve several entries |
| `label` | entry, optional | A non-empty free-text name. It appears in the generated comments, the start-up log, the census and the title comment of the original body when the function database has no name |

The file is a build input, so it is strict: JSON with no duplicate keys, and any other key is an
error (a typo must not silently do nothing). There can be one entry per address. A missing default
file (`config/overrides.json`) means no overrides; a file named with `--overrides` must exist.

### Errors

`emit` checks the whole file before it writes anything, so a bad file leaves the output untouched.
It prints `error: ` and the message, and exits with the code in the table. The checks run in this
order and the first failure wins:

| Check | Message | Exit |
| --- | --- | --- |
| File named with `--overrides` unreadable | `cannot read overrides file <path>: <reason>` | 1 |
| JSON syntax | `invalid overrides file <path>: invalid JSON: <message> (line L, column C)` | 10 |
| Duplicate key | `invalid overrides file <path>: duplicate key "k" in an object` | 10 |
| Structure | `invalid overrides file <path>: <where>: <problem>`, where `<where>` is `top level`, `entries[i]` or `entries[i].<key>` | 10 |
| Two entries with one address | `invalid overrides file <path>: entries[i] and entries[j] both name 0x0001a2b0` | 10 |
| Wrong executable | `overrides file was made for a different ELF (SHA-256 <file>, expected <elf>)` | 4 |
| Entry is an import stub | `overrides entry 0x000dcf50 is the import stub of <module>::<name or fnid>; imports are replaced by HLE handlers (runtime/src/hle), not by overrides` | 10 |
| Entry is not a function entry | `overrides entry 0x0001a2b4 is not a function entry (inside fn_0001a2b0)`, or `(outside every function)` | 10 |

The structure problems are `expected an object`, `missing key "k"`, `unknown key "k"`,
`unsupported schema <value>`, `expected a hex string like "0x0001a2b0", got <value>`, `expected
"override" or "hook", got <value>`, `"<h>" is not a valid handler name`, `"<h>" is reserved`,
`expected a non-empty string` (a label), `expected a string`, `expected an array` and, for
`executable_sha256`, `expected 64 hex digits, got <value>`.

## Emitter options and workflow

| Option | Effect |
| --- | --- |
| `--overrides PATH` | Read this file instead of `config/overrides.json`; it must exist |
| `--no-overrides` | Do not read any file and emit every function unchanged (A/B testing) |

The two options exclude each other (usage error, exit code 2). The workflow is: edit the file (or
write a scratch file and pass it with `--overrides`), run `emit` into the generated directory, and
build. `emit` rewrites a file only when its content changes, so the build recompiles only what
changed:

- Adding or removing an entry changes `ppu_funcs.h`, which every chunk includes, so every chunk
  recompiles.
- Changing the kind or the label of an existing entry rewrites `ppu_overrides.c` (when the function
  database has no name for the function, a changed label also rewrites the title comment of the
  original body in its chunk). Changing a handler name also
  rewrites `ppu_override_handlers.h` and `ppu_override_stubs.c`.

With no entries, or with `--no-overrides`, every `ppu_chunk_*.c`, `ppu_imports.c`, `ppu_tables.c`
and `ppu_funcs.h` is byte for byte what `emit` wrote before this feature existed. The same
generated tree can therefore be rebuilt for A/B comparison by running `emit` twice.

`emit` prints these lines after the `calls:` line, only when there is something to say:

```
overrides: 2 entries (override 1, hook 1)
overrides: 0x0001a2b0 branches to its own entry; that loop does not run the hook handler again
overrides: disabled by --no-overrides
```

The second line appears once per entry whose original body branches to its own entry (see
[Limitations](#limitations)). The report (`emit_report.json`) gains the key `overrides`:
`enabled` (false with `--no-overrides`), `file_sha256` (null when no file was read) and `entries`,
each with `address`, `kind`, `handler`, `label` and `self_entry_loop`.

## Generated code

Every generated function is `fn_<address>` and call sites name it. For an entry, `emit` renames the
original body to `fn_<address>_orig` and writes a wrapper under the name `fn_<address>`. Call sites
are not changed at all; they already name the wrapper. The wrapper is in `ppu_overrides.c`, next
to the table of entries:

```c
static uint64_t override_calls[2];

const DalPpuOverride kDalPpuOverrides[2] = {
    { 0x0001a2b0u, kDalOverrideKindHook, fn_0001a2b0_orig, &override_calls[0], "dalHookLogCall", "frameStep" },
    { 0x0001c000u, kDalOverrideKindOverride, fn_0001c000_orig, &override_calls[1], "skipSpuWait", nullptr },
};
const uint32_t kDalPpuOverrideCount = 2;

// hook dalHookLogCall: frameStep
DAL_PPU_FUNC(fn_0001a2b0) {
    DAL_ENTER(0x0001a2b0u);
    dalCountCall(&override_calls[0]);
    dalHookLogCall(ctx, base, &kDalPpuOverrides[0]);
    DAL_TAIL(fn_0001a2b0_orig);
}

// override skipSpuWait
DAL_PPU_FUNC(fn_0001c000) {
    DAL_ENTER(0x0001c000u);
    dalCountCall(&override_calls[1]);
    skipSpuWait(ctx, base, &kDalPpuOverrides[1]);
}
```

The wrapper records the entry in the call trace and the preemption budget (`DAL_ENTER`) once per
call. The original body omits that step, so the entry is recorded once also when an override
handler runs the original. The counter is incremented before the handler runs, so inside the handler `*site->calls` is the
1-based number of this call. The handler is a normal call; only the transfer into the original body
is a tail call, so a hook leaves no host stack frame behind. A guest tail-call cycle that passes
through a hooked function therefore does not grow the host stack.

Files that `emit` writes for this feature, in the output directory:

| File | Contents |
| --- | --- |
| `ppu_overrides.c` | The table `kDalPpuOverrides` and its count, the counters and the wrappers (always written; with no entries the table has one zero row and the count is 0) |
| `ppu_override_handlers.h` | One `DAL_OVERRIDE_HANDLER(<name>);` declaration per distinct handler, sorted by name. Handler sources include it, so a definition with the wrong signature does not compile |
| `ppu_override_stubs.c` | One stand-in definition per distinct handler, compiled only into the stub-runtime link test (see [Missing handlers](#missing-handlers)) |
| `ppu_funcs.h` | With entries, also `DAL_PPU_FUNC(fn_<address>_orig);` for each entry |
| `sources.cmake` | `ppu_overrides.c` in `DAL_PPU_GENERATED_SOURCES`, and the new variable `DAL_PPU_OVERRIDE_STUB_SOURCES` |

### Why every path reaches the wrapper

| Path into the function | How it reaches `fn_<address>` |
| --- | --- |
| Direct call (`bl`) | `DAL_CALL(fn_<address>)` |
| Direct tail transfer: `b`, a conditional branch, a fall-through from the previous function, a switch case that lies outside the function containing the switch | `DAL_TAIL(fn_<address>)` |
| Computed transfer (`bctrl`, `bctr`) | `dalDispatch` looks the address up in the table built from `kDalPpuFuncMappings`, whose row for the address names `fn_<address>` |
| Calls from the runtime (`callGuestOpd`, `callGuestFunction`, thread entry, callbacks) | `dalDispatch`, as above |

Nothing bypasses the name: every generated function is `noinline`, there is no link-time
optimisation, and each wrapper contains its own address constant, so the linker cannot merge it with
another function. The one exception is described under [Limitations](#limitations).

## Handlers

A handler has this signature, declared by the macro `DAL_OVERRIDE_HANDLER(name)` in
`dal/ppu_override.h`:

```c
void name(DalPpuContext* __restrict ctx, uint8_t* __restrict base, const DalPpuOverride* site);
```

`ctx` is the guest thread's register context, `base` the guest memory base (use it with the
`dalRead*` and `dalWrite*` helpers), and `site` describes the entry:

| Member of `DalPpuOverride` | Meaning |
| --- | --- |
| `guest` | The function entry address |
| `kind` | `kDalOverrideKindOverride` or `kDalOverrideKindHook` |
| `original` | The original body (`fn_<address>_orig`), a `DalPpuFunc*` |
| `calls` | The counter of this entry; read it with `__atomic_load_n(site->calls, __ATOMIC_RELAXED)` |
| `handler_name` | The handler as written in the file |
| `label` | The label, or `nullptr` |

The parameters are `[[maybe_unused]]`, so a handler that ignores some of them compiles under
`-Wall -Wextra`. Define a handler in C++ with `extern "C" DAL_OVERRIDE_HANDLER(name) { ... }` (without
`extern "C"` in C) and include `ppu_override_handlers.h`.

### Rules

- **On entry** the registers are as the caller left them: arguments in r3..r10, f1..f13 and
  v2..v13, r1 the guest stack pointer, r2 the caller's TOC (the module TOC for calls inside the
  executable), LR the return address of the guest call (for a tail transfer, the LR of the
  tail-caller's caller).
- **Hook:** runs before the original with the original's arguments. Leave every register as found
  unless the change is the purpose, for example rewriting an argument; the original then sees the
  change. Never call `site->original` from a hook, because it would run twice.
- **Override:** the handler is the function. Return values go in r3 (64-bit) and f1 (floating
  point). Preserve the non-volatile state that the guest compiler relies on: r1, r2, r13,
  r14..r31, f14..f31, v20..v31, `vrsave` and CR fields 2 to 4. LR and CTR may be changed.
  Returning from the handler returns to the guest caller.
- **Running the original from an override:** `site->original(ctx, base)`. It runs with the
  registers as they are at that moment and returns with its result in r3. For "after" behaviour,
  call the original and then act; for "conditional" behaviour, call it only sometimes.
- **Calling other guest functions:** `callGuestFunction(ctx, base, opd, {args...})`
  (`hle/hle.h`), which builds a frame below r1 and saves and restores r2 and LR.
- A handler must not throw (it is called from C code), and must not block except through the
  runtime's blocking waits: it runs on a guest thread under the global guest lock, with the same
  rules as an HLE handler (see [Threads](runtime.md#threads)). It reports an unrecoverable problem
  with `dal::fatal`. It must not keep `ctx` or `base` beyond the call.

### Generic handlers

The runtime core ships four handlers that need no game code:

| Handler | Use | Effect |
| --- | --- | --- |
| `dalHookNop` | hook | Does nothing; the census still counts the calls |
| `dalHookLogCall` | hook | Logs the first 8 calls of each entry (see [Log lines](#log-lines-and-census)) |
| `dalOverrideReturn0` | override | Sets r3 to 0 and returns: stubs a function out |
| `dalOverrideOriginal` | override | Calls the original: tests the mechanism without changing behaviour |

### Game handlers

Handlers for one game go into `runtime/src/game/overrides/*.cpp`. The build globs this directory
into `dal_game` (a new file is picked up by the next build without a manual reconfigure), so
adding a handler needs no CMake edit. The files are compiled with `dal_game`'s options and may use
everything `dal_game` links: for example `core/log.h` and `hle/hle.h`.

A complete example: an override that runs the original and caps its `int` result at 100. Save it as
`runtime/src/game/overrides/clamp_result.cpp`:

```cpp
#include <algorithm>
#include <cstdint>

#include "ppu_override_handlers.h"

// Runs the original function, then caps its int result at 100.
extern "C" DAL_OVERRIDE_HANDLER(clampResult) {
    site->original(ctx, base);
    const auto result = static_cast<std::int32_t>(ctx->gpr[3]);
    // A 32-bit int is returned sign-extended to 64 bits.
    ctx->gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::min(result, 100)));
}
```

and name it in the file:

```json
{ "address": "0x0001c000", "kind": "override", "handler": "clampResult", "label": "result cap" }
```

Then run `emit` and build.

### Missing handlers

A handler named in the file but not defined is a link error of `dal_game` that names the symbol.
For the `clampResult` handler above, the linker prints:

```
lld-link: error: undefined symbol: clampResult
>>> referenced by generated\ppu\ppu_overrides.c:17
>>>               dal_ppu_generated.lib(ppu_overrides.c.obj):(fn_0001c000)
```

The link test `ppu_link` still links: it also compiles `ppu_override_stubs.c`, whose stand-in for
each handler calls `dalStubOverrideHandler`. If a test ever ran such a stand-in, the stub runtime
would print `dal: override handler <name> for 0x<address> called in the stub runtime` and exit with
status 3. A `sources.cmake` written by an older `emit` (without `DAL_PPU_OVERRIDE_STUB_SOURCES`)
stops the configuration with `was written by an older emitter; re-run: python tools/ps3tool.py emit
<EBOOT.elf> --out <dir>`.

## Log lines and census

`dal_game` checks the table against the dispatch table right after registering the functions
(before `--load-only` returns), so a broken table stops the start-up with a fatal stop. A correct
`emit` never writes such a table; the messages are:

| Check | Message |
| --- | --- |
| Addresses strictly ascending | `override table: 0x0001c000 follows 0x0001c000 (unsorted or duplicate)` |
| Known kind | `override table: 0x0001c000 has unknown kind 7` |
| Original, counter and handler name present | `override table: entry 1 has no original, counter or handler name` |
| Address present in the function table | `override table: 0x0001c000 is not in the function table` |
| Dispatch names the wrapper | `override table: dispatch to 0x0001c000 reaches the original body, not the wrapper` |

If the table is valid, it logs, with the `[host]` prefix:

```
dal: overrides: none
dal: overrides: 2 entries (override 1, hook 1)
dal: overrides: hook 0x0001a2b0 dalHookLogCall "frameStep"
dal: overrides: override 0x0001c000 skipSpuWait
```

The first form is logged when there are no entries; otherwise the summary line is followed by one
line per entry, with the label in quotes when there is one.

`dalHookLogCall` logs its first 8 calls per entry, then stays silent:

```
[t 01000000] dal: hook 0x0001a2b0 "frameStep" call 1: r3=0x1 r4=0x0 r5=0x0 r6=0x0 lr=0x1f2c4
```

The line gives the call number, r3 to r6 and LR as the handler sees them (`"label"` is left out
when there is none).

On every stop the census has a section when there is at least one entry, after the audio section
and before the threads. Entries are sorted by call count, highest first, then by address, and an
entry with 0 calls is listed too, because "never reached" is information:

```
census: overrides 2 entries, 5 calls
  0x0001c000 override skipSpuWait: 5
  0x0001a2b0 hook dalHookLogCall "frameStep": 0
```

## Limitations

- **A branch to the function's own entry is a loop.** Inside the original body it is a `goto`, not
  a call, so the handler does not run again for each iteration. `emit` prints the line
  `overrides: 0x... branches to its own entry; ...` and sets `self_entry_loop` in the report for
  these functions.
- **An override that calls the original keeps two host frames** (wrapper and handler) while the
  original runs. In a guest tail-call cycle that passes through such an override, the host stack
  grows by those frames per cycle. Use a hook for functions in such cycles.
- **Imports cannot be overridden.** Use an HLE handler.
- **One entry per address**, and only function entries: no hook in the middle of a function, no
  "after" hook, and no conditional hook (use an override that calls the original).
- **A change needs `emit` and a rebuild.** There is no `dal_game` option that switches entries off;
  A/B testing is `emit --no-overrides` and a rebuild.
- **Selection is by address.** The address is only valid for the executable named by
  `executable_sha256`.

## Porting to another title

The emitter, the contract header, the start-up check, the census and the four generic handlers
carry no game knowledge. For another title, write that title's `config/overrides.json` (with its
own `executable_sha256`) and replace the contents of `runtime/src/game/overrides/`. No CMake or
emitter change is needed. See [Applying the method to another PS3 game](porting-guide.md).
