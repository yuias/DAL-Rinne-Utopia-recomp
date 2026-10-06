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

// lfs/stfs: the FPR holds a double. `(double)` is exact (denormal floats stay normal doubles) and
// `(float)` rounds to nearest even (RPCS3 `LFS`, `STFS`).
DAL_FORCEINLINE double dalReadF32(const uint8_t* base, uint32_t ea) {
    uint32_t bits = dalRead32(base, ea);
    float f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return (double)f;
}

DAL_FORCEINLINE void dalWriteF32(uint8_t* base, uint32_t ea, double value) {
    float f = (float)value;
    uint32_t bits;
    __builtin_memcpy(&bits, &f, sizeof bits);
    dalWrite32(base, ea, bits);
}

// lswi/lswx: `count` bytes into consecutive registers starting at `rd` (wrapping after r31),
// four per register from the high end; a final partial word is zero-filled (RPCS3 `LSWX`).
DAL_FORCEINLINE void dalLoadString(DalPpuContext* ctx, const uint8_t* base, uint32_t ea,
                                   uint32_t rd, uint32_t count) {
    for (; count >= 4; count -= 4, ea += 4, rd = (rd + 1) & 31) {
        ctx->gpr[rd] = dalRead32(base, ea);
    }
    if (count) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; ++i) {
            value |= (uint32_t)dalRead8(base, ea + i) << ((3 ^ i) * 8);
        }
        ctx->gpr[rd] = value;
    }
}

// stswi/stswx, the mirror of dalLoadString (RPCS3 `STSWX`).
DAL_FORCEINLINE void dalStoreString(const DalPpuContext* ctx, uint8_t* base, uint32_t ea,
                                    uint32_t rs, uint32_t count) {
    for (; count >= 4; count -= 4, ea += 4, rs = (rs + 1) & 31) {
        dalWrite32(base, ea, (uint32_t)ctx->gpr[rs]);
    }
    if (count) {
        uint32_t value = (uint32_t)ctx->gpr[rs];
        for (uint32_t i = 0; i < count; ++i) {
            dalWrite8(base, ea + i, (uint8_t)(value >> ((3 ^ i) * 8)));
        }
    }
}
