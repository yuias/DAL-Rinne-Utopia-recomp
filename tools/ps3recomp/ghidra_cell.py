"""`ghidra-cell-lang` command: build the Cell PPU Ghidra language module from the user's Ghidra."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
import zipfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from . import ghidra
from .info import EXIT_IO, REPO_ROOT, CliError

CELL_LANGUAGE = "PowerPC:BE:64:CellPPU-32addr"
BASE_LANGUAGE = "PowerPC:BE:64:A2ALT-32addr"

MODULE_NAME = "CellPPU"
DEFAULT_SRC = REPO_ROOT / "tools" / "ghidra" / MODULE_NAME
DEFAULT_OUT = ghidra.GHIDRA_DIR / "cell"
SLASPEC_NAME = "ppc_cell.slaspec"
SLA_NAME = "ppc_cell.sla"
LDEFS_NAME = "cell.ldefs"
VERSION_PLACEHOLDER = "@extversion@"

_COMPILE_TIMEOUT = 600
_MAX_ERROR_LINES = 10

# Not staged from the stock language directory: ldefs/opinion would register duplicate language
# ids and loader opinions, a stale .sla must not be reused, and Ghidra finds .lang/.trans files
# application-wide in every module's data directory.
_SKIPPED_SUFFIXES = frozenset({".ldefs", ".opinion", ".sla", ".lang", ".trans"})


@dataclass(frozen=True)
class StockLanguage:
    slaspec: str
    pspec: str
    compilers: dict[str, str]  # compiler id -> cspec file name
    element: ET.Element


@dataclass(frozen=True)
class StageResult:
    stage_dir: Path
    stock_files: int
    module_files: int


def _display(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(path)


def read_application_properties(path: Path) -> dict[str, str]:
    props: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line[0] in "#!":
            continue
        key, sep, value = line.partition("=")
        if sep:
            props[key.strip()] = value.strip()
    return props


def find_stock_language(ldefs_text: str, language_id: str) -> StockLanguage:
    try:
        root = ET.fromstring(ldefs_text)
    except ET.ParseError as e:
        raise CliError(ghidra.EXIT_TOOL, "cannot parse the stock PowerPC ldefs: %s" % e)
    for element in root.findall("language"):
        if element.get("id") != language_id:
            continue
        slafile = element.get("slafile") or ""
        compilers = {c.get("id") or "": c.get("spec") or "" for c in element.findall("compiler")}
        return StockLanguage(slaspec=re.sub(r"\.sla$", ".slaspec", slafile),
                             pspec=element.get("processorspec") or "",
                             compilers=compilers, element=element)
    raise CliError(ghidra.EXIT_TOOL, "stock PowerPC language %s not found; update "
                   "tools/ghidra/CellPPU" % language_id)


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as e:
        raise CliError(EXIT_IO, "cannot read %s: %s" % (path, e.strerror))


def check_drift(stock: StockLanguage, src_dir: Path) -> None:
    """Fail if the stock entry names other files than the ones our module was written against."""
    lang_dir = src_dir / "data" / "languages"
    try:
        ours = ET.fromstring(_read_text(lang_dir / LDEFS_NAME)).find("language")
    except ET.ParseError as e:
        raise CliError(EXIT_IO, "cannot parse %s: %s" % (lang_dir / LDEFS_NAME, e))
    if ours is None:
        raise CliError(EXIT_IO, "no language entry in %s" % (lang_dir / LDEFS_NAME))
    include = re.search(r'^@include\s+"([^"]+)"', _read_text(lang_dir / SLASPEC_NAME),
                        re.MULTILINE)
    changes = []
    if include is None or include.group(1) != stock.slaspec:
        changes.append("slaspec is %r, ours includes %r"
                       % (stock.slaspec, include.group(1) if include else None))
    if ours.get("processorspec") != stock.pspec:
        changes.append("processorspec is %r, ours names %r"
                       % (stock.pspec, ours.get("processorspec")))
    for compiler in ours.findall("compiler"):
        cid, spec = compiler.get("id") or "", compiler.get("spec")
        if stock.compilers.get(cid) != spec:
            changes.append("compiler %r spec is %r, ours names %r"
                           % (cid, stock.compilers.get(cid), spec))
    if changes:
        raise CliError(ghidra.EXIT_TOOL, "stock PowerPC language changed: %s; update "
                       "tools/ghidra/CellPPU" % "; ".join(changes))


def stage_files(stock_module_dir: Path, module_dir: Path) -> list[tuple[Path, Path]]:
    """(source, relative destination) for the stock files and then the module's own files."""
    staged: list[tuple[Path, Path]] = []
    stock_lang = stock_module_dir / "data" / "languages"
    if stock_lang.is_dir():
        for path in sorted(stock_lang.iterdir()):
            # Only regular files at the top level: this leaves out old/ with its .lang/.trans files.
            if path.is_file() and path.suffix.lower() not in _SKIPPED_SUFFIXES:
                staged.append((path, Path("data") / "languages" / path.name))
    sleigh_args = stock_module_dir / "data" / "sleighArgs.txt"
    if sleigh_args.is_file():
        staged.append((sleigh_args, Path("data") / "sleighArgs.txt"))
    taken = {dest for _, dest in staged}
    for path in sorted(p for p in module_dir.rglob("*") if p.is_file()):
        dest = path.relative_to(module_dir)
        if dest in taken:
            raise CliError(ghidra.EXIT_TOOL, "module file %s has the name of a staged stock file"
                           % dest.as_posix())
        staged.append((path, dest))
    return staged


def stage_module(ghidra_dir: Path, version: str, src_dir: Path, out_dir: Path) -> StageResult:
    stock_module = ghidra_dir / "Ghidra" / "Processors" / "PowerPC"
    licence = ghidra_dir / "LICENSE"
    if not licence.is_file():
        raise CliError(ghidra.EXIT_TOOL, "Ghidra licence file not found: %s" % licence)
    if not (src_dir / "extension.properties").is_file():
        raise CliError(EXIT_IO, "module source has no extension.properties: %s" % src_dir)
    files = stage_files(stock_module, src_dir)
    stock_count = sum(1 for source, _ in files if not source.is_relative_to(src_dir))

    stage_dir = out_dir / MODULE_NAME
    try:
        # Only the module directory is removed; the rest of the output directory is left alone.
        if stage_dir.is_symlink() or stage_dir.is_file():
            stage_dir.unlink()
        elif stage_dir.exists():
            shutil.rmtree(stage_dir)
        for source, dest in files:
            target = stage_dir / dest
            target.parent.mkdir(parents=True, exist_ok=True)
            if dest == Path("extension.properties"):
                text = source.read_text(encoding="utf-8").replace(VERSION_PLACEHOLDER, version)
                with open(target, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
            else:
                shutil.copyfile(source, target)
        shutil.copyfile(licence, stage_dir / "LICENSE-Ghidra.txt")
    except OSError as e:
        raise CliError(EXIT_IO, "cannot stage %s: %s" % (e.filename, e.strerror))
    return StageResult(stage_dir, stock_count, len(files) - stock_count)


def build_sleigh_command(ghidra_dir: Path, slaspec: Path, sla: Path,
                         os_name: str = os.name) -> list[str]:
    name = "sleigh.bat" if os_name == "nt" else "sleigh"
    return [str(ghidra_dir / "support" / name), str(slaspec), str(sla)]


def compile_module(stage_dir: Path, ghidra_dir: Path, out_dir: Path,
                   run: Callable[..., subprocess.CompletedProcess] = subprocess.run,
                   os_name: str = os.name) -> tuple[int, float]:
    """Compile the staged slaspec; returns the size of the .sla and the elapsed seconds."""
    lang_dir = stage_dir / "data" / "languages"
    slaspec, sla = lang_dir / SLASPEC_NAME, lang_dir / SLA_NAME
    cmd = build_sleigh_command(ghidra_dir, slaspec, sla, os_name)
    log_path = out_dir / "sleigh.log"
    try:
        log = open(log_path, "wb")
    except OSError as e:
        raise CliError(ghidra.EXIT_IO, "cannot write %s: %s" % (log_path, e.strerror))
    started = time.monotonic()
    try:
        with log:
            # launch.bat runs `pause` on failure, so stdin must not be the console.
            proc = run(cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                       timeout=_COMPILE_TIMEOUT)
    except subprocess.TimeoutExpired:
        raise CliError(ghidra.EXIT_TOOL, "SLEIGH compile timed out after %d s; see %s"
                       % (_COMPILE_TIMEOUT, log_path))
    except OSError as e:
        raise CliError(ghidra.EXIT_TOOL, "cannot run SLEIGH compiler: %s" % e.strerror)
    elapsed = time.monotonic() - started
    if proc.returncode != 0 or not sla.is_file():
        lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        errors = [line.strip() for line in lines if line.lstrip().startswith("ERROR")]
        raise CliError(ghidra.EXIT_TOOL, "\n".join(
            ["SLEIGH compile failed; see %s" % log_path] + errors[:_MAX_ERROR_LINES]))
    return sla.stat().st_size, elapsed


def write_zip(stage_dir: Path, zip_path: Path) -> None:
    # One top-level directory with extension.properties directly inside: the shape the Install
    # Extensions dialog accepts.
    try:
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(p for p in stage_dir.rglob("*") if p.is_file()):
                zf.write(path, "%s/%s" % (MODULE_NAME, path.relative_to(stage_dir).as_posix()))
    except OSError as e:
        raise CliError(EXIT_IO, "cannot write %s: %s" % (e.filename, e.strerror))


def _install_info(ghidra_dir: Path) -> tuple[str, str]:
    path = ghidra_dir / "Ghidra" / "application.properties"
    try:
        props = read_application_properties(path)
    except OSError:
        props = {}
    version = props.get("application.version")
    release = props.get("application.release.name")
    if not version or not release:
        raise CliError(ghidra.EXIT_TOOL, "not a Ghidra install: %s" % ghidra_dir)
    return version, release


def build(ghidra_dir: Path, src_dir: Path, out_dir: Path, make_zip: bool = True,
          run: Callable[..., subprocess.CompletedProcess] = subprocess.run,
          os_name: str = os.name) -> int:
    version, release = _install_info(ghidra_dir)
    print("ghidra: %s %s (%s)" % (version, release, ghidra_dir))

    ldefs = ghidra_dir / "Ghidra" / "Processors" / "PowerPC" / "data" / "languages" / "ppc.ldefs"
    try:
        ldefs_text = ldefs.read_text(encoding="utf-8")
    except OSError as e:
        raise CliError(ghidra.EXIT_TOOL, "cannot read %s: %s" % (ldefs, e.strerror))
    stock = find_stock_language(ldefs_text, BASE_LANGUAGE)
    check_drift(stock, src_dir)
    print("base language: %s (%s)" % (BASE_LANGUAGE, stock.slaspec))

    try:
        out_dir.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        raise CliError(EXIT_IO, "cannot create %s: %s" % (e.filename, e.strerror))
    staged = stage_module(ghidra_dir, version, src_dir, out_dir)
    print("staged: %d stock files, %d module files -> %s"
          % (staged.stock_files, staged.module_files, _display(staged.stage_dir)))

    size, elapsed = compile_module(staged.stage_dir, ghidra_dir.resolve(), out_dir, run, os_name)
    print("compiled: %s (%d bytes, %.1f s)" % (SLA_NAME, size, elapsed))

    if make_zip:
        zip_path = out_dir / ("ghidra_%s_%s_%s.zip" % (version, release, MODULE_NAME))
        write_zip(staged.stage_dir, zip_path)
        print("zip: %s" % _display(zip_path))
    print("language: %s (install: see docs/ghidra-cell-language.md)" % CELL_LANGUAGE)
    return 0


def _run(args: argparse.Namespace) -> int:
    if args.command == "build":
        ghidra_dir = ghidra.resolve_ghidra_dir(args.ghidra_dir, os.environ)
        return build(ghidra_dir, Path(args.src).resolve(), Path(args.out).resolve(),
                     make_zip=not args.no_zip)
    raise AssertionError("unhandled subcommand %r" % args.command)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ps3tool.py ghidra-cell-lang",
        description="Build the Cell PPU language module for Ghidra.")
    sub = parser.add_subparsers(dest="command", required=True, metavar="{build}")
    build_parser = sub.add_parser(
        "build", help="stage the module with the stock PowerPC files and compile it",
        description="Stage a self-contained CellPPU module from the tracked sources and the "
                    "stock PowerPC language of a Ghidra install, compile it with that install's "
                    "SLEIGH compiler and write an extension zip next to it.")
    build_parser.add_argument("--ghidra-dir", metavar="DIR",
                              help="Ghidra install directory (default: $%s)"
                                   % ghidra.GHIDRA_DIR_ENV)
    build_parser.add_argument("--src", default=str(DEFAULT_SRC), metavar="DIR",
                              help="module sources (default: tools/ghidra/CellPPU)")
    build_parser.add_argument("--out", default=str(DEFAULT_OUT), metavar="DIR",
                              help="output directory (default: generated/ghidra/cell)")
    build_parser.add_argument("--no-zip", action="store_true",
                              help="do not write the extension zip")
    return parser


def main(argv: list[str]) -> int:
    parser = _parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as e:
        return e.code if isinstance(e.code, int) else 2
    try:
        return _run(args)
    except CliError as e:
        print("error: %s" % e, file=sys.stderr)
        return e.code
