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


class MemTest(unittest.TestCase):
    def test_every_member_of_the_group_is_handled(self):
        from ps3recomp.emit_db import GROUPS
        from ps3recomp.emit_core import load_handlers
        self.assertEqual(sorted(set(GROUPS["mem"]) - set(load_handlers())), [])

    def test_load_uses_the_truncated_ea(self):
        self.assertEqual(body(asm.encode("lwz", rD=3, rA=4, D=8))[0],
                         ["ctx->gpr[3] = dalRead32(base, "
                          "(uint32_t)(ctx->gpr[4] + 0x0000000000000008ull));"])

    def test_ra0_is_a_literal_address(self):
        self.assertEqual(body(asm.encode("lwz", rD=3, rA=0, D=0x100))[0],
                         ["ctx->gpr[3] = dalRead32(base, 0x00000100u);"])
        self.assertEqual(body(asm.encode("stb", rS=3, rA=0, D=-8))[0],
                         ["dalWrite8(base, 0xfffffff8u, (uint8_t)ctx->gpr[3]);"])
        self.assertEqual(body(asm.encode("lwzx", rD=3, rA=0, rB=5))[0],
                         ["ctx->gpr[3] = dalRead32(base, (uint32_t)ctx->gpr[5]);"])

    def test_update_form_writes_the_untruncated_sum_after_the_access(self):
        self.assertEqual(body(asm.encode("lwzu", rD=3, rA=4, D=8))[0],
                         ["uint64_t t0 = (ctx->gpr[4] + 0x0000000000000008ull);",
                          "ctx->gpr[3] = dalRead32(base, (uint32_t)t0);",
                          "ctx->gpr[4] = t0;"])
        self.assertEqual(body(asm.encode("stdux", rS=3, rA=4, rB=5))[0],
                         ["uint64_t t0 = (ctx->gpr[4] + ctx->gpr[5]);",
                          "dalWrite64(base, (uint32_t)t0, (uint64_t)ctx->gpr[3]);",
                          "ctx->gpr[4] = t0;"])

    def test_update_form_with_ra0_is_unhandled(self):
        for name, fields in (("lwzu", {"rD": 3, "rA": 0, "D": 4}),
                             ("stdu", {"rS": 3, "rA": 0, "DS": 8}),
                             ("lbzux", {"rD": 3, "rA": 0, "rB": 5}),
                             ("lfdu", {"frD": 1, "rA": 0, "D": 8}),
                             ("stfsux", {"frS": 1, "rA": 0, "rB": 5})):
            lines, stats = body(asm.encode(name, **fields))
            self.assertEqual(lines[0].startswith("DAL_UNHANDLED"), True, name)
            self.assertEqual(dict(stats.unhandled), {"%s:ra0" % name: 1})

    def test_algebraic_loads_sign_extend(self):
        self.assertEqual(body(asm.encode("lha", rD=3, rA=4, D=0))[0],
                         ["ctx->gpr[3] = (uint64_t)(int64_t)(int16_t)dalRead16(base, "
                          "(uint32_t)(ctx->gpr[4] + 0x0000000000000000ull));"])
        self.assertIn("(int32_t)dalRead32", body(asm.encode("lwa", rD=3, rA=4, DS=4))[0][0])

    def test_float_accesses_use_the_conversion_helpers(self):
        self.assertIn("ctx->fpr[1].f64 = dalReadF32(", body(asm.encode("lfs", frD=1, rA=4))[0][0])
        self.assertIn("dalWriteF32(base, ", body(asm.encode("stfs", frS=1, rA=4))[0][0])
        self.assertIn("ctx->fpr[1].u64 = dalRead64(", body(asm.encode("lfd", frD=1, rA=4))[0][0])
        self.assertIn("dalWrite64(base, ", body(asm.encode("stfd", frS=1, rA=4))[0][0])

    def test_stfiwx_stores_the_low_word_of_the_bits(self):
        self.assertEqual(body(asm.encode("stfiwx", frS=1, rA=4, rB=5))[0],
                         ["dalWrite32(base, (uint32_t)(ctx->gpr[4] + ctx->gpr[5]), "
                          "(uint32_t)ctx->fpr[1].u64);"])

    def test_byte_reversed_forms_swap_the_memory_value(self):
        self.assertIn("__builtin_bswap32(dalRead32(", body(asm.encode("lwbrx", rD=3, rA=4, rB=5))[0][0])
        self.assertIn("__builtin_bswap16(dalRead16(", body(asm.encode("lhbrx", rD=3, rA=4, rB=5))[0][0])
        self.assertIn("__builtin_bswap64((uint64_t)ctx->gpr[3])",
                      body(asm.encode("stdbrx", rS=3, rA=4, rB=5))[0][0])

    def test_lmw_unrolls_to_r31(self):
        lines, _ = body(asm.encode("lmw", rD=29, rA=4, D=-4))
        self.assertEqual(lines, [
            "uint64_t t0 = (ctx->gpr[4] + 0xfffffffffffffffcull);",
            "ctx->gpr[29] = dalRead32(base, (uint32_t)t0);",
            "ctx->gpr[30] = dalRead32(base, (uint32_t)(t0 + 4));",
            "ctx->gpr[31] = dalRead32(base, (uint32_t)(t0 + 8));"])

    def test_string_instructions_take_the_count_from_nb_or_xer(self):
        self.assertEqual(body(asm.encode("lswi", rD=5, rA=4, NB=0))[0],
                         ["dalLoadString(ctx, base, (uint32_t)ctx->gpr[4], 5, 32u);"])
        self.assertEqual(body(asm.encode("stswx", rS=5, rA=4, rB=6))[0],
                         ["dalStoreString(ctx, base, (uint32_t)(ctx->gpr[4] + ctx->gpr[6]), 5, "
                          "ctx->xer_count & 0x7f);"])


if __name__ == "__main__":
    unittest.main()
