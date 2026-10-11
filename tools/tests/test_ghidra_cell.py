import contextlib
import io
import os
import re
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

from ps3recomp import coverage, ghidra, ghidra_cell, ppu
from ps3recomp.info import CliError

MODULE_DIR = Path(__file__).resolve().parents[1] / "ghidra" / "CellPPU"
LANG_DIR = MODULE_DIR / "data" / "languages"

# Constructor header: ":name op1,... is <constraints>" on one line.
_HEADER = re.compile(r"^:(\w+)\s+(\S+)\s+is\s+(.*)$")


def _constructors() -> list[tuple[str, str, str]]:
    # (mnemonic, constraint text, first operand) per constructor header line.
    rows = []
    for line in (LANG_DIR / "cell_vmx.sinc").read_text(encoding="utf-8").splitlines():
        if not line.startswith(":"):
            continue
        match = _HEADER.match(line)
        if match is None:
            raise AssertionError(f"unparsable constructor header: {line!r}")
        rows.append((match.group(1), match.group(3), match.group(2).split(",")[0]))
    return rows


def _constraint(text: str, field: str) -> int:
    match = re.search(rf"\b{field}=(\d+)\b", text)
    if match is None:
        raise AssertionError(f"no {field}= in {text!r}")
    return int(match.group(1))


class CellSleighSourcesTest(unittest.TestCase):
    def test_sinc_matches_decoder_table(self):
        rows = _constructors()
        self.assertEqual(len(rows), len({name for name, _, _ in rows}), "duplicate constructor")
        self.assertEqual({name for name, _, _ in rows}, set(coverage.CELL_ONLY_FORMS))
        by_name = {spec.name: spec for spec in ppu.ALL_OPS}
        for name, text, first in rows:
            with self.subTest(name=name):
                spec = by_name[name]
                self.assertEqual(spec.form, "X")
                self.assertEqual(spec.primary, 31)
                self.assertEqual(_constraint(text, "OP"), spec.primary)
                self.assertEqual(_constraint(text, "XOP_1_10"), spec.xo)
                self.assertEqual(first, {"vD": "vrD", "vS": "vrS"}[spec.operands[0]])

    def test_sinc_constructors_shadow_stock(self):
        # Stock stvepx/stvepxl use the lvrxl/lvlxl patterns; $(NOTVLE) makes ours more specific.
        for name, text, _ in _constructors():
            with self.subTest(name=name):
                self.assertIn("$(NOTVLE)", text)
                self.assertRegex(text, r"\bRc=0\b")

    def test_ldefs(self):
        root = ET.parse(LANG_DIR / "cell.ldefs").getroot()
        languages = root.findall("language")
        self.assertEqual(len(languages), 1)
        language = languages[0]
        self.assertEqual(language.get("id"), "PowerPC:BE:64:CellPPU-32addr")
        self.assertEqual(language.get("processor"), "PowerPC")
        self.assertEqual(language.get("endian"), "big")
        self.assertEqual(language.get("size"), "32")
        self.assertEqual(language.get("variant"), "CellPPU-32addr")
        self.assertEqual(language.get("slafile"), "ppc_cell.sla")
        self.assertEqual(language.get("processorspec"), "ppc_64.pspec")
        self.assertIsNone(language.get("manualindexfile"))
        truncates = language.findall("truncate_space")
        self.assertEqual([(t.get("space"), t.get("size")) for t in truncates], [("ram", "4")])
        compilers = language.findall("compiler")
        self.assertEqual([(c.get("id"), c.get("spec")) for c in compilers],
                         [("default", "ppc_64_32.cspec")])
        self.assertEqual(
            [(e.get("tool"), e.get("name")) for e in language.findall("external_name")],
            [("gnu", "powerpc:e500mc64"),
             ("IDA-PRO", "ppc"),
             ("DWARF.register.mapping.file", "ppc.dwarf"),
             ("qemu", "qemu-ppc64abi32"),
             ("qemu_system", "qemu-system-ppc64")])

    def test_slaspec_includes(self):
        text = (LANG_DIR / "ppc_cell.slaspec").read_text(encoding="utf-8")
        includes = re.findall(r'^@include\s+"([^"]+)"', text, re.MULTILINE)
        self.assertEqual(includes, ["ppc_64_isa_altivec_be.slaspec", "cell_vmx.sinc"])

    def test_extension_properties(self):
        text = (MODULE_DIR / "extension.properties").read_text(encoding="utf-8")
        props = {}
        for line in text.splitlines():
            key, sep, value = line.partition("=")
            if sep:
                props[key] = value
        self.assertEqual(sorted(props), ["author", "createdOn", "description", "name", "version"])
        self.assertEqual(props["name"], "CellPPU")
        self.assertEqual(props["version"], "@extversion@")

    def test_module_manifest_is_empty(self):
        self.assertEqual((MODULE_DIR / "Module.manifest").read_bytes(), b"")


_STOCK_LDEFS = """<?xml version="1.0" encoding="UTF-8"?>
<language_definitions>
  <language processor="PowerPC" endian="big" size="32" variant="PowerISA-Altivec-64-32addr"
            version="1.7" slafile="ppc_64_isa_altivec_be.sla" processorspec="%(pspec)s"
            manualindexfile="../manuals/PowerISA.idx" id="PowerPC:BE:64:A2ALT-32addr">
    <description>Power ISA 3.0 Big Endian w/Altivec and 32-bit Addressing</description>
    <truncate_space space="ram" size="4"/>
    <compiler name="default" spec="ppc_64_32.cspec" id="default"/>
  </language>
</language_definitions>
"""

# Stock files that must be staged, and ones that must not.
_STAGED_STOCK = ["altivec.sinc", "ppc_64.pspec", "ppc_64_32.cspec", "ppc.dwarf",
                 "ppc_64_isa_altivec_be.slaspec"]
_SKIPPED_STOCK = ["ppc.opinion", "ppc_old.sla", "extra.lang", "extra.trans"]


def _make_install(root: Path, pspec: str = "ppc_64.pspec", licence: bool = True,
                  properties: bool = True) -> Path:
    ghidra_dir = root / "ghidra"
    lang = ghidra_dir / "Ghidra" / "Processors" / "PowerPC" / "data" / "languages"
    (lang / "old").mkdir(parents=True)
    for name in _STAGED_STOCK + _SKIPPED_STOCK:
        (lang / name).write_text("stock " + name, encoding="utf-8")
    (lang / "ppc.ldefs").write_text(_STOCK_LDEFS % {"pspec": pspec}, encoding="utf-8")
    (lang / "old" / "oldPPC.lang").write_text("old", encoding="utf-8")
    (lang / "old" / "oldPPC.trans").write_text("old", encoding="utf-8")
    (lang.parent / "sleighArgs.txt").write_text("", encoding="utf-8")
    if properties:
        (ghidra_dir / "Ghidra" / "application.properties").write_text(
            "#comment\napplication.version=9.8.7\napplication.release.name=TEST\n",
            encoding="utf-8")
    if licence:
        (ghidra_dir / "LICENSE").write_text("Apache", encoding="utf-8")
    return ghidra_dir


class FakeSleigh:
    """Stands in for subprocess.run: records the call, writes a log and the .sla."""

    def __init__(self, returncode: int = 0, log: bytes = b"", write_sla: bool = True):
        self.returncode = returncode
        self.log = log
        self.write_sla = write_sla
        self.calls: list[tuple[list[str], dict]] = []

    def __call__(self, cmd, **kwargs):
        self.calls.append((cmd, kwargs))
        kwargs["stdout"].write(self.log)
        if self.write_sla:
            Path(cmd[2]).write_bytes(b"sla")
        return subprocess.CompletedProcess(cmd, self.returncode)


class CellBuildTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.ghidra_dir = _make_install(self.root)
        self.out = self.root / "out"

    def build(self, run=None, ghidra_dir=None, src=MODULE_DIR, **kwargs):
        run = run or FakeSleigh()
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code = ghidra_cell.build(ghidra_dir or self.ghidra_dir, src, self.out, run=run,
                                     **kwargs)
        return code, stdout.getvalue(), run

    def staged_names(self) -> set[str]:
        stage = self.out / "CellPPU"
        return {p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file()}

    def test_staged_file_set(self):
        code, out, _ = self.build()
        self.assertEqual(code, 0)
        names = self.staged_names()
        for name in _STAGED_STOCK:
            self.assertIn("data/languages/" + name, names)
        for name in _SKIPPED_STOCK + ["ppc.ldefs"]:
            self.assertNotIn("data/languages/" + name, names)
        self.assertFalse(any("/old/" in n or n.endswith((".lang", ".trans")) for n in names))
        self.assertIn("data/sleighArgs.txt", names)
        self.assertIn("LICENSE-Ghidra.txt", names)
        for name in ("Module.manifest", "extension.properties", "data/languages/cell.ldefs",
                     "data/languages/ppc_cell.slaspec", "data/languages/cell_vmx.sinc",
                     "data/languages/ppc_cell.sla"):
            self.assertIn(name, names)
        self.assertIn("staged: 6 stock files, 5 module files -> ", out)
        self.assertIn("compiled: ppc_cell.sla (3 bytes, ", out)
        self.assertIn("base language: PowerPC:BE:64:A2ALT-32addr "
                      "(ppc_64_isa_altivec_be.slaspec)\n", out)
        self.assertTrue(out.endswith("language: PowerPC:BE:64:CellPPU-32addr "
                                     "(install: see docs/ghidra-cell-language.md)\n"))

    def test_only_the_module_directory_is_replaced(self):
        stale = self.out / "CellPPU" / "stale.txt"
        stale.parent.mkdir(parents=True)
        stale.write_text("x", encoding="utf-8")
        keep = self.out / "keep.txt"
        keep.write_text("x", encoding="utf-8")
        self.build()
        self.assertFalse(stale.exists())
        self.assertTrue(keep.exists())

    def test_extension_properties_version(self):
        self.build()
        text = (self.out / "CellPPU" / "extension.properties").read_bytes().decode("utf-8")
        self.assertIn("version=9.8.7\n", text)
        self.assertNotIn("@extversion@", text)
        self.assertNotIn("\r", text)

    def test_sleigh_command_and_stdin(self):
        _, _, run = self.build(os_name="nt")
        ((cmd, kwargs),) = run.calls
        lang = (self.out / "CellPPU" / "data" / "languages").resolve()
        self.assertEqual(cmd, [str(self.ghidra_dir.resolve() / "support" / "sleigh.bat"),
                               str(lang / "ppc_cell.slaspec"), str(lang / "ppc_cell.sla")])
        self.assertIs(kwargs["stdin"], subprocess.DEVNULL)
        self.assertEqual(kwargs["stderr"], subprocess.STDOUT)

    def test_build_sleigh_command(self):
        ghidra_dir, spec, sla = Path("/g"), Path("/s/a.slaspec"), Path("/s/a.sla")
        nt = ghidra_cell.build_sleigh_command(ghidra_dir, spec, sla, "nt")
        posix = ghidra_cell.build_sleigh_command(ghidra_dir, spec, sla, "posix")
        self.assertEqual(nt, [str(ghidra_dir / "support" / "sleigh.bat"), str(spec), str(sla)])
        self.assertEqual(posix, [str(ghidra_dir / "support" / "sleigh"), str(spec), str(sla)])

    def test_stock_drift(self):
        ghidra_dir = _make_install(self.root / "drift", pspec="ppc_64_new.pspec")
        with self.assertRaises(CliError) as caught:
            self.build(ghidra_dir=ghidra_dir)
        self.assertEqual(caught.exception.code, ghidra.EXIT_TOOL)
        self.assertIn("stock PowerPC language changed: ", str(caught.exception))
        self.assertIn("ppc_64_new.pspec", str(caught.exception))
        self.assertIn("update tools/ghidra/CellPPU", str(caught.exception))

    def test_name_collision(self):
        src = self.root / "src"
        for path in MODULE_DIR.rglob("*"):
            if path.is_file():
                target = src / path.relative_to(MODULE_DIR)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(path.read_bytes())
        (src / "data" / "languages" / "altivec.sinc").write_text("mine", encoding="utf-8")
        with self.assertRaises(CliError) as caught:
            self.build(src=src)
        self.assertEqual(caught.exception.code, ghidra.EXIT_TOOL)
        self.assertIn("data/languages/altivec.sinc", str(caught.exception))

    def test_compile_failure_reports_error_lines(self):
        log = b"INFO start\n" + b"".join(
            b"ERROR cell_vmx.sinc:%d: bad (SleighCompile)\n" % n for n in range(1, 15))
        with self.assertRaises(CliError) as caught:
            self.build(run=FakeSleigh(returncode=2, log=log, write_sla=False))
        self.assertEqual(caught.exception.code, ghidra.EXIT_TOOL)
        lines = str(caught.exception).splitlines()
        self.assertTrue(lines[0].startswith("SLEIGH compile failed; see "))
        self.assertTrue(lines[0].endswith("sleigh.log"))
        self.assertEqual(len(lines), 1 + 10)
        self.assertEqual(lines[1], "ERROR cell_vmx.sinc:1: bad (SleighCompile)")

    def test_zip_contents(self):
        _, out, _ = self.build()
        zip_path = self.out / "ghidra_9.8.7_TEST_CellPPU.zip"
        self.assertTrue(zip_path.is_file())
        self.assertIn("zip: ", out)
        with zipfile.ZipFile(zip_path) as zf:
            names = zf.namelist()
            self.assertEqual({n.split("/")[0] for n in names}, {"CellPPU"})
            self.assertEqual([n for n in names if n.endswith("extension.properties")],
                             ["CellPPU/extension.properties"])
            self.assertIn("CellPPU/data/languages/ppc_cell.sla", names)
            self.assertIn("CellPPU/LICENSE-Ghidra.txt", names)
            self.assertIn(b"version=9.8.7", zf.read("CellPPU/extension.properties"))

    def test_no_zip(self):
        _, out, _ = self.build(make_zip=False)
        self.assertEqual(list(self.out.glob("*.zip")), [])
        self.assertNotIn("zip:", out)

    def test_missing_application_properties(self):
        ghidra_dir = _make_install(self.root / "bare", properties=False)
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            code = ghidra_cell.main(["build", "--ghidra-dir", str(ghidra_dir),
                                     "--out", str(self.out)])
        self.assertEqual(code, ghidra.EXIT_TOOL)
        self.assertIn("error: not a Ghidra install: ", stderr.getvalue())

    def test_missing_licence(self):
        ghidra_dir = _make_install(self.root / "nolicence", licence=False)
        with self.assertRaises(CliError) as caught:
            self.build(ghidra_dir=ghidra_dir)
        self.assertEqual(caught.exception.code, ghidra.EXIT_TOOL)
        self.assertIn("LICENSE", str(caught.exception))

    def test_unknown_subcommand(self):
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(ghidra_cell.main(["frobnicate"]), 2)
            self.assertEqual(ghidra_cell.main([]), 2)


@unittest.skipUnless(os.environ.get("DAL_GHIDRA_LIVE") == "1"
                     and os.environ.get(ghidra.GHIDRA_DIR_ENV),
                     "set DAL_GHIDRA_LIVE=1 and GHIDRA_INSTALL_DIR to run against Ghidra")
class CellBuildLiveTest(unittest.TestCase):
    def test_real_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                code = ghidra_cell.main(["build", "--out", tmp])
            self.assertEqual(code, 0, stdout.getvalue())
            stage = Path(tmp) / "CellPPU"
            self.assertGreater((stage / "data" / "languages" / "ppc_cell.sla").stat().st_size, 0)
            self.assertFalse((stage / "data" / "languages" / "old").exists())
            self.assertTrue((stage / "data" / "sleighArgs.txt").is_file())
            self.assertTrue((stage / "LICENSE-Ghidra.txt").is_file())
            self.assertEqual(len(list(Path(tmp).glob("ghidra_*_CellPPU.zip"))), 1)


if __name__ == "__main__":
    unittest.main()
