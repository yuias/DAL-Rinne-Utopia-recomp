"""Tests for the `emit` output writer: files, determinism, chunking, trampolines, report."""

import contextlib
import hashlib
import io
import json
import os
import re
import struct
import unittest
from pathlib import Path
from unittest import mock

from ps3recomp import cell, emit, emit_output
from ps3recomp.elf import ElfFile
from ps3recomp.emit_db import GROUPS, DbThunk

from tests import ppc_asm as asm
from tests.elf_fixture import _PS3_TEXT_WORDS, PS3_TEXT_VA, build_ps3_fixture
from tests.test_emit_db import TempDirTestCase, db_doc, hexs

TEXT = PS3_TEXT_VA
STD_R2_40_R1 = 62 << 26 | 2 << 21 | 1 << 16 | 40        # std r2,40(r1)
FILES = ["ppu_chunk_000.c", "ppu_funcs.h", "ppu_imports.c", "ppu_override_handlers.h",
         "ppu_override_stubs.c", "ppu_overrides.c", "ppu_tables.c", "sources.cmake"]
REPORT_KEYS = ["schema", "generator", "input", "functions", "instructions", "import_trampolines",
               "thunks_without_toc_save", "chunks", "largest_functions", "unhandled", "edges",
               "dynamic_targets", "labels", "loop_hooks", "overrides", "output_bytes",
               "elapsed_s"]


class PlanChunksTest(unittest.TestCase):
    def test_balanced_split(self):
        self.assertEqual(emit_output.plan_chunks([3, 2, 2], 3), [(0, 1), (1, 2), (2, 3)])

    def test_single_chunk(self):
        self.assertEqual(emit_output.plan_chunks([3, 2, 2], 1), [(0, 3)])

    def test_fewer_chunks_than_requested(self):
        self.assertEqual(emit_output.plan_chunks([5, 5], 16), [(0, 1), (1, 2)])

    def test_big_function_does_not_create_empty_chunks(self):
        plan = emit_output.plan_chunks([100, 1, 1, 1], 4)
        self.assertEqual(plan[0], (0, 1))
        self.assertTrue(all(start < end for start, end in plan))
        self.assertLessEqual(len(plan), 4)
        self.assertEqual(plan[-1][1], 4)

    def test_no_functions(self):
        self.assertEqual(emit_output.plan_chunks([], 4), [])


class TrampolineTest(unittest.TestCase):
    THUNK = DbThunk(0x000DCF50, "sys_io", 0x1CF98800, "cellPadInit", 0x1004A4)
    WITH_SAVE = [asm.li(12, 0), asm.oris(12, 12, 0x10), asm.lwz(12, 0x4A4, 12), STD_R2_40_R1,
                 asm.lwz(0, 0, 12), asm.lwz(2, 4, 12), asm.mtctr(0), asm.bctr()]

    def test_with_toc_save(self):
        text, saves = emit_output.render_trampoline(self.THUNK, 7, self.WITH_SAVE)
        self.assertTrue(saves)
        self.assertEqual(text.splitlines(), [
            "// sys_io cellPadInit 0x1cf98800",
            "DAL_PPU_FUNC(imp_000dcf50) {",
            "    DAL_ENTER(0x000dcf50u);",
            "    dalWrite64(base, (uint32_t)(ctx->gpr[1] + 0x0000000000000028ull), ctx->gpr[2]);",
            "    DAL_HLE(7);",
            "}"])

    def test_without_toc_save(self):
        words = [w for w in self.WITH_SAVE if w != STD_R2_40_R1] + [asm.nop()]
        text, saves = emit_output.render_trampoline(self.THUNK, 0, words)
        self.assertFalse(saves)
        self.assertNotIn("dalWrite64", text)
        self.assertIn("DAL_HLE(0);", text)

    def test_other_store_is_not_a_toc_save(self):
        other = 62 << 26 | 3 << 21 | 1 << 16 | 40            # std r3,40(r1)
        self.assertIsNone(emit_output.toc_save_statement([other], 0))
        self.assertIsNone(emit_output.toc_save_statement([62 << 26 | 2 << 21 | 4 << 16], 0))

    def test_negative_displacement(self):
        word = 62 << 26 | 2 << 21 | 1 << 16 | (0x10000 - 8)  # std r2,-8(r1)
        self.assertIn("0xfffffffffffffff8ull", emit_output.toc_save_statement([word], 0))

    def test_unreadable_thunk(self):
        text, saves = emit_output.render_trampoline(self.THUNK, 1, None)
        self.assertFalse(saves)
        self.assertIn("DAL_HLE(1);", text)

    def test_unnamed_import_and_string_escaping(self):
        thunk = DbThunk(0x1000, "m", 5, None, 0)
        text, _ = emit_output.render_trampoline(thunk, 0, None)
        self.assertTrue(text.startswith("// m - 0x00000005\n"))
        self.assertEqual(emit_output.c_string(None), "nullptr")
        self.assertEqual(emit_output.c_string('a"b\\c'), '"a\\042b\\134c"')


class EmitFixtureCase(TempDirTestCase):
    """Runs `emit` on the fixture ELF with every output redirected into the temp directory."""

    def setUp(self):
        super().setUp()
        data = self.elf_bytes()
        self.elf_path = self.tmp / "fixture.elf"
        self.elf_path.write_bytes(data)
        self.sha = hashlib.sha256(data).hexdigest()
        self.text = cell.find_text_range(ElfFile.from_bytes(data))
        self.out = self.tmp / "out"
        self.report = self.tmp / "report.json"
        # The committed config/overrides.json is pinned to the game, not to the fixture.
        self.default_overrides = self.tmp / "default-overrides.json"
        patcher = mock.patch.object(emit, "DEFAULT_OVERRIDES", self.default_overrides)
        patcher.start()
        self.addCleanup(patcher.stop)

    def elf_bytes(self) -> bytes:
        return build_ps3_fixture()

    def make_db(self, functions=None):
        return self.write_db(db_doc(sha=self.sha, text=self.text, functions=functions))

    def run_cli(self, db_path, *extra):
        out, err = io.StringIO(), io.StringIO()
        argv = [str(self.elf_path), "--db", str(db_path), "--out", str(self.out),
                "--report", str(self.report), "--census-out", str(self.tmp / "census.json")]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = emit.main(argv + list(extra))
        return code, out.getvalue(), err.getvalue()

    def snapshot(self):
        return {p.name: p.read_bytes() for p in sorted(self.out.iterdir())}

    def three_functions(self):
        return self.make_db([(TEXT, [(TEXT, TEXT + 0xC)]),
                             (TEXT + 0xC, [(TEXT + 0xC, TEXT + 0x14)]),
                             (TEXT + 0x14, [(TEXT + 0x14, TEXT + 0x1C)])])


class EmitCliTest(EmitFixtureCase):

    def test_files_and_stdout(self):
        db = self.make_db([(TEXT, [(TEXT, TEXT + 0x1C)])])
        code, out, err = self.run_cli(db)
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), FILES)
        lines = out.splitlines()
        self.assertEqual(lines[0], "emit: 1 functions (7 insns) in 1 chunks, "
                                   "1 import trampolines (1 without TOC save)")
        self.assertTrue(lines[1].startswith("unhandled: "))
        self.assertTrue(lines[2].startswith("calls: direct 0, "))
        self.assertTrue(lines[3].startswith("output: "))
        self.assertTrue(lines[4].startswith("elapsed: "))
        for name, data in self.snapshot().items():
            self.assertTrue(data.startswith(
                (b"# " if name == "sources.cmake" else b"// ") + b"Generated by ps3tool emit "
                b"from executable " + self.sha.encode() + b"; do not edit.\n"), name)
            self.assertNotIn(b"\r", data)
            self.assertNotIn(str(self.tmp).encode(), data)

    def test_sources_cmake_is_relative(self):
        self.run_cli(self.three_functions(), "--chunks", "3")
        text = (self.out / "sources.cmake").read_text(encoding="utf-8")
        entries = re.findall(r'"([^"]+)"', text)
        self.assertEqual(entries, ["${CMAKE_CURRENT_LIST_DIR}/ppu_chunk_00%d.c" % i
                                   for i in range(3)]
                         + ["${CMAKE_CURRENT_LIST_DIR}/ppu_imports.c",
                            "${CMAKE_CURRENT_LIST_DIR}/ppu_tables.c",
                            "${CMAKE_CURRENT_LIST_DIR}/ppu_overrides.c",
                            "${CMAKE_CURRENT_LIST_DIR}/ppu_override_stubs.c"])
        self.assertIn('set(DAL_PPU_OVERRIDE_STUB_SOURCES '
                      '"${CMAKE_CURRENT_LIST_DIR}/ppu_override_stubs.c")\n', text)
        self.assertNotRegex(text, r"[A-Za-z]:[\\/]")

    def test_rerun_is_byte_identical_and_keeps_mtimes(self):
        db = self.three_functions()
        self.run_cli(db)
        first = self.snapshot()
        stamp = 1_000_000_000
        for path in self.out.iterdir():
            os.utime(path, (stamp, stamp))
        self.run_cli(db)
        self.assertEqual(self.snapshot(), first)
        self.assertEqual({p.stat().st_mtime for p in self.out.iterdir()}, {float(stamp)})

    def test_fewer_chunks_remove_stale_files(self):
        db = self.three_functions()
        self.run_cli(db, "--chunks", "3")
        self.assertEqual([p.name for p in sorted(self.out.glob("ppu_chunk_*.c"))],
                         ["ppu_chunk_000.c", "ppu_chunk_001.c", "ppu_chunk_002.c"])
        self.run_cli(db, "--chunks", "1")
        self.assertEqual([p.name for p in sorted(self.out.glob("ppu_chunk_*.c"))],
                         ["ppu_chunk_000.c"])
        self.assertNotIn("ppu_chunk_001.c", (self.out / "sources.cmake").read_text())

    def test_mapping_table_is_sorted_and_complete(self):
        self.run_cli(self.three_functions())
        text = (self.out / "ppu_tables.c").read_text(encoding="utf-8")
        guests = [int(m, 16) for m in re.findall(r"^    \{ 0x([0-9a-f]{8})u, (?:fn|imp)_", text,
                                                 re.M)]
        self.assertEqual(guests, sorted(guests))
        self.assertEqual(len(guests), 4)
        self.assertIn("kDalPpuFuncMappingCount = 4;", text)
        self.assertIn("kDalPpuImportCount = 1;", text)
        self.assertIn('{ 0x00030000u, 0x00001234u, "cellFoo", nullptr },', text)
        self.assertIn('kDalPpuExecutableSha256[] = "%s";' % self.sha, text)

    def test_max_unhandled_exits_9_after_writing(self):
        # The all-zero word after blr is invalid code, so it stays unhandled whatever the handlers cover.
        db = self.make_db([(TEXT, [(TEXT, TEXT + 0x20)])])
        code, _, _ = self.run_cli(db, "--max-unhandled", "0")
        self.assertEqual(code, 9)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), FILES)
        self.assertTrue(self.report.is_file())
        report = json.loads(self.report.read_text(encoding="utf-8"))
        total = report["unhandled"]["total"]
        self.assertGreater(total, 0)
        code, _, _ = self.run_cli(db, "--max-unhandled", str(total))
        self.assertEqual(code, 0)

    def test_report(self):
        code, _, _ = self.run_cli(self.three_functions())
        self.assertEqual(code, 0)
        raw = self.report.read_bytes()
        self.assertTrue(raw.endswith(b"}\n"))
        report = json.loads(raw.decode("utf-8"))
        self.assertEqual(list(report), REPORT_KEYS)
        self.assertEqual((report["schema"], report["generator"]), (1, "ps3tool emit"))
        self.assertEqual(report["input"]["executable_sha256"], self.sha)
        self.assertEqual(report["input"]["functions_db_sha256"],
                         hashlib.sha256(self.tmp.joinpath("functions.json").read_bytes())
                         .hexdigest())
        self.assertEqual((report["functions"], report["instructions"],
                          report["import_trampolines"]), (3, 7, 1))
        self.assertEqual(report["thunks_without_toc_save"], [hexs(0x30000)])
        self.assertEqual([c["functions"] for c in report["chunks"]], [1, 1, 1])
        self.assertEqual(report["largest_functions"][0], {"entry": hexs(TEXT), "instructions": 3})
        self.assertEqual(list(report["edges"]), list(emit_output.EDGE_KEYS))
        self.assertEqual(report["edges"]["syscalls"], 1)
        self.assertEqual(report["edges"]["returns"], 1)
        self.assertEqual(list(report["unhandled"]["by_group"]),
                         list(GROUPS))
        self.assertEqual(report["unhandled"]["total"],
                         sum(report["unhandled"]["by_mnemonic"].values()))
        self.assertEqual(report["output_bytes"],
                         sum(len(d) for d in self.snapshot().values()))

    def test_comments_option(self):
        db = self.make_db([(TEXT, [(TEXT, TEXT + 0x1C)])])
        self.run_cli(db, "--comments")
        text = (self.out / "ppu_chunk_000.c").read_text(encoding="utf-8")
        self.assertIn("// 0x00010000: 7c0802a6", text)


# Functions of the override fixture, one instruction each (the last two words stay unowned).
FN_B, FN_A, FN_C, FN_D, FN_E, FN_F = (TEXT + 4 * i for i in range(6))


class EmitOverridesCliTest(EmitFixtureCase):
    """B is called by A (bl), tail-called by C (b) and E (switch case); D calls through CTR."""

    def elf_bytes(self) -> bytes:
        words = [asm.blr(), asm.bl(FN_A, FN_B), asm.b(FN_C, FN_B), asm.bctrl(), asm.bctr(),
                 asm.b(FN_F, FN_F), asm.blr(), 0]
        data = build_ps3_fixture()
        old = struct.pack(">8I", *_PS3_TEXT_WORDS)
        self.assertEqual(data.count(old), 1)
        return data.replace(old, struct.pack(">8I", *words))

    def make_db(self, functions=None):
        doc = db_doc(sha=self.sha, text=self.text, functions=[
            (entry, [(entry, entry + 4)]) for entry in (FN_B, FN_A, FN_C, FN_D, FN_E, FN_F)])
        doc["jump_tables"] = [{"branch": hexs(FN_E), "table": hexs(0x50000), "kind": "abs",
                               "count": 1, "targets": [hexs(FN_B)]}]
        return self.write_db(doc)

    def write_overrides(self, entries, name="overrides.json", sha=None) -> Path:
        path = self.tmp / name
        path.write_text(json.dumps({"schema": 1, "executable_sha256": sha or self.sha,
                                    "entries": entries}), encoding="utf-8", newline="\n")
        return path

    @staticmethod
    def item(address, kind="hook", handler="dalHookLogCall", **extra):
        return {"address": hexs(address), "kind": kind, "handler": handler, **extra}

    def emit(self, *extra):
        code, out, err = self.run_cli(self.make_db(), "--chunks", "1", *extra)
        self.assertEqual((code, err), (0, ""))
        return out

    def chunk(self) -> str:
        return (self.out / "ppu_chunk_000.c").read_text(encoding="utf-8")

    def function_text(self, name) -> str:
        text = self.chunk()
        start = text.index("DAL_PPU_FUNC(%s) {" % name)
        return text[start:text.index("\n}\n", start)]

    def test_hook_reaches_every_path_and_moves_the_body(self):
        path = self.write_overrides([self.item(FN_B, label="bee")])
        self.emit("--overrides", str(path))
        chunk = self.chunk()
        self.assertIn("DAL_CALL(fn_00010000, 0x00010008u);", self.function_text("fn_00010004"))
        self.assertIn("DAL_TAIL(fn_00010000);", self.function_text("fn_00010008"))
        self.assertIn("case 0x00010000u: DAL_TAIL(fn_00010000);", self.function_text("fn_00010010"))
        self.assertIn("DAL_CALL_DYN(", self.function_text("fn_0001000c"))
        # The wrapper takes the name; the body keeps its title and loses DAL_ENTER.
        self.assertNotIn("DAL_PPU_FUNC(fn_00010000) {", chunk)
        self.assertIn("// bee\nDAL_PPU_FUNC(fn_00010000_orig) {\n    ", chunk)
        self.assertNotIn("DAL_ENTER(0x00010000u)", chunk)
        self.assertIn("DAL_ENTER(0x00010004u)", chunk)
        tables = (self.out / "ppu_tables.c").read_text(encoding="utf-8")
        self.assertIn("    { 0x00010000u, fn_00010000 },\n", tables)
        self.assertNotIn("_orig", tables)
        source = (self.out / "ppu_overrides.c").read_text(encoding="utf-8")
        self.assertIn('{ 0x00010000u, kDalOverrideKindHook, fn_00010000_orig, &override_calls[0], '
                      '"dalHookLogCall", "bee" },', source)
        self.assertIn("// hook dalHookLogCall: bee\nDAL_PPU_FUNC(fn_00010000) {\n"
                      "    DAL_ENTER(0x00010000u);\n    dalCountCall(&override_calls[0]);\n"
                      "    dalHookLogCall(ctx, base, &kDalPpuOverrides[0]);\n"
                      "    DAL_TAIL(fn_00010000_orig);\n}\n", source)
        funcs = (self.out / "ppu_funcs.h").read_text(encoding="utf-8")
        self.assertTrue(funcs.endswith(
            "\n// Original bodies of the entries in the overrides file; fn_%08x is the wrapper.\n"
            "DAL_PPU_FUNC(fn_00010000_orig);\n"))
        self.assertIn("DAL_OVERRIDE_HANDLER(dalHookLogCall);",
                      (self.out / "ppu_override_handlers.h").read_text(encoding="utf-8"))
        self.assertIn("DAL_OVERRIDE_HANDLER(dalHookLogCall) {",
                      (self.out / "ppu_override_stubs.c").read_text(encoding="utf-8"))

    def test_override_wrapper_returns_without_entering_the_body(self):
        path = self.write_overrides([self.item(FN_B, "override", "skipSpuWait")])
        self.emit("--overrides", str(path))
        chunk = self.chunk()
        self.assertIn("DAL_CALL(fn_00010000, 0x00010008u);", chunk)
        self.assertIn("DAL_PPU_FUNC(fn_00010000_orig) {", chunk)
        source = (self.out / "ppu_overrides.c").read_text(encoding="utf-8")
        self.assertIn("// override skipSpuWait\nDAL_PPU_FUNC(fn_00010000) {\n"
                      "    DAL_ENTER(0x00010000u);\n    dalCountCall(&override_calls[0]);\n"
                      "    skipSpuWait(ctx, base, &kDalPpuOverrides[0]);\n}\n", source)
        self.assertNotIn("DAL_TAIL(fn_00010000_orig)", source)

    def test_entries_are_written_in_address_order(self):
        path = self.write_overrides([self.item(FN_C, "override", "second"),
                                     self.item(FN_B, "hook", "first")])
        self.emit("--overrides", str(path))
        source = (self.out / "ppu_overrides.c").read_text(encoding="utf-8")
        self.assertLess(source.index("fn_00010000_orig, &override_calls[0]"),
                        source.index("fn_00010008_orig, &override_calls[1]"))
        funcs = (self.out / "ppu_funcs.h").read_text(encoding="utf-8")
        self.assertLess(funcs.index("fn_00010000_orig"), funcs.index("fn_00010008_orig"))

    def test_default_file_is_used_without_options(self):
        self.write_overrides([self.item(FN_B)], name=self.default_overrides.name)
        self.emit()
        self.assertIn("DAL_PPU_FUNC(fn_00010000_orig) {", self.chunk())

    def test_empty_configurations_give_identical_trees(self):
        trees = {}
        empty = self.write_overrides([])
        for label, extra in (("missing default", ()), ("no-overrides", ("--no-overrides",)),
                             ("empty file", ("--overrides", str(empty))),
                             ("empty default", None)):
            if extra is None:
                empty.replace(self.default_overrides)
                extra = ()
            self.out = self.tmp / label.replace(" ", "-")
            self.emit(*extra)
            trees[label] = self.snapshot()
        reference = trees["missing default"]
        self.assertEqual(sorted(reference), FILES)
        for label, tree in trees.items():
            self.assertEqual(tree, reference, label)
        for name in ("ppu_chunk_000.c", "ppu_funcs.h", "ppu_tables.c"):
            self.assertNotIn(b"_orig", reference[name])
        self.assertIn(b"DAL_ENTER(0x00010000u)", reference["ppu_chunk_000.c"])
        self.assertIn(b"const DalPpuOverride kDalPpuOverrides[1] = {};\n"
                      b"const uint32_t kDalPpuOverrideCount = 0;\n", reference["ppu_overrides.c"])

    def test_no_overrides_ignores_the_file_even_when_it_is_broken(self):
        self.default_overrides.write_text("not json", encoding="utf-8")
        out = self.emit("--no-overrides")
        self.assertIn("overrides: disabled by --no-overrides\n", out)
        self.assertNotIn("_orig", self.chunk())

    def test_overrides_and_no_overrides_are_exclusive(self):
        code, _, err = self.run_cli(self.make_db(), "--overrides", "x.json", "--no-overrides")
        self.assertEqual(code, 2)
        self.assertIn("not allowed with argument", err)
        self.assertFalse(self.out.exists())

    def test_bad_file_exits_before_anything_is_written(self):
        cases = [
            (10, self.write_overrides([self.item(FN_B + 2)], "inside.json"),
             "overrides entry 0x00010002 is not a function entry (inside fn_00010000)"),
            (10, self.write_overrides([self.item(0x30000)], "stub.json"),
             "overrides entry 0x00030000 is the import stub of cellFoo::0x00001234"),
            (4, self.write_overrides([], "other.json", sha="ab" * 32),
             "overrides file was made for a different ELF"),
            (1, self.tmp / "missing.json", "cannot read overrides file"),
        ]
        for code, path, message in cases:
            with self.subTest(message):
                got, out, err = self.run_cli(self.make_db(), "--overrides", str(path))
                self.assertEqual((got, out), (code, ""))
                self.assertIn("error: " + message, err)
                self.assertFalse(self.out.exists())
                self.assertFalse(self.report.exists())
                self.assertFalse((self.tmp / "census.json").exists())

    def test_report_section_and_stdout(self):
        hook = self.item(FN_F, label="spin")
        path = self.write_overrides([self.item(FN_B, "override", "dalOverrideReturn0"), hook])
        out = self.emit("--overrides", str(path))
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["overrides"], {
            "enabled": True,
            "file_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "entries": [
                {"address": "0x00010000", "kind": "override", "handler": "dalOverrideReturn0",
                 "label": None, "self_entry_loop": False},
                {"address": "0x00010014", "kind": "hook", "handler": "dalHookLogCall",
                 "label": "spin", "self_entry_loop": True}]})
        self.assertIn("\nL_00010014:\n", self.chunk())
        lines = out.splitlines()
        at = next(i for i, line in enumerate(lines) if line.startswith("calls: "))
        self.assertEqual(lines[at + 1:at + 3], [
            "overrides: 2 entries (override 1, hook 1)",
            "overrides: 0x00010014 branches to its own entry; "
            "that loop does not run the hook handler again"])
        self.assertTrue(lines[at + 3].startswith("output: "))

    def test_report_without_entries_and_with_no_overrides(self):
        out = self.emit()
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["overrides"],
                         {"enabled": True, "file_sha256": None, "entries": []})
        self.assertNotIn("overrides:", out)
        out = self.emit("--no-overrides")
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["overrides"],
                         {"enabled": False, "file_sha256": None, "entries": []})
        self.assertEqual(out.splitlines()[3], "overrides: disabled by --no-overrides")

    def test_output_is_deterministic(self):
        path = self.write_overrides([self.item(FN_B), self.item(FN_F, "override", "other")])
        self.emit("--overrides", str(path))
        first = self.snapshot()
        self.emit("--overrides", str(path))
        self.assertEqual(self.snapshot(), first)


if __name__ == "__main__":
    unittest.main()
