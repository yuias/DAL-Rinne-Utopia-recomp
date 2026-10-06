#pragma once

#include <cstdint>
#include <span>

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

// Resolves every import to a handler by (module, fnid); imports without one get the default
// handler. A name that differs between the registry and the import table is a warning. Replaces
// any earlier installation. Call before any guest thread runs.
void installImports(const DalImportInfo* imports, uint32_t count);

// Silences the per-call log of the default handler (census counting stays on).
void setQuietHle(bool quiet);

// Calls the function described by the guest function descriptor at `opd_addr` (entry address and
// TOC) and returns when it does. lr and r2 are saved and restored.
void callGuestOpd(DalPpuContext* ctx, uint8_t* base, uint32_t opd_addr);

}  // namespace dal
