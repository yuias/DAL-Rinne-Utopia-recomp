"""Tests for the emitter core: control flow, labels, unhandled counting and the handler registry."""

import sys
import types
import unittest
from importlib.machinery import ModuleSpec
from unittest import mock

from ps3recomp import emit_core
from ps3recomp.emit_core import EmitEnv, EmitStats, Op, emit_function, handler, u32, u64
from ps3recomp.ppu import decode
from ps3recomp.ppu_flow import CodeImage

from tests import ppc_asm as asm

BASE = 0x10000
OTHER = 0x20000           # a known function outside the function under test
STUB = 0x30000            # an import trampoline


def set_r3(op: Op) -> list[str]:
    return ["ctx->gpr[3] = %d;" % op.f("SIMM")]


# `ori` stands in for nop and `addi` for a plain statement; every other mnemonic is unhandled.
HANDLERS = {"ori": lambda op: [], "addi": set_r3}


def make_env(names=None, thunks=None, switches=None, comments=False) -> EmitEnv:
    return EmitEnv({OTHER: "fn_%08x" % OTHER} if names is None else names,
                   {STUB: "imp_%08x" % STUB} if thunks is None else thunks,
                   switches or {}, comments)


def emit(words, *, env=None, stats=None, start=BASE, entry=None, handlers=HANDLERS, title=None,
         addrs=None):
    image = CodeImage.from_words(start, words)
    entry = start if entry is None else entry
    stats = EmitStats() if stats is None else stats
    text = emit_function(image, entry, addrs or [start + 4 * i for i in range(len(words))],
                         env or make_env(), stats, name="fn_%08x" % entry, handlers=handlers,
                         title=title)
    return text, stats


def lines_of(text: str) -> list[str]:
    return [line.strip() for line in text.splitlines()]


class HelpersTest(unittest.TestCase):
    def test_literals(self):
        self.assertEqual(u32(0x7484C), "0x0007484cu")
        self.assertEqual(u32(-4), "0xfffffffcu")
        self.assertEqual(u64(-8), "0xfffffffffffffff8ull")

    def test_op_expressions(self):
        lwz = Op(decode(asm.lwz(9, -4, 10), BASE, strict=True))
        self.assertEqual(lwz.ea_d(), "(uint32_t)(ctx->gpr[10] + 0xfffffffffffffffcull)")
        self.assertEqual(lwz.ea64_d(), "(ctx->gpr[10] + 0xfffffffffffffffcull)")
        lwz0 = Op(decode(asm.lwz(9, -4, 0), BASE, strict=True))
        self.assertEqual(lwz0.ea_d(), "0xfffffffcu")
        ld = Op(decode(asm.ld(9, -8, 2), BASE, strict=True))
        self.assertEqual(ld.ea_d("DS"), "(uint32_t)(ctx->gpr[2] + 0xfffffffffffffff8ull)")
        lwzx = Op(decode(asm.lwzx(3, 4, 5), BASE, strict=True))
        self.assertEqual(lwzx.ea_x(), "(uint32_t)(ctx->gpr[4] + ctx->gpr[5])")
        self.assertEqual(lwzx.ea64_x(), "(ctx->gpr[4] + ctx->gpr[5])")
        lwzx0 = Op(decode(asm.lwzx(3, 0, 5), BASE, strict=True))
        self.assertEqual(lwzx0.ea_x(), "(uint32_t)ctx->gpr[5]")
        self.assertEqual(lwzx0.record("ctx->gpr[3]"), "dalRecord(ctx, ctx->gpr[3]);")
        self.assertEqual((lwzx0.gpr(7), lwzx0.fpr(1), lwzx0.vr(2)),
                         ("ctx->gpr[7]", "ctx->fpr[1]", "ctx->vr[2]"))

    def test_op_flags(self):
        add_o = Op(decode(asm.add(3, 4, 5) | 1 << 10 | 1, BASE, strict=True))
        self.assertEqual((add_o.name, add_o.rc, add_o.oe), ("add", True, True))
        plain = Op(decode(asm.add(3, 4, 5), BASE, strict=True))
        self.assertEqual((plain.rc, plain.oe), (False, False))

    def test_unsupported_sets_the_key(self):
        op = Op(decode(asm.lwz(9, 0, 0), BASE, strict=True))
        self.assertIsNone(op.unsupported("ra0"))
        self.assertEqual(op.unhandled_key, "lwz:ra0")


class FunctionShapeTest(unittest.TestCase):
    def test_empty_body_function_with_a_return(self):
        text, stats = emit([asm.blr()])
        self.assertEqual(text, "DAL_PPU_FUNC(fn_00010000) {\n"
                               "    DAL_ENTER(0x00010000u);\n"
                               "    return;\n"
                               "}\n")
        self.assertEqual((stats.functions, stats.instructions, stats.labels), (1, 1, 0))
        self.assertEqual(stats.edges["returns"], 1)

    def test_title_is_a_sanitised_comment(self):
        text, _ = emit([asm.blr()], title="foo\\\nbar\u00e9")
        self.assertEqual(text.splitlines()[0], "// foo??bar?")

    def test_comments_option(self):
        text, _ = emit([asm.li(3, 5), asm.blr()], env=make_env(comments=True))
        self.assertIn("    // 0x00010000: 38600005  li r3,0x5\n", text)
        self.assertIn("    // 0x00010004: 4e800020  blr\n", text)

    def test_no_comments_by_default(self):
        text, _ = emit([asm.li(3, 5), asm.blr()])
        self.assertNotIn("//", text)

    def test_output_is_deterministic(self):
        words = [asm.li(3, 1), asm.b(BASE + 4, BASE), asm.blr()]
        self.assertEqual(emit(words)[0], emit(words)[0])

    def test_multi_line_handler_result_gets_a_block(self):
        def two(op):
            return ["uint64_t t0 = ctx->gpr[3];", "ctx->gpr[4] = t0;"]

        text, _ = emit([asm.li(3, 1), asm.blr()], handlers={"addi": two})
        self.assertEqual(lines_of(text)[2:7], ["{", "uint64_t t0 = ctx->gpr[3];",
                                               "ctx->gpr[4] = t0;", "}", "return;"])


class BranchTest(unittest.TestCase):
    def test_forward_goto_has_no_loop_hook(self):
        words = [asm.b(BASE, BASE + 8), asm.li(3, 1), asm.blr()]
        text, stats = emit(words)
        self.assertEqual(text, "DAL_PPU_FUNC(fn_00010000) {\n"
                               "    DAL_ENTER(0x00010000u);\n"
                               "    goto L_00010008;\n"
                               "    ctx->gpr[3] = 1;\n"
                               "L_00010008:\n"
                               "    return;\n"
                               "}\n")
        self.assertEqual((stats.labels, stats.loop_hooks), (1, 0))

    def test_backward_branch_text(self):
        words = [asm.li(3, 1), asm.b(BASE + 4, BASE), asm.blr()]
        text, stats = emit(words)
        print("\n" + text)
        self.assertEqual(text, "DAL_PPU_FUNC(fn_00010000) {\n"
                               "    DAL_ENTER(0x00010000u);\n"
                               "L_00010000:\n"
                               "    ctx->gpr[3] = 1;\n"
                               "    { DAL_LOOP(0x00010004u); goto L_00010000; }\n"
                               "    return;\n"
                               "}\n")
        self.assertEqual((stats.labels, stats.loop_hooks), (1, 1))

    def test_branch_to_self_is_a_backward_branch(self):
        text, stats = emit([asm.b(BASE, BASE)])
        self.assertIn("    { DAL_LOOP(0x00010000u); goto L_00010000; }\n", text)
        self.assertEqual(stats.loop_hooks, 1)

    def test_branch_to_own_entry_labels_the_entry(self):
        words = [asm.li(3, 1), asm.b(BASE + 4, BASE), asm.blr()]
        text, _ = emit(words, env=make_env(names={BASE: "fn_00010000"}))
        self.assertIn("DAL_ENTER(0x00010000u);\nL_00010000:\n", text)
        self.assertEqual(text.count("L_00010000:"), 1)

    def test_labels_only_where_targeted(self):
        words = [asm.li(3, 1), asm.li(3, 2), asm.b(BASE + 8, BASE + 12), asm.li(3, 3), asm.blr()]
        text, stats = emit(words)
        self.assertEqual(text.count(" L_") + text.count("\nL_"), 2)   # one goto, one label
        self.assertIn("L_0001000c:\n", text)
        self.assertEqual(stats.labels, 1)

    def test_always_taken_bc_acts_like_b(self):
        words = [asm.bc(20, 0, BASE, BASE + 8), asm.li(3, 1), asm.blr()]
        text, _ = emit(words)
        self.assertIn("    goto L_00010008;\n", text)

    def test_absolute_branch_target(self):
        text, stats = emit([asm.b(BASE, OTHER, aa=True)])
        self.assertIn("    DAL_TAIL(fn_00020000);\n", text)
        self.assertEqual(stats.edges["tails_direct"], 1)


class TailTest(unittest.TestCase):
    def test_tail_to_function(self):
        text, stats = emit([asm.b(BASE, OTHER)])
        self.assertIn("    DAL_TAIL(fn_00020000);\n", text)
        self.assertEqual(stats.edges["tails_direct"], 1)

    def test_tail_to_import(self):
        text, stats = emit([asm.b(BASE, STUB)])
        self.assertIn("    DAL_TAIL(imp_00030000);\n", text)
        self.assertEqual(stats.edges["tails_import"], 1)

    def test_tail_to_dynamic_target_is_recorded(self):
        text, stats = emit([asm.b(BASE, 0x40000)])
        self.assertIn("    DAL_TAIL_DYN(0x00010000u, 0x00040000u);\n", text)
        self.assertEqual(stats.edges["tails_dynamic"], 1)
        self.assertEqual(stats.dynamic_targets, [(BASE, 0x40000, "tail")])

    def test_bctr_is_a_dynamic_tail(self):
        text, stats = emit([asm.bctr()])
        self.assertIn("    DAL_TAIL_DYN(0x00010000u, ctx->ctr);\n", text)
        self.assertEqual(stats.edges["tails_dynamic"], 1)
        self.assertEqual(stats.dynamic_targets, [])

    def test_tail_to_a_target_inside_the_function_when_it_is_also_a_function(self):
        # Entry classification wins for calls; branches stay local.
        env = make_env(names={BASE + 8: "fn_00010008"})
        text, _ = emit([asm.b(BASE, BASE + 8), asm.li(3, 1), asm.blr()], env=env)
        self.assertIn("    goto L_00010008;\n", text)


class CallTest(unittest.TestCase):
    def test_call_kinds(self):
        words = [asm.bl(BASE, OTHER), asm.bl(BASE + 4, STUB), asm.bl(BASE + 8, 0x40000),
                 asm.bctrl(), asm.blrl(), asm.blr()]
        text, stats = emit(words)
        body = lines_of(text)[2:-1]
        self.assertEqual(body, [
            "DAL_CALL(fn_00020000, 0x00010004u);",
            "DAL_CALL(imp_00030000, 0x00010008u);",
            "DAL_CALL_DYN(0x00010008u, 0x00040000u, 0x0001000cu);",
            "DAL_CALL_DYN(0x0001000cu, ctx->ctr, 0x00010010u);",
            "DAL_CALL_DYN(0x00010010u, ctx->lr, 0x00010014u);",
            "return;"])
        self.assertEqual(stats.edges["calls_direct"], 1)
        self.assertEqual(stats.edges["calls_import"], 1)
        self.assertEqual(stats.edges["calls_dynamic_const"], 1)
        self.assertEqual(stats.edges["calls_indirect"], 2)
        self.assertEqual(stats.dynamic_targets, [(BASE + 8, 0x40000, "call")])

    def test_call_to_own_entry_is_a_call_not_a_goto(self):
        text, stats = emit([asm.bl(BASE, BASE), asm.blr()], env=make_env(names={BASE: "fn_00010000"}))
        self.assertIn("    DAL_CALL(fn_00010000, 0x00010004u);\n", text)
        self.assertNotRegex(text, r"(^|[ \n])L_")
        self.assertEqual(stats.labels, 0)

    def test_call_to_a_mid_function_address_is_dynamic(self):
        text, stats = emit([asm.bl(BASE, BASE + 8), asm.blr(), asm.blr()])
        self.assertIn("    DAL_CALL_DYN(0x00010000u, 0x00010008u, 0x00010004u);\n", text)
        self.assertEqual(stats.dynamic_targets, [(BASE, BASE + 8, "call")])

    def test_get_pc_idiom_only_writes_lr(self):
        words = [asm.bc(20, 31, BASE, BASE + 4, lk=True), asm.blr()]
        text, stats = emit(words)
        self.assertEqual(lines_of(text)[2:-1], ["ctx->lr = 0x00010004u;", "return;"])
        self.assertEqual(stats.edges["calls_direct"] + stats.edges["calls_dynamic_const"], 0)
        self.assertEqual(sum(stats.unhandled.values()), 0)

    def test_get_pc_via_b_lk(self):
        text, _ = emit([asm.b(BASE, BASE + 4, lk=True), asm.blr()])
        self.assertEqual(lines_of(text)[2], "ctx->lr = 0x00010004u;")

    def test_conditional_get_pc_is_unhandled_for_now(self):
        _, stats = emit([asm.bc(16, 0, BASE, BASE + 4, lk=True), asm.blr()])
        self.assertEqual(stats.unhandled["bc"], 1)


class FallThroughTest(unittest.TestCase):
    def test_call_then_fall_through_out_of_the_function(self):
        env = make_env(names={OTHER: "fn_00020000", BASE + 4: "fn_00010004"})
        text, stats = emit([asm.bl(BASE, OTHER)], env=env)
        self.assertEqual(lines_of(text)[2:-1],
                         ["DAL_CALL(fn_00020000, 0x00010004u);", "DAL_TAIL(fn_00010004);"])
        self.assertEqual(stats.edges["fallthrough_tails"], 1)
        self.assertEqual(stats.edges["tails_direct"], 0)

    def test_fall_through_to_an_import_and_to_a_dynamic_target(self):
        _, stats = emit([asm.li(3, 1)], env=make_env(thunks={BASE + 4: "imp_00010004"}))
        self.assertEqual(stats.edges["fallthrough_tails"], 1)
        text, stats = emit([asm.li(3, 1)])
        self.assertEqual(lines_of(text)[3], "DAL_TAIL_DYN(0x00010000u, 0x00010004u);")
        self.assertEqual(stats.dynamic_targets, [(BASE, BASE + 4, "fallthrough")])
        self.assertEqual(stats.edges["fallthrough_tails"], 1)
        self.assertEqual(stats.edges["tails_dynamic"], 0)

    def test_no_tail_after_transfers_or_inside_the_function(self):
        for word in (asm.blr(), asm.bctr(), asm.b(BASE, OTHER), asm.trap()):
            _, stats = emit([word])
            self.assertEqual(stats.edges["fallthrough_tails"], 0)
        _, stats = emit([asm.li(3, 1), asm.blr()])
        self.assertEqual(stats.edges["fallthrough_tails"], 0)

    def test_gap_between_ranges_counts_as_not_owned(self):
        words = [asm.li(3, 1), asm.nop(), asm.blr()]
        text, stats = emit(words, addrs=[BASE, BASE + 8])
        self.assertEqual(lines_of(text)[2:-1],
                         ["ctx->gpr[3] = 1;", "DAL_TAIL_DYN(0x00010000u, 0x00010004u);", "return;"])
        self.assertEqual(stats.instructions, 2)


class SyscallAndTrapTest(unittest.TestCase):
    def test_sc_falls_through(self):
        text, stats = emit([asm.sc(), asm.blr()])
        self.assertEqual(lines_of(text)[2:-1], ["DAL_SYSCALL(0x00010000u);", "return;"])
        self.assertEqual(stats.edges["syscalls"], 1)

    def test_unconditional_trap_has_no_fall_through_tail(self):
        for word in (asm.trap(), asm.tw(31, 3, 4), asm.twi(31, 3, 5)):
            text, stats = emit([word])
            self.assertEqual(lines_of(text)[2:-1], ["DAL_TRAP(0x00010000u);"])
            self.assertEqual(stats.edges["traps"], 1)
            self.assertEqual(stats.edges["fallthrough_tails"], 0)

    def test_conditional_traps_are_unhandled_for_now(self):
        _, stats = emit([asm.tw(4, 3, 4), asm.twi(1, 3, 5), asm.blr()])
        self.assertEqual(stats.unhandled["tw"], 1)
        self.assertEqual(stats.unhandled["twi"], 1)


class UnhandledTest(unittest.TestCase):
    def test_missing_handler_counts_by_name(self):
        words = [asm.mflr(0), asm.mflr(0), asm.lwz(3, 0, 4), asm.blr()]
        text, stats = emit(words)
        self.assertEqual(dict(stats.unhandled), {"mfspr": 2, "lwz": 1})
        self.assertEqual(stats.unhandled_sites, {"mfspr": [BASE, BASE + 4], "lwz": [BASE + 8]})
        self.assertIn("    DAL_UNHANDLED(0x00010000u, 0x7c0802a6u);\n", text)

    def test_invalid_word(self):
        text, stats = emit([0x00000000, asm.blr()])
        self.assertIn("    DAL_UNHANDLED(0x00010000u, 0x00000000u);\n", text)
        self.assertEqual(dict(stats.unhandled), {"invalid": 1})
        self.assertEqual(stats.edges["fallthrough_tails"], 0)

    def test_invalid_last_word_has_no_fall_through(self):
        _, stats = emit([0x00000000])
        self.assertEqual(stats.edges["fallthrough_tails"], 0)

    def test_nop_is_unhandled_without_a_handler(self):
        _, stats = emit([asm.nop(), asm.blr()], handlers={})
        self.assertEqual(stats.unhandled["ori"], 1)

    def test_handler_returning_empty_list_is_handled(self):
        text, stats = emit([asm.nop(), asm.blr()])
        self.assertEqual(sum(stats.unhandled.values()), 0)
        self.assertEqual(lines_of(text)[2:-1], ["return;"])

    def test_unsupported_key_and_example_limit(self):
        def ra0(op):
            return op.unsupported("ra0")

        words = [asm.li(3, i) for i in range(7)] + [asm.blr()]
        _, stats = emit(words, handlers={"addi": ra0})
        self.assertEqual(stats.unhandled["addi:ra0"], 7)
        self.assertEqual(stats.unhandled_sites["addi:ra0"], [BASE + 4 * i for i in range(5)])
        self.assertNotIn("addi", stats.unhandled)

    def test_conditional_branch_forms_are_unhandled_for_now(self):
        words = [asm.bgt(0, BASE, BASE + 12), asm.bclr(12, 2), asm.bcctr(12, 2), asm.blr()]
        text, stats = emit(words)
        self.assertEqual(dict(stats.unhandled), {"bc": 1, "bclr": 1, "bcctr": 1})
        self.assertNotRegex(text, r"(^|[ \n])L_")
        self.assertEqual(stats.labels, 0)

    def test_conditional_linking_indirect_branch_is_not_a_call(self):
        _, stats = emit([asm.bclr(12, 2, lk=True), asm.bcctr(12, 2, lk=True), asm.blr()])
        self.assertEqual(dict(stats.unhandled), {"bclr": 1, "bcctr": 1})
        self.assertEqual(stats.edges["calls_indirect"], 0)


class RegistryTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.dict(emit_core._REGISTRY, clear=True)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_register_and_return_the_function(self):
        @handler("addi", "addis")
        def fn(op):
            return []

        self.assertIs(emit_core._REGISTRY["addis"], fn)

    def test_duplicate_name(self):
        handler("addi")(lambda op: [])
        with self.assertRaises(ValueError):
            handler("addi")(lambda op: [])
        # A failed registration leaves the other names untouched.
        with self.assertRaises(ValueError):
            handler("addis", "addi")(lambda op: [])
        self.assertNotIn("addis", emit_core._REGISTRY)

    def test_unknown_name(self):
        with self.assertRaises(ValueError):
            handler("nosuchop")

    def test_repeated_name_in_one_call(self):
        with self.assertRaises(ValueError):
            handler("addi", "addi")

    def test_load_handlers_rejects_a_name_from_the_wrong_group(self):
        module_name = "ps3recomp.emit_ops_int_alu"
        fake = types.ModuleType(module_name)
        fake.__spec__ = ModuleSpec(module_name, None)

        def fn(op):
            return []

        fn.__module__ = module_name
        handler("lwz")(fn)                       # `lwz` belongs to group `mem`
        with mock.patch.dict(sys.modules, {module_name: fake}):
            with self.assertRaises(ValueError) as ctx:
                emit_core.load_handlers()
        self.assertIn("lwz", str(ctx.exception))

    def test_load_handlers_accepts_a_name_from_its_own_group(self):
        module_name = "ps3recomp.emit_ops_int_alu"
        fake = types.ModuleType(module_name)
        fake.__spec__ = ModuleSpec(module_name, None)

        def fn(op):
            return []

        fn.__module__ = module_name
        handler("addi")(fn)
        with mock.patch.dict(sys.modules, {module_name: fake}):
            self.assertIs(emit_core.load_handlers()["addi"], fn)


if __name__ == "__main__":
    unittest.main()
