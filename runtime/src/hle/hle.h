#pragma once

#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>

#include "dal/ppu_context.h"
#include "lv2/syscalls.h"

namespace dal {

// An HLE function, identified by the module that exports it and its FNID.
struct HleEntry {
  const char* module;
  uint32_t fnid;
  const char* name;
  GuestHandler handler;  // null: the default handler runs
};

// Handlers of the sysPrxForUser module.
std::span<const HleEntry> sysPrxForUserHandlers();

// Handlers of the cellSysmodule module.
std::span<const HleEntry> cellSysmoduleHandlers();

// Resolves every import to a handler by (module, fnid); imports without one get the default
// handler. A name that differs between the registry and the import table is a warning. Replaces
// any earlier installation. Call before any guest thread runs.
void installImports(const DalImportInfo* imports, uint32_t count);

// Silences the per-call log of the default handler (census counting stays on).
void setQuietHle(bool quiet);

// Calls the function described by the guest function descriptor at `opd_addr` (entry address and
// TOC) and returns when it does. lr and r2 are saved and restored.
void callGuestOpd(DalPpuContext* ctx, uint8_t* base, uint32_t opd_addr);

// Calls a guest function through its OPD with up to 8 arguments in r3..r10 and returns r3.
// Saves and restores lr, r2 and r1; lowers r1 by a 0x70-byte frame (16-aligned, back chain stored)
// so the callee's LR/CR saves cannot touch the HLE caller's frame.
std::uint64_t callGuestFunction(DalPpuContext* ctx, std::uint8_t* base, std::uint32_t opd_addr,
                                std::initializer_list<std::uint64_t> args);

// Low 32 bits of argument register r(3 + n).
inline std::uint32_t argU32(const DalPpuContext* ctx, int n) {
  return static_cast<std::uint32_t>(ctx->gpr[3 + n]);
}

// Reads a NUL-terminated guest string of at most `max` bytes (the terminator is not required).
std::string readGuestString(const std::uint8_t* base, std::uint32_t addr, std::uint32_t max);

}  // namespace dal
