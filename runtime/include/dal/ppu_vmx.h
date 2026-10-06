#pragma once

#include "ppu_context.h"
#include "ppu_int.h"

// Vector helpers for generated code. Register layout: see "Vector registers" in ppu_context.h
// (host lane i = big-endian element n-1-i, so the 128-bit value of a register has big-endian byte 0
// in its most significant byte).

// Partial vector loads and stores. `sh` below is ea & 15.

DAL_FORCEINLINE DalU128 dalVrBits(DalVr v) {
    DalU128 bits;
    __builtin_memcpy(&bits, &v, sizeof bits);
    return bits;
}

DAL_FORCEINLINE DalVr dalVrFromBits(DalU128 bits) {
    DalVr v;
    __builtin_memcpy(&v, &bits, sizeof v);
    return v;
}

// lvlx/lvlxl: the bytes from ea to the end of its 16-byte block go to the left (big-endian low
// numbers) of the register, the rest is zero (RPCS3 `LVLX`).
DAL_FORCEINLINE DalVr dalLvlx(const uint8_t* base, uint32_t ea) {
    return dalVrFromBits(dalVrBits(dalReadVr(base, ea)) << ((ea & 15u) * 8u));
}

// lvrx/lvrxl: the bytes from the block start up to (not including) ea go to the right of the
// register, the rest is zero; an aligned ea loads nothing and does not touch memory (RPCS3 `LVRX`).
DAL_FORCEINLINE DalVr dalLvrx(const uint8_t* base, uint32_t ea) {
    uint32_t sh = ea & 15u;
    if (sh == 0) return dalVrFromBits(0);
    return dalVrFromBits(dalVrBits(dalReadVr(base, ea)) >> ((16u - sh) * 8u));
}

// stvlx/stvlxl: big-endian bytes 0 .. 15-sh go to ea .. (RPCS3 `STVLX`).
DAL_FORCEINLINE void dalStvlx(uint8_t* base, uint32_t ea, DalVr v) {
    uint8_t* p = base + ea;
    for (uint32_t j = 0; j < 16u - (ea & 15u); j++) p[j] = DAL_VR_B(v, j);
}

// stvrx/stvrxl: big-endian bytes 16-sh .. 15 go to the start of the block; nothing when sh == 0
// (RPCS3 `STVRX`).
DAL_FORCEINLINE void dalStvrx(uint8_t* base, uint32_t ea, DalVr v) {
    uint32_t sh = ea & 15u;
    uint8_t* p = base + (ea & ~15u);
    for (uint32_t k = 0; k < sh; k++) p[k] = DAL_VR_B(v, 16u - sh + k);
}

// Element stores write the element at its natural position in the register, at the element-aligned
// address (RPCS3 `STVEBX`, `STVEHX`, `STVEWX`). The element loads need no helper: RPCS3 `LVEBX`,
// `LVEHX` and `LVEWX` load the whole aligned block like `LVX`, which leaves the other elements
// undefined by the architecture.
DAL_FORCEINLINE void dalStvebx(uint8_t* base, uint32_t ea, DalVr v) {
    dalWrite8(base, ea, DAL_VR_B(v, ea & 15u));
}

DAL_FORCEINLINE void dalStvehx(uint8_t* base, uint32_t ea, DalVr v) {
    ea &= ~1u;
    dalWrite16(base, ea, DAL_VR_H(v, (ea & 15u) >> 1));
}

DAL_FORCEINLINE void dalStvewx(uint8_t* base, uint32_t ea, DalVr v) {
    ea &= ~3u;
    dalWrite32(base, ea, DAL_VR_W(v, (ea & 15u) >> 2));
}

// lvsl/lvsr: permute control vectors; big-endian byte j is sh + j (lvsl) or 16 - sh + j (lvsr)
// (RPCS3 `LVSL`, `LVSR`).
DAL_FORCEINLINE DalVr dalLvsl(uint32_t ea) {
    DalU8x16 lanes = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    DalVr v;
    v.u8 = lanes + (uint8_t)(ea & 15u);
    return v;
}

DAL_FORCEINLINE DalVr dalLvsr(uint32_t ea) {
    DalU8x16 lanes = {31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16};
    DalVr v;
    v.u8 = lanes - (uint8_t)(ea & 15u);
    return v;
}

// vsel: bits of b where c is set, bits of a elsewhere (RPCS3 `VSEL`).
DAL_FORCEINLINE DalVr dalVsel(DalVr a, DalVr b, DalVr c) {
    DalVr v;
    v.u32 = (b.u32 & c.u32) | (a.u32 & ~c.u32);
    return v;
}

// VSCR: NJ is bit 16 and SAT bit 0 of the register's big-endian word 3 (AltiVec PEM). RPCS3's
// interpreter indexes `_u32[3]`, which is big-endian word 0 in its byte-reversed register layout,
// while its LLVM translator uses element 0 of the same byte-reversed layout, which is the PEM word;
// the PEM position is kept here because guest code is built for hardware.
DAL_FORCEINLINE DalVr dalMfvscr(const DalPpuContext* ctx) {
    return dalVrFromBeWords(0, 0, 0, ((uint32_t)(ctx->vscr_nj & 1) << 16) | (ctx->vscr_sat & 1));
}

DAL_FORCEINLINE void dalMtvscr(DalPpuContext* ctx, DalVr v) {
    uint32_t vscr = dalVrBeWord(v, 3);
    ctx->vscr_nj = (vscr >> 16) & 1;
    ctx->vscr_sat = vscr & 1;
}
