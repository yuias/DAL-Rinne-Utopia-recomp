// HLE handlers of the sysPrxForUser module. Behaviour follows RPCS3 `Modules/sysPrxForUser.cpp`,
// `sys_ppu_thread_.cpp`, `sys_lwmutex_.cpp`, `sys_heap.cpp`, `sys_prx_.cpp` and `sys_spu_.cpp`. Handlers run under the
// guest lock.

#include <algorithm>
#include <cstdint>
#include <format>
#include <set>

#include "core/boot.h"
#include "core/census.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace dal {
namespace {

constexpr char kModule[] = "sysPrxForUser";

// Heap ids come from the same 1..1023 range RPCS3 uses; the heaps themselves hold no state because
// every allocation comes from the main region.
constexpr std::uint32_t kMaxHeapId = 1023;
constexpr std::uint32_t kHeapAlign = 0x10000;
std::set<std::uint32_t> g_heaps;

// sys_initialize_tls(thread_id, tls_vaddr, tls_filesz, tls_memsz): a no-op when r13 is already
// set, which is the case for the main thread after boot; the game calls it again itself.
void sysInitializeTls(DalPpuContext* ctx, std::uint8_t* base) {
  initializeTls(ctx, base, static_cast<std::uint32_t>(ctx->gpr[3]),
                static_cast<std::uint32_t>(ctx->gpr[4]), static_cast<std::uint32_t>(ctx->gpr[5]),
                static_cast<std::uint32_t>(ctx->gpr[6]));
}

void sysTimeGetSystemTime(DalPpuContext* ctx, std::uint8_t* base) {
  (void)base;
  ctx->gpr[3] = systemTimeUs();
}

void hleProcessExit(DalPpuContext* ctx, std::uint8_t* base) {
  (void)base;
  const auto status = static_cast<std::int32_t>(ctx->gpr[3]);
  stopProcess(ExitCode::ProcessExit, std::format("dal: stop: process exit status {}", status));
}

// _sys_heap_create_heap(name, ...) returns the heap id, 0 when all ids are taken.
void sysHeapCreateHeap(DalPpuContext* ctx, std::uint8_t* base) {
  (void)base;
  for (std::uint32_t id = 1; id <= kMaxHeapId; id++) {
    if (g_heaps.insert(id).second) {
      ctx->gpr[3] = id;
      return;
    }
  }
  ctx->gpr[3] = 0;
}

void sysHeapDeleteHeap(DalPpuContext* ctx, std::uint8_t* base) {
  (void)base;
  g_heaps.erase(static_cast<std::uint32_t>(ctx->gpr[3]));
  ctx->gpr[3] = kCellOk;
}

// _sys_heap_malloc(heap, size): a 64 KiB-aligned block of the main region, 0 on failure.
void sysHeapMalloc(DalPpuContext* ctx, std::uint8_t* base) {
  (void)base;
  ctx->gpr[3] = process().main.allocate(static_cast<std::uint32_t>(ctx->gpr[4]), kHeapAlign);
}

// _sys_heap_memalign(heap, align, size)
void sysHeapMemalign(DalPpuContext* ctx, std::uint8_t* base) {
  (void)base;
  const std::uint32_t align = std::max(static_cast<std::uint32_t>(ctx->gpr[4]), kHeapAlign);
  ctx->gpr[3] = process().main.allocate(static_cast<std::uint32_t>(ctx->gpr[5]), align);
}

// _sys_heap_free(heap, addr): an address that is not a block start is ignored.
void sysHeapFree(DalPpuContext* ctx, std::uint8_t* base) {
  (void)base;
  process().main.free(static_cast<std::uint32_t>(ctx->gpr[4]));
  ctx->gpr[3] = kCellOk;
}

// sys_lwmutex_create(sys_lwmutex_t* lwmutex, sys_lwmutex_attribute_t* attr)
void sysLwmutexCreate(DalPpuContext* ctx, std::uint8_t* base) {
  setResultCode(ctx, lwmutexCreate(base, static_cast<std::uint32_t>(ctx->gpr[3]),
                                   static_cast<std::uint32_t>(ctx->gpr[4])));
}

void sysLwmutexDestroy(DalPpuContext* ctx, std::uint8_t* base) {
  setResultCode(ctx, lwmutexDestroy(ctx, base, static_cast<std::uint32_t>(ctx->gpr[3])));
}

// sys_lwmutex_lock(sys_lwmutex_t* lwmutex, u64 timeout)
void sysLwmutexLock(DalPpuContext* ctx, std::uint8_t* base) {
  setResultCode(ctx, lwmutexLock(ctx, base, static_cast<std::uint32_t>(ctx->gpr[3]), ctx->gpr[4]));
}

void sysLwmutexTryLock(DalPpuContext* ctx, std::uint8_t* base) {
  setResultCode(ctx, lwmutexTryLock(ctx, base, static_cast<std::uint32_t>(ctx->gpr[3])));
}

void sysLwmutexUnlock(DalPpuContext* ctx, std::uint8_t* base) {
  setResultCode(ctx, lwmutexUnlock(ctx, base, static_cast<std::uint32_t>(ctx->gpr[3])));
}

// Registration and notification functions with no effect here: nothing ever runs the registered
// callbacks, because the process ends through sys_process_exit without an exit-spawn.
void returnOk(DalPpuContext* ctx, std::uint8_t* base) {
  (void)base;
  ctx->gpr[3] = kCellOk;
}

constexpr HleEntry kHandlers[] = {
    {kModule, 0x1573dc3f, "sys_lwmutex_lock", sysLwmutexLock},
    {kModule, 0x1bc200f4, "sys_lwmutex_unlock", sysLwmutexUnlock},
    {kModule, 0x2c847572, "_sys_process_atexitspawn", returnOk},
    {kModule, 0x2f85c0ef, "sys_lwmutex_create", sysLwmutexCreate},
    {kModule, 0x35168520, "_sys_heap_malloc", sysHeapMalloc},
    {kModule, 0x42b23552, "sys_prx_register_library", returnOk},
    {kModule, 0x44265c08, "_sys_heap_memalign", sysHeapMemalign},
    {kModule, 0x45fe2fce, "_sys_spu_printf_initialize", returnOk},
    {kModule, 0x744680a2, "sys_initialize_tls", sysInitializeTls},
    {kModule, 0x8461e528, "sys_time_get_system_time", sysTimeGetSystemTime},
    {kModule, 0x8a561d92, "_sys_heap_free", sysHeapFree},
    {kModule, 0x96328741, "_sys_process_at_Exitspawn", returnOk},
    {kModule, 0xa2c7ba64, "sys_prx_exitspawn_with_level", returnOk},
    {kModule, 0xaede4b03, "_sys_heap_delete_heap", sysHeapDeleteHeap},
    {kModule, 0xaeb78725, "sys_lwmutex_trylock", sysLwmutexTryLock},
    {kModule, 0xb2fcf2c8, "_sys_heap_create_heap", sysHeapCreateHeap},
    {kModule, 0xc3476d0c, "sys_lwmutex_destroy", sysLwmutexDestroy},
    {kModule, 0xdd3b27ac, "_sys_spu_printf_finalize", returnOk},
    {kModule, 0xe6f2c1e7, "sys_process_exit", hleProcessExit},
};

}  // namespace

std::span<const HleEntry> sysPrxForUserHandlers() { return kHandlers; }

}  // namespace dal
