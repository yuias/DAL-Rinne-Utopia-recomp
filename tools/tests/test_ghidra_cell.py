import re
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from ps3recomp import coverage, ppu

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


if __name__ == "__main__":
    unittest.main()
