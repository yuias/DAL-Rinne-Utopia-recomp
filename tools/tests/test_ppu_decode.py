"""Tests for the PPU decoder engine and its opcode tables."""

import struct
import unittest

from ps3recomp import ppu
from ps3recomp.ppu import ALL_OPS, bits, decode, decode_range, op

# (word, addr, mnemonic, expected field values)
INT_CASES = [
    (0x7C0802A6, 0x10000, "mfspr", {"rD": 0, "SPR": 8}),
    (0x7C0803A6, 0x10000, "mtspr", {"SPR": 8, "rS": 0}),
    (0x7D8903A6, 0x10000, "mtspr", {"SPR": 9, "rS": 12}),
    (0x4E800020, 0x10000, "bclr", {"BO": 20, "BI": 0}),
    (0x4E800420, 0x10000, "bcctr", {"BO": 20, "BI": 0}),
    (0x4E800421, 0x10000, "bcctrl", {"BO": 20, "BI": 0}),
    (0x60000000, 0x10000, "ori", {"rA": 0, "rS": 0, "UIMM": 0}),
    (0x38600000, 0x10000, "addi", {"rD": 3, "rA": 0, "SIMM": 0}),
    (0x3C600001, 0x10000, "addis", {"rD": 3, "rA": 0, "SIMM": 1}),
    (0x38210070, 0x10000, "addi", {"rD": 1, "rA": 1, "SIMM": 0x70}),
    (0xF821FF91, 0x10000, "stdu", {"rS": 1, "rA": 1, "DS": -112}),
    (0xFBE1FFF8, 0x10000, "std", {"rS": 31, "rA": 1, "DS": -8}),
    (0xE8010080, 0x10000, "ld", {"rD": 0, "rA": 1, "DS": 0x80}),
    (0x90010008, 0x10000, "stw", {"rS": 0, "rA": 1, "D": 8}),
    (0x80630000, 0x10000, "lwz", {"rD": 3, "rA": 3, "D": 0}),
    (0x44000002, 0x10000, "sc", {}),
    (0x7C7F1B78, 0x10000, "or", {"rA": 31, "rS": 3, "rB": 3, "Rc": 0}),
    (0x7C642A15, 0x10000, "add.", {"rD": 3, "rA": 4, "rB": 5, "OE": 0, "Rc": 1}),
    (0x7C642E14, 0x10000, "addo", {"rD": 3, "rA": 4, "rB": 5, "OE": 1, "Rc": 0}),
    (0x7C000026, 0x10000, "mfcr", {"rD": 0}),
    (0x7C0004AC, 0x10000, "sync", {"L_sync": 0}),
    (0x7C2004AC, 0x10000, "sync", {"L_sync": 1}),
    (0x4C00012C, 0x10000, "isync", {}),
    (0x7C6307B4, 0x10000, "extsw", {"rA": 3, "rS": 3}),
    (0x78630020, 0x10000, "rldicl", {"rA": 3, "rS": 3, "SH64": 0, "MB64": 32}),
    (0x788345E4, 0x10000, "rldicr", {"rA": 3, "rS": 4, "SH64": 8, "MB64": 55}),
    (0x5483103A, 0x10000, "rlwinm", {"rA": 3, "rS": 4, "SH": 2, "MB": 0, "ME": 29}),
    (0x7C831E74, 0x10000, "sradi", {"rA": 3, "rS": 4, "SH64": 3}),
    (0x7C60212D, 0x10000, "stwcx.", {"rS": 3, "rA": 0, "rB": 4}),
    (0x2C030000, 0x10000, "cmpi", {"crfD": 0, "L": 0, "rA": 3, "SIMM": 0}),
    (0x2FA30000, 0x10000, "cmpi", {"crfD": 7, "L": 1, "rA": 3, "SIMM": 0}),
    (0x4182000C, 0x10000, "bc", {"BO": 12, "BI": 2, "BD": 12, "AA": 0, "LK": 0}),
    (0x4200FFF8, 0x10008, "bc", {"BO": 16, "BI": 0, "BD": -8}),
    (0x4BFFFFFC, 0x10000, "b", {"LI": -4}),
    (0x48000011, 0x10000, "bl", {"LI": 0x10, "LK": 1}),
]


class BitsTest(unittest.TestCase):
    def test_ibm_bit_numbering(self):
        self.assertEqual(bits(0x7C0802A6, 0, 5), 31)
        self.assertEqual(bits(0x7C0802A6, 21, 30), 339)
        self.assertEqual(bits(0x80000000, 0, 0), 1)
        self.assertEqual(bits(0x00000001, 31, 31), 1)

    def test_swapped_spr_halves(self):
        # Bits 11-15 hold the low half, bits 16-20 the high half.
        word = 0x7C0002A6 | (3 << 16) | (8 << 11)
        self.assertEqual(ppu.FIELDS["SPR"](word), 8 << 5 | 3)


class StructureTest(unittest.TestCase):
    def test_table_not_empty(self):
        self.assertGreater(len(ALL_OPS), 150)

    def test_match_within_mask(self):
        for spec in ALL_OPS:
            with self.subTest(spec=spec.name):
                self.assertEqual(spec.match & ~spec.mask & 0xFFFFFFFF, 0)

    def test_specs_decode_from_own_match(self):
        for spec in ALL_OPS:
            with self.subTest(spec=spec.name):
                self.assertIs(decode(spec.match).spec, spec)

    def test_no_duplicate_mask_match(self):
        seen = {}
        for spec in ALL_OPS:
            key = (spec.mask, spec.match)
            self.assertNotIn(key, seen, f"{spec.name} duplicates {seen.get(key)}")
            seen[key] = spec.name

    def test_overlaps_are_nested(self):
        by_primary = {}
        for spec in ALL_OPS:
            by_primary.setdefault(spec.primary, []).append(spec)
        for specs in by_primary.values():
            for i, a in enumerate(specs):
                for b in specs[i + 1:]:
                    if (a.match ^ b.match) & a.mask & b.mask:
                        continue
                    superset = (a.mask & b.mask) in (a.mask, b.mask) and a.mask != b.mask
                    self.assertTrue(superset, f"{a.name} and {b.name} overlap ambiguously")

    def test_duplicate_detected(self):
        dup = op("dup", "D", 14)
        with self.assertRaises(ValueError):
            ppu._build_lookup((dup, dup))

    def test_op_validation(self):
        with self.assertRaises(ValueError):
            op("bad", "D", 14, xo=1)
        with self.assertRaises(ValueError):
            op("bad", "X", 31)
        with self.assertRaises(ValueError):
            op("bad", "DS", 58, xo=4)
        with self.assertRaises(ValueError):
            op("bad", "Q", 1)


class DecodeTest(unittest.TestCase):
    def test_known_integer_encodings(self):
        for word, addr, mnemonic, fields in INT_CASES:
            with self.subTest(word="%08x" % word):
                insn = decode(word, addr)
                self.assertIsNotNone(insn.spec)
                self.assertEqual(insn.addr, addr)
                self.assertEqual(insn.mnemonic, mnemonic)
                for name, value in fields.items():
                    self.assertEqual(insn.field(name), value, name)

    def test_unknown_words(self):
        for word in (0x00000000, 0x04000000):
            with self.subTest(word="%08x" % word):
                insn = decode(word)
                self.assertIsNone(insn.spec)
                self.assertIsNone(insn.mnemonic)

    def test_absolute_branch_suffix(self):
        self.assertEqual(decode(0x48000003).mnemonic, "bla")
        self.assertEqual(decode(0x48000002).mnemonic, "ba")

    def test_extended_opcode_collisions_resolved(self):
        # XO (bits 22-30) and X (bits 21-30) forms share primary 31.
        self.assertEqual(decode(0x7C642A14).mnemonic, "add")
        self.assertEqual(decode(0x7C642B78).mnemonic, "or")
        self.assertEqual(decode(0x7C6307B4).mnemonic, "extsw")

    def test_decode_range(self):
        data = struct.pack(">3I", 0x60000000, 0x4E800020, 0x00000000) + b"\xAA\xBB"
        insns = list(decode_range(data, 0x10000))
        self.assertEqual([i.addr for i in insns], [0x10000, 0x10004, 0x10008])
        self.assertEqual([i.mnemonic for i in insns], ["ori", "bclr", None])


if __name__ == "__main__":
    unittest.main()
