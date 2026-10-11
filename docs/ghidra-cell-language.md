# Ghidra language for the Cell PPU

Ghidra's stock PowerPC languages cannot decode the eight vector load/store instructions that only the
Cell PPU has (`lvlx lvlxl lvrx lvrxl stvlx stvlxl stvrx stvrxl`). Ghidra's disassembly stops at each
of them, so every function that contains one is cut short. This repository therefore ships the sources
of a Ghidra language module, `CellPPU`, that adds the eight instructions with real p-code to the stock
`PowerPC:BE:64:A2ALT-32addr` language. The new language id is `PowerPC:BE:64:CellPPU-32addr`.

The module is not specific to one game: any PS3 PPU executable can be analysed with it. Nothing changes
unless it is asked for: `ghidra-export` without `--cell` still uses the stock language, and the
repository never writes into a Ghidra install or a Ghidra settings directory by itself.

The pieces, in the order a user meets them:

1. `ghidra-cell-lang build` stages a self-contained module and compiles it ([Building](#building)).
2. `ghidra-cell-lang verify` checks a language without the game ([Verifying](#verifying)).
3. `ghidra-export --cell [--staged]` analyses an ELF with the language ([Using the language](#using-the-language)).
4. The GUI and runs outside this repository's tools need the module installed ([Installing](#installing)).

## The instructions

All eight are X-form, primary opcode 31, with operands `vD,rA,rB` (loads) or `vS,rA,rB` (stores) and
the reserved bit 31 clear. The decoder in `tools/ps3recomp/ppu_vmx.py` is the source of truth for the
encodings and `runtime/include/dal/ppu_vmx.h` for the semantics. With `sh = ea & 15`, `ea = (rA|0) + rB`
and `blk` the aligned 16-byte block at `ea & ~15` (byte 0 is the most significant):

| Instruction | XO | Semantics |
| --- | --- | --- |
| `lvlx` | 519 | `vD = blk << 8*sh` (bytes `ea` to the block end, left-justified, the rest zero) |
| `lvlxl` | 775 | as `lvlx`; the `l` form only adds a cache hint |
| `lvrx` | 551 | `vD = sh ? blk >> 8*(16-sh) : 0` (bytes from the block start to `ea-1`, right-justified, the rest zero) |
| `lvrxl` | 807 | as `lvrx`; cache hint |
| `stvlx` | 647 | writes register bytes `0 .. 15-sh` to `ea` up to the block end |
| `stvlxl` | 903 | as `stvlx`; cache hint |
| `stvrx` | 679 | writes register bytes `16-sh .. 15` to the block start up to `ea-1`; nothing when `sh == 0` |
| `stvrxl` | 935 | as `stvrx`; cache hint |

The same eight names are the Cell-only forms of the disassembler's opcode table. Their p-code is
written with shifts only (no 16-byte literals), so the decompiler shows readable C for them.

### Other encodings that were considered

| Candidate | Stock `A2ALT-32addr` | Decision |
| --- | --- | --- |
| `lvlx`, `lvrx` | not decoded | in |
| `stvlx`, `stvlxl`, `stvrx`, `stvrxl` | not decoded | in (other titles use the store forms for unaligned stores) |
| `lvlxl`, `lvrxl` | decoded as the unrelated embedded `stvepxl` (XO 775) and `stvepx` (XO 807) | in |
| stock `stvepx`, `stvepxl` (embedded external-PID stores) | the stock constructors at XO 807 and 775 | shadowed in this language: not Cell instructions and not in this repository's decoder |
| VMX128 (opcodes 4, 5, 6 with 128 vector registers) | n/a | out: an Xbox 360 (Xenon) extension, not part of the Cell PPU |
| `mfspr`/`mtspr` of user SPRs (LR, CTR, XER, VRSAVE), `mftb`, `mtocrf`, `mfocrf`, `sync 1` | decoded | out; the `verify` table check covers every row of the decoder |
| Cell privileged and hypervisor SPRs (HID*, TSCR, CTRL, ...), `attn` | n/a | out: user-mode executables cannot use them |
| `dcbz` (128-byte line on the Cell) | decoded | out: the line size affects semantics only |
| `lvsl`, `lvsr`, `dst`, `dstst`, `dss` | decoded | out: standard AltiVec, confirmed by `verify` |
| `sc` words with a non-zero LEV field | data | out: not Cell-specific and never code |

## Files

```
tools/ghidra/CellPPU/
  Module.manifest                  empty; marks a Ghidra module
  extension.properties             name=CellPPU, description, author, createdOn, version=@extversion@
  data/languages/cell.ldefs        the language entry
  data/languages/ppc_cell.slaspec  includes the stock AltiVec PowerPC slaspec, then cell_vmx.sinc
  data/languages/cell_vmx.sinc     the eight constructors and their macros
tools/ghidra/CellLangCheck.java    Ghidra script behind `verify`
tools/install-ghidra-cell.ps1      optional installer
```

- The tracked tree alone does not compile: `ppc_cell.slaspec` and `cell.ldefs` name stock files by bare
  file name (`ppc_64_isa_altivec_be.slaspec`, `ppc_64.pspec`, `ppc_64_32.cspec`, `ppc.dwarf`), which
  resolve only after `build` has put them next to our files. This keeps Apache-2.0 files out of the
  repository and uses exactly the stock files of the Ghidra version in use. `build` is the only way to
  compile the module.
- `cell.ldefs` copies the stock `A2ALT-32addr` entry (processor `PowerPC`, endian big, size 32,
  `truncate_space` of `ram` to 4 bytes, compiler `default`, the same `external_name` entries) and
  changes the id, variant (`CellPPU-32addr`), SLEIGH file (`ppc_cell.sla`), description and version
  (`1.0`; Ghidra records it in programs, so it is raised when constructors or p-code change). Keeping
  the processor name and the register set means Ghidra's PowerPC ELF support and analyzers apply
  unchanged. `manualindexfile` is left out because it points into the stock module's manuals.
- Every constructor in `cell_vmx.sinc` starts with `$(NOTVLE) &` (context `vle=0`, which always holds
  in this language). Stock `stvepx` and `stvepxl` have the same patterns as `lvrxl` and `lvlxl`, and
  identical patterns are a SLEIGH error; the extra constraint makes ours more specific, so ours win and
  the two embedded instructions no longer decode in this language.

## Building

```sh
python tools/ps3tool.py ghidra-cell-lang build [--ghidra-dir DIR] [--src DIR] [--out DIR] [--no-zip]
```

Requirements: Ghidra 12.x (written against 12.1.4). `--ghidra-dir` or `GHIDRA_INSTALL_DIR` names the
install, as for `ghidra-export`. No game file is needed.

| Option | Meaning |
| --- | --- |
| `--ghidra-dir DIR` | The Ghidra install directory (default: `GHIDRA_INSTALL_DIR`). |
| `--src DIR` | Module sources with the layout above (default `tools/ghidra/CellPPU`); another module source works, for example a modified copy. |
| `--out DIR` | Output directory (default `generated/ghidra/cell`). |
| `--no-zip` | Do not write the extension zip. |

Steps, first failure wins:

1. Read `<ghidra>/Ghidra/application.properties` for the version and release name.
2. Read the stock `ppc.ldefs`, find `PowerPC:BE:64:A2ALT-32addr` and check that it still names the files
   our sources were written against (SLEIGH file, processor spec, compiler specs).
3. Stage `<out>/CellPPU`. Only that directory is deleted first, never anything else under `<out>`.
   - Copied from `<ghidra>/Ghidra/Processors/PowerPC/data/languages/`: the regular files directly in it
     (`.sinc`, `.slaspec`, `.pspec`, `.cspec`, `.dwarf`).
   - Not copied: `*.ldefs` and `*.opinion` (they would register duplicate language ids and loader
     opinions), `*.sla` (a stale compiled file must not be reused), `*.lang` and `*.trans` (Ghidra
     finds those in every module), and the `old/` subdirectory.
   - Copied from the stock module's `data/`: `sleighArgs.txt` (empty; Ghidra uses it when it recompiles
     at load time).
   - Our module files are copied over that, with `@extversion@` in `extension.properties` replaced by
     the install's version. A module file with the name of a staged stock file is an error.
   - `<ghidra>/LICENSE` becomes `LICENSE-Ghidra.txt`.
4. Compile only `ppc_cell.slaspec` with the install's compiler (`<ghidra>/support/sleigh.bat` on
   Windows, `<ghidra>/support/sleigh` elsewhere), run with two absolute path arguments (the slaspec and
   the `.sla`), no options, standard input closed (the Windows launcher would wait for a key press after
   a failure), a 600 s limit. The compile takes a few seconds and the `.sla` is about 190 KB. The
   compiler's output goes to `<out>/sleigh.log`; stock files produce harmless warnings there.
5. Unless `--no-zip`: write `<out>/ghidra_<version>_<release>_CellPPU.zip`, which holds one top-level
   directory `CellPPU/` with `extension.properties` directly inside, the shape the Install Extensions
   dialog accepts.

The `.sla` is compiled after everything else is staged, so it is newer than its sources.

Standard output:

```
ghidra: 12.1.4 PUBLIC (<ghidra>)
base language: PowerPC:BE:64:A2ALT-32addr (ppc_64_isa_altivec_be.slaspec)
staged: 57 stock files, 5 module files -> generated/ghidra/cell/CellPPU
compiled: ppc_cell.sla (193588 bytes, 3.0 s)
zip: generated/ghidra/cell/ghidra_12.1.4_PUBLIC_CellPPU.zip
language: PowerPC:BE:64:CellPPU-32addr (install: see docs/ghidra-cell-language.md)
```

The counts and sizes are those of Ghidra 12.1.4. The `zip:` line is absent with `--no-zip`. Paths
outside the repository are printed as they are.

| Exit | Meaning |
| --- | --- |
| 0 | Staged, compiled and zipped. |
| 1 | I/O: the output directory, the staging, `sleigh.log` or the zip cannot be written, or the module source cannot be read or has no `extension.properties`. |
| 2 | Usage error. |
| 6 | Ghidra or SLEIGH: no install directory given, `Ghidra/application.properties` missing or without version (`not a Ghidra install: <dir>`), the stock `ppc.ldefs` unreadable or without the `A2ALT-32addr` entry, `<ghidra>/LICENSE` missing, a module file named like a stock file, the SLEIGH compiler cannot be run or times out, or the compile fails. |

Two exit-6 messages are worth knowing:

- Drift: `stock PowerPC language changed: <what>; update tools/ghidra/CellPPU`. The stock entry now
  names another SLEIGH file, processor spec or compiler spec than `ppc_cell.slaspec` and `cell.ldefs`
  do, so the sources need updating for this Ghidra version.
- Compile failure: `SLEIGH compile failed; see <out>/sleigh.log`, followed by up to ten of the
  compiler's `ERROR <file>:<line>: <message>` lines.

## Loading the staged module for one run

The staged module can be used without installing anything. Ghidra loads the modules named in the Java
system property `ghidra.external.modules`, and `analyzeHeadless` takes extra Java options from the
environment variable `GHIDRA_HEADLESS_JAVA_OPTIONS`. `ghidra-cell-lang verify` (in its default mode) and
`ghidra-export --cell --staged` therefore run Ghidra with
`GHIDRA_HEADLESS_JAVA_OPTIONS=-Dghidra.external.modules=<repo>/generated/ghidra/cell/CellPPU` (an
existing value of the variable is kept and the option is appended). Only that child process sees it.
Nothing is installed, copied or written outside the repository.

Before it starts Ghidra, the tool checks, read-only, and exits with 6 on the first problem:

1. The staged directory has no `Module.manifest` or no `data/languages/ppc_cell.sla`:
   `staged module not found or not built: <dir>; run python tools/ps3tool.py ghidra-cell-lang build`.
2. The staged directory's path contains whitespace: `staged module path must not contain spaces`. The
   launcher splits the option on spaces; use a `--out` below a path without spaces.
3. `GHIDRA_HEADLESS_JAVA_OPTIONS` already sets `ghidra.external.modules`: unset it.
4. `CellPPU` is already installed (`<ghidra>/Ghidra/Processors/CellPPU`,
   `<ghidra>/Ghidra/Extensions/CellPPU` or `<settings>/Extensions/CellPPU`). Ghidra aborts at startup
   with `Multiple modules collided with same name: CellPPU` when a module exists in two places, so the
   tool stops first with `CellPPU is already installed at <path>; Ghidra cannot load a second copy`.
   With the language installed, use `verify --language PowerPC:BE:64:CellPPU-32addr` and
   `ghidra-export --cell` without `--staged` instead, or uninstall it first.

The user settings directory in check 4 is derived from the environment only, as described under
[Settings directory](#settings-directory). A settings directory moved with the Java property
`application.settingsdir` is not seen by the check.

## Installing

Installing is optional. It is needed for the Ghidra GUI and for runs outside this repository's tools.
After `build`, either copy the folder yourself, use the zip, or run the script. Restart Ghidra
afterwards.

Install `CellPPU` in one place only. Ghidra aborts at startup when the module exists in more than one
(the `Processors` directory, `<ghidra>/Ghidra/Extensions`, the user `Extensions` directory or an
external module).

| Target | Location | Notes |
| --- | --- | --- |
| `User` (default) | `<settings>/Extensions/CellPPU` | Survives replacing the Ghidra install directory, for example by a package manager. The directory Install Extensions writes to. Every folder in it is loaded as a module, in the GUI and headless. |
| `Processors` | `<ghidra>/Ghidra/Processors/CellPPU` | Lives inside the install directory, so it is lost when that directory is replaced. |

### Settings directory

`<settings>` is Ghidra's user settings directory for the install's version, `ghidra_<version>_<release>`
below a base directory chosen in this order:

1. The Java property `application.settingsdir` (not used by the script).
2. The environment variable `XDG_CONFIG_HOME`, on every OS including Windows: the base is
   `<XDG_CONFIG_HOME>/ghidra`, but `<XDG_CONFIG_HOME>/<user>-ghidra` when `XDG_CONFIG_HOME` lies outside
   the user's home directory. `<user>` is the clean user name: spaces removed and any leading
   `DOMAIN\` or path part dropped. A blank value counts as unset.
3. Windows: `%APPDATA%\ghidra`. Linux: `~/.config/ghidra`. macOS: `~/Library/ghidra`.

An example for Ghidra 12.1.4 PUBLIC on Windows is `%APPDATA%\ghidra\ghidra_12.1.4_PUBLIC`. The Python
helpers cannot tell which of the two names in rule 2 Java picks, so they check both.

### By hand

- Zip: in Ghidra, File > Install Extensions, add `generated/ghidra/cell/ghidra_<version>_<release>_CellPPU.zip`.
  The dialog warns when the extension's version differs from the Ghidra version and offers
  "Install Anyway".
- Folder: copy `generated/ghidra/cell/CellPPU` to one of the locations above.

### The install script

```powershell
pwsh -NoProfile -File tools/install-ghidra-cell.ps1 [-GhidraDir <dir>] [-Target User|Processors]
     [-SettingsDir <dir>] [-Source <dir>] [-Uninstall] [-WhatIf] [-Confirm:$false]
```

It needs PowerShell 7. It only works from a built module (run `build` first).

| Parameter | Meaning |
| --- | --- |
| `-GhidraDir <dir>` | The Ghidra install directory (default: `GHIDRA_INSTALL_DIR`; there is no search). It must contain `Ghidra/application.properties` and `Ghidra/Processors/PowerPC`. |
| `-Target User\|Processors` | Where to install (default `User`). |
| `-SettingsDir <dir>` | The settings directory (default: the rule above, from the environment). For the `User` target it must already exist, so start Ghidra once; only its `Extensions` folder may be created. |
| `-Source <dir>` | The staged module (default `generated/ghidra/cell/CellPPU` in this repository). |
| `-Uninstall` | Remove the installed module instead of installing it. |
| `-WhatIf` | Print the plan and write nothing. |
| `-Confirm:$false` | Do not ask before writing. Without it the script asks. |

The script compares the source and the destination by relative path and SHA-256 and prints a plan:
added, updated, removed (in the destination only) and unchanged files. Nothing to change prints
`up to date` and exits without asking. Otherwise it asks for confirmation, copies added and updated
files (the `.sla` last), deletes removed files and directories left empty, and sets the `.sla`'s
modification time to be newer than every other installed file, so Ghidra never sees it as stale.

Safety rules, all checked before any write:

- The destination must lie inside the settings directory (`User`) or the Ghidra install (`Processors`)
  and its last path part must be `CellPPU`. An existing destination must contain an
  `extension.properties` with `name=CellPPU`; otherwise it is a foreign directory and is left alone.
- No existing directory on the way to the destination, and no entry inside the destination or the
  source, may be a symbolic link or junction.
- The source must contain `extension.properties` with `name=CellPPU` and a `version=` equal to the
  target Ghidra's version, and `data/languages/ppc_cell.sla`; it must not overlap the destination.
  Otherwise the script names the `build` command.
- Duplicate-module guard (install only): if `CellPPU` already exists in another place Ghidra loads
  modules from, the script refuses. For `User` those places are `<ghidra>/Ghidra/Processors/CellPPU`
  and `<ghidra>/Ghidra/Extensions/CellPPU`; for `Processors` they are `<settings>/Extensions/CellPPU`
  and `<ghidra>/Ghidra/Extensions/CellPPU`.
- A relative `XDG_CONFIG_HOME` or `APPDATA` is an error (pass `-SettingsDir`), as is an unset `APPDATA`
  on Windows.
- Nothing outside the destination is written, apart from creating `<settings>/Extensions`.

`-Uninstall` removes the destination after the same ownership and link checks and the same
confirmation. It skips the duplicate guard and does not need the source or an existing settings
directory.

Output, one line per item, on standard output:

```
ghidra: <version> <release> (<ghidra>)
target: <destination>
plan: added A, updated U, removed R, unchanged N
  added data/languages/ppc_cell.sla
  updated ...
  removed ...
installed: restart Ghidra to load PowerPC:BE:64:CellPPU-32addr
```

The last line is one of:

| Line | When |
| --- | --- |
| `installed: restart Ghidra to load PowerPC:BE:64:CellPPU-32addr` | Files were written. |
| `up to date: <destination>` | Nothing to change (printed instead of the plan). |
| `cancelled: nothing changed` | The confirmation was declined or `-WhatIf` was given. |
| `uninstalled: <destination>` | `-Uninstall` removed the module (preceded by `plan: remove N files`). |
| `not installed: <destination>` | `-Uninstall` found no module. |

Exit code 0 covers success, up to date, cancelled, `-WhatIf` and not installed. Exit code 1 is any
error, reported on standard error as `install-ghidra-cell: <message>`; nothing is written then. The
`ghidra:` and `target:` lines may precede the error.

### After a Ghidra upgrade, and uninstalling

The `.sla` and the version in `extension.properties` belong to one Ghidra version (the compiled format is
tied to it), and a package manager that replaces the install directory removes a `Processors` copy.
After an upgrade, run `build` again with the new install and install again; with the `User` target the
old settings directory is left behind and can be removed by hand. To uninstall, run the script with
`-Uninstall` (same `-Target`, `-GhidraDir` and `-SettingsDir`) or delete the folder.

## Verifying

```sh
python tools/ps3tool.py ghidra-cell-lang verify [--ghidra-dir DIR] [--staged [DIR] | --language ID]
                                                [--out DIR] [--timeout SEC]
```

`verify` checks a language without the game, on a generated test image (`<out>/image.bin`, loaded at
`0x00010000` as a raw binary). Three things are checked:

1. Table: one representative word for every row of the decoder's opcode table (379 rows) is
   disassembled by Ghidra alone, without following flow, and compared with the decoder through the same
   normalisation as `ghidra-diff`. A row is `match`, `mismatch` (mnemonic or operands differ),
   `unknown` (Ghidra does not decode it) or `skipped` (no word that strictly decodes back to the row).
2. Semantics: the hand-derived cases of the semantics harness for the eight instructions (see
   [Semantics harness](emitter.md#semantics-harness)) plus three plain controls
   (`lvx_loads_the_aligned_block`, `lvx_ignores_the_low_ea_bits`, `stvx_stores_the_aligned_block`) run
   in Ghidra's emulator and the registers and memory are compared. A case is `pass`, `fail` (the first
   differing register or byte is reported) or `not_decoded`. Vector registers are `v<n>` here and
   `vs<32+n>` in Ghidra; reports use `v<n>`.
3. Decompile: a six-instruction sample using `lvlx`, `lvrx`, `vor`, `stvlx` and `stvrx` is decompiled.
   It is `ok` when the decompiler completes without an error message and the C text has neither
   `halt_baddata` nor `Bad instruction`.

| Option | Meaning |
| --- | --- |
| `--ghidra-dir DIR` | The Ghidra install directory (default: `GHIDRA_INSTALL_DIR`). |
| `--staged [DIR]` | Check the staged module, loaded for this run as described in [Loading the staged module for one run](#loading-the-staged-module-for-one-run). The default when neither option is given; `DIR` defaults to `generated/ghidra/cell/CellPPU`. The language is `PowerPC:BE:64:CellPPU-32addr`. |
| `--language ID` | Check an installed language instead (the stock `PowerPC:BE:64:A2ALT-32addr` as a baseline, or the Cell language after installing it). No module is passed and the environment is unchanged. Cannot be combined with `--staged`. |
| `--out DIR` | Output directory (default `generated/ghidra/cell/verify`). |
| `--timeout SEC` | Ghidra run timeout in seconds, at least 1 (default 300). |

The output directory holds `image.bin`, `request.json`, `result.json`, `report.json`, `decompile.c`, the
Ghidra project and `logs/`. The test image has the table words from `0x00010000` (one per row), the
cases at `0x00014000` with a stride of `0x40`, the decompile sample at `0x00018000` and a zeroed data
area at `0x00020000..0x00020100`.

Expected output for the staged module (the order of the `table:` lines for the eight Cell-only rows
follows the opcode table):

```
language: PowerPC:BE:64:CellPPU-32addr (staged generated/ghidra/cell/CellPPU)
table: 379 rows, match 371, mismatch 8, unknown 0, skipped 0
table: mismatch tdi ours 'tdi 3,r4,0x10' ghidra 'tdi r4,0x10'
...
table: lvlx 0x7c642c0e -> ghidra 'lvlx v3,r4,r5': ok
...
table: stvrxl 0x7c642f4e -> ghidra 'stvrxl v3,r4,r5': ok
semantics: 26 cases, pass 26, fail 0, not decoded 0
decompile: ok
report: generated/ghidra/cell/verify/report.json
```

Line formats:

| Line | Meaning |
| --- | --- |
| `language: <id> (staged <dir>)` or `(installed)` | The language that Ghidra reported running. |
| `table: <n> rows, match <m>, mismatch <k>, unknown <u>, skipped <s>` | Table totals. |
| `table: <name> 0x<word> -> ghidra '<text>': ok\|FAIL\|info` | One line per Cell-only row. `unknown` replaces the quoted text when Ghidra did not decode the word. `<name> skipped: <label>` replaces it for a skipped row. |
| `table: unknown <name> 0x<word>` | A non-Cell row Ghidra does not decode (always `info`). |
| `table: mismatch <name> ours '<text>' ghidra '<text>'` | A non-Cell row that decodes differently (always `info`). |
| `table: skipped <name>` | A non-Cell row with no representative word. |
| `semantics: <n> cases, pass <p>, fail <f>, not decoded <d>` | Case totals. |
| `semantics: FAIL\|info <case>: <detail>` | A case that did not pass: `<reg> expected 0x... got 0x...`, `0x<addr> expected 0x.. got 0x..`, an emulator error, or `not decoded: ...`. |
| `semantics: skipped <case>: <reason>` | A case the script cannot run (too many words, other registers, memory outside the data area). |
| `decompile: ok\|FAIL <reason>\|info <reason>` | The decompile result. |
| `report: <path>` | The JSON report (the same data plus `ok` and `failures`). |

Exit codes:

| Exit | Meaning |
| --- | --- |
| 0 | The checks required for the language passed. |
| 1 | I/O: the output cannot be written, or the semantics cases cannot be imported. |
| 2 | Usage: `--staged` with `--language`, or `--timeout` below 1. |
| 6 | Ghidra: no install directory, launcher missing, Ghidra exited non-zero (a language that is not installed ends here), no or invalid `result.json`, Ghidra ran another language than requested, or one of the module checks above. |
| 7 | The language check failed: `language check failed for <id>: <failures>`. |

What fails the run depends on the language id:

- `PowerPC:BE:64:CellPPU-32addr` (staged or installed): any Cell-only row that is not `match`, any
  Cell case or control that is not `pass`, or a decompile that is not `ok`. Non-Cell mismatches stay
  `info`.
- Any other language: only the three controls must pass. Cell rows, Cell cases and the decompile are
  `info`, and non-Cell rows are always `info`.

Eight non-Cell rows are `mismatch` under both languages. They are display differences of the
representative words, not decode gaps: `tdi`, `twi`, `tw`, `td` (Ghidra omits the TO field value 3),
`ldarx` (Ghidra prints the EH operand `,0x0`), `mtfsb0` and `mtfsb1` (Ghidra names the FPSCR bit),
`mtfsfi` (Ghidra prints `0x1,0x2`).

The stock baseline, `verify --language PowerPC:BE:64:A2ALT-32addr`, exits 0 and shows the problem the
language fixes: match 363, mismatch 10, unknown 6. The six unknown rows are `lvlx`, `lvrx`, `stvlx`,
`stvrx`, `stvlxl` and `stvrxl`; the two further mismatches are `lvlxl` and `lvrxl`, which stock Ghidra
decodes as `stvepxl` and `stvepx`. All of them are `info` there, and so are the eight display
differences above. Any other row that is not `match` under the stock language would be a decoding gap
beyond the eight instructions.

To check an installed copy yourself, install the module and run
`verify --language PowerPC:BE:64:CellPPU-32addr`. It applies the same strict rules as the staged mode.
`--staged` cannot be used while the module is installed (exit 6, see above).

## Using the language

```sh
python tools/ps3tool.py ghidra-export <elf> --cell [--staged [DIR]]
```

- `--cell` uses `PowerPC:BE:64:CellPPU-32addr` and cannot be combined with `--language` (usage error,
  exit 2). It has its own default files so the stock run's files stay intact: project
  `generated/ghidra/project_cell`, export `generated/ghidra/eboot_ghidra_cell.json`, logs
  `generated/ghidra/logs_cell`. `--project-dir` and `-o` override the first two. `--check-only --cell`
  checks `eboot_ghidra_cell.json`. All other options work as in
  [Ghidra cross-check](analysis-tools.md#ghidra-cross-check).
- Without `--staged` the language must be installed. With `--staged [DIR]` (only with `--cell`, else
  exit 2; `DIR` defaults to `generated/ghidra/cell/CellPPU`) the staged module is loaded for the run as
  described above. `--check-only` runs no Ghidra, so `--staged` has no effect there.
- In an export made with the Cell language, the language check requires `lvlx` and `lvrx` to decode
  like this repository's decoder: the lines read `language check: lvlx at 0x........ -> ghidra 'lvlx': ok`
  and the run exits 7 if one is `FAIL`. For other languages those two stay `info`.
- If Ghidra exits non-zero, the error ends with `; is the Cell PPU language installed? see
  docs/ghidra-cell-language.md`, or with `--staged` `; is the staged module current? run
  ghidra-cell-lang build`.

Ghidra's loader has no opinion for the new language: in the GUI, import the ELF and choose
`PowerPC:BE:64:CellPPU-32addr` (compiler `default`) in the language dialog by hand. The Ghidra scripts
of this repository (`Ps3Prepare.java`, `ExportAnalysis.java`) work with it unchanged, because the
processor name and the registers are those of PowerPC.

`config/ghidra_summary.json` is made from the base Cell export with the installed language, and
`config/functions.json` from that summary (see
[Committed files and regeneration](function-recovery.md#committed-files-and-regeneration)). Compared
with the earlier stock summary, every function extent in `config/functions.json` is unchanged; 98
functions add `ghidra` to their `sources`. The comparison commands (`ghidra-diff`, `ghidra-summary`, `recover`) work on a Cell
export as on a stock one; see [Function recovery](function-recovery.md#report) for the report.

### Measured on the game executable

Stock and Cell exports from one session, with the same seeds. "Base" seeds the entry point and the
exports; "seeded" adds the extra function starts. The language was loaded with `--staged`. All numbers
are counts.

| Run | Ghidra functions | code | data | undefined | `ghidra-diff` `match` | `cell_truncated` | recovered functions | entries differing from `config/functions.json` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| stock base | 2549 | 175301 | 891 | 33535 | not run | 76 | 2531 | 0 |
| Cell base | 2649 | 196865 | 1137 | 11725 | 196865 | 0 | 2531 | 0 |
| stock seeded | 2691 | 195610 | 1101 | 13016 | not run | 80 | 2530 | 1 |
| Cell seeded | 2691 | 202870 | 1137 | 5720 | 202870 | 0 | 2530 | 1 |

- `ghidra-diff` on both Cell exports reports 0 in `ghidra_code_we_unknown`, `mnemonic_mismatch` and
  `operand_mismatch`.
- Ghidra function extents equal to this repository's: 2275 (stock base) to 2449 (Cell base), and 2407
  (stock seeded) to 2487 (Cell seeded).
- The one differing entry in the seeded runs appears with either language, so it is not caused by the
  language.
- An export takes 21 to 25 s.

## Limitations

- The base language is Power ISA 3.0 with AltiVec, as in the stock language this one extends, so Ghidra
  also decodes instructions that the Cell PPU does not have.
- `stvepx` and `stvepxl` do not decode in this language (see [Files](#files)).
- The p-code reads the aligned 16-byte block even where the hardware accesses no byte (`lvrx` and
  `stvrx` with `sh == 0`), and the store forms read the block and write all 16 bytes back with the
  untouched bytes unchanged. This matters only for memory-mapped I/O, which user-mode PS3 code does not
  reach this way. The 32-bit address truncation comes from the language definition, not the p-code.
- The instructions decode only with the reserved low bit of the word clear, as the decoder's strict
  mode requires.
- Instruction-manual lookup (`manualindexfile`) is not available for this language.
- The compiler spec is the stock `default` one for the 32-bit-address PowerPC language: there is no PS3
  calling convention or TOC handling in it (`Ps3Prepare.java` does the TOC).
- VMX128, SPU, and the privileged Cell instructions and SPRs are not included.
- The module is built against one Ghidra version and must be rebuilt after an upgrade.
- `verify` uses Ghidra's `EmulatorHelper`, which is deprecated for removal since Ghidra 12.1 but still
  works; `PcodeEmulator` is its replacement if it is removed.
- The duplicate-module check of the repository's tools sees only the standard locations, not a
  settings directory moved with `application.settingsdir`.

## Licence

The files in `tools/ghidra/CellPPU/` are covered by the repository's [licence](../LICENSE) (0BSD).
Ghidra and its PowerPC language files are Apache-2.0. `build` copies the stock files from your own
Ghidra install into `generated/ghidra/cell/` (git-ignored) together with Ghidra's licence file
(`LICENSE-Ghidra.txt`), and the install script copies that folder into a Ghidra directory that already
contains them. Nothing from Ghidra is committed or published, and
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) is unchanged. Anyone who shares a built zip or
folder shares Apache-2.0 files and must keep `LICENSE-Ghidra.txt` in it. The PowerPC language files
carry no licence headers of their own.
