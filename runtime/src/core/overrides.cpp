#include "core/overrides.h"

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/census.h"
#include "core/fatal.h"
#include "core/log.h"

namespace dal {
namespace {

// Written by installOverrides before guest threads exist (or by tests); read by the census.
const DalPpuOverride* g_overrides = nullptr;
std::uint32_t g_override_count = 0;

const char* kindName(std::uint32_t kind) { return kind == kDalOverrideKindHook ? "hook" : "override"; }

std::string describe(const DalPpuOverride& o) {
    std::string text = std::format("{} {}", kindName(o.kind), o.handler_name);
    if (o.label != nullptr) {
        text += std::format(" \"{}\"", o.label);
    }
    return text;
}

}  // namespace

void installOverrides(const DalPpuOverride* overrides, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; i++) {
        const DalPpuOverride& o = overrides[i];
        if (i > 0 && o.guest <= overrides[i - 1].guest) {
            fatal(FatalKind::Internal, std::format("override table: {:#010x} follows {:#010x} (unsorted or duplicate)",
                                                   o.guest, overrides[i - 1].guest));
        }
        if (o.kind != kDalOverrideKindOverride && o.kind != kDalOverrideKindHook) {
            fatal(FatalKind::Internal, std::format("override table: {:#010x} has unknown kind {}", o.guest, o.kind));
        }
        if (o.original == nullptr || o.calls == nullptr || o.handler_name == nullptr) {
            fatal(FatalKind::Internal,
                  std::format("override table: entry {} has no original, counter or handler name", i));
        }
        DalPpuFunc* target = dalLookup(o.guest);
        if (target == nullptr) {
            fatal(FatalKind::Internal, std::format("override table: {:#010x} is not in the function table", o.guest));
        }
        // The mapping must name the wrapper; the original is reachable only through it.
        if (target == o.original) {
            fatal(FatalKind::Internal, std::format("override table: dispatch to {:#010x} reaches the original body, "
                                                   "not the wrapper",
                                                   o.guest));
        }
    }

    g_overrides = overrides;
    g_override_count = count;
    if (count == 0) {
        setOverrideCensusProvider(nullptr);
        logInfo("dal: overrides: none");
        return;
    }

    const auto hooks = static_cast<std::uint32_t>(
            std::ranges::count(std::span(overrides, count), kDalOverrideKindHook, &DalPpuOverride::kind));
    logInfo("dal: overrides: {} entries (override {}, hook {})", count, count - hooks, hooks);
    for (std::uint32_t i = 0; i < count; i++) {
        const DalPpuOverride& o = overrides[i];
        logInfo("dal: overrides: {} {:#010x} {}{}", kindName(o.kind), o.guest, o.handler_name,
                o.label != nullptr ? std::format(" \"{}\"", o.label) : std::string());
    }
    setOverrideCensusProvider(&overrideCensusLines);
}

std::vector<std::string> overrideCensusLines() {
    std::vector<std::string> lines;
    if (g_override_count == 0) {
        return lines;
    }

    std::vector<std::pair<std::uint64_t, const DalPpuOverride*>> rows;
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < g_override_count; i++) {
        const std::uint64_t calls = __atomic_load_n(g_overrides[i].calls, __ATOMIC_RELAXED);
        rows.emplace_back(calls, &g_overrides[i]);
        total += calls;
    }
    // The table is sorted by address, so a stable sort keeps the address order among equal counts.
    std::ranges::stable_sort(rows, [](const auto& a, const auto& b) { return a.first > b.first; });

    lines.push_back(std::format("census: overrides {} entries, {} calls", g_override_count, total));
    for (const auto& [calls, o] : rows) {
        // Entries never reached are listed too: that is information.
        lines.push_back(std::format("  {:#010x} {}: {}", o->guest, describe(*o), calls));
    }
    return lines;
}

}  // namespace dal
