#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dal/ppu_override.h"

namespace dal {

// Checks the generated override table against the dispatch table, logs the active entries and
// registers the census section. Call after dalRegisterFuncs. A second call replaces the first (tests).
void installOverrides(const DalPpuOverride* overrides, std::uint32_t count);

// The census section, empty when no entry is installed. Reads the counters with relaxed atomic loads.
std::vector<std::string> overrideCensusLines();

}  // namespace dal
