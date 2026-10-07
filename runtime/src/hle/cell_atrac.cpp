// HLE of cellAtracSetDataAndGetMemSize, the one cellAtrac import the title scene cannot do without.
// Its "ATRAC3 Decode Thread" asks for the decoder's work memory size first; the default handler
// leaves the output untouched, and the thread then waits on an event flag while holding a lock the
// main thread needs, so the game stops drawing once the opening movie ends. The call reports the work
// memory size RPCS3 reports (`Emu/Cell/Modules/cellAtrac.cpp` `cellAtracSetDataAndGetMemSize`).
//
// Placeholder: every other cellAtrac import stays default-handled, so nothing is decoded and the
// title music is silent. The size is only what the game allocates for a decoder that does not exist.

#include <cstdint>

#include "core/guest_memory.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace dal {
namespace {

constexpr char kModule[] = "cellAtrac";
constexpr std::uint32_t kWorkMemoryBytes = 0x1000;

// cellAtracSetDataAndGetMemSize(CellAtracHandle* handle, u8* buffer, u32 read_bytes, u32 buffer_bytes,
//                               u32* work_memory_bytes)
void atracSetDataAndGetMemSize(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t out = argU32(ctx, 4);
  if (out == 0 || !process().memory.isCommitted(out, 4)) {
    setResultCode(ctx, kCellEfault);
    return;
  }
  dalWrite32(base, out, kWorkMemoryBytes);
  setResultCode(ctx, kCellOk);
}

constexpr HleEntry kHandlers[] = {
    {kModule, 0x66afc68e, "cellAtracSetDataAndGetMemSize", atracSetDataAndGetMemSize},
};

}  // namespace

std::span<const HleEntry> cellAtracHandlers() { return kHandlers; }

}  // namespace dal
