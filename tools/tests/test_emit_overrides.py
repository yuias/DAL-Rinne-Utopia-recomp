"""Tests for the overrides file loader and its checks against the function DB."""

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from ps3recomp.emit_db import DbFunction, DbThunk, FunctionDb
from ps3recomp.emit_overrides import (DEFAULT_OVERRIDES, EXIT_BAD_OVERRIDES, OverrideConfig,
                                      OverrideEntry, check_overrides, load_overrides,
                                      parse_overrides)
from ps3recomp.info import REPO_ROOT, CliError

SHA = "ab" * 32
FILE_SHA = "cd" * 32

FN_A = 0x10000
FN_B = 0x10020


def doc(entries=None, **top) -> dict:
    result = {"schema": 1, "executable_sha256": SHA, "entries": [] if entries is None else entries}
    result.update(top)
    return result


def entry(address="0x00010000", kind="hook", handler="dalHookNop", **extra) -> dict:
    return {"address": address, "kind": kind, "handler": handler, **extra}


def make_db(sha: str = SHA) -> FunctionDb:
    functions = (DbFunction(FN_A, None, ((FN_A, FN_A + 0x10), (FN_A + 0x18, FN_A + 0x1C))),
                 DbFunction(FN_B, None, ((FN_B, FN_B + 0x08),)))
    thunks = (DbThunk(0x30000, "cellFoo", 0x1234, "cellFooOpen", 0x40000),
              DbThunk(0x30010, "cellBar", 0xABCD, None, 0x40008))
    return FunctionDb(sha, FILE_SHA, (FN_A, FN_B + 0x08), (FN_A, FN_B + 0x08), 0x28000,
                      functions, thunks, {})


class ParseOverridesTest(unittest.TestCase):
    def assertRejected(self, document, message):
        with self.assertRaises(ValueError) as ctx:
            parse_overrides(document, FILE_SHA)
        self.assertEqual(str(ctx.exception), message)

    def test_valid_document_is_sorted_by_address(self):
        config = parse_overrides(doc([
            entry("0x0001c000", "override", "skipSpuWait", label="wait", comment="why"),
            entry("0x0001A2B0", "hook", "dalHookLogCall")], comment="top"), FILE_SHA)
        self.assertEqual(config, OverrideConfig(SHA, FILE_SHA, (
            OverrideEntry(0x1A2B0, "hook", "dalHookLogCall", None),
            OverrideEntry(0x1C000, "override", "skipSpuWait", "wait"))))

    def test_optional_keys_absent_give_none(self):
        config = parse_overrides(doc([entry()]), FILE_SHA)
        self.assertIsNone(config.entries[0].label)

    def test_upper_case_sha_is_normalized(self):
        config = parse_overrides(doc(executable_sha256=SHA.upper()), FILE_SHA)
        self.assertEqual(config.sha256, SHA)
        self.assertEqual(config.entries, ())

    def test_short_address_forms(self):
        config = parse_overrides(doc([entry("0x1"), entry("0xFFFFFFFF")]), FILE_SHA)
        self.assertEqual([e.address for e in config.entries], [1, 0xFFFFFFFF])

    def test_shared_handler_is_accepted(self):
        config = parse_overrides(doc([entry("0x00010000", handler="dalHookNop"),
                                      entry("0x00010020", handler="dalHookNop")]), FILE_SHA)
        self.assertEqual([e.handler for e in config.entries], ["dalHookNop", "dalHookNop"])

    def test_top_level_not_an_object(self):
        self.assertRejected([], "top level: expected an object")

    def test_unknown_top_level_key(self):
        self.assertRejected(doc(entires=[]), 'top level: unknown key "entires"')

    def test_missing_top_level_keys(self):
        for key in ("schema", "executable_sha256", "entries"):
            document = doc()
            del document[key]
            self.assertRejected(document, 'top level: missing key "%s"' % key)

    def test_unsupported_schema(self):
        self.assertRejected(doc(schema=2), "top level: unsupported schema 2")
        self.assertRejected(doc(schema=True), "top level: unsupported schema true")
        self.assertRejected(doc(schema="1"), 'top level: unsupported schema "1"')

    def test_executable_sha256_problems(self):
        self.assertRejected(doc(executable_sha256=5),
                            "top level.executable_sha256: expected a string")
        self.assertRejected(doc(executable_sha256="abc"),
                            'top level.executable_sha256: expected 64 hex digits, got "abc"')

    def test_top_level_comment_must_be_a_string(self):
        self.assertRejected(doc(comment=1), "top level.comment: expected a string")

    def test_entries_must_be_an_array(self):
        self.assertRejected(doc(entries={}), "top level.entries: expected an array")

    def test_entry_not_an_object(self):
        self.assertRejected(doc([entry(), "x"]), "entries[1]: expected an object")

    def test_unknown_entry_key(self):
        self.assertRejected(doc([entry(function="main")]), 'entries[0]: unknown key "function"')

    def test_missing_entry_keys(self):
        for key in ("address", "kind", "handler"):
            item = entry()
            del item[key]
            self.assertRejected(doc([item]), 'entries[0]: missing key "%s"' % key)

    def test_bad_addresses(self):
        for bad in ("1a2b0", "0x", "0x123456789", "0xzz", " 0x10", "0x10\n", 4, None):
            self.assertRejected(
                doc([entry(address=bad)]),
                'entries[0].address: expected a hex string like "0x0001a2b0", got %s'
                % json.dumps(bad))

    def test_bad_kind(self):
        for bad in ("Hook", "after", 1):
            self.assertRejected(doc([entry(), entry("0x00010020", kind=bad)]),
                                'entries[1].kind: expected "override" or "hook", got %s'
                                % json.dumps(bad))

    def test_handler_must_be_a_string(self):
        self.assertRejected(doc([entry(handler=7)]), "entries[0].handler: expected a string")

    def test_invalid_handler_names(self):
        for bad in ("", "1abc", "_x", "__x", "a-b", "a b", "a" * 65, "x\n", "hé"):
            self.assertRejected(doc([entry(handler=bad)]),
                                'entries[0].handler: "%s" is not a valid handler name' % bad)

    def test_longest_valid_handler_name(self):
        parse_overrides(doc([entry(handler="a" * 64)]), FILE_SHA)

    def test_reserved_handler_prefixes(self):
        for bad in ("fn_00010000", "imp_00010000", "L_00010000", "case_x", "fn_", "L_a"):
            self.assertRejected(doc([entry(handler=bad)]),
                                'entries[0].handler: "%s" is reserved' % bad)

    def test_prefix_like_names_are_not_reserved(self):
        for good in ("fn", "fnord", "impl", "Lx", "cases", "FN_x"):
            parse_overrides(doc([entry(handler=good)]), FILE_SHA)

    def test_c_keyword_handlers_are_reserved(self):
        for bad in ("int", "while", "nullptr", "static_assert", "typeof_unqual"):
            self.assertRejected(doc([entry(handler=bad)]),
                                'entries[0].handler: "%s" is reserved' % bad)

    def test_label_must_be_a_non_empty_string(self):
        for bad in ("", 3, None):
            self.assertRejected(doc([entry(label=bad)]),
                                "entries[0].label: expected a non-empty string")

    def test_entry_comment_must_be_a_string(self):
        self.assertRejected(doc([entry(comment=[])]), "entries[0].comment: expected a string")

    def test_duplicate_address(self):
        self.assertRejected(doc([entry("0x00010020"), entry("0x00010000"), entry("0x00010020")]),
                            "entries[0] and entries[2] both name 0x00010020")
        # Different spellings of one address are still duplicates.
        self.assertRejected(doc([entry("0x10000"), entry("0x00010000")]),
                            "entries[0] and entries[1] both name 0x00010000")

    def test_structure_errors_win_over_duplicates(self):
        self.assertRejected(doc([entry(), entry(), entry(kind="x")]),
                            'entries[2].kind: expected "override" or "hook", got "x"')

    def test_first_entry_error_wins(self):
        self.assertRejected(doc([entry(address="bad", kind="bad")]),
                            'entries[0].address: expected a hex string like "0x0001a2b0", '
                            'got "bad"')


class LoadOverridesTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)

    def write(self, text, name="overrides.json") -> Path:
        path = self.tmp / name
        path.write_bytes(text.encode("utf-8"))
        return path

    def assertLoadFails(self, path, code, message, required=True):
        with self.assertRaises(CliError) as ctx:
            load_overrides(path, required=required)
        self.assertEqual(ctx.exception.code, code)
        self.assertEqual(str(ctx.exception), message)

    def test_loads_a_valid_file(self):
        text = json.dumps(doc([entry("0x00010020", "override", "skip", label="l"), entry()]))
        path = self.write(text)
        config = load_overrides(path, required=True)
        self.assertEqual(config.sha256, SHA)
        self.assertEqual(config.file_sha256, hashlib.sha256(text.encode()).hexdigest())
        self.assertEqual([e.address for e in config.entries], [0x10000, 0x10020])
        self.assertEqual(config.entries[1].label, "l")

    def test_missing_explicit_file_is_an_io_error(self):
        path = self.tmp / "absent.json"
        with self.assertRaises(CliError) as ctx:
            load_overrides(path, required=True)
        self.assertEqual(ctx.exception.code, 1)
        self.assertTrue(str(ctx.exception).startswith("cannot read overrides file %s: " % path))

    def test_missing_default_file_returns_none(self):
        self.assertIsNone(load_overrides(self.tmp / "absent.json", required=False))

    def test_unreadable_default_file_is_still_an_error(self):
        # A directory exists but cannot be read as a file; it must not pass as "no overrides".
        with self.assertRaises(CliError) as ctx:
            load_overrides(self.tmp, required=False)
        self.assertEqual(ctx.exception.code, 1)

    def test_json_syntax_error_reports_line_and_column(self):
        path = self.write('{\n  "schema": 1,\n  "entries": [,]\n}\n')
        self.assertLoadFails(
            path, EXIT_BAD_OVERRIDES,
            "invalid overrides file %s: invalid JSON: Expecting value (line 3, column 15)" % path)

    def test_empty_file_is_a_syntax_error(self):
        path = self.write("")
        self.assertLoadFails(
            path, EXIT_BAD_OVERRIDES,
            "invalid overrides file %s: invalid JSON: Expecting value (line 1, column 1)" % path)

    def test_non_json_constants_are_rejected(self):
        path = self.write('{"schema": NaN}')
        self.assertLoadFails(
            path, EXIT_BAD_OVERRIDES,
            "invalid overrides file %s: invalid JSON: unexpected constant NaN" % path)

    def test_invalid_utf8_is_a_syntax_error(self):
        path = self.tmp / "bin.json"
        path.write_bytes(b'{"schema": "\xff"}')
        with self.assertRaises(CliError) as ctx:
            load_overrides(path, required=True)
        self.assertEqual(ctx.exception.code, EXIT_BAD_OVERRIDES)
        self.assertIn("invalid JSON: not UTF-8", str(ctx.exception))

    def test_duplicate_key_in_top_level_object(self):
        path = self.write('{"schema": 1, "schema": 1}')
        self.assertLoadFails(
            path, EXIT_BAD_OVERRIDES,
            'invalid overrides file %s: duplicate key "schema" in an object' % path)

    def test_duplicate_key_in_an_entry(self):
        path = self.write('{"schema": 1, "executable_sha256": "%s", "entries": [{"address": '
                          '"0x1", "address": "0x2"}]}' % SHA)
        self.assertLoadFails(
            path, EXIT_BAD_OVERRIDES,
            'invalid overrides file %s: duplicate key "address" in an object' % path)

    def test_structure_error_is_prefixed_with_the_path(self):
        path = self.write(json.dumps(doc([entry(kind="x")])))
        self.assertLoadFails(
            path, EXIT_BAD_OVERRIDES,
            'invalid overrides file %s: entries[0].kind: expected "override" or "hook", '
            'got "x"' % path)

    def test_duplicate_address_through_the_loader(self):
        path = self.write(json.dumps(doc([entry(), entry()])))
        self.assertLoadFails(
            path, EXIT_BAD_OVERRIDES,
            "invalid overrides file %s: entries[0] and entries[1] both name 0x00010000" % path)

    def test_default_path_is_config_overrides_json(self):
        self.assertEqual(DEFAULT_OVERRIDES, REPO_ROOT / "config" / "overrides.json")


class CheckOverridesTest(unittest.TestCase):
    def config(self, *addresses, sha=SHA):
        return OverrideConfig(sha, FILE_SHA, tuple(OverrideEntry(a, "hook", "dalHookNop")
                                                   for a in sorted(addresses)))

    def check(self, config, db=None):
        check_overrides(config, elf_sha256=SHA, db=make_db() if db is None else db)

    def assertCheckFails(self, config, code, message):
        with self.assertRaises(CliError) as ctx:
            self.check(config)
        self.assertEqual(ctx.exception.code, code)
        self.assertEqual(str(ctx.exception), message)

    def test_function_entries_pass(self):
        self.check(self.config(FN_A, FN_B))

    def test_no_entries_pass_even_for_any_function_set(self):
        self.check(self.config())

    def test_pin_mismatch_exits_4(self):
        self.assertCheckFails(
            self.config(FN_A, sha="12" * 32), 4,
            "overrides file was made for a different ELF (SHA-256 %s, expected %s)"
            % ("12" * 32, SHA))

    def test_elf_sha256_comparison_ignores_case(self):
        check_overrides(self.config(FN_A), elf_sha256=SHA.upper(), db=make_db())

    def test_pin_is_checked_before_entries(self):
        self.assertCheckFails(
            self.config(0x30000, sha="12" * 32), 4,
            "overrides file was made for a different ELF (SHA-256 %s, expected %s)"
            % ("12" * 32, SHA))

    def test_import_stub_with_a_name(self):
        self.assertCheckFails(
            self.config(0x30000), EXIT_BAD_OVERRIDES,
            "overrides entry 0x00030000 is the import stub of cellFoo::cellFooOpen; imports are "
            "replaced by HLE handlers (runtime/src/hle), not by overrides")

    def test_import_stub_without_a_name_shows_the_fnid(self):
        self.assertCheckFails(
            self.config(0x30010), EXIT_BAD_OVERRIDES,
            "overrides entry 0x00030010 is the import stub of cellBar::0x0000abcd; imports are "
            "replaced by HLE handlers (runtime/src/hle), not by overrides")

    def test_address_inside_a_function(self):
        self.assertCheckFails(
            self.config(FN_A + 4), EXIT_BAD_OVERRIDES,
            "overrides entry 0x00010004 is not a function entry (inside fn_00010000)")

    def test_address_inside_a_later_range_of_a_function(self):
        self.assertCheckFails(
            self.config(FN_A + 0x18), EXIT_BAD_OVERRIDES,
            "overrides entry 0x00010018 is not a function entry (inside fn_00010000)")

    def test_address_in_a_gap_between_ranges_is_outside(self):
        self.assertCheckFails(
            self.config(FN_A + 0x10), EXIT_BAD_OVERRIDES,
            "overrides entry 0x00010010 is not a function entry (outside every function)")

    def test_address_outside_every_function(self):
        for addr in (0x100, FN_B + 0x08, 0x90000):
            self.assertCheckFails(
                self.config(addr), EXIT_BAD_OVERRIDES,
                "overrides entry 0x%08x is not a function entry (outside every function)" % addr)

    def test_first_bad_entry_in_address_order_is_reported(self):
        self.assertCheckFails(
            self.config(0x30000, FN_A + 4), EXIT_BAD_OVERRIDES,
            "overrides entry 0x00010004 is not a function entry (inside fn_00010000)")


if __name__ == "__main__":
    unittest.main()
