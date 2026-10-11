#pragma once

// Override and hook entries of the generated code (see docs/overrides.md). Compiles as C23 and C++26.
#include "ppu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors the "kind" values of the overrides file.
enum DalOverrideKind { kDalOverrideKindOverride = 0, kDalOverrideKindHook = 1 };

typedef struct DalPpuOverride {
    uint32_t guest;            // function entry address
    uint32_t kind;             // DalOverrideKind
    DalPpuFunc* original;      // the emitted guest body, fn_%08x_orig
    uint64_t* calls;           // incremented by the wrapper before the handler runs
    const char* handler_name;  // as written in the overrides file
    const char* label;         // nullptr when the entry has none
} DalPpuOverride;

// The parameters are [[maybe_unused]] because most handlers use only some of them.
#define DAL_OVERRIDE_HANDLER(name)                                                                       \
    void name([[maybe_unused]] DalPpuContext* __restrict ctx, [[maybe_unused]] uint8_t* __restrict base, \
              [[maybe_unused]] const DalPpuOverride* site)

// Defined by generated code (ppu_overrides.c), sorted by guest address.
extern const DalPpuOverride kDalPpuOverrides[];
extern const uint32_t kDalPpuOverrideCount;

// Generic handlers, implemented by the runtime core.
DAL_OVERRIDE_HANDLER(dalHookNop);           // counting only
DAL_OVERRIDE_HANDLER(dalHookLogCall);       // logs the first calls of each entry
DAL_OVERRIDE_HANDLER(dalOverrideReturn0);   // r3 = 0, return: stubs a function out
DAL_OVERRIDE_HANDLER(dalOverrideOriginal);  // runs the original: checks the mechanism itself

// Guest code runs under the guest lock, so a plain increment is enough; the relaxed atomics make
// the census read from another thread well defined.
DAL_FORCEINLINE void dalCountCall(uint64_t* counter) {
    __atomic_store_n(counter, __atomic_load_n(counter, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
}

#ifdef __cplusplus
}  // extern "C"
#endif
