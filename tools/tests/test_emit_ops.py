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


class IntRotateTest(unittest.TestCase):
    def test_every_member_of_the_group_is_handled(self):
        from ps3recomp.emit_db import GROUPS
        from ps3recomp.emit_core import load_handlers
        self.assertEqual(sorted(set(GROUPS["int_rotate"]) - set(load_handlers())), [])

    def test_rlwinm_masks_in_the_high_half(self):
        # slwi 8: mask(32, 55)
        self.assertEqual(body(asm.encode("rlwinm", rA=3, rS=4, SH=8, MB=0, ME=23))[0],
                         ["ctx->gpr[3] = dalRotl32Dup(ctx->gpr[4], 8) & 0x00000000ffffff00ull;"])
        # MB > ME: the wrapping mask keeps the duplicated high word.
        self.assertEqual(body(asm.encode("rlwinm", rA=3, rS=4, SH=0, MB=28, ME=3))[0],
                         ["ctx->gpr[3] = dalRotl32Dup(ctx->gpr[4], 0) & 0xfffffffff000000full;"])

    def test_rld_forms_drop_a_full_mask_and_a_zero_rotate(self):
        self.assertEqual(body(asm.encode("rldicl", rA=3, rS=4, SH64=0, MB64=0))[0],
                         ["ctx->gpr[3] = ctx->gpr[4];"])
        self.assertEqual(body(asm.encode("rldicl", rA=3, rS=4, SH64=0, MB64=32))[0],
                         ["ctx->gpr[3] = ctx->gpr[4] & 0x00000000ffffffffull;"])
        self.assertEqual(body(asm.encode("rldicr", rA=3, rS=4, SH64=8, MB64=55))[0],
                         ["ctx->gpr[3] = dalRotl64(ctx->gpr[4], 8) & 0xffffffffffffff00ull;"])

    def test_rldimi_keeps_the_unmasked_bits_of_ra(self):
        self.assertEqual(body(asm.encode("rldimi", rA=3, rS=4, SH64=16, MB64=32))[0],
                         ["ctx->gpr[3] = (ctx->gpr[3] & ~0x00000000ffff0000ull) | "
                          "(dalRotl64(ctx->gpr[4], 16) & 0x00000000ffff0000ull);"])

    def test_record_form_records_the_64_bit_result(self):
        lines, _ = body(asm.encode("rlwinm", rA=3, rS=4, SH=0, MB=28, ME=3, Rc=1))
        self.assertEqual(lines[-3:], ["uint64_t t0 = dalRotl32Dup(ctx->gpr[4], 0) & "
                                      "0xfffffffff000000full;",
                                      "ctx->gpr[3] = t0;", "dalRecord(ctx, t0);"])

    def test_algebraic_shifts_go_through_the_carry_helpers(self):
        self.assertEqual(body(asm.encode("srawi", rA=3, rS=4, SH=5))[0],
                         ["ctx->gpr[3] = dalSraw(ctx, ctx->gpr[4], 5);"])
        self.assertEqual(body(asm.encode("sradi", rA=3, rS=4, SH64=63))[0],
                         ["ctx->gpr[3] = dalSrad(ctx, ctx->gpr[4], 63);"])
        self.assertEqual(body(asm.encode("sraw", rA=3, rS=4, rB=5))[0],
                         ["ctx->gpr[3] = dalSraw(ctx, ctx->gpr[4], ctx->gpr[5]);"])
        self.assertEqual(body(asm.encode("srad", rA=3, rS=4, rB=5))[0],
                         ["ctx->gpr[3] = dalSrad(ctx, ctx->gpr[4], ctx->gpr[5]);"])

    def test_plain_shifts_do_not_touch_the_carry(self):
        for name, helper in (("slw", "dalSlw"), ("srw", "dalSrw"), ("sld", "dalSld"),
                             ("srd", "dalSrd")):
            self.assertEqual(body(asm.encode(name, rA=3, rS=4, rB=5))[0],
                             ["ctx->gpr[3] = %s(ctx->gpr[4], ctx->gpr[5]);" % helper])


class IntCarryMuldivTest(unittest.TestCase):
    def test_every_member_of_the_group_is_handled(self):
        from ps3recomp.emit_db import GROUPS
        from ps3recomp.emit_core import load_handlers
        self.assertEqual(sorted(set(GROUPS["int_carry_muldiv"]) - set(load_handlers())), [])

    def test_addic_takes_the_carry_from_the_64_bit_add(self):
        self.assertEqual(body(asm.encode("addic", rD=3, rA=4, SIMM=-1))[0],
                         ["ctx->gpr[3] = dalAddCarry(ctx, ctx->gpr[4], 0xffffffffffffffffull, 0);"])

    def test_addic_dot_records_and_addic_does_not(self):
        lines, _ = body(asm.encode("addic.", rD=3, rA=4, SIMM=1))
        self.assertEqual(lines[-1], "dalRecord(ctx, t0);")
        lines, _ = body(asm.encode("addic", rD=3, rA=4, SIMM=1))
        self.assertFalse(any("dalRecord" in line for line in lines))

    def test_subtract_forms_invert_the_minuend(self):
        self.assertEqual(body(asm.encode("subfc", rD=3, rA=4, rB=5))[0],
                         ["ctx->gpr[3] = dalAddCarry(ctx, ~ctx->gpr[4], ctx->gpr[5], 1);"])
        self.assertEqual(body(asm.encode("subfe", rD=3, rA=4, rB=5))[0],
                         ["ctx->gpr[3] = dalAddCarry(ctx, ~ctx->gpr[4], ctx->gpr[5], "
                          "ctx->xer_ca);"])
        self.assertEqual(body(asm.encode("subfic", rD=3, rA=4, SIMM=7))[0],
                         ["ctx->gpr[3] = dalAddCarry(ctx, ~ctx->gpr[4], 0x0000000000000007ull, "
                          "1);"])

    def test_extended_forms_read_ca_and_use_constant_operands(self):
        self.assertEqual(body(asm.encode("addze", rD=3, rA=4))[0],
                         ["ctx->gpr[3] = dalAddCarry(ctx, ctx->gpr[4], 0x0000000000000000ull, "
                          "ctx->xer_ca);"])
        self.assertEqual(body(asm.encode("addme", rD=3, rA=4))[0],
                         ["ctx->gpr[3] = dalAddCarry(ctx, ctx->gpr[4], 0xffffffffffffffffull, "
                          "ctx->xer_ca);"])

    def test_oe_forms_copy_the_operands_before_writing_rd(self):
        lines, _ = body(asm.encode("addc", rD=3, rA=4, rB=5, OE=1))
        self.assertEqual(lines, ["uint64_t t0 = ctx->gpr[4];", "uint64_t t1 = ctx->gpr[5];",
                                 "uint64_t t2 = dalAddCarry(ctx, t0, t1, 0);",
                                 "ctx->gpr[3] = t2;", "dalSetOv(ctx, dalAddOverflows(t0, t1, t2));"])

    def test_oe_forms_set_overflow_and_plain_forms_do_not(self):
        for name in ("addc", "adde", "addze", "addme", "subfc", "subfe", "subfze", "subfme",
                     "mullw", "mulld", "divw", "divwu", "divd", "divdu"):
            fields = {"rD": 3, "rA": 4}
            if name not in ("addze", "addme", "subfze", "subfme"):
                fields["rB"] = 5
            plain, _ = body(asm.encode(name, **fields))
            with_oe, _ = body(asm.encode(name, OE=1, **fields))
            self.assertFalse(any("dalSetOv" in line for line in plain), name)
            self.assertTrue(any("dalSetOv" in line for line in with_oe), name)

    def test_high_multiplies_have_no_oe_form(self):
        for name in ("mulhw", "mulhwu", "mulhd", "mulhdu"):
            with self.assertRaises(ValueError):
                asm.encode(name, rD=3, rA=4, rB=5, OE=1)

    def test_divisions_go_through_the_guarded_helpers(self):
        for name, helper in (("divw", "dalDivw"), ("divwu", "dalDivwu"), ("divd", "dalDivd"),
                             ("divdu", "dalDivdu")):
            self.assertEqual(body(asm.encode(name, rD=3, rA=4, rB=5))[0],
                             ["ctx->gpr[3] = %s(ctx->gpr[4], ctx->gpr[5]);" % helper])

    def test_record_form_records_the_full_result(self):
        lines, _ = body(asm.encode("mulhwu", rD=3, rA=4, rB=5, Rc=1))
        self.assertEqual(lines, ["uint64_t t0 = dalMulhwu(ctx->gpr[4], ctx->gpr[5]);",
                                 "ctx->gpr[3] = t0;", "dalRecord(ctx, t0);"])

    def test_mulli_multiplies_unsigned(self):
        self.assertEqual(body(asm.encode("mulli", rD=3, rA=4, SIMM=-3))[0],
                         ["ctx->gpr[3] = ctx->gpr[4] * 0xfffffffffffffffdull;"])


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


class SysTest(unittest.TestCase):
    def test_every_member_of_the_group_is_handled(self):
        from ps3recomp.emit_db import GROUPS
        from ps3recomp.emit_core import load_handlers
        self.assertEqual(sorted(set(GROUPS["sys"]) - set(load_handlers())), [])

    def test_spr_moves_use_the_context_fields(self):
        self.assertEqual(body(asm.mflr(3))[0], ["ctx->gpr[3] = ctx->lr;"])
        self.assertEqual(body(asm.mtctr(3))[0], ["ctx->ctr = ctx->gpr[3];"])
        self.assertEqual(body(asm.encode("mfspr", rD=3, SPR=1))[0],
                         ["ctx->gpr[3] = dalMfxer(ctx);"])
        self.assertEqual(body(asm.encode("mtspr", SPR=1, rS=3))[0], ["dalMtxer(ctx, ctx->gpr[3]);"])
        self.assertEqual(body(asm.encode("mtspr", SPR=256, rS=3))[0],
                         ["ctx->vrsave = (uint32_t)ctx->gpr[3];"])

    def test_unknown_spr_is_unhandled_with_its_number(self):
        for name, fields, key in (("mfspr", {"rD": 3, "SPR": 1013}, "mfspr:spr=1013"),
                                  ("mtspr", {"SPR": 18, "rS": 3}, "mtspr:spr=18")):
            lines, stats = body(asm.encode(name, **fields))
            self.assertTrue(lines[0].startswith("DAL_UNHANDLED"), name)
            self.assertEqual(dict(stats.unhandled), {key: 1})

    def test_mftb_calls_the_time_base_hook(self):
        self.assertEqual(body(asm.encode("mftb", rD=3, TBR=268))[0],
                         ["ctx->gpr[3] = dalMftb(ctx);"])
        self.assertEqual(body(asm.encode("mftb", rD=3, TBR=269))[0],
                         ["ctx->gpr[3] = dalMftb(ctx) >> 32;"])
        lines, stats = body(asm.encode("mftb", rD=3, TBR=300))
        self.assertTrue(lines[0].startswith("DAL_UNHANDLED"))
        self.assertEqual(dict(stats.unhandled), {"mftb:tbr=300": 1})

    def test_cache_hints_emit_no_statement(self):
        for name in ("dcbt", "dcbtst", "dcbf", "dcbst", "icbi"):
            lines, stats = body(asm.encode(name, rA=4, rB=5))
            self.assertEqual(lines, [], name)
            self.assertEqual(sum(stats.unhandled.values()), 0, name)

    def test_dcbz_calls_the_runtime_line_clear(self):
        self.assertEqual(body(asm.encode("dcbz", rA=4, rB=5))[0],
                         ["dalDcbz(ctx, base, (uint32_t)(ctx->gpr[4] + ctx->gpr[5]));"])
        self.assertEqual(body(asm.encode("dcbz", rA=0, rB=5))[0],
                         ["dalDcbz(ctx, base, (uint32_t)ctx->gpr[5]);"])

    def test_barriers_pass_their_kind(self):
        self.assertEqual(body(asm.encode("sync", L_sync=0))[0], ["dalSync(ctx, kDalSyncHeavy);"])
        self.assertEqual(body(asm.encode("sync", L_sync=1))[0], ["dalSync(ctx, kDalSyncLight);"])
        self.assertEqual(body(asm.encode("eieio"))[0], ["dalSync(ctx, kDalSyncEieio);"])
        self.assertEqual(body(asm.encode("isync"))[0], ["dalSync(ctx, kDalSyncIsync);"])

    def test_reservations_go_through_the_runtime(self):
        self.assertEqual(body(asm.encode("lwarx", rD=5, rA=0, rB=4))[0],
                         ["ctx->gpr[5] = dalLwarx(ctx, base, (uint32_t)ctx->gpr[4]);"])
        self.assertEqual(body(asm.encode("ldarx", rD=5, rA=3, rB=4))[0],
                         ["ctx->gpr[5] = dalLdarx(ctx, base, (uint32_t)(ctx->gpr[3] + ctx->gpr[4]));"])
        self.assertEqual(body(asm.encode("stwcx.", rS=6, rA=0, rB=4))[0],
                         ["bool t0 = dalStwcx(ctx, base, (uint32_t)ctx->gpr[4], "
                          "(uint32_t)ctx->gpr[6]);",
                          "dalSetCr0Conditional(ctx, t0);"])
        self.assertIn("dalStdcx(ctx, base, ", body(asm.encode("stdcx.", rS=6, rA=0, rB=4))[0][0])

    def test_condition_register_moves(self):
        self.assertEqual(body(asm.encode("mfcr", rD=3))[0], ["ctx->gpr[3] = dalMfcr(ctx);"])
        self.assertEqual(body(asm.encode("mtcrf", CRM=0xFF, rS=3))[0],
                         ["dalMtcrf(ctx, 0xffu, (uint32_t)ctx->gpr[3]);"])
        self.assertEqual(body(asm.encode("mtcrf", CRM=0, rS=3))[0], [])
        self.assertEqual(body(asm.encode("mfocrf", rD=3, FXM=0x20))[0],
                         ["ctx->gpr[3] = (uint64_t)dalGetCrField(ctx, 2) << 20;"])
        self.assertEqual(body(asm.encode("mtocrf", CRM=0x01, rS=3))[0],
                         ["dalSetCrField(ctx, 7, (uint32_t)(ctx->gpr[3] >> 0) & 15u);"])
        self.assertEqual(body(asm.encode("mcrf", crfD=2, crfS=5))[0],
                         ["dalSetCrField(ctx, 2, dalGetCrField(ctx, 5));"])

    def test_cr_logical_ops_are_byte_operations(self):
        self.assertEqual(body(asm.encode("cror", crbD=2, crbA=1, crbB=2))[0],
                         ["ctx->cr[2] = (uint8_t)(ctx->cr[1] | ctx->cr[2]);"])
        self.assertEqual(body(asm.encode("crnor", crbD=3, crbA=4, crbB=5))[0],
                         ["ctx->cr[3] = (uint8_t)((ctx->cr[4] | ctx->cr[5]) ^ 1);"])
        self.assertEqual(body(asm.encode("crxor", crbD=6, crbA=6, crbB=6))[0],
                         ["ctx->cr[6] = 0;"])
        self.assertEqual(body(asm.encode("creqv", crbD=6, crbA=6, crbB=6))[0],
                         ["ctx->cr[6] = 1;"])


class FpTest(unittest.TestCase):
    def test_every_member_of_the_group_is_handled(self):
        from ps3recomp.emit_db import GROUPS
        from ps3recomp.emit_core import load_handlers
        self.assertEqual(sorted(set(GROUPS["fp"]) - set(load_handlers())), [])

    def test_single_precision_rounds_the_double_result(self):
        self.assertEqual(body(asm.encode("fadds", frD=4, frA=1, frB=2))[0],
                         ["ctx->fpr[4].f64 = (double)(float)(ctx->fpr[1].f64 + ctx->fpr[2].f64);"])
        self.assertEqual(body(asm.encode("fadd", frD=4, frA=1, frB=2))[0],
                         ["ctx->fpr[4].f64 = ctx->fpr[1].f64 + ctx->fpr[2].f64;"])
        self.assertEqual(body(asm.encode("fmuls", frD=4, frA=1, frC=2))[0],
                         ["ctx->fpr[4].f64 = (double)(float)(ctx->fpr[1].f64 * ctx->fpr[2].f64);"])

    def test_multiply_add_forms_use_the_fused_builtin(self):
        fields = {"frD": 4, "frA": 1, "frC": 2, "frB": 3}
        a, b, c = "ctx->fpr[1].f64", "ctx->fpr[3].f64", "ctx->fpr[2].f64"
        expected = {
            "fmadd": "ctx->fpr[4].f64 = __builtin_fma(%s, %s, %s);" % (a, c, b),
            "fmsub": "ctx->fpr[4].f64 = __builtin_fma(%s, %s, -%s);" % (a, c, b),
            "fnmadd": "ctx->fpr[4].f64 = -__builtin_fma(%s, %s, %s);" % (a, c, b),
            "fnmsub": "ctx->fpr[4].f64 = -__builtin_fma(%s, %s, -%s);" % (a, c, b),
            "fmadds": "ctx->fpr[4].f64 = (double)(float)(__builtin_fma(%s, %s, %s));" % (a, c, b),
        }
        for name, line in expected.items():
            self.assertEqual(body(asm.encode(name, **fields))[0], [line], name)

    def test_sign_operations_work_on_the_bit_pattern(self):
        self.assertEqual(body(asm.encode("fneg", frD=4, frB=1))[0],
                         ["ctx->fpr[4].u64 = ctx->fpr[1].u64 ^ 0x8000000000000000ull;"])
        self.assertEqual(body(asm.encode("fabs", frD=4, frB=1))[0],
                         ["ctx->fpr[4].u64 = ctx->fpr[1].u64 & 0x7fffffffffffffffull;"])
        self.assertEqual(body(asm.encode("fnabs", frD=4, frB=1))[0],
                         ["ctx->fpr[4].u64 = ctx->fpr[1].u64 | 0x8000000000000000ull;"])
        self.assertEqual(body(asm.encode("fmr", frD=4, frB=1))[0],
                         ["ctx->fpr[4].u64 = ctx->fpr[1].u64;"])

    def test_integer_conversions_go_through_the_saturating_helpers(self):
        for name, helper in (("fctiw", "dalFctiw"), ("fctiwz", "dalFctiwz"),
                             ("fctid", "dalFctid"), ("fctidz", "dalFctidz")):
            self.assertEqual(body(asm.encode(name, frD=4, frB=1))[0],
                             ["ctx->fpr[4].u64 = %s(ctx->fpr[1].f64);" % helper], name)
        self.assertEqual(body(asm.encode("fcfid", frD=4, frB=1))[0],
                         ["ctx->fpr[4].f64 = (double)(int64_t)ctx->fpr[1].u64;"])

    def test_compares_write_the_selected_field(self):
        for name in ("fcmpu", "fcmpo"):
            self.assertEqual(body(asm.encode(name, crfD=3, frA=1, frB=2))[0],
                             ["dalFcmp(ctx, 3, ctx->fpr[1].f64, ctx->fpr[2].f64);"], name)

    def test_record_form_copies_fpscr_to_cr1(self):
        lines, _ = body(asm.encode("fadd", frD=4, frA=1, frB=2, Rc=1))
        self.assertEqual(lines[-1], "dalRecordFp(ctx);")
        lines, _ = body(asm.encode("fadd", frD=4, frA=1, frB=2))
        self.assertFalse(any("dalRecordFp" in line for line in lines))

    def test_fpscr_moves_call_the_runtime_hooks(self):
        self.assertEqual(body(asm.encode("mffs", frD=1))[0], ["ctx->fpr[1].u64 = dalMffs(ctx);"])
        self.assertEqual(body(asm.encode("mtfsf", FM=0xFF, frB=1))[0],
                         ["dalMtfsf(ctx, 0xffu, ctx->fpr[1].u64);"])
        self.assertEqual(body(asm.encode("mtfsfi", crfD=3, IMM_fp=9))[0],
                         ["dalMtfsf(ctx, 0x10u, (uint64_t)9u << 16);"])
        self.assertEqual(body(asm.encode("mtfsb0", crbD=5))[0], ["dalMtfsb(ctx, 5, 0);"])
        self.assertEqual(body(asm.encode("mtfsb1", crbD=5))[0], ["dalMtfsb(ctx, 5, 1);"])
        self.assertEqual(body(asm.encode("mcrfs", crfD=1, crfS=4))[0],
                         ["dalSetCrField(ctx, 1, (uint32_t)(dalMffs(ctx) >> 12) & 15u);"])


if __name__ == "__main__":
    unittest.main()
