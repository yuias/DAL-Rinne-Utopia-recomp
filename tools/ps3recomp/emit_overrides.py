"""Overrides file (`config/overrides.json`) loader and validation for the `emit` command."""

from __future__ import annotations

import bisect
import hashlib
import json
import re
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from .emit_core import _comment_text, u32
from .emit_db import FunctionDb
from .emit_output import banner, c_string
from .info import EXIT_IO, EXIT_PIN_MISMATCH, REPO_ROOT, CliError

EXIT_BAD_OVERRIDES = 10
SCHEMA = 1
DEFAULT_OVERRIDES = REPO_ROOT / "config" / "overrides.json"
KINDS = ("override", "hook")

_TOP_KEYS = ("schema", "executable_sha256", "entries", "comment")
_TOP_REQUIRED = ("schema", "executable_sha256", "entries")
_ENTRY_KEYS = ("address", "kind", "handler", "label", "comment")
_ENTRY_REQUIRED = ("address", "kind", "handler")

_ADDRESS = re.compile(r"0x[0-9a-fA-F]{1,8}")
_SHA256 = re.compile(r"[0-9a-fA-F]{64}")
# A leading underscore is rejected here: such identifiers are reserved in C.
_HANDLER = re.compile(r"[A-Za-z][A-Za-z0-9_]{0,63}")
# Namespaces of generated symbols (fn_%08x, imp_%08x, L_%08x, case_*).
_RESERVED_PREFIXES = ("fn_", "imp_", "L_", "case_")
_C23_KEYWORDS = frozenset(
    "alignas alignof auto bool break case char const constexpr continue default do double else "
    "enum extern false float for goto if inline int long nullptr register restrict return short "
    "signed sizeof static static_assert struct switch thread_local true typedef typeof "
    "typeof_unqual union unsigned void volatile while".split())


@dataclass(frozen=True)
class OverrideEntry:
    address: int
    kind: str                 # "override" | "hook"
    handler: str
    label: str | None = None


@dataclass(frozen=True)
class OverrideConfig:
    sha256: str               # executable pin, lower case
    file_sha256: str          # SHA-256 of the file bytes
    entries: tuple[OverrideEntry, ...]   # sorted by address


def _json(value: object) -> str:
    return json.dumps(value, ensure_ascii=True)


def _check_keys(obj: dict, where: str, allowed: Sequence[str], required: Sequence[str]) -> None:
    # Unknown keys first: a misspelled key would otherwise be reported as a missing one.
    for key in obj:
        if key not in allowed:
            raise ValueError('%s: unknown key "%s"' % (where, key))
    for key in required:
        if key not in obj:
            raise ValueError('%s: missing key "%s"' % (where, key))


def _parse_entry(item: object, index: int) -> OverrideEntry:
    where = "entries[%d]" % index
    if not isinstance(item, dict):
        raise ValueError("%s: expected an object" % where)
    _check_keys(item, where, _ENTRY_KEYS, _ENTRY_REQUIRED)

    address = item["address"]
    if not isinstance(address, str) or not _ADDRESS.fullmatch(address):
        raise ValueError('%s.address: expected a hex string like "0x0001a2b0", got %s'
                         % (where, _json(address)))
    kind = item["kind"]
    if not isinstance(kind, str) or kind not in KINDS:
        raise ValueError('%s.kind: expected "override" or "hook", got %s' % (where, _json(kind)))
    handler = item["handler"]
    if not isinstance(handler, str):
        raise ValueError("%s.handler: expected a string" % where)
    if not _HANDLER.fullmatch(handler):
        raise ValueError('%s.handler: "%s" is not a valid handler name' % (where, handler))
    if handler.startswith(_RESERVED_PREFIXES) or handler in _C23_KEYWORDS:
        raise ValueError('%s.handler: "%s" is reserved' % (where, handler))
    label = item.get("label")
    if "label" in item and (not isinstance(label, str) or not label):
        raise ValueError("%s.label: expected a non-empty string" % where)
    if "comment" in item and not isinstance(item["comment"], str):
        raise ValueError("%s.comment: expected a string" % where)
    return OverrideEntry(int(address, 16), kind, handler, label)


def parse_overrides(doc: object, file_sha256: str) -> OverrideConfig:
    """Validate a decoded overrides document; ValueError has a one-line message, no path."""
    if not isinstance(doc, dict):
        raise ValueError("top level: expected an object")
    _check_keys(doc, "top level", _TOP_KEYS, _TOP_REQUIRED)

    schema = doc["schema"]
    # bool is an int subclass in Python; `true` must not pass for schema 1.
    if isinstance(schema, bool) or schema != SCHEMA:
        raise ValueError("top level: unsupported schema %s" % _json(schema))
    sha = doc["executable_sha256"]
    if not isinstance(sha, str):
        raise ValueError("top level.executable_sha256: expected a string")
    if not _SHA256.fullmatch(sha):
        raise ValueError("top level.executable_sha256: expected 64 hex digits, got %s" % _json(sha))
    if "comment" in doc and not isinstance(doc["comment"], str):
        raise ValueError("top level.comment: expected a string")
    items = doc["entries"]
    if not isinstance(items, list):
        raise ValueError("top level.entries: expected an array")

    entries = [_parse_entry(item, i) for i, item in enumerate(items)]
    first: dict[int, int] = {}
    for j, entry in enumerate(entries):
        i = first.setdefault(entry.address, j)
        if i != j:
            raise ValueError("entries[%d] and entries[%d] both name 0x%08x"
                             % (i, j, entry.address))
    entries.sort(key=lambda e: e.address)
    return OverrideConfig(sha.lower(), file_sha256, tuple(entries))


def _no_duplicate_keys(pairs: list[tuple[str, object]]) -> dict:
    obj: dict[str, object] = {}
    for key, value in pairs:
        if key in obj:
            raise ValueError('duplicate key "%s" in an object' % key)
        obj[key] = value
    return obj


def _reject_constant(name: str) -> object:
    raise ValueError("invalid JSON: unexpected constant %s" % name)


def load_overrides(path: Path, *, required: bool) -> OverrideConfig | None:
    """Read and validate the file. None means the default file is absent (`required` is False)."""
    path = Path(path)
    try:
        raw = path.read_bytes()
    except FileNotFoundError as e:
        if not required:
            return None
        raise CliError(EXIT_IO, "cannot read overrides file %s: %s" % (path, e.strerror))
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read overrides file %s: %s" % (path, e.strerror))
    try:
        try:
            doc = json.loads(raw.decode("utf-8"), object_pairs_hook=_no_duplicate_keys,
                             parse_constant=_reject_constant)
        except json.JSONDecodeError as e:
            raise ValueError("invalid JSON: %s (line %d, column %d)" % (e.msg, e.lineno, e.colno))
        except UnicodeDecodeError as e:
            raise ValueError("invalid JSON: not UTF-8 (%s)" % e.reason)
        except RecursionError:
            raise ValueError("invalid JSON: nested too deeply")
        return parse_overrides(doc, hashlib.sha256(raw).hexdigest())
    except ValueError as e:
        raise CliError(EXIT_BAD_OVERRIDES, "invalid overrides file %s: %s" % (path, e))


def check_overrides(config: OverrideConfig, *, elf_sha256: str, db: FunctionDb) -> None:
    """Check the entries against the ELF pin and the function DB; raises CliError."""
    elf_sha256 = elf_sha256.lower()
    if config.sha256 != elf_sha256:
        raise CliError(EXIT_PIN_MISMATCH,
                       "overrides file was made for a different ELF (SHA-256 %s, expected %s)"
                       % (config.sha256, elf_sha256))
    thunks = {t.addr: t for t in db.thunks}
    entries = {f.entry for f in db.functions}
    spans = sorted((start, end, f.entry) for f in db.functions for start, end in f.ranges)
    starts = [s[0] for s in spans]
    for entry in config.entries:
        addr = entry.address
        thunk = thunks.get(addr)
        if thunk is not None:
            raise CliError(EXIT_BAD_OVERRIDES,
                           "overrides entry 0x%08x is the import stub of %s::%s; imports are "
                           "replaced by HLE handlers (runtime/src/hle), not by overrides"
                           % (addr, thunk.module, thunk.name or "0x%08x" % thunk.fnid))
        if addr in entries:
            continue
        # Ranges never overlap, so the only candidate is the last span starting at or before addr.
        k = bisect.bisect_right(starts, addr) - 1
        if k >= 0 and addr < spans[k][1]:
            where = "inside fn_%08x" % spans[k][2]
        else:
            where = "outside every function"
        raise CliError(EXIT_BAD_OVERRIDES,
                       "overrides entry 0x%08x is not a function entry (%s)" % (addr, where))


_KIND_ENUMERATOR = {"override": "kDalOverrideKindOverride", "hook": "kDalOverrideKindHook"}


def orig_name(name: str) -> str:
    return name + "_orig"


@dataclass(frozen=True)
class OverrideSymbols:
    """Names of the generated table, its count and the counters (scoped per case in the harness)."""
    table: str = "kDalPpuOverrides"
    count: str | None = "kDalPpuOverrideCount"   # None: not emitted
    calls: str = "override_calls"
    static_table: bool = False                   # True: `static const` table


def render_wrapper(entry: OverrideEntry, index: int, name: str, symbols: OverrideSymbols) -> str:
    """The function `name` that runs the handler of `entry`; `index` is its table row."""
    title = "// %s %s" % (entry.kind, _comment_text(entry.handler))
    if entry.label:
        title += ": " + _comment_text(entry.label)
    site = "&%s[%d]" % (symbols.table, index)
    lines = [title,
             "DAL_PPU_FUNC(%s) {" % name,
             "    DAL_ENTER(%s);" % u32(entry.address),
             "    dalCountCall(&%s[%d]);" % (symbols.calls, index),
             "    %s(ctx, base, %s);" % (entry.handler, site)]
    # Only the transfer into the body is a tail call, so a hook leaves no frame behind.
    if entry.kind == "hook":
        lines.append("    DAL_TAIL(%s);" % orig_name(name))
    lines.append("}")
    return "\n".join(lines) + "\n"


def render_table(entries: Sequence[OverrideEntry], names: Mapping[int, str],
                 symbols: OverrideSymbols) -> str:
    """Counters (when there are entries), the `DalPpuOverride` table and optionally its count."""
    prefix = "static " if symbols.static_table else ""
    lines: list[str] = []
    if not entries:
        # C has no zero-length arrays; the single slot stays zero.
        lines.append("%sconst DalPpuOverride %s[1] = {};" % (prefix, symbols.table))
    else:
        lines += ["static uint64_t %s[%d];" % (symbols.calls, len(entries)), "",
                  "%sconst DalPpuOverride %s[%d] = {" % (prefix, symbols.table, len(entries))]
        for i, e in enumerate(entries):
            lines.append("    { %s, %s, %s, &%s[%d], %s, %s }," % (
                u32(e.address), _KIND_ENUMERATOR[e.kind], orig_name(names[e.address]),
                symbols.calls, i, c_string(e.handler), c_string(e.label)))
        lines.append("};")
    if symbols.count is not None:
        lines.append("const uint32_t %s = %d;" % (symbols.count, len(entries)))
    return "\n".join(lines) + "\n"


def handler_declarations(entries: Sequence[OverrideEntry]) -> list[str]:
    return ["DAL_OVERRIDE_HANDLER(%s);" % h for h in sorted({e.handler for e in entries})]


def render_overrides_source(sha256: str, entries: Sequence[OverrideEntry],
                            names: Mapping[int, str]) -> str:
    """ppu_overrides.c: the table, the counters and the wrappers."""
    symbols = OverrideSymbols()
    text = (banner(sha256) + '\n#include "ppu_funcs.h"\n#include "ppu_override_handlers.h"\n\n'
            + render_table(entries, names, symbols))
    for i, e in enumerate(entries):
        text += "\n" + render_wrapper(e, i, names[e.address], symbols)
    return text


def render_handler_header(sha256: str, entries: Sequence[OverrideEntry]) -> str:
    decls = handler_declarations(entries)
    lines = [banner(sha256).rstrip("\n"), "#pragma once", "", '#include "dal/ppu_override.h"', "",
             "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]
    if decls:
        lines += decls + [""]
    lines += ["#ifdef __cplusplus", '}  // extern "C"', "#endif"]
    return "\n".join(lines) + "\n"


def render_handler_stubs(sha256: str, entries: Sequence[OverrideEntry]) -> str:
    """ppu_override_stubs.c: handler stand-ins for the stub-runtime link test."""
    text = (banner(sha256)
            + "// Stand-ins for the override handlers, linked only into the stub-runtime link "
              "test.\n"
            + '\n#include "ppu_override_handlers.h"\n#include "dal/ppu_stub.h"\n')
    for h in sorted({e.handler for e in entries}):
        text += ("\nDAL_OVERRIDE_HANDLER(%s) {\n    (void)ctx;\n    (void)base;\n"
                 "    dalStubOverrideHandler(site);\n}\n" % h)
    return text
