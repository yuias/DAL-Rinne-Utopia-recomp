// HLE handlers of the cellSysutil callback pump and the BGM playback status. Behaviour follows
// RPCS3 `Emu/Cell/Modules/cellSysutil.cpp` (function of the same name). Handlers run under the
// guest lock, so the tables need no mutex.
//
// Deviations from RPCS3:
//  - The queue has no abort handles (RPCS3 uses them to drop queued savedata/dialog callbacks of a
//    closed object); nothing here needs them yet.
//  - CELL_SYSUTIL_REQUEST_EXITGAME does not arm RPCS3's termination watchdog.
//  - The BGM state is the status struct only; no system BGM is played, so the player state stays
//    "stop" and the fade ratio 0.
// RPCS3 sends no system events on its own at startup; the first ones come from the HLE modules that
// emulate the system software (message dialog, save data, trophy installation, ...).

#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <utility>

#include "core/guest_memory.h"
#include "core/log.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"

namespace dal {
namespace {

constexpr char kModule[] = "cellSysutil";

constexpr std::uint32_t kSlotCount = 4;

// CellSysutilBgmPlaybackStatus: u8 playerState, u8 enableState, char contentId[16],
// u8 currentFadeRatio, char reserved[13].
constexpr std::uint32_t kBgmStatusSize = 32;
constexpr std::uint8_t kBgmPlayerStop = 1;     // CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP
constexpr std::uint8_t kBgmEnable = 0;         // CELL_SYSUTIL_BGMPLAYBACK_STATUS_ENABLE
constexpr std::uint8_t kBgmDisable = 1;        // CELL_SYSUTIL_BGMPLAYBACK_STATUS_DISABLE
constexpr std::uint32_t kBgmErrorParam = 0x8002b101;  // CELL_SYSUTIL_BGMPLAYBACK_ERROR_PARAM

struct Slot {
  std::uint32_t func = 0;  // OPD of void(u64 status, u64 param, void* userdata); 0 = free
  std::uint32_t userdata = 0;
};

std::array<Slot, kSlotCount> g_slots;
std::deque<SysutilCall> g_queue;
bool g_draw_started = false;
std::uint8_t g_bgm_enable_state = kBgmDisable;

// cellSysutilRegisterCallback(u32 slot, CellSysutilCallback func, void* userdata)
void registerCallback(DalPpuContext* ctx, std::uint8_t*) {
  const std::uint32_t slot = argU32(ctx, 0);
  if (slot >= kSlotCount) {
    ctx->gpr[3] = kSysutilErrorValue;
    return;
  }
  g_slots[slot] = {argU32(ctx, 1), argU32(ctx, 2)};
  logInfo("dal: sysutil: callback slot {} = {:#010x} (userdata {:#x})", slot, g_slots[slot].func,
          g_slots[slot].userdata);
  ctx->gpr[3] = kCellOk;
}

// cellSysutilUnregisterCallback(u32 slot)
void unregisterCallback(DalPpuContext* ctx, std::uint8_t*) {
  const std::uint32_t slot = argU32(ctx, 0);
  if (slot >= kSlotCount) {
    ctx->gpr[3] = kSysutilErrorValue;
    return;
  }
  g_slots[slot] = {};
  logInfo("dal: sysutil: callback slot {} cleared", slot);
  ctx->gpr[3] = kCellOk;
}

// cellSysutilCheckCallback()
void checkCallback(DalPpuContext* ctx, std::uint8_t* base) {
  // Take the whole queue first: what a callback queues runs on the next check, as in RPCS3's
  // pop_all.
  std::deque<SysutilCall> batch;
  batch.swap(g_queue);
  for (const SysutilCall& call : batch) {
    call(ctx, base);
  }
  ctx->gpr[3] = kCellOk;
}

// cellSysutilEnableBgmPlayback()
void enableBgmPlayback(DalPpuContext* ctx, std::uint8_t*) {
  g_bgm_enable_state = kBgmEnable;
  ctx->gpr[3] = kCellOk;
}

// cellSysutilGetBgmPlaybackStatus(CellSysutilBgmPlaybackStatus* status)
void getBgmPlaybackStatus(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t status_addr = argU32(ctx, 0);
  if (status_addr == 0) {
    ctx->gpr[3] = kBgmErrorParam;
    return;
  }
  for (std::uint32_t i = 0; i < kBgmStatusSize; i++) dalWrite8(base, status_addr + i, 0);
  dalWrite8(base, status_addr, kBgmPlayerStop);
  dalWrite8(base, status_addr + 1, g_bgm_enable_state);
  ctx->gpr[3] = kCellOk;
}

constexpr HleEntry kHandlers[] = {
    {kModule, 0x02ff3c1b, "cellSysutilUnregisterCallback", unregisterCallback},
    {kModule, 0x189a74da, "cellSysutilCheckCallback", checkCallback},
    {kModule, 0x220894e3, "cellSysutilEnableBgmPlayback", enableBgmPlayback},
    {kModule, 0x9d98afa0, "cellSysutilRegisterCallback", registerCallback},
    {kModule, 0xa11552f6, "cellSysutilGetBgmPlaybackStatus", getBgmPlaybackStatus},
};

}  // namespace

std::span<const HleEntry> cellSysutilHandlers() { return kHandlers; }

void queueSysutilCall(SysutilCall call) { g_queue.push_back(std::move(call)); }

std::int32_t queueSysutilEvent(std::uint64_t status, std::uint64_t param) {
  if (status == kSysutilDrawingBegin) {
    if (g_draw_started) {
      logError("dal: sysutil: a second DRAWING_BEGIN was queued");
      return static_cast<std::int32_t>(kSysutilErrorBusy);
    }
    g_draw_started = true;
  } else if (status == kSysutilDrawingEnd) {
    if (!g_draw_started) {
      logError("dal: sysutil: DRAWING_END was queued without a DRAWING_BEGIN");
      return -1;
    }
    g_draw_started = false;
  }

  // The registered slots are captured now; a callback registered later does not see this event.
  std::int32_t count = 0;
  for (const Slot slot : g_slots) {
    if (slot.func == 0) continue;
    g_queue.push_back([slot, status, param](DalPpuContext* ctx, std::uint8_t* base) {
      callGuestFunction(ctx, base, slot.func, {status, param, slot.userdata});
    });
    count++;
  }
  return count;
}

void resetCellSysutil() {
  g_slots = {};
  g_queue.clear();
  g_draw_started = false;
  g_bgm_enable_state = kBgmDisable;
}

}  // namespace dal
