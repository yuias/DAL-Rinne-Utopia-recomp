import contextlib
import io
import json
import os
import tempfile
import time
import unittest
from pathlib import Path

from ps3recomp import (cell, disasm, emit, ghidra_diff, ghidra_summary, info, jumptab, nids,
                       probe, recover, seeds)
from ps3recomp.elf import ElfFile
from ps3recomp.ppu import decode_range
from ps3recomp.ppu_flow import CodeImage

_ELF_PATH = os.environ.get("DAL_EBOOT_ELF")
_GHIDRA_JSON = os.environ.get("DAL_GHIDRA_JSON")
_COMMITTED_SUMMARY = ghidra_summary.REPO_ROOT / "config" / "ghidra_summary.json"

_IMPORT_COUNTS = [
    ("sys_io", 6), ("cellAtrac", 15), ("cellAudio", 11), ("sys_fs", 9),
    ("cellGcmSys", 26), ("cellPamf", 1), ("cellPngDec", 7), ("cellSail", 27),
    ("cellSpurs", 4), ("cellSysmodule", 2), ("cellGame", 4), ("sceNpTrophy", 10),
    ("cellSysutil", 18), ("sceNp", 3), ("sysPrxForUser", 23),
]

_VERIFIED_NAMES = [
    ("cellSpurs", "cellSpursInitializeWithAttribute2"),
    ("cellSpurs", "cellSpursGetSpuThreadGroupId"),
    ("cellSpurs", "_cellSpursAttributeInitialize"),
    ("cellSpurs", "cellSpursFinalize"),
    ("cellSail", "cellSailPlayerBoot"),
    ("cellAtrac", "cellAtracDecode"),
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

    def test_import_names(self):
        by_module = {m["name"]: {f["name"] for f in m["functions"]}
                     for m in self.doc["imports"]}
        for module, name in _VERIFIED_NAMES:
            with self.subTest(module=module, name=name):
                self.assertIn(name, by_module[module])

    def test_no_unresolved_imports(self):
        unresolved = [(m["name"], f["fnid"]) for m in self.doc["imports"]
                      for f in m["functions"] if f["name"] is None]
        self.assertEqual(unresolved, [])


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
        self.assertEqual(sc["sites"], 217)
        self.assertEqual(sc["sites"], sc["unresolved"] + sum(sc["by_number"].values()),
                         "census: %r" % (sc,))
        self.assertLess(sc["unresolved"], 17)
        self.assertEqual(sc["names"]["403"], "sys_tty_write")


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


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file()
                     and _GHIDRA_JSON and Path(_GHIDRA_JSON).is_file(),
                     "DAL_EBOOT_ELF and DAL_GHIDRA_JSON must name existing files")
class RealGhidraSummaryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "summary.json"
            with contextlib.redirect_stdout(io.StringIO()),                     contextlib.redirect_stderr(io.StringIO()):
                cls.exit_code = ghidra_summary.main(
                    [_ELF_PATH, "--ghidra", _GHIDRA_JSON, "-o", str(out_path)])
            cls.summary = ghidra_summary.load_summary(out_path)

    def test_exit_code(self):
        self.assertEqual(self.exit_code, 0)

    def test_function_count(self):
        self.assertEqual(len(self.summary.functions), 2549)

    def test_word_kinds(self):
        start, end = self.summary.text
        words = (end - start) // 4
        code = sum(b - a for a, b in self.summary.code_ranges) // 4
        data = sum(b - a for a, b in self.summary.data_ranges) // 4
        self.assertEqual((code, data, words - code - data), (175301, 891, 33535))

    def test_entries_are_in_or_next_to_text(self):
        # Ghidra also starts functions in the executable sections around .text
        # (the section below it and the import stubs); everything else is in .text.
        start, end = self.summary.text
        outside = [f.entry for f in self.summary.functions if not start <= f.entry < end]
        self.assertEqual(len(self.summary.functions) - len(outside), 2388)
        stray = [hex(a) for a in outside if not (a < start and a >= 0x10200
                                                 or end <= a < 0xDE410)]
        self.assertEqual(stray, [])


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file() and _COMMITTED_SUMMARY.is_file(),
                     "DAL_EBOOT_ELF and config/ghidra_summary.json are required")
class RealSeedsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.elf = ElfFile.from_path(_ELF_PATH)
        cls.text = cell.find_text_range(cls.elf)
        cls.image = CodeImage.from_elf(cls.elf, cls.text)
        cls.thunks = seeds.find_import_thunks(
            cls.elf, nids.load_nid_table(nids.DEFAULT_NIDS))
        summary = ghidra_summary.load_summary(_COMMITTED_SUMMARY)
        cls.census = seeds.collect_recovery_seeds(
            cls.elf, cls.image, summary, cls.thunks, jump_table_targets=set(), extra=[],
            prologue=False)

    def test_anchor_counts(self):
        counts = self.census.counts
        expected = {"opd": 854, "opd_not_ghidra": 0, "bl_targets_in_text": 1697,
                    "bl_targets_not_ghidra": 139, "bl_targets_thunk": 161, "thunks": 166,
                    "union_ghidra_bl_opd": 2688}
        self.assertEqual({k: counts[k] for k in expected}, expected)

    def test_thunks_load_their_slots(self):
        self.assertEqual(seeds.check_thunk_slots(self.elf, self.thunks), [])

    def test_stub_range_is_executable_and_outside_text(self):
        start, end = seeds.stub_range(self.thunks)
        self.assertTrue(any(s.p_vaddr <= start and end <= s.p_vaddr + s.p_filesz
                            for s in self.elf.exec_segments()))
        self.assertTrue(end <= self.text[0] or start >= self.text[1])

    def test_every_seed_starts_on_a_decodable_word_in_text(self):
        for seed in self.census.seeds:
            self.assertTrue(self.image.valid(seed.addr), hex(seed.addr))


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file(),
                     "DAL_EBOOT_ELF is not set or does not name an existing file")
class RealJumpTableTest(unittest.TestCase):
    # bctr site -> entry count (all rel32 tables placed right after the branch).
    EXPECTED_COUNTS = {
        0x966E4: 90, 0x96E74: 47, 0xC5EAC: 84, 0xC68D4: 25, 0xC8968: 84, 0xC8BBC: 25,
        0xCA058: 25, 0xCF948: 84, 0xCFDCC: 25, 0xD030C: 13, 0xD07F4: 48, 0xD1300: 148,
        0xD1804: 21, 0xD33A4: 13,
    }

    @classmethod
    def setUpClass(cls):
        elf = ElfFile.from_path(_ELF_PATH)
        cls.image = CodeImage.from_elf(elf, cell.find_text_range(elf))
        cls.tables, cls.unresolved = jumptab.recover_jump_tables(
            cls.image, jumptab.make_reader(elf), cell.parse_entry(elf).toc)

    def test_every_site_is_accounted_for(self):
        sites = {insn.addr for insn in self.image.insns
                 if insn.spec is not None and insn.spec.name == "bcctr"
                 and not insn.field("LK")}
        self.assertEqual(len(sites), 14)
        self.assertEqual(set(self.tables) | set(self.unresolved), sites)
        self.assertEqual(set(self.tables) & set(self.unresolved), set())

    def test_all_sites_resolve_with_expected_counts(self):
        self.assertEqual(self.unresolved, {})
        self.assertEqual({a: t.count for a, t in self.tables.items()}, self.EXPECTED_COUNTS)

    def test_tables_follow_the_branch(self):
        for addr, table in self.tables.items():
            with self.subTest(site=hex(addr)):
                self.assertEqual((table.kind, table.table, table.table_in_text),
                                 ("rel32", addr + 4, True))

    def test_entry_totals(self):
        targets = [t for table in self.tables.values() for t in table.targets]
        self.assertEqual((len(targets), len(set(targets))), (732, 143))

    def test_targets_are_decodable_code_in_text(self):
        for table in self.tables.values():
            for target in table.targets:
                self.assertTrue(self.image.valid(target), hex(target))


_RECOVER_CACHE = []


def _recover_result():
    """One `recover` run per process; the command takes a while on the real ELF."""
    if not _RECOVER_CACHE:
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "functions.json"
            report_path = Path(tmp) / "report.json"
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
                code = recover.main([_ELF_PATH, "-o", str(out_path), "--report", str(report_path),
                                     "--seeds-out", str(Path(tmp) / "seeds.txt")])
            db = report = None
            if code == 0:
                db = json.loads(out_path.read_text(encoding="utf-8"))
            if report_path.is_file():
                report = json.loads(report_path.read_text(encoding="utf-8"))
        _RECOVER_CACHE.append((code, stdout.getvalue(), db, report))
    return _RECOVER_CACHE[0]


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file() and _COMMITTED_SUMMARY.is_file(),
                     "DAL_EBOOT_ELF and config/ghidra_summary.json are required")
class RealRecoverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.code, cls.stdout, cls.db, cls.report = _recover_result()

    def test_exit_code_and_convergence(self):
        self.assertEqual(self.code, 0)
        self.assertTrue(self.report["partition"]["converged"])
        self.assertIn("elapsed:", self.stdout)

    def test_every_ghidra_start_in_text_is_a_function_entry(self):
        summary = ghidra_summary.load_summary(_COMMITTED_SUMMARY)
        start, end = summary.text
        rejected = {int(r["addr"], 16) for r in self.report["seeds"]["rejected"]}
        entries = {int(f["entry"], 16) for f in self.db["functions"]}
        missing = [hex(f.entry) for f in summary.functions
                   if start <= f.entry < end and f.entry not in rejected
                   and f.entry not in entries]
        self.assertEqual(missing, [])

    def test_thunks(self):
        self.assertEqual(len(self.db["import_thunks"]), 166)
        self.assertEqual(self.report["thunks"]["count"], 166)
        self.assertEqual(self.report["thunks"]["slot_mismatches"], [])

    def test_function_count_covers_the_seed_union(self):
        counts = self.report["seeds"]["counts"]
        floor = counts["union_ghidra_bl_opd"] - counts["rejected"]
        self.assertGreaterEqual(len(self.db["functions"]), floor)
        self.assertGreaterEqual(len(self.db["functions"]), 2688 - counts["rejected"])

    def test_jump_tables_are_all_resolved(self):
        jt = self.report["jump_tables"]
        self.assertEqual((jt["bcctr_sites"], jt["resolved"], jt["targets"]), (14, 14, 732))
        self.assertEqual(self.db["unresolved_indirect"], [])
        self.assertEqual(len(self.db["jump_tables"]), 14)

    def test_database_is_sorted_and_ranges_are_disjoint(self):
        entries = [f["entry"] for f in self.db["functions"]]
        self.assertEqual(entries, sorted(entries))
        spans = sorted((int(a, 16), int(b, 16))
                       for f in self.db["functions"] for a, b in f["ranges"])
        for (_, prev_end), (next_start, _) in zip(spans, spans[1:]):
            self.assertLessEqual(prev_end, next_start)


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file() and _COMMITTED_SUMMARY.is_file(),
                     "DAL_EBOOT_ELF and config/ghidra_summary.json are required")
class RealCoverageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        _, cls.stdout, _, report = _recover_result()
        cls.coverage = report["coverage"]
        cls.ghidra = report["ghidra"]

    def test_uncovered_words_add_up(self):
        cov = self.coverage
        self.assertEqual(cov["text_words"], 209727)
        self.assertEqual(cov["owned_words"] + cov["uncovered"]["words"], cov["text_words"])
        by_class = cov["uncovered"]["by_class"]
        self.assertEqual(sum(v["words"] for v in by_class.values()), cov["uncovered"]["words"])
        # One run per recovered switch table, 732 entries in all.
        self.assertEqual((by_class["jump_table"]["words"], by_class["jump_table"]["runs"]),
                         (732, 14))

    def test_cell_truncated_ghidra_functions_are_extended(self):
        cell_info = self.ghidra["cell_truncated"]
        self.assertEqual(cell_info["count"], 76)
        # Observed: every truncated function is owned past its Cell-only word by the same entry.
        self.assertEqual(cell_info["extended"], 76)
        self.assertEqual(cell_info["not_extended"], [])

    def test_ghidra_functions_are_never_smaller_in_our_partition(self):
        ext = self.ghidra["extents"]
        self.assertEqual((ext["ours_smaller"]["count"], ext["different"]["count"]), (0, 0))
        self.assertEqual(self.ghidra["code_not_owned"]["words"], 0)

    def test_stdout_has_the_coverage_lines(self):
        prefixes = [ln.split(":")[0] for ln in self.stdout.splitlines()]
        self.assertEqual(prefixes[-5:], ["coverage", "ghidra extents", "ghidra code not owned",
                                         "syscalls", "elapsed"])


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file() and _COMMITTED_SUMMARY.is_file(),
                     "DAL_EBOOT_ELF and config/ghidra_summary.json are required")
class RealSyscallCfgTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        _, cls.stdout, _, report = _recover_result()
        cls.syscalls = report["syscalls"]

    def test_census_total_and_the_site_the_probe_cannot_resolve(self):
        sc = self.syscalls
        self.assertEqual(sc["sites"] + sc["sites_outside_text"], 217)
        self.assertIn("0x000c7eec", sc["cfg_only"])
        self.assertEqual(sc["probe_disagreements"], [])
        self.assertNotIn("0x000c7eec", [u["addr"] for u in sc["unresolved"]])

    def test_stdout_line(self):
        line = next(ln for ln in self.stdout.splitlines() if ln.startswith("syscalls:"))
        self.assertIn("%d sites in .text" % self.syscalls["sites"], line)


_EMIT_CENSUS_CACHE = []


def _emit_census_result():
    """One `emit --census` run per process, against the committed function DB."""
    if not _EMIT_CENSUS_CACHE:
        with tempfile.TemporaryDirectory() as tmp:
            census_path = Path(tmp) / "census.json"
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(io.StringIO()):
                code = emit.main([_ELF_PATH, "--census", "--census-out", str(census_path)])
            doc = None
            if census_path.is_file():
                doc = json.loads(census_path.read_text(encoding="utf-8"))
        _EMIT_CENSUS_CACHE.append((code, stdout.getvalue(), doc))
    return _EMIT_CENSUS_CACHE[0]


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file(),
                     "DAL_EBOOT_ELF is not set or does not name an existing file")
class RealEmitCensusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.code, cls.stdout, cls.doc = _emit_census_result()

    def test_census_totals(self):
        self.assertEqual(self.code, 0)
        self.assertEqual((self.doc["words"], self.doc["distinct"]), (206045, 168))
        self.assertEqual(self.doc["invalid_owned"], [])

    def test_every_counted_mnemonic_has_a_group(self):
        self.assertTrue(all(m["group"] is not None for m in self.doc["mnemonics"]))
        self.assertEqual(sum(m["count"] for m in self.doc["mnemonics"]), self.doc["words"])
        self.assertEqual(sum(g["words"] for g in self.doc["by_group"].values()),
                         self.doc["words"])

    def test_stdout_header(self):
        self.assertEqual(self.stdout.splitlines()[0],
                         "census: 206045 words, 168 distinct mnemonics, 0 invalid owned")


@unittest.skipUnless(_ELF_PATH and Path(_ELF_PATH).is_file(),
                     "DAL_EBOOT_ELF is not set or does not name an existing file")
class RealEbootNidTest(unittest.TestCase):
    _VERIFIED = _VERIFIED_NAMES

    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "info.json"
            with contextlib.redirect_stdout(io.StringIO()),                     contextlib.redirect_stderr(io.StringIO()):
                info.main([_ELF_PATH, "-o", str(out_path)])
            doc = json.loads(out_path.read_text(encoding="utf-8"))
        cls.fnids = {m["name"]: {int(f["fnid"], 16) for f in m["functions"]}
                     for m in doc["imports"]}

    def test_verified_names_are_imported(self):
        for module, name in self._VERIFIED:
            with self.subTest(module=module, name=name):
                self.assertIn(nids.compute_nid(name), self.fnids[module])

    def test_known_answer(self):
        self.assertEqual(nids.compute_nid("cellSpursInitializeWithAttribute2"), 0x30AA96C4)
        self.assertEqual(nids.compute_nid("cellSailPlayerBoot"), 0xBDF21B0F)
        self.assertEqual(nids.compute_nid("cellAtracDecode"), 0x8EB0E65F)


if __name__ == "__main__":
    unittest.main()
