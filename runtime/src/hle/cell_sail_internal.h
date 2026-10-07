#pragma once

// Bookkeeping the cellSail handler files share: the adapter table the player binds adapters from, and
// the PAMF database block that CreateDatabase writes and cellPamfReaderGetNumberOfSpecificStreams
// reads. Everything runs under the guest lock.

#include <cstdint>

#include "core/guest_memory.h"
#include "media/pamf.h"

namespace dal::sail_hle {

inline bool guestMapped(std::uint32_t addr, std::uint32_t size) {
  return addr != 0 && process().memory.isCommitted(addr, size);
}

enum class AdapterKind { Graphics, Sound };

// Ties an initialised adapter to `player`. kCellOk, or kErrorInvalidState when the adapter is not
// initialised or already belongs to another player.
std::uint32_t bindAdapter(AdapterKind kind, std::uint32_t adapter, std::uint32_t player);

// Releases every adapter bound to `player`, so the adapters can be finalized.
void unbindAdapters(std::uint32_t player);

// Forgets all adapters. Intended for tests.
void resetAdapters();

// Fills the 128-byte database block at `addr` for a parsed header. False when the block is not
// mapped. Only stream counts are stored, so the block stays valid after the descriptor it came from
// is gone.
bool writePamfDatabase(std::uint8_t* base, std::uint32_t addr, const PamfInfo& info);

// Invalidates a block written by writePamfDatabase. False when `addr` does not hold one.
bool clearPamfDatabase(std::uint8_t* base, std::uint32_t addr);

}  // namespace dal::sail_hle
