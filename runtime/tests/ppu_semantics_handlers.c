// Handlers of the override and hook cases (tools/tests/ppu_semantics/cases_overrides.py).
//
// The generated header declares every handler the cases name, so a definition with another
// signature does not compile.

#include "ppu_semantics_cases.h"

DAL_OVERRIDE_HANDLER(testOverrideAdd100) { ctx->gpr[3] += 100; }

DAL_OVERRIDE_HANDLER(testOverrideOriginalTwice) {
    site->original(ctx, base);
    site->original(ctx, base);
}

DAL_OVERRIDE_HANDLER(testHookMark) {
    ctx->gpr[20] += 1;
    ctx->gpr[21] = site->guest;
    ctx->gpr[22] = ctx->lr;
}

DAL_OVERRIDE_HANDLER(testHookSetR4) { ctx->gpr[4] = 7; }
