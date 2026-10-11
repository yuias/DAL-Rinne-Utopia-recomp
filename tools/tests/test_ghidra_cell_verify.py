import contextlib
import io
import json
import os
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from ps3recomp import ghidra, ghidra_cell, ghidra_cell_verify as gcv, ppu
from ps3recomp.coverage import CELL_ONLY_FORMS
from ps3recomp.info import CliError
from ps3recomp.ppu_format import format_insn

BLK_WORDS = (0x01122334, 0x45566778, 0x899AABBC, 0xCDDEEFF0)
STOCK = ghidra.DEFAULT_LANGUAGE
CELL = ghidra_cell.CELL_LANGUAGE


def _good_result(meta: gcv.RequestMeta, language: str = CELL) -> dict:
    """A result.json in which every row matches, every case passes and the decompile is clean."""
    table = []
    for row in meta.rows:
        if row.word is None:
            continue
        text = format_insn(ppu.decode(row.word, row.addr, strict=True), False)
        mnemonic, _, operands = text.partition(" ")
        table.append({"addr": "0x%08x" % row.addr, "mnemonic": mnemonic,
                      "operands": operands or None})
    cases = [{"name": c.name, "error": None,
              "regs": {gcv.ghidra_register(n): "0x%x" % v for n, v in c.expect_regs},
              "mem": [{"addr": "0x%08x" % a, "hex": d.hex()} for a, d in c.expect_mem]}
             for c in meta.cases]
    return {"language": language, "table": table, "cases": cases,
            "decompile": {"completed": True, "error": "", "c": "void f(void) { return; }"}}


def _entry(result: dict, key: str, ident: str) -> dict:
    # The table is keyed by address text, the cases by name.
    field = "addr" if key == "table" else "name"
    return next(e for e in result[key] if e[field] == ident)


def _fake_install(root: Path) -> Path:
    ghidra_dir = root / "ghidra"
    (ghidra_dir / "support").mkdir(parents=True)
    for name in ("analyzeHeadless", "analyzeHeadless.bat"):
        (ghidra_dir / "support" / name).write_text("", encoding="utf-8")
    (ghidra_dir / "Ghidra").mkdir()
    (ghidra_dir / "Ghidra" / "application.properties").write_text(
        "application.version=9.8.7\napplication.release.name=TEST\n", encoding="utf-8")
    return ghidra_dir


class FakeHeadless:
    """Stands in for ghidra._run_headless: records the call and writes result.json."""

    def __init__(self, result: dict | None):
        self.result = result
        self.calls: list[tuple[list[str], Path, int, object]] = []

    def __call__(self, cmd, log_dir, timeout, env=None):
        self.calls.append((cmd, log_dir, timeout, env))
        if self.result is not None:
            result_path = Path(cmd[cmd.index("-postScript") + 3])
            result_path.write_text(json.dumps(self.result), encoding="utf-8")
        return 0.0


class RequestBuilderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.meta = gcv.build_request()

    def test_every_row_gets_a_decoding_word_or_is_listed(self):
        self.assertEqual(len(self.meta.rows), len(ppu.ALL_OPS))
        for row in self.meta.rows:
            with self.subTest(name=row.spec.name):
                if row.word is None:
                    self.assertIn(row.spec.name, self.meta.skipped_rows)
                else:
                    self.assertIs(ppu.decode(row.word, strict=True).spec, row.spec)
        self.assertEqual(self.meta.skipped_rows, [])

    def test_field_encoder_is_the_inverse_of_the_decoder_fields(self):
        for name, value in gcv._OPERAND_VALUES.items():
            with self.subTest(field=name):
                word = gcv.encode_field(0, name, value)
                self.assertEqual(ppu.FIELDS[name](word), value)

    def test_operand_values_reach_the_decoder(self):
        for row in self.meta.rows:
            if row.word is None:
                continue
            insn = ppu.decode(row.word, strict=True)
            for name in gcv.operand_fields(row.spec):
                with self.subTest(op=row.spec.name, field=name):
                    self.assertEqual(insn.field(name), gcv._OPERAND_VALUES[name])

    def test_image_layout(self):
        image = self.meta.image
        self.assertEqual(len(image), 0x10100)
        word_at = lambda addr: struct.unpack_from(">I", image, addr - 0x10000)[0]
        for row in self.meta.rows:
            self.assertEqual(word_at(0x10000 + 4 * row.index), row.word)
        self.assertEqual(self.meta.request["table"][0], "0x00010000")
        self.assertEqual(len(self.meta.request["table"]), len(ppu.ALL_OPS))
        for i, case in enumerate(self.meta.cases):
            self.assertEqual(case.addr, 0x14000 + 0x40 * i)
            for j, word in enumerate(case.words):
                self.assertEqual(word_at(case.addr + 4 * j), word)
        sample = [ppu.decode(word_at(0x18000 + 4 * i)).spec.name for i in range(6)]
        self.assertEqual(sample, ["lvlx", "lvrx", "vor", "stvlx", "stvrx", "bclr"])
        self.assertEqual(word_at(0x18014), 0x4E800020)
        self.assertEqual(self.meta.request["decompile"], "0x00018000")
        self.assertEqual(self.meta.request["image_base"], "0x00010000")
        self.assertEqual(image[0x10000:], bytes(0x100))

    def test_case_selection(self):
        # 23 Cell cases (loads, idioms, stores) and the three controls.
        self.assertEqual(len(self.meta.cases), 26)
        kinds = [c.kind for c in self.meta.cases]
        self.assertEqual(kinds.count("cell"), 23)
        self.assertEqual(kinds.count("control"), 3)
        self.assertEqual({c.name for c in self.meta.cases if c.kind == "control"},
                         set(gcv.CONTROL_CASES))
        for case in self.meta.cases:
            names = {n for n in case.insn_names if n}
            self.assertEqual(case.kind == "cell", bool(names & CELL_ONLY_FORMS), case.name)
        self.assertEqual(self.meta.skipped_cases, ())

    def test_case_request(self):
        by_name = {c["name"]: c for c in self.meta.request["cases"]}
        self.assertEqual(by_name["lvlx_sh_1"], {
            "name": "lvlx_sh_1", "addr": "0x%08x" % next(
                c.addr for c in self.meta.cases if c.name == "lvlx_sh_1"),
            "count": 1, "regs": {"r4": "0x20000", "r5": "0x1"},
            "mem": [{"addr": "0x00020000",
                     "hex": "0112233445566778899aabbccddeeff00a1b2c3d4e5f60718293a4b5c6d7e8f9"}],
            "read_regs": ["vs35"], "read_mem": []})
        store = by_name["stvlx_sh_5"]
        self.assertEqual(store["read_regs"], [])
        self.assertEqual(store["read_mem"], [{"addr": "0x00020000", "len": 48}])
        self.assertEqual(int(store["regs"]["vs35"], 16), gcv.vector_value(BLK_WORDS))
        self.assertEqual(by_name["unaligned_load_idiom_sh_7"]["count"], 3)

    def test_vector_conversion(self):
        self.assertEqual(gcv.vector_value(BLK_WORDS), 0x0112233445566778899AABBCCDDEEFF0)
        with self.assertRaises(ValueError):
            gcv.vector_value((1, 2, 3))

    def test_register_names(self):
        self.assertEqual(gcv.ghidra_register("v3"), "vs35")
        self.assertEqual(gcv.ghidra_register("v31"), "vs63")
        self.assertEqual(gcv.ghidra_register("r4"), "r4")
        self.assertEqual(gcv.our_register("vs35"), "v3")
        self.assertEqual(gcv.our_register("r4"), "r4")
        for bad in ("v32", "r32", "vscr_nj", "x1"):
            with self.assertRaises(ValueError):
                gcv.ghidra_register(bad)
        for bad in ("vs31", "vs64", "v3", "r40"):
            with self.assertRaises(ValueError):
                gcv.our_register(bad)

    def test_cases_with_other_keys_are_skipped_and_listed(self):
        class Fake:
            callees = switch_targets = {}
            mem_init = mem_expect = {}

            def __init__(self, name, words, init, expect):
                self.name, self.words, self.init, self.expect = name, words, init, expect

        lvx = next(r.word for r in self.meta.rows if r.spec.name == "lvx")
        cases = [Fake(n, (lvx,), {}, {"v3": BLK_WORDS}) for n in gcv.CONTROL_CASES]
        cases.append(Fake("vscr", (lvx,), {}, {"vscr_nj": 1}))
        cases.append(Fake("lvlx_vscr", (next(r.word for r in self.meta.rows
                                              if r.spec.name == "lvlx"),), {"vscr_sat": 1}, {}))
        selected, skipped = gcv.select_cases(cases)
        self.assertEqual([c.name for c, _ in selected], list(gcv.CONTROL_CASES))
        self.assertEqual([n for n, _ in skipped], ["lvlx_vscr"])
        self.assertIn("vscr_sat", skipped[0][1])


class EvaluateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.meta = gcv.build_request()

    def run_verify(self, result: dict, language: str | None = None):
        """Returns (exit code, stdout) of verify() with a fake Ghidra that answers `result`."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ghidra_dir = _fake_install(root)
            stage = root / "stage"
            fake = FakeHeadless(result)
            out = io.StringIO()
            kwargs = {"language": language} if language else {"stage_dir": stage}
            code = 0
            with contextlib.redirect_stdout(out):
                try:
                    gcv.verify(ghidra_dir, out_dir=root / "out", run_headless=fake,
                               module_env=lambda *a: {"MARK": "1"}, **kwargs)
                except CliError as e:
                    code = e.code
                    self.last_error = str(e)
            report = root / "out" / "report.json"
            self.last_report = json.loads(report.read_text("utf-8")) if report.is_file() else None
        return code, out.getvalue()

    def test_all_good_under_the_cell_language(self):
        code, out = self.run_verify(_good_result(self.meta))
        self.assertEqual(code, 0)
        self.assertIn("language: %s (staged " % CELL, out)
        self.assertIn("table: 379 rows, match 379, mismatch 0, unknown 0, skipped 0\n", out)
        self.assertIn("table: lvlx 0x7c642c0e -> ghidra 'lvlx v3,r4,r5': ok\n", out)
        self.assertEqual(out.count(": ok\n"), 8 + 1)  # eight Cell rows and the decompile line
        self.assertIn("semantics: 26 cases, pass 26, fail 0, not decoded 0\n", out)
        self.assertIn("decompile: ok\n", out)
        self.assertTrue(out.rstrip().endswith("report.json"))
        self.assertTrue(self.last_report["ok"])

    def test_result_naming_another_language_is_a_tool_error(self):
        for reported in ("", STOCK):
            with self.subTest(reported=reported):
                result = _good_result(self.meta)
                result["language"] = reported
                code, out = self.run_verify(result)
                self.assertEqual(code, ghidra.EXIT_TOOL)
                self.assertIn(repr(reported), self.last_error)
                self.assertIn(repr(CELL), self.last_error)
                self.assertNotIn("table:", out)
        result = _good_result(self.meta, CELL)
        code, _ = self.run_verify(result, language=STOCK)
        self.assertEqual(code, ghidra.EXIT_TOOL)

    def test_malformed_hex_in_the_result_is_a_tool_error(self):
        for bad in ("zz", None, 5):
            with self.subTest(bad=bad):
                result = _good_result(self.meta)
                _entry(result, "cases", "stvlx_sh_5")["mem"][0]["hex"] = bad
                code, _ = self.run_verify(result)
                self.assertEqual(code, ghidra.EXIT_TOOL)
                self.assertIn("invalid Ghidra result", self.last_error)

    def test_missing_cell_row_fails_under_the_cell_language(self):
        result = _good_result(self.meta)
        lvlx = next(r for r in self.meta.rows if r.spec.name == "lvlx")
        _entry(result, "table", "0x%08x" % lvlx.addr)["mnemonic"] = None
        code, out = self.run_verify(result)
        self.assertEqual(code, ghidra.EXIT_LANGUAGE_CHECK)
        self.assertIn("table: lvlx 0x7c642c0e -> ghidra unknown: FAIL\n", out)
        self.assertEqual(self.last_report["failures"], ["table lvlx"])

    def test_missing_cell_row_is_info_under_another_language(self):
        result = _good_result(self.meta, STOCK)
        lvlx = next(r for r in self.meta.rows if r.spec.name == "lvlx")
        _entry(result, "table", "0x%08x" % lvlx.addr)["mnemonic"] = None
        code, out = self.run_verify(result, language=STOCK)
        self.assertEqual(code, 0)
        self.assertIn("table: lvlx 0x7c642c0e -> ghidra unknown: info\n", out)
        self.assertIn("language: %s (installed)\n" % STOCK, out)
        self.assertNotIn("FAIL", out)

    def test_cell_cases_and_decompile_are_info_under_another_language(self):
        result = _good_result(self.meta, STOCK)
        lvlx = next(r for r in self.meta.rows if r.spec.name == "lvlx")
        _entry(result, "table", "0x%08x" % lvlx.addr)["mnemonic"] = None
        _entry(result, "cases", "lvlx_sh_1")["error"] = "no instruction"
        result["decompile"] = {"completed": False, "error": "boom", "c": ""}
        code, out = self.run_verify(result, language=STOCK)
        self.assertEqual(code, 0)
        self.assertIn("semantics: 26 cases, pass 25, fail 0, not decoded 1\n", out)
        self.assertIn("semantics: info lvlx_sh_1: not decoded", out)
        self.assertIn("decompile: info not completed: boom\n", out)

    def test_other_rows_are_info(self):
        result = _good_result(self.meta, STOCK)
        add = next(r for r in self.meta.rows if r.spec.name == "add")
        sub = next(r for r in self.meta.rows if r.spec.name == "subf")
        _entry(result, "table", "0x%08x" % add.addr)["mnemonic"] = None
        _entry(result, "table", "0x%08x" % sub.addr)["mnemonic"] = "neg"
        code, out = self.run_verify(result, language=STOCK)
        self.assertEqual(code, 0)
        self.assertIn("table: unknown add 0x%08x\n" % add.word, out)
        self.assertIn("table: mismatch subf ours 'subf r3,r4,r5' ghidra 'neg r3,r4,r5'", out)
        self.assertIn("mismatch 1, unknown 1, skipped 0", out)

    def test_failing_control_fails_under_any_language(self):
        for language in (CELL, STOCK):
            with self.subTest(language=language):
                result = _good_result(self.meta, language)
                _entry(result, "cases", "lvx_loads_the_aligned_block")["regs"]["vs35"] = "0x1"
                code, out = self.run_verify(result, language=None if language == CELL else STOCK)
                self.assertEqual(code, ghidra.EXIT_LANGUAGE_CHECK)
                self.assertIn("semantics: FAIL lvx_loads_the_aligned_block: v3 expected 0x"
                              "0112233445566778899aabbccddeeff0 got 0x%032x\n" % 1, out)

    def test_register_mismatch_uses_our_register_name(self):
        result = _good_result(self.meta)
        _entry(result, "cases", "lvlx_sh_1")["regs"]["vs35"] = "0x2"
        code, out = self.run_verify(result)
        self.assertEqual(code, ghidra.EXIT_LANGUAGE_CHECK)
        self.assertIn("semantics: FAIL lvlx_sh_1: v3 expected 0x12233445566778899aabbccddeeff000"
                      " got 0x%032x\n" % 2, out)
        self.assertIn("pass 25, fail 1", out)

    def test_memory_mismatch_names_the_first_byte(self):
        result = _good_result(self.meta)
        entry = _entry(result, "cases", "stvlx_sh_5")
        data = bytearray(bytes.fromhex(entry["mem"][0]["hex"]))
        data[7] ^= 0xFF
        entry["mem"][0]["hex"] = data.hex()
        code, out = self.run_verify(result)
        self.assertEqual(code, ghidra.EXIT_LANGUAGE_CHECK)
        self.assertIn("semantics: FAIL stvlx_sh_5: 0x00020007 expected 0x", out)

    def test_decompile_with_bad_data_fails_under_the_cell_language(self):
        for text in ("halt_baddata();", "/* Bad instruction */"):
            with self.subTest(text=text):
                result = _good_result(self.meta)
                result["decompile"]["c"] = "void f(void) { %s }" % text
                code, out = self.run_verify(result)
                self.assertEqual(code, ghidra.EXIT_LANGUAGE_CHECK)
                self.assertIn("decompile: FAIL ", out)
                self.assertEqual(self.last_report["failures"], ["decompile"])

    def test_emulator_error_is_blamed_on_decoding_only_for_unmatched_mnemonics(self):
        result = _good_result(self.meta, STOCK)
        lvlx = next(r for r in self.meta.rows if r.spec.name == "lvlx")
        _entry(result, "table", "0x%08x" % lvlx.addr)["mnemonic"] = None
        _entry(result, "cases", "lvlx_sh_1")["error"] = "bad"
        _entry(result, "cases", "lvx_loads_the_aligned_block")["error"] = "bad"
        meta = self.meta
        report = gcv.evaluate(meta, result)
        status = {c.name: c.status for c in report.cases}
        self.assertEqual(status["lvlx_sh_1"], "not_decoded")
        self.assertEqual(status["lvx_loads_the_aligned_block"], "fail")
        self.assertEqual(report.failures(), ["semantics lvx_loads_the_aligned_block"])

    def test_missing_result_file_is_a_tool_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(CliError) as cm:
                gcv.verify(_fake_install(root), out_dir=root / "out", language=STOCK,
                           run_headless=FakeHeadless(None))
            self.assertEqual(cm.exception.code, ghidra.EXIT_TOOL)
            self.assertIn("without writing", str(cm.exception))


class RunTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.ghidra_dir = _fake_install(self.root)
        self.meta = gcv.build_request()

    def test_staged_command_and_environment(self):
        fake = FakeHeadless(_good_result(self.meta))
        envs = []

        def module_env(stage, ghidra_dir, environ):
            envs.append((stage, ghidra_dir, environ))
            return {"GHIDRA_HEADLESS_JAVA_OPTIONS": "-Dmarker"}

        stage, environ = self.root / "stage", {"A": "b"}
        with contextlib.redirect_stdout(io.StringIO()):
            code = gcv.verify(self.ghidra_dir, out_dir=self.root / "out", stage_dir=stage,
                              run_headless=fake, module_env=module_env, environ=environ,
                              timeout=77)
        self.assertEqual(code, 0)
        self.assertEqual(envs, [(stage, self.ghidra_dir, environ)])
        (cmd, log_dir, timeout, env), = fake.calls
        self.assertEqual(env, {"GHIDRA_HEADLESS_JAVA_OPTIONS": "-Dmarker"})
        self.assertEqual((log_dir, timeout), (self.root / "out" / "logs", 77))
        self.assertEqual(cmd[cmd.index("-processor") + 1], CELL)
        self.assertEqual(cmd[cmd.index("-cspec") + 1], "default")
        self.assertEqual(cmd[cmd.index("-loader-baseAddr") + 1], "0x10000")
        self.assertEqual(cmd[cmd.index("-loader") + 1], "BinaryLoader")
        self.assertEqual(cmd[cmd.index("-postScript") + 1], "CellLangCheck.java")
        self.assertIn("-noanalysis", cmd)
        self.assertEqual(cmd[-5], "-deleteProject")
        self.assertEqual(Path(cmd[cmd.index("-scriptPath") + 1]), ghidra.SCRIPT_DIR)
        self.assertEqual(Path(cmd[cmd.index("-import") + 1]), self.root / "out" / "image.bin")

    def test_language_command_leaves_the_environment_alone(self):
        fake = FakeHeadless(_good_result(self.meta, STOCK))
        module_env = mock.Mock()
        with contextlib.redirect_stdout(io.StringIO()):
            gcv.verify(self.ghidra_dir, out_dir=self.root / "out", language=STOCK,
                       run_headless=fake, module_env=module_env)
        module_env.assert_not_called()
        (cmd, _, _, env), = fake.calls
        self.assertIsNone(env)
        self.assertEqual(cmd[cmd.index("-processor") + 1], STOCK)

    def test_staged_environment_carries_the_external_module(self):
        stage = self.root / "stage"
        (stage / "data" / "languages").mkdir(parents=True)
        (stage / "Module.manifest").write_bytes(b"")
        (stage / "data" / "languages" / ghidra_cell.SLA_NAME).write_bytes(b"sla")
        fake = FakeHeadless(_good_result(self.meta))
        environ = {"XDG_CONFIG_HOME": str(self.root / "config"), "KEEP": "1"}
        with contextlib.redirect_stdout(io.StringIO()):
            gcv.verify(self.ghidra_dir, out_dir=self.root / "out", stage_dir=stage,
                       run_headless=fake, environ=environ)
        env = fake.calls[0][3]
        self.assertEqual(env["GHIDRA_HEADLESS_JAVA_OPTIONS"],
                         "-Dghidra.external.modules=%s" % stage.resolve())
        self.assertEqual(env["KEEP"], "1")
        self.assertNotIn("GHIDRA_HEADLESS_JAVA_OPTIONS", environ)

    def test_files_written(self):
        out = self.root / "out"
        fake = FakeHeadless(_good_result(self.meta))
        with contextlib.redirect_stdout(io.StringIO()):
            gcv.verify(self.ghidra_dir, out_dir=out, stage_dir=self.root / "stage",
                       run_headless=fake, module_env=lambda *a: {})
        self.assertEqual((out / "image.bin").read_bytes(), self.meta.image)
        request = json.loads((out / "request.json").read_text(encoding="utf-8"))
        self.assertEqual(request, self.meta.request)
        self.assertTrue((out / "report.json").is_file())
        self.assertIn("return", (out / "decompile.c").read_text(encoding="utf-8"))

    def test_staged_and_language_together_exit_2(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            code = ghidra_cell.main(["verify", "--staged", "--language", STOCK])
        self.assertEqual(code, 2)

    def test_options_select_the_mode(self):
        cases = {
            (): (ghidra_cell_default_stage(), None),
            ("--staged",): (ghidra_cell_default_stage(), None),
            ("--staged", str(self.root / "s")): ((self.root / "s").resolve(), None),
            ("--language", STOCK): (None, STOCK),
        }
        for argv, (stage, language) in cases.items():
            with self.subTest(argv=argv), mock.patch.object(gcv, "verify", return_value=0) as m:
                with mock.patch.dict("os.environ", {"GHIDRA_INSTALL_DIR": str(self.ghidra_dir)}):
                    self.assertEqual(ghidra_cell.main(["verify", *argv]), 0)
                kwargs = m.call_args.kwargs
                self.assertEqual(kwargs["stage_dir"], stage)
                self.assertEqual(kwargs["language"], language)

    def test_timeout_must_be_positive(self):
        with contextlib.redirect_stderr(io.StringIO()):
            code = ghidra_cell.main(["verify", "--ghidra-dir", str(self.ghidra_dir),
                                     "--timeout", "0"])
        self.assertEqual(code, 2)


def ghidra_cell_default_stage() -> Path:
    return (ghidra_cell.DEFAULT_OUT / "CellPPU").resolve()


@unittest.skipUnless(os.environ.get("DAL_GHIDRA_LIVE") == "1"
                     and os.environ.get(ghidra.GHIDRA_DIR_ENV),
                     "set DAL_GHIDRA_LIVE=1 and GHIDRA_INSTALL_DIR to run against Ghidra")
class VerifyLiveTest(unittest.TestCase):
    def test_staged_and_stock_runs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(ghidra_cell.main(["build", "--out", str(root / "cell")]), 0)
                code = ghidra_cell.main(["verify", "--staged", str(root / "cell" / "CellPPU"),
                                         "--out", str(root / "verify")])
            self.assertEqual(code, 0, stdout.getvalue())
            report = json.loads((root / "verify" / "report.json").read_text(encoding="utf-8"))
            self.assertEqual((report["language"], report["ok"]), (CELL, True))
            self.assertEqual(report["decompile"]["status"], "ok")

            with contextlib.redirect_stdout(stdout):
                code = ghidra_cell.main(["verify", "--language", STOCK,
                                         "--out", str(root / "verify_stock")])
            self.assertEqual(code, 0, stdout.getvalue())
            report = json.loads((root / "verify_stock" / "report.json")
                                .read_text(encoding="utf-8"))
            self.assertEqual((report["language"], report["ok"]), (STOCK, True))


if __name__ == "__main__":
    unittest.main()
