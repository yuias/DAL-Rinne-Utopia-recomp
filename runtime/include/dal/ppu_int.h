#pragma once

#include "ppu_context.h"

// Integer helpers for generated code. Arithmetic stays on unsigned types so that no emitted
// expression has undefined behaviour.

// Signed overflow of the 64-bit sum r = a + b: both addends have the same sign and the result's
// differs (RPCS3 `ADD`; `SUBF` passes `~a` for the minuend).
DAL_FORCEINLINE bool dalAddOverflows(uint64_t a, uint64_t b, uint64_t r) {
    return ((a ^ r) & (b ^ r)) >> 63;
}

// cntlzw looks at the low word only; both forms return the operand width for zero (the builtin
// is undefined there).
DAL_FORCEINLINE uint64_t dalCntlzw(uint64_t v) {
    uint32_t w = (uint32_t)v;
    return w ? (uint64_t)__builtin_clz(w) : 32u;
}

DAL_FORCEINLINE uint64_t dalCntlzd(uint64_t v) { return v ? (uint64_t)__builtin_clzll(v) : 64u; }
