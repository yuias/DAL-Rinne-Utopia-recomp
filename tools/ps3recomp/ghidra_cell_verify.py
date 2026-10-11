"""`ghidra-cell-lang verify`: check a Ghidra PowerPC language against our decoder and semantics.

A small test image is disassembled and partly emulated by a Ghidra script (`CellLangCheck.java`).
This module builds the image and the request for that script and evaluates its answer against our
opcode table, the hand-derived semantics cases and a decompiler run.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
from collections import Counter
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path

from . import ghidra, ghidra_cell
from .coverage import CELL_ONLY_FORMS
from .ghidra_norm import compare
from .info import EXIT_IO, REPO_ROOT, CliError
from .ppu import ALL_OPS, FIELD_BITS, OpSpec, decode
from .ppu_format import format_insn

EXIT_USAGE = 2

IMAGE_BASE = 0x00010000
IMAGE_SIZE = 0x10100
CASE_BASE = 0x00014000
CASE_STRIDE = 0x40
MAX_CASE_WORDS = CASE_STRIDE // 4
DECOMPILE_ADDR = 0x00018000
DATA_START = 0x00020000
DATA_END = 0x00020100

DEFAULT_STAGE = ghidra_cell.DEFAULT_OUT / ghidra_cell.MODULE_NAME
DEFAULT_OUT = ghidra_cell.DEFAULT_OUT / "verify"
DEFAULT_TIMEOUT = 300
SCRIPT_NAME = "CellLangCheck.java"
PROJECT_NAME = "cellcheck"

# Cases that must pass under every language: plain VMX loads and stores the stock language decodes.
CONTROL_CASES = ("lvx_loads_the_aligned_block", "lvx_ignores_the_low_ea_bits",
                 "stvx_stores_the_aligned_block")

# Operand values of the representative words.
_OPERAND_VALUES: dict[str, int] = {
    **dict.fromkeys(("rD", "rS", "frD", "frS", "vD", "vS", "crbD", "TO"), 3),
    "BO": 12,
    **dict.fromkeys(("rA", "frA", "vA", "crbA"), 4),
    "BI": 2,
    **dict.fromkeys(("rB", "frB", "vB", "crbB", "SH", "NB"), 5),
    **dict.fromkeys(("frC", "vC", "MB"), 6),
    "ME": 27,
    "crfD": 1,
    "crfS": 2,
    "L": 0,
    "L_sync": 0,
    **dict.fromkeys(("UIMM", "SIMM", "D", "DS", "BD", "LI"), 0x10),
    "SH64": 5,
    "MB64": 6,
    **dict.fromkeys(("CRM", "FXM"), 0x80),
    "FM": 0xFF,
    "IMM_fp": 1,
    "UIMM5": 3,
    "SIMM5": -3,
    "SHB": 3,
    "STRM": 1,
    "SPR": 8,
    "TBR": 268,
}

_REGISTER = re.compile(r"^([rv])(\d+)$")
_GHIDRA_VECTOR_BASE = 32


# --- representative words ----------------------------------------------------------------------

def _place(word: int, first: int, last: int, value: int) -> int:
    """OR `value` into the IBM bit range first..last (bit 0 is the most significant)."""
    width = last - first + 1
    return word | (value & ((1 << width) - 1)) << (31 - last)


def encode_field(word: int, name: str, value: int) -> int:
    """Inverse of `ppu.FIELDS[name]` for the fields the representative words use."""
    if name in ("SPR", "TBR"):  # the two five-bit halves are swapped
        return _place(_place(word, 11, 15, value & 31), 16, 20, value >> 5)
    if name == "SH64":
        return _place(_place(word, 16, 20, value & 31), 30, 30, value >> 5)
    if name == "MB64":
        return _place(_place(word, 21, 25, value & 31), 26, 26, value >> 5)
    if name in ("DS", "BD"):  # word offsets in the encoding
        return _place(word, 16, 29, value >> 2)
    if name == "LI":
        return _place(word, 6, 29, value >> 2)
    ((first, last),) = FIELD_BITS[name]
    return _place(word, first, last, value)


def operand_fields(spec: OpSpec) -> list[str]:
    """Field names of the operands; `D(rA)` stands for two fields."""
    return [part for operand in spec.operands
            for part in operand.replace(")", "").split("(")]


def representative_word(spec: OpSpec) -> int | None:
    """A word with fixed valid operands that strictly decodes back to `spec`, else None."""
    # strict_match, not match: it also carries the bits some rows require set (sc, stwcx.).
    word = spec.strict_match
    for name in operand_fields(spec):
        word = encode_field(word, name, _OPERAND_VALUES[name])
    return word if decode(word, strict=True).spec == spec else None


# --- registers and values ----------------------------------------------------------------------

def ghidra_register(name: str) -> str:
    """Our `r<n>` / `v<n>` as Ghidra's register name (`v<n>` is `vs<32+n>`)."""
    match = _REGISTER.match(name)
    if match is None or int(match.group(2)) > 31:
        raise ValueError("not a register: %r" % name)
    number = int(match.group(2))
    return "r%d" % number if match.group(1) == "r" else "vs%d" % (_GHIDRA_VECTOR_BASE + number)


def our_register(name: str) -> str:
    """Ghidra's `r<n>` / `vs<32+n>` as our register name."""
    match = re.match(r"^(r|vs)(\d+)$", name)
    if match is not None:
        number = int(match.group(2))
        if match.group(1) == "r" and number <= 31:
            return "r%d" % number
        if match.group(1) == "vs" and _GHIDRA_VECTOR_BASE <= number < 2 * _GHIDRA_VECTOR_BASE:
            return "v%d" % (number - _GHIDRA_VECTOR_BASE)
    raise ValueError("not a register of ours: %r" % name)


def vector_value(words: tuple[int, ...] | list[int]) -> int:
    """Four big-endian words as one 128-bit integer."""
    if len(words) != 4:
        raise ValueError("a vector has four words, got %d" % len(words))
    out = 0
    for word in words:
        out = out << 32 | (word & 0xFFFFFFFF)
    return out


def _register_value(name: str, value: object) -> int:
    if name.startswith("v"):
        if not isinstance(value, (tuple, list)):
            raise ValueError("%s needs four words" % name)
        return vector_value(value)
    if not isinstance(value, int):
        raise ValueError("%s needs an integer" % name)
    return value & (1 << 64) - 1


# --- request -----------------------------------------------------------------------------------

@dataclass(frozen=True)
class TableRow:
    index: int
    spec: OpSpec
    word: int | None  # None: no representative word, the row is skipped

    @property
    def addr(self) -> int:
        return IMAGE_BASE + 4 * self.index


@dataclass(frozen=True)
class CaseMeta:
    name: str
    kind: str  # "cell" | "control"
    addr: int
    words: tuple[int, ...]
    expect_regs: tuple[tuple[str, int], ...]  # (our register name, value)
    expect_mem: tuple[tuple[int, bytes], ...]
    insn_names: tuple[str | None, ...]


@dataclass(frozen=True)
class RequestMeta:
    rows: tuple[TableRow, ...]
    cases: tuple[CaseMeta, ...]
    skipped_cases: tuple[tuple[str, str], ...]
    image: bytes
    request: dict

    @property
    def skipped_rows(self) -> list[str]:
        return [row.spec.name for row in self.rows if row.word is None]


def _hex_addr(value: int) -> str:
    return "0x%08x" % value


def _load_semantics():
    """The hand-derived cases and the assembler of the semantics harness (tests package)."""
    try:
        from tests import ppc_asm
        from tests.ppu_semantics import cases_vmx_mem
    except ImportError as e:
        raise CliError(EXIT_IO, "cannot import the semantics cases (is tools/ on sys.path?): %s"
                       % e)
    return ppc_asm, cases_vmx_mem


def _case_problem(case) -> str | None:
    """Why a case cannot be run by the script, or None."""
    if not 0 < len(case.words) <= MAX_CASE_WORDS:
        return "needs %d words, at most %d fit" % (len(case.words), MAX_CASE_WORDS)
    if case.callees or case.switch_targets:
        return "uses callees or switch targets"
    for key, value in (*case.init.items(), *case.expect.items()):
        match = _REGISTER.match(key)
        if match is None or int(match.group(2)) > 31:
            return "uses the unsupported key %r" % key
        try:
            _register_value(key, value)
        except ValueError as e:
            return str(e)
    for addr, data in (*case.mem_init.items(), *case.mem_expect.items()):
        if addr < DATA_START or addr + len(data) > DATA_END:
            return "memory 0x%x+%d is outside the data area" % (addr, len(data))
    return None


def select_cases(cases) -> tuple[list[tuple[object, str]], list[tuple[str, str]]]:
    """(case, kind) for the Cell cases and the controls, and (name, reason) for skipped ones."""
    selected: list[tuple[object, str]] = []
    skipped: list[tuple[str, str]] = []
    for case in cases:
        names = {decode(word, strict=True).spec for word in case.words}
        is_cell = any(spec is not None and spec.name in CELL_ONLY_FORMS for spec in names)
        if not is_cell and case.name not in CONTROL_CASES:
            continue
        problem = _case_problem(case)
        if problem is not None:
            skipped.append((case.name, problem))
        else:
            selected.append((case, "cell" if is_cell else "control"))
    missing = [name for name in CONTROL_CASES
               if name not in {case.name for case, _ in selected}]
    if missing:
        raise CliError(EXIT_IO, "control cases not available: %s" % ", ".join(missing))
    return selected, skipped


def _case_meta(case, kind: str, index: int) -> CaseMeta:
    specs = [decode(word, strict=True).spec for word in case.words]
    return CaseMeta(
        name=case.name, kind=kind, addr=CASE_BASE + CASE_STRIDE * index, words=tuple(case.words),
        expect_regs=tuple((key, _register_value(key, value)) for key, value in case.expect.items()),
        expect_mem=tuple(sorted((addr, bytes(data)) for addr, data in case.mem_expect.items())),
        insn_names=tuple(spec.name if spec is not None else None for spec in specs))


def _case_request(case, meta: CaseMeta) -> dict:
    return {
        "name": meta.name,
        "addr": _hex_addr(meta.addr),
        "count": len(meta.words),
        "regs": {ghidra_register(key): "0x%x" % _register_value(key, value)
                 for key, value in case.init.items()},
        "mem": [{"addr": _hex_addr(addr), "hex": bytes(data).hex()}
                for addr, data in sorted(case.mem_init.items())],
        "read_regs": [ghidra_register(key) for key, _ in meta.expect_regs],
        "read_mem": [{"addr": _hex_addr(addr), "len": len(data)}
                     for addr, data in meta.expect_mem],
    }


def build_request() -> RequestMeta:
    """The test image, the request for the script and what is needed to evaluate its answer."""
    ppc_asm, cases_vmx_mem = _load_semantics()
    selected, skipped = select_cases(cases_vmx_mem.CASES)

    image = bytearray(IMAGE_SIZE)

    def put(addr: int, words) -> None:
        for i, word in enumerate(words):
            struct.pack_into(">I", image, addr - IMAGE_BASE + 4 * i, word)

    rows = []
    for index, spec in enumerate(ALL_OPS):
        word = representative_word(spec)
        rows.append(TableRow(index, spec, word))
        if word is not None:
            put(IMAGE_BASE + 4 * index, [word])

    metas, case_requests = [], []
    for index, (case, kind) in enumerate(selected):
        meta = _case_meta(case, kind, index)
        put(meta.addr, meta.words)
        metas.append(meta)
        case_requests.append(_case_request(case, meta))

    enc = ppc_asm.encode
    put(DECOMPILE_ADDR, [
        enc("lvlx", vD=1, rA=0, rB=3), enc("lvrx", vD=2, rA=3, rB=4),
        enc("vor", vD=3, vA=1, vB=2), enc("stvlx", vS=3, rA=0, rB=5),
        enc("stvrx", vS=3, rA=5, rB=4), ppc_asm.blr()])

    request = {
        "image_base": _hex_addr(IMAGE_BASE),
        "table": [_hex_addr(row.addr) for row in rows if row.word is not None],
        "cases": case_requests,
        "decompile": _hex_addr(DECOMPILE_ADDR),
    }
    return RequestMeta(tuple(rows), tuple(metas), tuple(skipped), bytes(image), request)


# --- evaluation --------------------------------------------------------------------------------

@dataclass(frozen=True)
class RowResult:
    name: str
    word: int | None
    status: str  # match | mismatch | unknown | skipped
    ours: str
    ghidra: str | None
    cell_only: bool


@dataclass(frozen=True)
class CaseResult:
    name: str
    kind: str
    status: str  # pass | fail | not_decoded
    detail: str


@dataclass(frozen=True)
class DecompileResult:
    status: str  # ok | fail
    reason: str
    c: str


@dataclass(frozen=True)
class Report:
    language: str
    strict: bool
    rows: tuple[RowResult, ...]
    cases: tuple[CaseResult, ...]
    skipped_cases: tuple[tuple[str, str], ...]
    decompile: DecompileResult

    def gating(self, case: CaseResult) -> bool:
        """Whether a case that does not pass fails the run."""
        return self.strict or case.kind == "control"

    def failures(self) -> list[str]:
        out = []
        if self.strict:
            out += ["table %s" % row.name for row in self.rows
                    if row.cell_only and row.status != "match"]
        out += ["semantics %s" % case.name for case in self.cases
                if case.status != "pass" and self.gating(case)]
        if self.strict and self.decompile.status != "ok":
            out.append("decompile")
        return out

    def row_counts(self) -> Counter:
        return Counter(row.status for row in self.rows)

    def lines(self) -> list[str]:
        counts = self.row_counts()
        out = ["table: %d rows, match %d, mismatch %d, unknown %d, skipped %d"
               % (len(self.rows), counts["match"], counts["mismatch"], counts["unknown"],
                  counts["skipped"])]
        for row in self.rows:
            if row.cell_only:
                label = "ok" if row.status == "match" else ("FAIL" if self.strict else "info")
                if row.status == "skipped":
                    out.append("table: %s skipped: %s" % (row.name, label))
                else:
                    seen = "unknown" if row.ghidra is None else repr(row.ghidra)
                    out.append("table: %s 0x%08x -> ghidra %s: %s"
                               % (row.name, row.word, seen, label))
            elif row.status == "unknown":
                out.append("table: unknown %s 0x%08x" % (row.name, row.word))
            elif row.status == "mismatch":
                out.append("table: mismatch %s ours %r ghidra %r"
                           % (row.name, row.ours, row.ghidra))
            elif row.status == "skipped":
                out.append("table: skipped %s" % row.name)
        counts = Counter(case.status for case in self.cases)
        out.append("semantics: %d cases, pass %d, fail %d, not decoded %d"
                   % (len(self.cases), counts["pass"], counts["fail"], counts["not_decoded"]))
        for case in self.cases:
            if case.status != "pass":
                label = "FAIL" if self.gating(case) else "info"
                out.append("semantics: %s %s: %s" % (label, case.name, case.detail))
        for name, reason in self.skipped_cases:
            out.append("semantics: skipped %s: %s" % (name, reason))
        if self.decompile.status == "ok":
            out.append("decompile: ok")
        else:
            out.append("decompile: %s %s" % ("FAIL" if self.strict else "info",
                                             self.decompile.reason))
        return out

    def to_json(self) -> dict:
        counts = self.row_counts()
        failures = self.failures()
        return {
            "language": self.language,
            "strict": self.strict,
            "ok": not failures,
            "failures": failures,
            "table": {
                "rows": len(self.rows),
                **{key: counts[key] for key in ("match", "mismatch", "unknown", "skipped")},
                "entries": [{"name": r.name,
                             "word": None if r.word is None else "0x%08x" % r.word,
                             "status": r.status, "ours": r.ours, "ghidra": r.ghidra,
                             "cell_only": r.cell_only} for r in self.rows],
            },
            "cases": [{"name": c.name, "kind": c.kind, "status": c.status, "detail": c.detail}
                      for c in self.cases],
            "skipped_cases": [{"name": n, "reason": r} for n, r in self.skipped_cases],
            "decompile": {"status": self.decompile.status, "reason": self.decompile.reason},
        }


def _parse_hex(value: object, what: str) -> int:
    if not isinstance(value, str):
        raise CliError(ghidra.EXIT_TOOL, "invalid Ghidra result: %s is not a hex string" % what)
    try:
        return int(value, 16)
    except ValueError:
        raise CliError(ghidra.EXIT_TOOL, "invalid Ghidra result: bad hex %r in %s" % (value, what))


def _parse_bytes(value: object, what: str) -> bytes:
    try:
        if isinstance(value, str):
            return bytes.fromhex(value)
    except ValueError:
        pass
    raise CliError(ghidra.EXIT_TOOL, "invalid Ghidra result: bad hex bytes in %s" % what)


def _dict_list(result: Mapping, key: str) -> list[dict]:
    value = result.get(key, [])
    if not isinstance(value, list) or not all(isinstance(e, dict) for e in value):
        raise CliError(ghidra.EXIT_TOOL, "invalid Ghidra result: %r is not a list of objects" % key)
    return value


def _ghidra_text(entry: Mapping | None) -> tuple[str | None, str | None]:
    """(mnemonic, operands) of a table entry, None for a word Ghidra did not decode."""
    if entry is None or not isinstance(entry.get("mnemonic"), str):
        return None, None
    operands = entry.get("operands")
    return entry["mnemonic"], operands if isinstance(operands, str) else None


def _row_result(row: TableRow, entry: Mapping | None) -> RowResult:
    spec = row.spec
    cell_only = spec.name in CELL_ONLY_FORMS
    if row.word is None:
        return RowResult(spec.name, None, "skipped", "", None, cell_only)
    insn = decode(row.word, row.addr, strict=True)
    ours = format_insn(insn, False)
    mnemonic, operands = _ghidra_text(entry)
    if mnemonic is None:
        return RowResult(spec.name, row.word, "unknown", ours, None, cell_only)
    text = mnemonic if not operands else "%s %s" % (mnemonic, operands)
    status = "match" if compare(insn, mnemonic, operands) == "match" else "mismatch"
    return RowResult(spec.name, row.word, status, ours, text, cell_only)


def _first_difference(meta: CaseMeta, entry: Mapping) -> str | None:
    regs = entry.get("regs")
    regs = regs if isinstance(regs, dict) else {}
    for name, expected in meta.expect_regs:
        width = 32 if name.startswith("v") else 16
        raw = regs.get(ghidra_register(name))
        got = None if raw is None else _parse_hex(raw, "register " + name)
        if got != expected:
            return "%s expected 0x%0*x got %s" % (
                name, width, expected, "nothing" if got is None else "0x%0*x" % (width, got))
    mem = {_parse_hex(e.get("addr"), "mem addr"): _parse_bytes(e.get("hex"), "mem hex")
           for e in _dict_list(entry, "mem")}
    for addr, expected in meta.expect_mem:
        got = mem.get(addr, b"")
        for i, byte in enumerate(expected):
            if i >= len(got) or got[i] != byte:
                return "0x%08x expected 0x%02x got %s" % (
                    addr + i, byte, "nothing" if i >= len(got) else "0x%02x" % got[i])
    return None


def _case_result(meta: CaseMeta, entry: Mapping | None, rows: Mapping[str, RowResult]
                 ) -> CaseResult:
    if entry is None:
        return CaseResult(meta.name, meta.kind, "not_decoded", "no result for the case")
    error = entry.get("error")
    if error:
        # An emulator failure is blamed on decoding when a mnemonic of the case did not match.
        undecoded = [n for n in meta.insn_names if n is None or rows[n].status != "match"]
        if undecoded:
            return CaseResult(meta.name, meta.kind, "not_decoded", "not decoded: %s" % error)
        return CaseResult(meta.name, meta.kind, "fail", "emulator error: %s" % error)
    difference = _first_difference(meta, entry)
    if difference is not None:
        return CaseResult(meta.name, meta.kind, "fail", difference)
    return CaseResult(meta.name, meta.kind, "pass", "")


def _decompile_result(value: object) -> DecompileResult:
    if not isinstance(value, dict):
        return DecompileResult("fail", "no decompile result", "")
    c = value.get("c") if isinstance(value.get("c"), str) else ""
    error = value.get("error") if isinstance(value.get("error"), str) else ""
    if not value.get("completed"):
        reason = "not completed: %s" % (error or "no message")
    elif error:
        reason = "error: %s" % error
    elif "halt_baddata" in c:
        reason = "the C text contains halt_baddata"
    elif "Bad instruction" in c:
        reason = "the C text contains a bad instruction"
    else:
        return DecompileResult("ok", "", c)
    return DecompileResult("fail", reason, c)


def evaluate(request_meta: RequestMeta, result: Mapping, requested: str | None = None
             ) -> Report:
    """Compare the script's `result` with our decoder and the expected semantics.

    Strictness follows the language that was requested, so a result that names another language
    (or none) cannot relax the checks; it is an error instead.
    """
    language = result.get("language")
    language = language if isinstance(language, str) else ""
    if requested is not None:
        if language != requested:
            raise CliError(ghidra.EXIT_TOOL, "Ghidra ran language %r, requested %r"
                           % (language, requested))
        language = requested
    table = {_parse_hex(e.get("addr"), "table addr"): e for e in _dict_list(result, "table")}
    rows = tuple(_row_result(row, table.get(row.addr)) for row in request_meta.rows)
    by_name = {row.name: row for row in rows}
    entries = {e.get("name"): e for e in _dict_list(result, "cases")}
    cases = tuple(_case_result(meta, entries.get(meta.name), by_name)
                  for meta in request_meta.cases)
    return Report(language, language == ghidra_cell.CELL_LANGUAGE, rows, cases,
                  request_meta.skipped_cases, _decompile_result(result.get("decompile")))


# --- running -----------------------------------------------------------------------------------

def build_verify_command(*, launcher: Path, out_dir: Path, language: str, script_dir: Path
                         ) -> list[str]:
    log_dir = out_dir / "logs"
    return [
        str(launcher), str(out_dir / "project"), PROJECT_NAME,
        "-import", str(out_dir / "image.bin"), "-overwrite", "-loader", "BinaryLoader",
        "-loader-baseAddr", "0x%x" % IMAGE_BASE,
        "-processor", language, "-cspec", ghidra.DEFAULT_CSPEC, "-noanalysis",
        "-scriptPath", str(script_dir),
        "-postScript", SCRIPT_NAME, str(out_dir / "request.json"), str(out_dir / "result.json"),
        "-deleteProject",
        "-log", str(log_dir / "application.log"),
        "-scriptlog", str(log_dir / "script.log"),
    ]


def _display(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(path)


def _write(path: Path, data: str | bytes) -> None:
    try:
        if isinstance(data, bytes):
            path.write_bytes(data)
        else:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(data)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (path, e.strerror))


def _read_result(path: Path, log_dir: Path) -> dict:
    try:
        with open(path, "r", encoding="utf-8") as f:
            result = json.load(f)
    except FileNotFoundError:
        raise CliError(ghidra.EXIT_TOOL, "Ghidra finished without writing %s; see logs in %s"
                       % (path, log_dir))
    except (OSError, ValueError) as e:
        raise CliError(ghidra.EXIT_TOOL, "invalid Ghidra result %s: %s" % (path, e))
    if not isinstance(result, dict):
        raise CliError(ghidra.EXIT_TOOL, "invalid Ghidra result %s: not a JSON object" % path)
    return result


def verify(ghidra_dir: Path, *, out_dir: Path, stage_dir: Path | None = None,
           language: str | None = None, timeout: int = DEFAULT_TIMEOUT,
           run_headless: Callable[..., float] = ghidra._run_headless,
           module_env: Callable[..., dict[str, str]] = ghidra_cell.external_module_env,
           environ: Mapping[str, str] | None = None) -> int:
    """Run the check against the staged module (`stage_dir`) or an installed `language`."""
    if (stage_dir is None) == (language is None):
        raise CliError(EXIT_USAGE, "give either a staged module or a language id")
    env = None
    if stage_dir is not None:
        language = ghidra_cell.CELL_LANGUAGE
        env = module_env(stage_dir, ghidra_dir, environ)
    launcher = ghidra.headless_launcher(ghidra_dir)

    request_meta = build_request()
    log_dir = out_dir / "logs"
    request_path, result_path = out_dir / "request.json", out_dir / "result.json"
    try:
        (out_dir / "project").mkdir(parents=True, exist_ok=True)
        result_path.unlink(missing_ok=True)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot prepare %s: %s" % (e.filename, e.strerror))
    _write(out_dir / "image.bin", request_meta.image)
    _write(request_path, json.dumps(request_meta.request, indent=2) + "\n")

    cmd = build_verify_command(launcher=launcher.resolve(), out_dir=out_dir, language=language,
                               script_dir=ghidra.SCRIPT_DIR)
    run_headless(cmd, log_dir, timeout, env)
    result = _read_result(result_path, log_dir)

    report = evaluate(request_meta, result, language)
    shown = language
    print("language: %s (%s)" % (shown, "staged %s" % _display(stage_dir) if stage_dir is not None
                                 else "installed"))
    for line in report.lines():
        print(line)
    report_path = out_dir / "report.json"
    _write(report_path, json.dumps(report.to_json(), indent=2) + "\n")
    _write(out_dir / "decompile.c", report.decompile.c)
    print("report: %s" % _display(report_path))

    failures = report.failures()
    if failures:
        raise CliError(ghidra.EXIT_LANGUAGE_CHECK, "language check failed for %s: %s"
                       % (shown, "; ".join(failures)))
    return 0


# --- command line ------------------------------------------------------------------------------

def add_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--ghidra-dir", metavar="DIR",
                        help="Ghidra install directory (default: $%s)" % ghidra.GHIDRA_DIR_ENV)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--staged", nargs="?", const=str(DEFAULT_STAGE), default=None,
                        metavar="DIR",
                        help="check the staged module, loaded as an external module for this run "
                             "(default; DIR: generated/ghidra/cell/CellPPU)")
    source.add_argument("--language", metavar="ID",
                        help="check an installed language id instead")
    parser.add_argument("--out", default=str(DEFAULT_OUT), metavar="DIR",
                        help="output directory (default: generated/ghidra/cell/verify)")
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT, metavar="SEC",
                        help="Ghidra run timeout in seconds (default: %(default)s)")


def run(args: argparse.Namespace) -> int:
    if args.timeout < 1:
        raise CliError(EXIT_USAGE, "--timeout must be at least 1")
    ghidra_dir = ghidra.resolve_ghidra_dir(args.ghidra_dir, os.environ)
    stage_dir = language = None
    if args.language is not None:
        language = args.language
    else:
        stage_dir = Path(args.staged or DEFAULT_STAGE).resolve()
    return verify(ghidra_dir, out_dir=Path(args.out).resolve(), stage_dir=stage_dir,
                  language=language, timeout=args.timeout)
