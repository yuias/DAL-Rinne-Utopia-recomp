import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path

from ps3recomp import info

_ELF_PATH = os.environ.get("DAL_EBOOT_ELF")

_IMPORT_COUNTS = [
    ("sys_io", 6), ("cellAtrac", 15), ("cellAudio", 11), ("sys_fs", 9),
    ("cellGcmSys", 26), ("cellPamf", 1), ("cellPngDec", 7), ("cellSail", 27),
    ("cellSpurs", 4), ("cellSysmodule", 2), ("cellGame", 4), ("sceNpTrophy", 10),
    ("cellSysutil", 18), ("sceNp", 3), ("sysPrxForUser", 23),
]


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file(),
                     "DAL_EBOOT_ELF is not set or does not name an existing file")
class RealEbootInfoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "info.json"
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                cls.exit_code = info.main([_ELF_PATH, "-o", str(out_path)])
            cls.doc = json.loads(out_path.read_text(encoding="utf-8"))

    def test_pin_verified(self):
        self.assertEqual(self.exit_code, 0)
        self.assertEqual(self.doc["input"]["pin"], "verified")
        self.assertEqual(self.doc["input"]["size"], 1144888)

    def test_entry(self):
        self.assertEqual(self.doc["elf"]["entry"], "0x00101638")
        self.assertEqual(self.doc["elf"]["machine"], 21)
        entry = self.doc["entry"]
        self.assertEqual(entry["kind"], "opd")
        func = int(entry["func"], 16)
        exec_segments = [s for s in self.doc["segments"]
                         if s["type_name"] == "LOAD" and "X" in s["flags"]]
        self.assertEqual(len(exec_segments), 1)
        code = exec_segments[0]
        self.assertEqual(code["vaddr"], "0x00010000")
        self.assertEqual(code["filesz"], 0xE1BC8)
        self.assertTrue(0x10000 <= func < 0x10000 + code["memsz"])

    def test_headers(self):
        self.assertEqual(len(self.doc["segments"]), 8)
        self.assertEqual(len(self.doc["sections"]), 28)
        self.assertIsNone(self.doc["opd"])
        data = [s for s in self.doc["segments"]
                if s["type_name"] == "LOAD" and s["vaddr"] == "0x00100000"]
        self.assertEqual(len(data), 1)
        self.assertEqual((data[0]["filesz"], data[0]["memsz"]), (0x25140, 0x1018A0))

    def test_tls(self):
        tls = self.doc["tls"]
        self.assertEqual(tls["vaddr"], "0x0010245c")
        self.assertEqual(tls["offset"], "0x000f245c")
        self.assertEqual((tls["filesz"], tls["memsz"]), (4, 0x110))

    def test_params(self):
        pp = self.doc["process_param"]
        self.assertEqual((pp["addr"], pp["magic"]), ("0x000f1b60", "0x13bcc5f6"))
        prx = self.doc["prx_param"]
        self.assertEqual((prx["addr"], prx["magic"]), ("0x000f1b88", "0x1b434cec"))
        self.assertEqual((prx["libstub_start"], prx["libstub_end"]),
                         ("0x000de7a0", "0x000dea34"))

    def test_imports(self):
        imports = self.doc["imports"]
        self.assertEqual(len(imports), 15)
        self.assertEqual(sum(len(m["functions"]) for m in imports), 166)
        self.assertEqual([(m["name"], len(m["functions"])) for m in imports],
                         _IMPORT_COUNTS)


if __name__ == "__main__":
    unittest.main()
