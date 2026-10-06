# DAL Rinne Utopia Recomp

Static recompilation of the PS3 title *Date A Live: Rinne Utopia* (BLJM61048) for
Windows, built from your own copy of the game.

This repository contains no game data. No executables, keys, or decrypted binaries are
included or accepted by version control; you supply your own `EBOOT.elf`, and all tool
output is written to ignored directories.

Current state: build scaffold, a small runtime stub, and analysis tools for the PPU
executable (ELF summary, SPU probe, disassembler, Ghidra cross-check, NID and syscall name
tables, function recovery). Recompilation itself is not implemented yet.

## Prerequisites

- Visual Studio 2026 with the Windows SDK (provides the MSVC headers and libraries)
- PowerShell 7 and the `VSSetup` module (`Install-Module VSSetup -Scope CurrentUser`)
- clang and clang++ with lld (a standalone LLVM install; see `-LlvmBin` below)
- CMake 3.29 or newer
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
pwsh -NoProfile -File tools/build.ps1 [-Preset debug|release] [-Clean] [-Test] [-EbootElf <path>] [-LlvmBin <dir>]
```

- `-Preset` selects the CMake preset (`debug` by default, or `release`). Build output goes
  to `build/<preset>/`.
- `-Clean` removes that preset's build directory before configuring.
- `-Test` runs CTest after the build: a runtime version check, a `musttail` support
  check, and the Python unit tests.
- `-EbootElf <path>` passes the decrypted ELF to the tests so the checks against the real
  executable run instead of being skipped.
- `-LlvmBin <dir>` puts a specific LLVM `bin` directory first on `PATH`. It defaults to the
  `DAL_LLVM_BIN` environment variable. Use it when the clang bundled with Visual Studio is
  too old or when clang is not on `PATH`.

The script enters the Visual Studio developer shell itself unless it is already running in
one, then configures, builds, and optionally tests.

Examples:

```powershell
pwsh -NoProfile -File tools/build.ps1 -Preset debug -Clean -Test
pwsh -NoProfile -File tools/build.ps1 -Preset release -Test -EbootElf game/EBOOT.elf
```

## Analysis tools

`tools/ps3tool.py` has eight subcommands. Each takes the path to the decrypted ELF, except
`nidgen`, which reads an RPCS3 checkout.

```sh
python tools/ps3tool.py info   <elf> [-o OUTPUT] [--target-config FILE] [--nids FILE] [--no-verify]
python tools/ps3tool.py probe  <elf> [-o OUTPUT] [--syscalls FILE]
python tools/ps3tool.py disasm <elf> (--start ADDR (--count N | --end ADDR) | --entry [--count N] | --stats [--start ADDR --end ADDR] [--json OUT]) [--raw] [--strict]
python tools/ps3tool.py ghidra-export <elf> [--ghidra-dir DIR] [--language ID] [--cspec ID] [--project-dir DIR] [-o OUT] [--start ADDR] [--end ADDR] [--timeout SEC] [--seeds-file PATH] [--check-only]
python tools/ps3tool.py ghidra-diff   <elf> --ghidra JSON [--start ADDR] [--end ADDR] [--strict] [-o OUT] [--top N] [--examples K]
python tools/ps3tool.py ghidra-summary <elf> --ghidra JSON [-o OUT]
python tools/ps3tool.py recover <elf> [--ghidra-summary PATH | --no-ghidra] [--extra-seeds PATH] [--nids FILE] [--syscalls FILE] [-o OUT] [--report PATH] [--seeds-out PATH] [--prologue-seeds] [--max-rounds N]
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
  extra seeds. It then partitions `.text` into functions until the set of functions no
  longer changes. It writes the function database (`-o`, default
  `generated/analysis/functions.json`), a report (`--report`, default
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
- `text` and `toc`: the `.text` range and the TOC base;
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
| `coverage` | Words owned and not owned in `.text`; uncovered runs classified as `zero`, `padding`, `jump_table`, `data` or `code`; prologue candidates by position |
| `ghidra` | Function extents compared with Ghidra's (`equal`, `ours_larger`, `ours_smaller`, `different`, `missing`), Ghidra functions cut at Cell-only vector instructions and whether ours extend past them, Ghidra code not owned by any function, data references by target class, Ghidra starts reached only by branches |
| `syscalls` | `sc` sites and the lv2 number resolved at each by walking the CFG back to the `r11` load, unresolved sites with reasons, and disagreements with `probe` |

Lists in the report are capped unless the field is documented as complete. `coverage`,
`ghidra` and `syscalls` are only present when the partition converged, and `ghidra` is
absent with `--no-ghidra`.

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
are skipped without it. The tests never run Ghidra themselves. The tests of `recover` that
use the real executable also need `DAL_EBOOT_ELF`.

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
| `runtime/` | Runtime library and executable stub (`dal_runtime`) |
| `tools/build.ps1` | Windows build wrapper |
| `tools/ps3tool.py` | Command-line entry point for the analysis tools |
| `tools/ps3recomp/` | ELF/PRX parsing, PPU decoder and formatter, probe, Ghidra export, diff and summary, function recovery (seeds, partition, jump tables, coverage, syscall resolution), name table generator |
| `tools/ghidra/` | Java `GhidraScript`s run by `ghidra-export` |
| `tools/tests/` | Unit tests |
| `config/target.json` | Pinned size and hashes of the target executable |
| `config/nids.json`, `config/lv2_syscalls.json` | FNID and lv2 syscall name tables generated from RPCS3 sources |
| `config/ghidra_summary.json`, `config/extra_seeds.json`, `config/functions.json` | Ghidra summary, extra function starts and the recovered function database; see "Function recovery" |
| `generated/` | Tool output (git-ignored) |
| `build/` | Build output (git-ignored) |
| `game/` | Suggested place for your own `EBOOT.elf` (git-ignored) |
