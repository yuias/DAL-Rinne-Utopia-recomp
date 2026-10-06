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

// rlw*: rotate the low word and duplicate it into both halves, so that masks reaching into the
// high half (MB > ME) see the rotated word there (RPCS3 `dup32`, `rol32`). The count is mod 32.
DAL_FORCEINLINE uint64_t dalRotl32Dup(uint64_t v, uint32_t n) {
    uint32_t w = (uint32_t)v;
    n &= 31;
    uint32_t r = n ? (w << n) | (w >> (32 - n)) : w;
    return ((uint64_t)r << 32) | r;
}

DAL_FORCEINLINE uint64_t dalRotl64(uint64_t v, uint32_t n) {
    n &= 63;
    return n ? (v << n) | (v >> (64 - n)) : v;
}

// slw/srw: the amount is the low 6 bits of rB; 32..63 gives 0. The result is zero-extended.
DAL_FORCEINLINE uint64_t dalSlw(uint64_t v, uint64_t amount) {
    return (amount & 0x20) ? 0 : (uint64_t)(uint32_t)(v << (amount & 31));
}

DAL_FORCEINLINE uint64_t dalSrw(uint64_t v, uint64_t amount) {
    return (amount & 0x20) ? 0 : (uint64_t)((uint32_t)v >> (amount & 31));
}

// sld/srd: the amount is the low 7 bits of rB; 64..127 gives 0.
DAL_FORCEINLINE uint64_t dalSld(uint64_t v, uint64_t amount) {
    return (amount & 0x40) ? 0 : v << (amount & 63);
}

DAL_FORCEINLINE uint64_t dalSrd(uint64_t v, uint64_t amount) {
    return (amount & 0x40) ? 0 : v >> (amount & 63);
}

// sraw/srawi: arithmetic shift of the sign-extended low word (`amount` is taken mod 64); CA = the
// word is negative and a 1 bit was shifted out. Amounts above 31 give the sign fill (RPCS3 `SRAW`).
DAL_FORCEINLINE uint64_t dalSraw(DalPpuContext* ctx, uint64_t v, uint64_t amount) {
    uint32_t w = (uint32_t)v;
    uint32_t n = (uint32_t)(amount & 63);
    bool negative = w >> 31;
    if (n > 31) {
        ctx->xer_ca = negative;
        return negative ? ~0ull : 0;
    }
    ctx->xer_ca = negative && (w & ((1u << n) - 1)) != 0;
    // Shift on the unsigned word and refill the vacated bits with the sign.
    uint32_t r = (w >> n) | (negative && n ? ~0u << (32 - n) : 0);
    return (uint64_t)(int64_t)(int32_t)r;
}

// srad/sradi: 64-bit form; the amount is the low 7 bits of rB (RPCS3 `SRAD`).
DAL_FORCEINLINE uint64_t dalSrad(DalPpuContext* ctx, uint64_t v, uint64_t amount) {
    uint32_t n = (uint32_t)(amount & 127);
    bool negative = v >> 63;
    if (n > 63) {
        ctx->xer_ca = negative;
        return negative ? ~0ull : 0;
    }
    ctx->xer_ca = negative && (v & ((1ull << n) - 1)) != 0;
    return (v >> n) | (negative && n ? ~0ull << (64 - n) : 0);
}

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

// stwcx./stdcx.: CR0 = {lt 0, gt 0, eq = store happened, so = XER.SO} (RPCS3 `STWCX`).
DAL_FORCEINLINE void dalSetCr0Conditional(DalPpuContext* ctx, bool stored) {
    ctx->cr[0] = 0;
    ctx->cr[1] = 0;
    ctx->cr[2] = stored;
    ctx->cr[3] = ctx->xer_so;
}
