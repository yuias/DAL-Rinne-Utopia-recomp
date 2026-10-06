// HLE dispatch: the generated import stubs call dalHleCall(index), which runs the registered
// handler for that import or the default one.

#include "hle/hle.h"

#include <format>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "dal/ppu_context.h"
#include "lv2/cell_error.h"

namespace dal {
namespace {

// The default handler logs this many calls of each import, then only counts.
constexpr std::uint32_t kDefaultLogLimit = 8;

struct InstalledImport {
  const char* module = nullptr;
  const char* name = nullptr;
  GuestHandler handler = nullptr;
  std::uint32_t default_calls = 0;
};

// Written by installImports before any guest thread starts; the call counters are updated under
// the guest lock.
std::vector<InstalledImport> g_imports;
bool g_quiet = false;

}  // namespace

// Replaced by the sysPrxForUser handler file once it has handlers.
std::span<const HleEntry> sysPrxForUserHandlers() { return {}; }

void installImports(const DalImportInfo* imports, std::uint32_t count) {
  std::map<std::pair<std::string, std::uint32_t>, const HleEntry*> registry;
  for (const HleEntry& entry : sysPrxForUserHandlers()) {
    registry.emplace(std::pair{std::string(entry.module), entry.fnid}, &entry);
  }

  g_imports.assign(count, InstalledImport{});
  std::uint32_t implemented = 0;
  for (std::uint32_t i = 0; i < count; i++) {
    const DalImportInfo& info = imports[i];
    InstalledImport& slot = g_imports[i];
    slot.module = info.module;
    slot.name = info.name;
    const auto it = registry.find({info.module != nullptr ? info.module : "", info.fnid});
    if (it == registry.end()) continue;

    const HleEntry& entry = *it->second;
    if (info.name != nullptr && entry.name != nullptr && std::string_view(info.name) != entry.name) {
      logWarn("dal: hle: {}::{} (fnid {:#010x}) is registered as {}", info.module, info.name,
              info.fnid, entry.name);
    }
    slot.handler = entry.handler;
    if (slot.handler != nullptr) implemented++;
  }
  logInfo("dal: hle: {} imports, {} with handlers", count, implemented);
}

void setQuietHle(bool quiet) { g_quiet = quiet; }

void callGuestOpd(DalPpuContext* ctx, uint8_t* base, std::uint32_t opd_addr) {
  const std::uint64_t saved_lr = ctx->lr;
  const std::uint64_t saved_toc = ctx->gpr[2];
  const std::uint32_t entry = dalRead32(base, opd_addr);
  ctx->gpr[2] = dalRead32(base, opd_addr + 4);
  ctx->lr = 0;
  ctx->jump_site = opd_addr;
  ctx->jump_target = entry;
  dalDispatch(ctx, base);
  ctx->gpr[2] = saved_toc;
  ctx->lr = saved_lr;
}

}  // namespace dal

extern "C" {

void dalHleCall(DalPpuContext* ctx, uint8_t* base, uint32_t import_index) {
  if (import_index >= dal::g_imports.size()) {
    dal::fatal(dal::FatalKind::Internal,
               std::format("hle call: import index {} out of range ({} installed)", import_index,
                           dal::g_imports.size()));
  }
  dal::InstalledImport& import = dal::g_imports[import_index];
  const bool implemented = import.handler != nullptr;
  dal::censusHle(import.module, import.name, implemented);

  if (implemented) {
    import.handler(ctx, base);
    return;
  }

  if (!dal::g_quiet && import.default_calls < dal::kDefaultLogLimit) {
    dal::logInfo("dal: default hle {}::{}({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x})",
                 import.module != nullptr ? import.module : "?",
                 import.name != nullptr ? import.name : "?", ctx->gpr[3], ctx->gpr[4], ctx->gpr[5],
                 ctx->gpr[6], ctx->gpr[7], ctx->gpr[8]);
  }
  import.default_calls++;
  ctx->gpr[3] = dal::kCellOk;
}

}  // extern "C"
