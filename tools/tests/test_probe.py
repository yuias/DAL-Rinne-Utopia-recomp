import contextlib
import hashlib
import io
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from ps3recomp import probe
from ps3recomp.elf import ElfFile

from tests import elf_fixture as fx
from tests.elf_fixture import FixtureSegment, build_elf32_be

TOOLS_DIR = Path(__file__).resolve().parents[1]
SPU_VA = 0x20200


class ProbeTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.out_path = self.tmp / "out" / "probe.json"

    def run_probe(self, data: bytes, *extra):
        elf_path = self.tmp / "input.elf"
        elf_path.write_bytes(data)
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = probe.main([str(elf_path), "-o", str(self.out_path), *extra])
        return code, out.getvalue(), err.getvalue()

    def load_output(self) -> dict:
        return json.loads(self.out_path.read_text(encoding="utf-8"))


class SpuFixtureTest(ProbeTestBase):
    def setUp(self):
        super().setUp()
        self.data = fx.build_ps3_fixture(with_spu=True)
        self.code, self.stdout, _ = self.run_probe(self.data)
        self.doc = self.load_output()

    def test_one_spu_image(self):
        self.assertEqual(self.code, 0)
        self.assertEqual(self.doc["schema"], 1)
        self.assertEqual(len(self.doc["spu_images"]), 1)
        img = self.doc["spu_images"][0]
        offset = ElfFile.from_bytes(self.data).va_to_offset(SPU_VA)
        spu = build_elf32_be(entry=0x0, segments=[FixtureSegment(1, 0x0, bytes(32), flags=5)])
        self.assertEqual(img["file_offset"], "0x%08x" % offset)
        self.assertEqual(img["vaddr"], "0x%08x" % SPU_VA)
        self.assertEqual(img["size"], len(spu))
        self.assertEqual(img["sha256"], hashlib.sha256(spu).hexdigest())
        self.assertEqual((img["class"], img["machine"], img["e_type"]), (32, 23, 2))
        self.assertEqual(img["entry"], "0x00000000")
        self.assertEqual(img["segments"], [{"type": "0x00000001", "vaddr": "0x00000000",
                                            "filesz": 32, "memsz": 32, "flags": "R-X"}])

    def test_invalid_hit_and_no_other_elfs(self):
        self.assertGreaterEqual(self.doc["invalid_magic_hits"], 1)
        self.assertEqual(self.doc["other_elfs"], [])

    def test_imports(self):
        self.assertEqual(self.doc["summary"]["spu_related_imports"], ["cellSpurs"])
        self.assertEqual(self.doc["summary"]["spu_image_count"], 1)
        by_name = {i["name"]: i for i in self.doc["imports"]}
        self.assertFalse(by_name["cellSysutil"]["spu_related"])
        self.assertTrue(by_name["cellSpurs"]["spu_related"])
        self.assertEqual(by_name["cellSysutil"]["num_func"], 2)
        self.assertEqual(by_name["cellSpurs"]["num_func"], 1)

    def test_stdout(self):
        lines = self.stdout.splitlines()
        self.assertRegex(lines[0], r"^spu file_off=0x[0-9a-f]{8} vaddr=0x00020200 "
                                   r"size=0x[0-9a-f]{8} entry=0x00000000 sha256=[0-9a-f]{16}$")
        self.assertEqual(lines[1:], ["spu images: 1", "spu-related imports: cellSpurs"])


class NoSpuFixtureTest(ProbeTestBase):
    def test_no_spu(self):
        code, stdout, _ = self.run_probe(fx.build_ps3_fixture())
        self.assertEqual(code, 0)
        doc = self.load_output()
        self.assertEqual(doc["spu_images"], [])
        self.assertEqual(doc["other_elfs"], [])
        self.assertEqual(doc["invalid_magic_hits"], 0)
        self.assertEqual(doc["summary"]["spu_image_count"], 0)
        self.assertIn("spu images: 0", stdout)

    def test_other_elf_is_not_spu(self):
        data = bytearray(fx.build_ps3_fixture())
        ppc = build_elf32_be(entry=0, segments=[], machine=20)
        data.extend(bytes(-len(data) % 16))
        data.extend(ppc)
        code, _, _ = self.run_probe(bytes(data))
        self.assertEqual(code, 0)
        doc = self.load_output()
        self.assertEqual(doc["spu_images"], [])
        self.assertEqual([e["machine"] for e in doc["other_elfs"]], [20])


class ErrorTest(ProbeTestBase):
    def test_missing_file(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = probe.main([str(self.tmp / "missing.elf"), "-o", str(self.out_path)])
        self.assertEqual(code, 1)
        self.assertEqual(len(err.getvalue().strip().splitlines()), 1)

    def test_self_input(self):
        code, _, err = self.run_probe(b"SCE\0" + bytes(64))
        self.assertEqual(code, 3)
        self.assertIn("encrypted SELF", err)

    def test_elf32_input(self):
        spu = build_elf32_be(entry=0, segments=[])
        code, _, _ = self.run_probe(spu)
        self.assertEqual(code, 3)


class CliTest(ProbeTestBase):
    def test_help_lists_output_option(self):
        proc = subprocess.run([sys.executable, str(TOOLS_DIR / "ps3tool.py"), "probe", "--help"],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0)
        self.assertRegex(proc.stdout, re.compile(r"-o\b"))

    def test_subprocess_run(self):
        elf_path = self.tmp / "input.elf"
        elf_path.write_bytes(fx.build_ps3_fixture(with_spu=True))
        proc = subprocess.run([sys.executable, str(TOOLS_DIR / "ps3tool.py"), "probe",
                               str(elf_path), "-o", str(self.out_path)],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("spu images: 1", proc.stdout)


if __name__ == "__main__":
    unittest.main()
