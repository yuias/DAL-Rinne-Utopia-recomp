// lv2 SPU syscalls the game issues before any SPU thread exists: sys_spu_initialize (169) and
// sys_spu_thread_group_connect_event_all_threads (251). Behaviour follows RPCS3
// `Emu/Cell/lv2/sys_spu.cpp`. No SPU thread is ever created, so the "port already in use" scan of
// RPCS3 never finds a user. Handlers run under the guest lock.

#include <bit>
#include <cstdint>

#include "core/guest_memory.h"
#include "dal/ppu_context.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace dal {

// 169 sys_spu_initialize(u32 max_usable_spu, u32 max_raw_spu): RPCS3 pins the usable count to 6 and
// only rejects more than 5 raw SPUs (the busy check cannot fail without SPU threads).
void sysSpuInitialize(DalPpuContext* ctx, std::uint8_t* base) {
  (void)base;
  setResultCode(ctx, static_cast<std::uint32_t>(ctx->gpr[4]) > 5 ? kCellEinval : kCellOk);
}

// 251 sys_spu_thread_group_connect_event_all_threads(u32 id, u32 eq, u64 req, u8* spup): the group
// id is not checked until fake groups exist. The port is the lowest bit set in `req`.
void sysSpuThreadGroupConnectEventAllThreads(DalPpuContext* ctx, std::uint8_t* base) {
  const auto eq = static_cast<std::uint32_t>(ctx->gpr[4]);
  const std::uint64_t req = ctx->gpr[5];
  const auto spup = static_cast<std::uint32_t>(ctx->gpr[6]);
  if (req == 0) {
    setResultCode(ctx, kCellEinval);
    return;
  }
  if (!eventQueueExists(eq)) {
    setResultCode(ctx, kCellEsrch);
    return;
  }
  if (spup == 0) {
    setResultCode(ctx, kCellEfault);
    return;
  }
  dalWrite8(base, spup, static_cast<std::uint8_t>(std::countr_zero(req)));
  setResultCode(ctx, kCellOk);
}

}  // namespace dal
