"""Override and hook cases: every way into a function reaches its wrapper.

The handlers are defined in runtime/tests/ppu_semantics_handlers.c:
  testOverrideAdd100        r3 += 100 (the callee body, `r3 += 1`, must not run)
  testOverrideOriginalTwice runs the original body twice
  testHookMark              r20 += 1, r21 = site->guest, r22 = LR at the wrapper
  testHookSetR4             r4 = 7
The callee `A` is `addi r3,r3,1` unless a case says otherwise. Each case names the call counts it
expects, so a path that bypasses the wrapper shows up as a wrong count as well as a wrong result.
"""

from tests import ppc_asm as asm

from . import CODE_BASE, Case

A = 0x18000
B = 0x18100
REF = "PowerPC Book I 2.4 branch instructions"
INC_R3 = (asm.addi(3, 3, 1),)


def at(index: int) -> int:
    return CODE_BASE + 4 * index


def case(name: str, words, init=None, expect=None, **kwargs) -> Case:
    return Case(name, REF, tuple(words), init or {}, expect or {}, **kwargs)


OVERRIDE_ADD100 = {A: ("override", "testOverrideAdd100")}
CALLS_A_ONCE = {A: 1}

CASES = [
    # --- overrides: the body is replaced on every path ---------------------------------------
    case("override_direct_call", [asm.bl(at(0), A)], {"r3": 5}, {"r3": 105},
         callees={A: INC_R3}, overrides=OVERRIDE_ADD100, expect_calls=CALLS_A_ONCE),
    case("override_tail_call", [asm.b(at(0), A)], {"r3": 5}, {"r3": 105},
         callees={A: INC_R3}, overrides=OVERRIDE_ADD100, expect_calls=CALLS_A_ONCE),
    case("override_dispatch", [asm.mtctr(12), asm.bctrl()], {"r3": 5, "r12": A}, {"r3": 105},
         callees={A: INC_R3}, overrides=OVERRIDE_ADD100, expect_calls=CALLS_A_ONCE),
    # A switch table entry outside the function is a tail transfer to the callee's name.
    case("override_switch_case", [asm.bctr(), asm.blr()], {"r3": 5, "ctr": A}, {"r3": 105},
         callees={A: INC_R3}, switch_targets={at(0): (at(1), A)},
         overrides=OVERRIDE_ADD100, expect_calls=CALLS_A_ONCE),
    # The case function itself is overridden: the driver enters through the wrapper.
    case("override_case_entry", INC_R3, {"r3": 5}, {"r3": 105},
         overrides={CODE_BASE: ("override", "testOverrideAdd100")},
         expect_calls={CODE_BASE: 1}),
    # The original runs twice from the handler: 5 + 1 + 1.
    case("override_runs_original", [asm.bl(at(0), A)], {"r3": 5}, {"r3": 7},
         callees={A: INC_R3}, overrides={A: ("override", "testOverrideOriginalTwice")},
         expect_calls=CALLS_A_ONCE),

    # --- hooks: the handler runs first, then the original -----------------------------------
    # LR at the wrapper is the return address of the `bl`.
    case("hook_direct_call", [asm.bl(at(0), A)], {"r3": 5},
         {"r3": 6, "r20": 1, "r21": A, "r22": at(1)},
         callees={A: INC_R3}, overrides={A: ("hook", "testHookMark")}, expect_calls=CALLS_A_ONCE),
    # The hook sets r4 = 7 before the original `add r3,r4,r4` reads it.
    case("hook_rewrites_argument", [asm.bl(at(0), A)], {"r4": 1}, {"r3": 14, "r4": 7},
         callees={A: (asm.add(3, 4, 4),)}, overrides={A: ("hook", "testHookSetR4")},
         expect_calls=CALLS_A_ONCE),
    # X and Y tail-call each other until r3 reaches r5. Every round enters the hooked X through
    # its name, so the wrapper runs 1000000 times; the hook's frame must be gone before the body
    # runs, or the host stack would grow by a million frames.
    case("hook_tail_cycle", [asm.bl(at(0), A)], {"r3": 0, "r5": 1000000},
         {"r3": 1000000, "r20": 1000000},
         callees={A: (asm.addi(3, 3, 1), asm.encode("cmp", crfD=0, L=0, rA=3, rB=5),
                      asm.bclr(4, 0), asm.b(A + 12, B)),
                  B: (asm.b(B, A),)},
         overrides={A: ("hook", "testHookMark")}, expect_calls={A: 1000000}),
    # A branch to the function's own entry is a loop inside the body: the hook runs once for the
    # call, not once per iteration.
    case("hook_self_loop", [asm.bl(at(0), A)], {"r3": 0, "r5": 5}, {"r3": 5, "r20": 1},
         callees={A: (asm.addi(3, 3, 1), asm.encode("cmp", crfD=0, L=0, rA=3, rB=5),
                      asm.bc(12, 0, A + 8, A))},
         overrides={A: ("hook", "testHookMark")}, expect_calls=CALLS_A_ONCE),
]
