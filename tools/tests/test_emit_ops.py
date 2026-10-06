"""Text-level tests of the group handlers: what the harness cannot express (no statement, helpers)."""

import unittest

from ps3recomp.emit_core import EmitEnv, EmitStats, emit_function
from ps3recomp.ppu_flow import CodeImage

from tests import ppc_asm as asm

BASE = 0x10000


def body(word: int) -> tuple[list[str], EmitStats]:
    """Statements emitted for one word, without the function frame and the trailing return."""
    stats = EmitStats()
    image = CodeImage.from_words(BASE, [word, asm.blr()])
    text = emit_function(image, BASE, [BASE, BASE + 4], EmitEnv({}, {}, {}), stats, name="f")
    lines = [line.strip() for line in text.splitlines()]
    # A multi-line handler result is wrapped in a block; the frame is 2 lines, then `return;`, `}`.
    return lines[3:-3] if lines[2] == "{" else lines[2:-2], stats


class IntAluTest(unittest.TestCase):
    def test_nop_emits_no_statement(self):
        lines, stats = body(0x60000000)
        self.assertEqual(lines, [])
        self.assertEqual(sum(stats.unhandled.values()), 0)

    def test_every_member_of_the_group_is_handled(self):
        from ps3recomp.emit_db import GROUPS
        from ps3recomp.emit_core import load_handlers
        self.assertEqual(sorted(set(GROUPS["int_alu"]) - set(load_handlers())), [])

    def test_li_and_lis_use_a_literal(self):
        self.assertEqual(body(asm.li(3, -2))[0], ["ctx->gpr[3] = 0xfffffffffffffffeull;"])
        self.assertEqual(body(asm.lis(3, -1))[0], ["ctx->gpr[3] = 0xffffffffffff0000ull;"])

    def test_addi_adds_the_register(self):
        self.assertEqual(body(asm.addi(3, 4, 8))[0],
                         ["ctx->gpr[3] = ctx->gpr[4] + 0x0000000000000008ull;"])

    def test_record_form_calls_the_cr0_helper(self):
        lines, _ = body(asm.encode("and", rA=3, rS=4, rB=5, Rc=1))
        self.assertIn("dalRecord(ctx, t0);", lines)

    def test_andi_always_records(self):
        lines, _ = body(asm.encode("andi.", rA=3, rS=4, UIMM=1))
        self.assertIn("dalRecord(ctx, t0);", lines)

    def test_ori_does_not_record(self):
        lines, _ = body(asm.encode("ori", rA=3, rS=4, UIMM=1))
        self.assertEqual(lines, ["ctx->gpr[3] = ctx->gpr[4] | 0x0000000000000001ull;"])

    def test_oe_forms_set_overflow_and_plain_forms_do_not(self):
        for name, fields in (("add", {"rD": 3, "rA": 4, "rB": 5}),
                             ("subf", {"rD": 3, "rA": 4, "rB": 5}),
                             ("neg", {"rD": 3, "rA": 4})):
            plain, _ = body(asm.encode(name, **fields))
            with_oe, _ = body(asm.encode(name, OE=1, **fields))
            self.assertFalse(any("dalSetOv" in line for line in plain), name)
            self.assertTrue(any("dalSetOv" in line for line in with_oe), name)

    def test_compare_width_follows_l(self):
        word = asm.encode("cmp", crfD=2, L=0, rA=4, rB=5)
        self.assertEqual(body(word)[0],
                         ["dalCmpS(ctx, 2, (int32_t)ctx->gpr[4], (int32_t)ctx->gpr[5]);"])
        word = asm.encode("cmpl", crfD=2, L=1, rA=4, rB=5)
        self.assertEqual(body(word)[0],
                         ["dalCmpU(ctx, 2, (uint64_t)ctx->gpr[4], (uint64_t)ctx->gpr[5]);"])
        word = asm.encode("cmpi", crfD=0, L=0, rA=4, SIMM=-1)
        self.assertEqual(body(word)[0], ["dalCmpS(ctx, 0, (int32_t)ctx->gpr[4], -1);"])
        word = asm.encode("cmpli", crfD=0, L=1, rA=4, UIMM=0xFFFF)
        self.assertEqual(body(word)[0],
                         ["dalCmpU(ctx, 0, (uint64_t)ctx->gpr[4], 0x000000000000ffffull);"])

    def test_count_leading_zeros_uses_the_guarded_helpers(self):
        self.assertEqual(body(asm.encode("cntlzw", rA=3, rS=4))[0],
                         ["ctx->gpr[3] = dalCntlzw(ctx->gpr[4]);"])
        self.assertEqual(body(asm.encode("cntlzd", rA=3, rS=4))[0],
                         ["ctx->gpr[3] = dalCntlzd(ctx->gpr[4]);"])


if __name__ == "__main__":
    unittest.main()
