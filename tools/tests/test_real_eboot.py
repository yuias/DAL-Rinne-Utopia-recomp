import contextlib
import io
import json
import os
import tempfile
import time
import unittest
from pathlib import Path

from ps3recomp import disasm, ghidra_diff, info, probe
from ps3recomp.elf import ElfFile
from ps3recomp.ppu import decode_range

_ELF_PATH = os.environ.get("DAL_EBOOT_ELF")
_GHIDRA_JSON = os.environ.get("DAL_GHIDRA_JSON")

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


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file(),
                     "DAL_EBOOT_ELF is not set or does not name an existing file")
class RealEbootProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "probe.json"
            with contextlib.redirect_stdout(io.StringIO()),                     contextlib.redirect_stderr(io.StringIO()):
                cls.exit_code = probe.main([_ELF_PATH, "-o", str(out_path)])
            cls.doc = json.loads(out_path.read_text(encoding="utf-8"))

    def test_no_embedded_spu_images(self):
        self.assertEqual(self.exit_code, 0)
        self.assertEqual(self.doc["summary"]["spu_image_count"], 0)
        self.assertEqual(self.doc["spu_images"], [])

    def test_spu_related_imports(self):
        self.assertEqual(self.doc["summary"]["spu_related_imports"], ["cellSpurs"])
        imports = self.doc["imports"]
        self.assertEqual(len(imports), 15)
        spurs = [i for i in imports if i["name"] == "cellSpurs"]
        self.assertEqual(len(spurs), 1)
        self.assertEqual(spurs[0]["num_func"], 4)

    def test_syscall_census(self):
        self.assertIn("syscalls", self.doc)
        sc = self.doc["syscalls"]
        self.assertEqual(sc["sites"], sc["unresolved"] + sum(sc["by_number"].values()),
                         "census: %r" % (sc,))


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file(),
                     "DAL_EBOOT_ELF is not set or does not name an existing file")
class RealEbootDisasmTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.code_segment = ElfFile.from_path(_ELF_PATH).exec_segments()[0]

    @staticmethod
    def _run(*args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = disasm.main([_ELF_PATH, *args])
        return code, out.getvalue()

    @staticmethod
    def _top_unknown(doc: dict) -> str:
        return ", ".join("(%d, 0x%03x)x%d" % (u["primary"], u["xo_21_30"], u["count"])
                         for u in doc["top_unknown_patterns"][:5])

    def _stats(self, *args):
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "stats.json"
            started = time.monotonic()
            code, _ = self._run("--stats", *args, "--json", str(out_path))
            elapsed = time.monotonic() - started
            self.assertEqual(code, 0)
            return json.loads(out_path.read_text(encoding="utf-8")), elapsed

    def test_entry_listing_decodes(self):
        code, out = self._run("--entry", "--count", "32")
        lines = out.splitlines()
        self.assertEqual(code, 0)
        self.assertEqual(len(lines), 32)
        for line in lines[:8]:
            self.assertNotIn(".long", line)

    def test_stats_whole_code_segment(self):
        doc, elapsed = self._stats()
        self.assertEqual(doc["total_words"], self.code_segment.p_filesz // 4)
        self.assertLess(elapsed, 30, "stats took %.1f s" % elapsed)
        self.assertGreater(doc["decoded_pct"], 90,
                           "decoded %.2f%% (%d of %d); top unknown: %s"
                           % (doc["decoded_pct"], doc["decoded"], doc["total_words"],
                              self._top_unknown(doc)))

    def test_strict_sc_count(self):
        # Only the 217 lv2 call sites (0x44000002) are valid `sc` words; the rest of the
        # 504 non-strict `sc` hits are ASCII or float data.
        elf = ElfFile.from_path(_ELF_PATH)
        seg = self.code_segment
        data = elf.read_va(seg.p_vaddr, seg.p_filesz // 4 * 4)
        strict_sc = sum(1 for insn in decode_range(data, seg.p_vaddr, strict=True)
                        if insn.spec is not None and insn.spec.name == "sc")
        self.assertEqual(strict_sc, 217)

    def test_stats_strict_block(self):
        doc, _ = self._stats()
        strict = doc["strict"]
        self.assertEqual(strict["decoded"] + strict["rejected"], doc["decoded"])
        self.assertGreater(strict["rejected"], 0)
        self.assertEqual(strict["top_rejected"][0]["mnemonic"], "sc")

    def test_stats_text_range(self):
        doc, _ = self._stats("--start", "0x10230", "--end", "0xdcf2c")
        self.assertGreater(doc["decoded_pct"], 99,
                           "decoded %.2f%% (%d of %d); top unknown: %s"
                           % (doc["decoded_pct"], doc["decoded"], doc["total_words"],
                              self._top_unknown(doc)))


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file()
                     and _GHIDRA_JSON and Path(_GHIDRA_JSON).is_file(),
                     "DAL_EBOOT_ELF and DAL_GHIDRA_JSON must name existing files")
class RealGhidraDiffTest(unittest.TestCase):
    def test_no_decoder_bug_candidates(self):
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "diff.json"
            with contextlib.redirect_stdout(io.StringIO()),                     contextlib.redirect_stderr(io.StringIO()):
                code = ghidra_diff.main([_ELF_PATH, "--ghidra", _GHIDRA_JSON,
                                         "-o", str(out_path)])
            self.assertEqual(code, 0)
            report = json.loads(out_path.read_text(encoding="utf-8"))
        categories = report["categories"]
        bugs = sum(categories[k] for k in ghidra_diff.BUG_CATEGORIES)
        top = ["%s: %s -> %s x%d" % (key, p["ours"], p["ghidra"], p["count"])
               for key in ghidra_diff.BUG_CATEGORIES for p in report["pairs"][key][:5]]
        self.assertEqual(bugs, 0, "%d words differ; top pairs: %s" % (bugs, "; ".join(top)))

    def _run_diff(self, *args):
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "diff.json"
            with contextlib.redirect_stdout(io.StringIO()),                     contextlib.redirect_stderr(io.StringIO()):
                code = ghidra_diff.main([_ELF_PATH, "--ghidra", _GHIDRA_JSON,
                                         "-o", str(out_path), *args])
            self.assertEqual(code, 0)
            return json.loads(out_path.read_text(encoding="utf-8"))

    def test_strict_rejects_no_ghidra_code_word(self):
        lax = self._run_diff()
        strict = self._run_diff("--strict")
        self.assertIs(strict["strict"], True)
        self.assertEqual(strict["categories"]["ghidra_code_we_unknown"],
                         lax["categories"]["ghidra_code_we_unknown"],
                         "strict mode rejects words Ghidra analysed as code; top pairs: %s"
                         % strict["pairs"]["ghidra_code_we_unknown"][:5])
        for key in ("mnemonic_mismatch", "operand_mismatch"):
            self.assertEqual(strict["categories"][key], lax["categories"][key])


if __name__ == "__main__":
    unittest.main()
