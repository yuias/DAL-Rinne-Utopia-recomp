# DAL Rinne Utopia Recomp

Static recompilation of the PS3 title *Date A Live: Rinne Utopia* (BLJM61048) for
Windows, built from your own copy of the game.

This repository contains no game data. No executables, keys, or decrypted binaries are
included or accepted by version control; you supply your own `EBOOT.elf`, and all tool
output is written to ignored directories.

Current state: build scaffold, analysis tools for the PPU executable (ELF summary, SPU probe,
disassembler, Ghidra cross-check, NID and syscall name tables, function recovery), an emitter
that turns the recovered functions into C source, and a runtime skeleton (`dal_game`) that
loads the executable, runs it through the generated code on real guest threads and stops at the
first missing piece with a census of what the game asked for. Most of the system API is not
implemented yet, so the game does not get past early start-up. See "Emitting C" and "Running
the game" for what is and is not implemented.

## Prerequisites

- Visual Studio 2026 with the Windows SDK (provides the MSVC headers and libraries)
- PowerShell 7 and the `VSSetup` module (`Install-Module VSSetup -Scope CurrentUser`)
- clang and clang++ with lld (a standalone LLVM install; see `-LlvmBin` below)
- CMake 3.29 or newer
- git, with the submodules initialised: `git submodule update --init` (spdlog under `externals/`;
  clone with `--recurse-submodules` to do it in one step)
- Ninja
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
  (`ppu_semantics`), the runtime core unit tests (`runtime_core`), the Python unit tests, and,
  with `-PpuDir`, the link check of the generated code (`ppu_link`). With `-PpuDir`,
  `-EbootElf` and `-GameDir` together it also runs `game_boot`; see "Running the game".
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
- `DAL_PPU_WERROR` (default `ON`) adds `-Werror` for generated code and the code that
  mirrors it (header check, semantics harness, link test). Every such target is compiled with
  `-O1 -march=x86-64-v3 -ffp-contract=off -fpatchable-function-entry=8 -Wall -Wextra`,
  whatever the preset's optimization level.

## Analysis tools

`tools/ps3tool.py` has nine subcommands. Each takes the path to the decrypted ELF, except
`nidgen`, which reads an RPCS3 checkout.

```sh
python tools/ps3tool.py info   <elf> [-o OUTPUT] [--target-config FILE] [--nids FILE] [--no-verify]
python tools/ps3tool.py probe  <elf> [-o OUTPUT] [--syscalls FILE]
python tools/ps3tool.py disasm <elf> (--start ADDR (--count N | --end ADDR) | --entry [--count N] | --stats [--start ADDR --end ADDR] [--json OUT]) [--raw] [--strict]
python tools/ps3tool.py ghidra-export <elf> [--ghidra-dir DIR] [--language ID] [--cspec ID] [--project-dir DIR] [-o OUT] [--start ADDR] [--end ADDR] [--timeout SEC] [--seeds-file PATH] [--check-only]
python tools/ps3tool.py ghidra-diff   <elf> --ghidra JSON [--start ADDR] [--end ADDR] [--strict] [-o OUT] [--top N] [--examples K]
python tools/ps3tool.py ghidra-summary <elf> --ghidra JSON [-o OUT]
python tools/ps3tool.py recover <elf> [--ghidra-summary PATH | --no-ghidra] [--extra-seeds PATH] [--nids FILE] [--syscalls FILE] [-o OUT] [--report PATH] [--seeds-out PATH] [--prologue-seeds] [--max-rounds N]
python tools/ps3tool.py emit <elf> [--db PATH] [--out DIR] [--chunks N] [--comments] [--report PATH] [--census] [--census-out PATH] [--max-unhandled N]
python tools/ps3tool.py nidgen --rpcs3 DIR [--source-commit COMMIT] [--nids-out FILE] [--syscalls-out FILE]
```

- `info` checks the file against the pinned SHA-256 and writes a JSON summary (segments,
  sections, imports, exports) to `generated/analysis/eboot_info.json`. It refuses encrypted
  input. `--no-verify` skips the hash check. Each import is given a name from the FNID
  table (`--nids`, default `config/nids.json`). The summary line reports how many imports
  are named, and every import without a name is listed as `unresolved import`. If the
  default table is missing, names stay `null` and a note is printed.
- `probe` searches for embedded SPU images and SPU-related imports, counts lv2 syscall
  sites and numbers, and writes `generated/analysis/probe.json`. The syscall number is
  recovered by scanning backwards from each `sc` for the `r11` load, through conditional
  branches; sites where it cannot be determined are counted as unresolved. Numbers are
  mapped to names with the table given by `--syscalls` (default
  `config/lv2_syscalls.json`).
- `disasm` prints PPU disassembly for an address range (`--start` with `--count` or
  `--end`) or from the entry function (`--entry`). `--raw` disables simplified mnemonics.
  `--strict` prints words whose reserved bits are set as `.long` instead of decoding them.
  `--stats` decodes every word of the executable segments (or of `[--start, --end)`) and
  reports the number of decoded and unknown words, the most common mnemonics and unknown
  opcode patterns, and a strict section: how many words still decode under reserved-bit
  checking and which mnemonics are rejected most often. `--json OUT` also writes the
  statistics as JSON.
- `ghidra-export` and `ghidra-diff` cross-check the decoder against Ghidra; see below.
- `ghidra-summary` and `recover` find and delimit functions; see "Function recovery".
- `emit` writes C source for the recovered functions; see "Emitting C".
- `nidgen` builds the name tables from RPCS3 sources; see below.

Run `python tools/ps3tool.py <command> --help` for the full option list.

### Ghidra cross-check

`ghidra-export` runs Ghidra headless on the ELF and writes a JSON file with the function
list and a per-word listing of the code range. `ghidra-diff` then compares that listing
with this repository's PPU decoder word by word. This is a validation aid for the decoder;
nothing else depends on it.

Requirements: Ghidra 12.x and a JDK 21 (the JDK that Ghidra 12 itself requires). Point the
tool at the Ghidra install directory with the `GHIDRA_INSTALL_DIR` environment variable or
`--ghidra-dir DIR`. The export scripts are Java `GhidraScript`s in `tools/ghidra/`, so no
Python environment for Ghidra is needed.

```sh
python tools/ps3tool.py ghidra-export <elf>
python tools/ps3tool.py ghidra-diff   <elf> --ghidra generated/ghidra/eboot_ghidra.json
python tools/ps3tool.py ghidra-diff   <elf> --ghidra generated/ghidra/eboot_ghidra.json --strict
```

- `ghidra-export` imports the ELF with language `PowerPC:BE:64:A2ALT-32addr` (override with
  `--language` and `--cspec`), runs auto-analysis, and exports the range given by
  `--start`/`--end` (default: the largest executable section). Ghidra is seeded with the
  entry point and the exported functions; `--seeds-file` adds more starts (see "Feeding
  recovered starts back to Ghidra"). Analysis of the real
  executable takes under a minute and is cut off after `--timeout` seconds (default 3600).
  The tool then checks that Ghidra decodes a few probe instructions (including AltiVec
  ones) as expected, so a wrong language setting is reported. `--check-only` skips Ghidra
  and only validates and language-checks an existing export.
- Output goes under `generated/ghidra/`: the export JSON (`eboot_ghidra.json`), the Ghidra
  project (`project/`) and the Ghidra logs (`logs/`). All of it is git-ignored.
- `ghidra-diff` classifies every word of the range into one category and prints the
  counts. The report is written to `generated/analysis/ghidra_diff.json` (`-o` to change
  it), with the most frequent mismatching pairs and a few examples for each.

  | Category | Meaning |
  | --- | --- |
  | `match` | Ghidra treats the word as code and both decoders agree |
  | `known_difference` | Both decode it, but the mnemonic pair is a recorded, explained difference |
  | `ghidra_code_we_unknown` | Ghidra treats the word as code and this decoder does not know it |
  | `mnemonic_mismatch` | Both decode it and the mnemonics differ |
  | `operand_mismatch` | Both decode it with the same mnemonic and the operands differ |
  | `we_code_ghidra_noncode` | Ghidra treats the word as data or undefined, this decoder decodes it |
  | `both_noncode_unknown` | Ghidra treats the word as data or undefined and this decoder does not know it |

  `ghidra_code_we_unknown`, `mnemonic_mismatch` and `operand_mismatch` are the categories
  that point at decoder bugs. The last two are only recorded.
- `--strict` makes `ghidra-diff` decode with reserved-bit checking, as `disasm --strict`
  does. Words that have reserved bits set then count as unknown to this decoder, so the
  category counts change accordingly.

## Function recovery

Function recovery uses Ghidra for function starts and data references, and this
repository's own decoder for function extents. Ghidra truncates some functions at the
Cell-only vector load/store instructions (`lvlx`, `lvrx`, `stvlx`, `stvrx` and their `l`
variants) that it cannot decode, so `recover` re-derives every function body itself from the
seeds. It also resolves `bctr` jump tables, recognises import thunks, resolves lv2 syscall
numbers over the recovered control-flow graph and reports coverage.

### Commands

```sh
python tools/ps3tool.py ghidra-export  <elf>
python tools/ps3tool.py ghidra-summary <elf> --ghidra generated/ghidra/eboot_ghidra.json -o config/ghidra_summary.json
python tools/ps3tool.py recover        <elf> -o config/functions.json
```

- `ghidra-summary` reduces the large `ghidra-export` JSON to a small file: function starts,
  names (only non-default ones), bodies, references from data into `.text`, Ghidra's code and
  data ranges, and the `bctr` sites. It records the SHA-256 of the ELF, the `.text` range and
  the Ghidra version and language, and prints a count for each kind of entry. The summary
  contains addresses and names only, never instruction words.
- `recover` needs no Ghidra installation. It reads the committed summary
  (`--ghidra-summary`, default `config/ghidra_summary.json`) and combines it with its own
  seeds: `bl` targets, function-descriptor (OPD) entries, the entry point, exports and
  extra seeds. It then partitions the code range into functions until the set of functions
  no longer changes. The code range is `.text` plus the executable sections that adjoin it
  (the small initializer and finalizer sections), up to but excluding the import stubs.
  `disasm`, `ghidra-diff` and `probe` still work on `.text` alone. It writes the function
  database (`-o`, default `generated/analysis/functions.json`), a report (`--report`, default
  `generated/analysis/recovery_report.json`) and the list of recovered starts that Ghidra
  lacks (`--seeds-out`, default `generated/analysis/ghidra_seeds.txt`).
- `--no-ghidra` runs `recover` without a summary, from the ELF's own seeds only. Without
  it, a missing default summary is an error that points at `ghidra-summary` and
  `--no-ghidra`.
- `--prologue-seeds` additionally seeds code that looks like a function prologue after a
  flow boundary. It is off by default; the report counts such candidates either way.
- `--max-rounds N` limits the partition rounds (default 64).
- `--nids` and `--syscalls` select the name tables (default: the committed ones).

`recover` exits with 4 if the summary does not match the ELF's hash or `.text` range, and
with 8 if the partition does not converge within `--max-rounds`. In the second case the
function database is not written.

### Extra seeds

`config/extra_seeds.json` lists function starts that neither Ghidra nor the `bl` scan
provides:

```json
{"schema": 1, "seeds": [{"addr": "0x000cc194", "reason": "..."}]}
```

`reason` is a free-text justification. `recover` reads the file by default when it exists
(`--extra-seeds PATH` selects another one, which must exist). The entries are counted as
`manual` seeds. Two kinds of reason occur in the committed file:

- a hand-written explanation, for a start that Ghidra missed and that would otherwise be
  absorbed into the body of a neighbouring function;
- `ghidra-feedback`, for starts that Ghidra reported only after being given the recovered
  starts (see below).

### Feeding recovered starts back to Ghidra

`recover` writes the starts it found that Ghidra's summary lacks to `--seeds-out`. Passing
that file to `ghidra-export --seeds-file` seeds a second Ghidra run with them. The file
holds one hex address per line; blank lines and `#` comments are ignored. The seeded run
needs its own project directory and output file so that the base export is left alone:

```sh
python tools/ps3tool.py ghidra-export  <elf> --seeds-file generated/analysis/ghidra_seeds.txt --project-dir generated/ghidra/project_seeded -o generated/ghidra/eboot_ghidra_seeded.json
python tools/ps3tool.py ghidra-summary <elf> --ghidra generated/ghidra/eboot_ghidra_seeded.json -o generated/ghidra/summary_seeded.json
python tools/ps3tool.py recover        <elf> --ghidra-summary generated/ghidra/summary_seeded.json -o generated/analysis/functions_seeded.json --report generated/analysis/recovery_report_seeded.json
```

Function entries in the seeded summary that are missing from `config/functions.json` are
candidates for `config/extra_seeds.json` with the reason `ghidra-feedback`. The committed
`config/ghidra_summary.json` always comes from the base export (entry point and exports as
the only seeds), so it does not depend on an earlier recovery run.

### Committed files and regeneration

| File | Produced by | Notes |
| --- | --- | --- |
| `config/ghidra_summary.json` | `ghidra-summary` on the base `ghidra-export` | Needs Ghidra and the ELF; regenerate when the Ghidra version, language setting or base seeding changes |
| `config/extra_seeds.json` | by hand, or from the feedback run | Small and reviewable; edit it directly |
| `config/functions.json` | `recover -o config/functions.json` | Input for later stages; needs only the ELF and the files above |

None of them contains instruction words or other code bytes. `generated/` is not committed:
the exports, the seeded export and the report stay local.

Run `recover -o config/functions.json` again whenever the recovery code, the summary, the
extra seeds or the name tables change. Two runs on the same inputs write byte-identical
files, so `git diff config/` shows exactly what the change affected. To rebuild all three
from scratch:

```sh
python tools/ps3tool.py ghidra-export  <elf>
python tools/ps3tool.py ghidra-summary <elf> --ghidra generated/ghidra/eboot_ghidra.json -o config/ghidra_summary.json
python tools/ps3tool.py recover        <elf> -o config/functions.json
```

### Function database

`config/functions.json` contains:

- `input`: the ELF SHA-256, the summary SHA-256 (`null` with `--no-ghidra`) and the number
  of extra seeds;
- `text`, `code` and `toc`: the `.text` range, the code range that was recovered and emitted,
  and the TOC base;
- `functions`: for each function its entry, a name (Ghidra's non-default name, otherwise
  `null`), the sources that found it (`bl`, `ghidra`, `ghidra_data_ref`, `opd`, `entry`,
  `manual`, and `prologue` with `--prologue-seeds`) and its address ranges;
- `import_thunks`: the import stubs, with module, FNID, resolved name and the slot they
  load from;
- `jump_tables`: resolved `bctr` tables with their branch site, table address, entry kind
  and targets;
- `unresolved_indirect`: indirect branches that were not resolved, each with a reason.

### Report

The report is JSON; the same information is summarised on stdout, one line per topic. Its
top-level sections:

| Section | Contents |
| --- | --- |
| `seeds` | Seed counts per source and the rejected candidates with reasons |
| `thunks` | Import thunk count, slot mismatches, thunks that no `bl` reaches |
| `jump_tables` | `bctr` sites, resolved tables and targets, unresolved sites by reason |
| `partition` | Rounds, convergence, functions by source, promoted shared code, tail-edge kinds, invalid flow, rejected calls, external targets, functions seeded only by an unowned `bl` |
| `coverage` | Words owned and not owned in the code range; uncovered runs classified as `zero`, `padding`, `jump_table`, `data` or `code`; prologue candidates by position |
| `ghidra` | Function extents compared with Ghidra's (`equal`, `ours_larger`, `ours_smaller`, `different`, `missing`), Ghidra functions cut at Cell-only vector instructions and whether ours extend past them, Ghidra code not owned by any function, data references by target class, Ghidra starts reached only by branches |
| `syscalls` | `sc` sites and the lv2 number resolved at each by walking the CFG back to the `r11` load, unresolved sites with reasons, and disagreements with `probe` |

Lists in the report are capped unless the field is documented as complete. `coverage`,
`ghidra` and `syscalls` are only present when the partition converged, and `ghidra` is
absent with `--no-ghidra`.

## Emitting C

`emit` turns the function database and the ELF into C source. The flow is `recover`, then
`emit`, then a build with `-PpuDir`:

```sh
python tools/ps3tool.py recover <elf> -o config/functions.json
python tools/ps3tool.py emit    <elf> --out generated/ppu --max-unhandled 0
pwsh -NoProfile -File tools/build.ps1 -PpuDir generated/ppu -Test
```

`emit` reads the function database (`--db`, default `config/functions.json`), checks that it
matches the ELF's hash and code range, and emits every function of the database as one C
function named `fn_<address>`. Branches inside a function become labels `L_<address>` and
`goto`, and resolved jump tables become `switch`. Direct calls are C calls, tail calls are
`musttail` returns, and transfers to computed targets go through `dalDispatch`. Backward
edges carry a hook (`DAL_LOOP`) that counts a preemption budget. Import stubs become
trampolines `imp_<address>` that call the runtime hook `dalHleCall` with an index into the
generated import table. The output is deterministic: two runs on the same inputs write
byte-identical files, and a file is only rewritten when its content changes.

### Output

Everything goes to `--out` (default `generated/ppu`; git-ignored):

| File | Contents |
| --- | --- |
| `ppu_chunk_NNN.c` | The function bodies, split into `--chunks` files (default 16) of similar instruction counts, so that they compile in parallel |
| `ppu_imports.c` | The import trampolines |
| `ppu_tables.c` | `kDalPpuFuncMappings` (guest address to host function, one row per function and per trampoline), `kDalPpuImports` (stub address, FNID, module, name) and the executable's SHA-256 |
| `ppu_funcs.h` | Declarations of the functions and trampolines |
| `sources.cmake` | The list of sources that `DAL_PPU_GENERATED_DIR` includes |

Chunk files that a smaller `--chunks` makes unnecessary are removed. `--comments` annotates
the code with the guest disassembly; it is off by default because it makes the files larger.

The report (`--report`, default `generated/analysis/emit_report.json`) is JSON, and
`emit` prints a summary on stdout:

| Field | Contents |
| --- | --- |
| `functions`, `instructions`, `import_trampolines` | Emitted counts |
| `thunks_without_toc_save` | Import stubs that do not save the caller's TOC pointer |
| `chunks`, `largest_functions`, `output_bytes` | Size of the output |
| `unhandled` | Words emitted as a call to `dalUnhandled`: total, per mnemonic, per group, with example addresses |
| `edges` | Calls, tail calls, switches, syscalls and traps by kind, for example `calls_direct` and `calls_indirect` |
| `dynamic_targets` | Transfers whose constant target is not a recovered function or stub, with site and target |
| `labels`, `loop_hooks`, `elapsed_s` | Emitted labels, loop hooks, run time |

`--census` only counts the owned instructions by mnemonic and exits, without writing C.
The census (`--census-out`, default `generated/analysis/emit_census.json`) lists each
mnemonic with its count and handler group. `--max-unhandled N` makes `emit` exit with 9 if
more than `N` words are emitted as unhandled. For the committed function database and the
pinned ELF, the code range holds 206065 words of 168 distinct mnemonics in 2531 functions,
`emit` writes 16 chunks and 166 import trampolines, and `--max-unhandled 0` exits with 0.

### Contract header and stub runtime

Generated code depends only on the headers in `runtime/include/dal/` (`dal/ppu.h` includes
all of them). `ppu_context.h` is the contract between generated code and a runtime:

- the CPU context `DalPpuContext` (registers, condition register and XER as one byte per bit,
  FPSCR, VSCR, reservation, call trace) and the vector and FPR types;
- guest memory accessors for a flat big-endian 32-bit address space, where `base` must point
  at a reservation of 4 GiB plus a guard area;
- the hooks the runtime implements: `dalDispatch`, `dalHleCall`, `dalSyscall`, `dalYield`,
  `dalUnhandled`, `dalTrap`, the reservation and cache operations, `dalMftb` and the FPSCR
  moves.

`ppu_int.h`, `ppu_fp.h` and `ppu_vmx.h` hold the inline helpers that emitted code calls.
Vector registers are stored byte-reversed relative to guest memory; the `DAL_VR_*`
accessors take big-endian element numbers. Most helpers cite the RPCS3 interpreter handler
they follow.

`runtime/src/ppu_stub_runtime.cpp` (library `dal_ppu_stub`) implements the hooks only far
enough to link and test generated code: dispatch works, and every hook that would need the
system (HLE calls, unknown targets, unhandled words, traps) prints a message and exits with
an error status. Syscalls only set `r3` to 0. `ppu_stub.h` holds test-only additions. The stub
is only for the link check, the header check and the semantics harness; the runtime that runs the
game is described in "Running the game".

### Semantics harness

`tools/tests/ppu_semantics/` holds hand-written cases, one module per instruction group
(`cases_<group>.py`). `tools/tests/ppu_semantics/gen.py` assembles each case, runs it through
the emitter's own `emit_function`, and writes `ppu_semantics_cases.{h,c}` into the build
directory. `runtime/tests/ppu_semantics_main.c` runs every case natively: it sets the initial
state, calls the emitted function, and compares registers, flags and memory with the expected
state. CTest runs it as `ppu_semantics`; it needs neither the ELF nor a generated tree.

To add a case, append to `CASES` in the module of the instruction's group:

```python
case("addi_wraps_64", "ADDI", enc("addi", rD=3, rA=4, SIMM=1), {"r4": M64}, {"r3": 0})
```

The arguments are a unique name (`[a-z0-9_]+`), the reference (the RPCS3 handler or the
architecture rule the expected value comes from), the instruction words (built with
`tools/tests/ppc_asm.py`), the initial state and the expected state. State keys are `rN`,
`fN` and `vN` registers, `crN` field nibbles, `lr`, `ctr`, `vrsave`, `fpscr`, and the flags
`xer_so`, `xer_ov`, `xer_ca`, `vscr_sat` and `vscr_nj`. `Case` in
`tools/tests/ppu_semantics/__init__.py` also takes memory images (`mem_init`, `mem_expect`),
extra callee functions and switch targets. Expected values are derived by hand, not by
running the emitter.

### What is and is not implemented

Every instruction word in the code range of the pinned ELF has an emitter handler, so
`unhandled` in the report is 0. Known deviations from the architecture or from RPCS3:

- FPSCR is modelled only as far as RPCS3 models it. `fcmpu` and `fcmpo` write the FPCC
  field; `mffs`, `mtfsf`, `mtfsfi`, `mtfsb0` and `mtfsb1` move the raw image. Exception
  flags, the enables and the rounding-control bits are never derived from an operation or
  acted on: rounding is always to nearest even and non-IEEE mode is not modelled. `mcrfs`
  does not clear the copied exception bits. With Rc=1, CR1 receives FPSCR bits 0..3, as the
  architecture defines (RPCS3 writes other bits).
- Scalar floating-point arithmetic is host double arithmetic. The sign of a default NaN that
  an invalid operation produces is the host's (negative on x86-64); it is not normalized to
  the PowerPC value. The vector floating-point helpers do normalize: a NaN result becomes
  `0x7fc00000`.
- `vsl` and `vsr` use the low three bits of byte 15 of `vB` as the shift count, which is the
  architectural definition (the result is undefined there when the bytes of `vB` differ).
  RPCS3 shifts each byte by its own count, so the two differ for such inputs.
- VSCR keeps NJ and SAT as separate fields in the context. `mfvscr` and `mtvscr` place them at
  bits 16 and 0 of word 3 of the register in big-endian numbering, as in the AltiVec manual.
  RPCS3's interpreter and its LLVM translator disagree on that word, and the manual's
  position is used.
- `fres`, `frsqrte` and the vector estimate instructions return exactly computed results,
  which lie within the architected estimate error, not the hardware estimate values.
- The stub runtime does not implement import calls, lv2 syscalls, threads or memory
  allocation: it exits on import calls and returns 0 from syscalls. The runtime in `dal_game`
  implements a subset; see "Running the game".

## Running the game

`dal_game` loads the decrypted `EBOOT.elf` into a 4 GiB guest address space, sets up the main
PPU thread the way RPCS3 does and calls the entry function through the generated code. It is
built only when `DAL_PPU_GENERATED_DIR` is set (`-PpuDir`), because it links the generated
functions. The runtime core (`dal_runtime_core`, everything under `runtime/src/core`,
`runtime/src/lv2` and `runtime/src/hle`) does not reference generated symbols and is always
built, together with its unit tests (`runtime_core`). Only `runtime/src/game_main.cpp` passes the
generated tables to the core. The executable is checked against the SHA-256 that the generated
code was emitted from.

```sh
dal_game --game-dir DIR [--elf PATH] [--seconds N] [--strict] [--log FILE] [--quiet-hle] [--load-only]
```

| Option | Meaning |
| --- | --- |
| `--game-dir DIR` | Required. The game directory, the one that contains `PS3_GAME`. It must exist. Nothing is read from it yet apart from the default ELF location. |
| `--elf PATH` | The decrypted executable. Default: `DIR/PS3_GAME/USRDIR/EBOOT.elf`. |
| `--seconds N` | Watchdog: stop after `N` seconds. 0 (the default) disables it. |
| `--strict` | Treat a jump to an address that is not a recovered function as a fatal error (see "Dispatch"). |
| `--log FILE` | Also write every log line to `FILE`, which is truncated. |
| `--quiet-hle` | Do not log the first calls of default-handled imports. The counts in the census are unaffected. |
| `--load-only` | Load and verify the executable, print the load summary and exit with 0 without running any guest code. |

Game paths are command-line arguments only; no tracked file contains one. The build wrapper takes
them as `-EbootElf` and `-GameDir` for the tests (CMake cache variables `DAL_EBOOT_ELF` and
`DAL_GAME_DIR`).

### Exit codes and stop lines

Every run ends with one stop line on stderr and a census (below), then exits:

| Code | Meaning | Stop line |
| --- | --- | --- |
| 0 | The guest called `sys_process_exit` or `_sys_process_exit` (also `--load-only` after loading) | `dal: stop: process exit status N` |
| 2 | Usage error: bad option, missing `--game-dir`, game directory missing, log file not writable | `error: ...` (usage text) |
| 3 | Fatal error: unhandled instruction, trap, guest fault (access outside the committed guest memory), unknown dispatch target with `--strict`, guest address space cannot be reserved | `dal: stop: fatal <kind>: ...` |
| 4 | Load error: file missing, not a PPU executable, or SHA-256 different from the one in the generated code | `error: ...` |
| 5 | The `--seconds` watchdog elapsed | `dal: stop: watchdog N s` |
| 6 | Stall: every live guest thread has been in an indefinite wait for at least 2 seconds | `dal: stop: stall (N threads blocked)` |
| 7 | All guest threads ended without a process exit | `dal: stop: no guest threads left` |

A fatal stop also prints the registers and the call trace (the most recent function entries) of
the faulting guest thread. The runtime never calls `abort`: the CRT and Windows error dialogs are
disabled, so a run always ends with an exit code, also when it is started from a script.

### Logging

Log lines go to stderr, and to `--log FILE` when given, one line each. The logger is a thin layer
over spdlog (the `externals/spdlog` submodule, built as a static library with `std::format`
instead of the bundled fmt); spdlog types do not appear in the runtime's own headers. A line is
prefixed with `[host]` or with `[t XXXXXXXX]`, the id of the guest thread that wrote it, followed
by `warn: ` or `error: ` for those levels. Output from the guest itself (`sys_tty_write`) goes to
stdout, unprefixed.

### Census

On every stop the runtime prints what the game did, sorted by count and then by name:

```
census: syscalls 18 calls, 14 distinct, 2 default-handled
  sc 141 sys_timer_usleep: 1 [impl]
  sc 169 sys_spu_initialize: 1 [default]
census: hle 118 calls, 21 distinct, 10 default-handled
  sysPrxForUser::sys_lwmutex_lock: 29 [impl]
  cellSysmodule::cellSysmoduleLoadModule: 15 [default]
census: unknown targets 0 distinct
census: threads 5 created, 5 live
  0x01000000 main_thread: running
  0x01000001 _SPU_printf_server: blocked in sys_event_queue_receive
```

`[impl]` marks a call that an implemented handler served and `[default]` one that the default
handler took (see below). An unknown target is printed as `0x<target> from 0x<site>: count`.
Thread states are `running`, `blocked in <what>` or `exited`. The first call of each syscall and
each import also logs `dal: first sc <number> <name>` or `dal: first hle <module>::<name>` while
the game runs; these lines are the progress markers that `game_boot` checks.

### Memory map

The guest space is one reservation of 4 GiB plus a guard area, big-endian, accessed only through
the `dalRead*` and `dalWrite*` helpers. Pages are committed on demand by the allocators below. An
access to an uncommitted page raises a fault that the runtime turns into a fatal stop with the
guest address and the access kind.

| Range | Use |
| --- | --- |
| `0x00010000..0x10000000` | Main region: the ELF image (committed at its segment addresses), per-thread TLS blocks and the heap handlers' allocations. 64 KiB granularity. |
| `0x10000000..0x20000000` | Not used yet (left free for the RSX context). |
| `0x20000000..0x30000000` | User memory with 1 MiB pages (`sys_memory_allocate` with flags 0x400 or 0). |
| `0x30000000..0x40000000` | User memory with 64 KiB pages (`sys_memory_allocate` with flags 0x200). |
| `0xD0000000..0xE0000000` | Thread stacks, 4 KiB pages, with an uncommitted 4 KiB guard page on each side of every stack. |

The primary stack size from the executable's process parameters is clamped as RPCS3 does (the
game's 0x8000 becomes 0x10000). The argument block (`argv`, `envp`, strings) sits at the top of
the stack. `sys_memory_get_user_memory_size` reports a 0xD500000 container. The timebase runs at
79.8 MHz, scaled from the host performance counter.

### Dispatch

`dalRegisterFuncs` builds a flat table indexed by guest address from the generated function
mappings, so a computed jump or an indirect call costs one lookup. A target that is not in the
table is an unknown target: the first occurrence of each (target, call site) pair is logged with
the call trace and counted in the census. With `--strict` it is a fatal stop. Without it the
dispatch returns, which acts like a callee that returned immediately, so the run goes on and the
census lists every such target.

### Threads

Each guest thread is a host thread (16 MiB host stack) with its own context. All guest code runs
under one global guest lock, a FIFO ticket lock, so guest threads never run in parallel. A thread
gives up the lock only in `dalYield` (called from loop hooks, when another thread is waiting), in
a blocking wait and when it exits. Blocking system calls (mutex, condition variable, event flag,
event queue, sleep, join) wait on host condition variables with the guest lock, and record what
they wait for for the census and for stall detection.

Priority scheduling is deferred. Runnable threads get the lock in arrival order, and
`sys_ppu_thread_set_priority` and `sys_ppu_thread_get_priority` only store and return the value.
Thread ids start at `0x01000000`. Interrupt threads are not implemented.

The host main thread is the supervisor. After boot it checks every 100 ms for the watchdog, a
stall and the absence of live guest threads.

### Implemented system calls

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
| Event queue and port | `sys_event_queue_create` (128), `_destroy` (129), `_receive` (130), `_drain` (133); `sys_event_port_create` (134), `_destroy` (135), `_connect_local` (136), `_disconnect` (137), `_send` (138) |
| Thread | `_sys_ppu_thread_exit` (41), `sys_ppu_thread_yield` (43), `sys_ppu_thread_join` (44), `sys_ppu_thread_set_priority` (47), `sys_ppu_thread_get_priority` (48) |

### Implemented imports

Imports are matched to handlers by module and FNID. All 23 `sysPrxForUser` imports of the
executable have handlers:

- `sys_initialize_tls` (per-thread TLS blocks laid out as RPCS3 does), `sys_ppu_thread_create`,
  `sys_ppu_thread_get_id`, `sys_ppu_thread_once`, `sys_ppu_thread_exit`;
- `sys_lwmutex_create`, `_destroy`, `_lock`, `_trylock`, `_unlock`;
- `_sys_heap_create_heap`, `_sys_heap_malloc`, `_sys_heap_memalign`, `_sys_heap_free`,
  `_sys_heap_delete_heap`;
- `sys_time_get_system_time`, `sys_process_exit`;
- `_sys_process_atexitspawn`, `_sys_process_at_Exitspawn`, `sys_prx_exitspawn_with_level`,
  `sys_prx_register_library`, `_sys_spu_printf_initialize`, `_sys_spu_printf_finalize`. These
  return `CELL_OK` and do nothing else.

The executable has 166 imports; the other 143 (cellSysmodule, cellSysutil, cellGcmSys, cellSpurs
and others) use the default handler.

### Default handlers

A system call without a handler, and an import without one, return 0 (`CELL_OK`) in `r3` and
change nothing else. The first call of each distinct syscall is logged as
`dal: default sc <number> <name>(r3..r6)`, and the first 8 calls of each import as
`dal: default hle <module>::<name>(r3..r8)` (none with `--quiet-hle`); later calls are only
counted. Returning success is a probe, not an implementation: guest code that depends on an
output parameter reads whatever was in memory, so a default-handled call can lead to a later
fault. The census shows which calls were default-handled, which is the work list for the next
handlers.

### The `game_boot` test

CTest registers `game_boot` only when `DAL_PPU_GENERATED_DIR`, `DAL_EBOOT_ELF` and
`DAL_GAME_DIR` are all set (`-PpuDir`, `-EbootElf`, `-GameDir`); otherwise configuration prints
that it is not registered. It runs `dal_game --game-dir <dir> --elf <elf>` with the arguments
from the pin file, with a 60 s time limit, and passes when the output matches the milestone
regular expression. The exit code is not checked: while the boot path is incomplete the run is
expected to end in a fatal stop somewhere after the milestone, and a stop before the milestone
leaves the pattern unmatched.

The pin file `cmake/game_boot.cmake` has two lines:

```cmake
set(DAL_GAME_BOOT_ARGS "--seconds 20")
set(DAL_GAME_BOOT_MILESTONE "dal: first hle cellGcmSys::cellGcmGetTiledPitchSize")
```

When a change lets the game get further, run `dal_game --game-dir <dir> --seconds 20`, take the
last `dal: first ...` line that appears on every run and set `DAL_GAME_BOOT_MILESTONE` to it. If
repeated runs differ, pin the latest line that all of them reach. Update only that file; the test
then guards against regressions.

### Current state

On the pinned executable the run loads, initializes thread-local storage, starts four more guest
threads (all of which block as expected: an SPU printf server on an event queue, a timer thread,
and two threads on event flags), calls into cellSysmodule, cellGcmSys, cellSpurs and cellSysutil
through the default handler, and stops in the main thread with a guest fault (read from address 0,
exit code 3) inside the game's assert helper. The cellGcmSys calls before it are still
default-handled, which is the next thing to implement.

### Not implemented

- Most HLE modules: cellSysmodule, cellSysutil, cellGame, cellGcmSys, cellSpurs and the rest
  use the default handler.
- File system: the `sys_fs_*` system calls use the default handler, and there is no virtual file
  system.
- Graphics (RSX, video memory at `0xC0000000`), audio, input.
- SPU and SPURS: SPU system calls use the default handler.
- Priority scheduling, interrupt threads, savestates.

## NID and syscall name tables

Imports are identified by FNID and lv2 syscalls by number. `nidgen` extracts both name
tables from the RPCS3 sources, which are not included in this repository. The generated
tables are committed:

- `config/nids.json`: module name, then FNID, to function name.
- `config/lv2_syscalls.json`: syscall number to name.

Each file records the RPCS3 repository URL and the commit it was generated from, so the
names can be traced to a specific revision. Regenerate them when the RPCS3 sources change:

```sh
git clone --depth 1 --filter=blob:none --sparse https://github.com/RPCS3/rpcs3.git <rpcs3-dir>
git -C <rpcs3-dir> sparse-checkout set rpcs3/Emu/Cell
python tools/ps3tool.py nidgen --rpcs3 <rpcs3-dir>
```

`--rpcs3` is the checkout root. The commit is read from the checkout's `.git` directory
(`--source-commit` overrides it). `--nids-out` and `--syscalls-out` select other output
paths, for example to compare a regenerated table with the committed one before replacing
it. `nidgen` only reads text files in the checkout and never executes anything from it. It
prints macro counts and any constructs it could not handle, and exits with a non-zero
status if the syscall table disagrees with its own comments.

## Tests

Python unit tests use synthetic ELF files built in memory. Tests that need the real
executable are skipped unless `DAL_EBOOT_ELF` points at it. The Ghidra comparison tests
additionally need `DAL_GHIDRA_JSON`, the path of an export written by `ghidra-export`, and
are skipped without it. The tests never run Ghidra themselves. The tests of `recover` and
`emit` that use the real executable also need `DAL_EBOOT_ELF`; the `emit` tests assert the
counts above and that no word is unhandled.

```sh
python -m unittest discover -s tools/tests -t tools -v
DAL_EBOOT_ELF=game/EBOOT.elf python -m unittest discover -s tools/tests -t tools -v
DAL_EBOOT_ELF=game/EBOOT.elf DAL_GHIDRA_JSON=generated/ghidra/eboot_ghidra.json python -m unittest discover -s tools/tests -t tools -v
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
| `runtime/` | Runtime executable stub (`dal_runtime`), stub runtime for generated code (`dal_ppu_stub`), the runtime core and `dal_game` (`runtime/src/core`, `lv2`, `hle`, `game_main.cpp`; see "Running the game") and runtime-side tests |
| `externals/spdlog` | spdlog submodule used by the runtime logger |
| `cmake/game_boot.cmake` | Pin file of the `game_boot` test: arguments and milestone |
| `tools/build.ps1` | Windows build wrapper |
| `tools/ps3tool.py` | Command-line entry point for the analysis tools |
| `tools/ps3recomp/` | ELF/PRX parsing, PPU decoder and formatter, probe, Ghidra export, diff and summary, function recovery (seeds, partition, jump tables, coverage, syscall resolution), name table generator |
| `tools/ghidra/` | Java `GhidraScript`s run by `ghidra-export` |
| `tools/ps3recomp/emit*.py` | The `emit` command: function DB loader and census, per-instruction handlers by group, output writer and report |
| `tools/tests/` | Unit tests |
| `tools/tests/ppu_semantics/` | Semantics harness cases and the C generator; see "Semantics harness" |
| `runtime/include/dal/` | Contract header and inline helpers for generated code |
| `config/target.json` | Pinned size and hashes of the target executable |
| `config/nids.json`, `config/lv2_syscalls.json` | FNID and lv2 syscall name tables generated from RPCS3 sources |
| `config/ghidra_summary.json`, `config/extra_seeds.json`, `config/functions.json` | Ghidra summary, extra function starts and the recovered function database; see "Function recovery" |
| `generated/` | Tool output, including `generated/ppu` from `emit` (git-ignored) |
| `build/` | Build output (git-ignored) |
| `game/` | Suggested place for your own `EBOOT.elf` (git-ignored) |
