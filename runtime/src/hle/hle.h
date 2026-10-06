#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

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

// Handlers of the cellSpurs module.
std::span<const HleEntry> cellSpursHandlers();

// Handlers of the cellGame module.
std::span<const HleEntry> cellGameHandlers();

// Forgets the outstanding cellGameDataCheck state. Intended for tests.
void resetCellGame();

// Handlers of the sceNp module.
std::span<const HleEntry> sceNpHandlers();

// Forgets the sceNp init state. Intended for tests.
void resetSceNp();

// Handlers of the sceNpTrophy module.
std::span<const HleEntry> sceNpTrophyHandlers();

// Forgets the trophy init state, contexts and handles. Intended for tests.
void resetSceNpTrophy();

// Handlers of the cellSysutil module that this file implements: the callback pump
// (Register/Unregister/CheckCallback) and the BGM playback status.
std::span<const HleEntry> cellSysutilHandlers();

// Status codes passed to cellSysutil callbacks (CELL_SYSUTIL_*) and the module's error codes.
inline constexpr std::uint64_t kSysutilRequestExitGame = 0x0101;
inline constexpr std::uint64_t kSysutilDrawingBegin = 0x0121;
inline constexpr std::uint64_t kSysutilDrawingEnd = 0x0122;
inline constexpr std::uint64_t kSysutilSystemMenuOpen = 0x0131;
inline constexpr std::uint64_t kSysutilSystemMenuClose = 0x0132;
inline constexpr std::uint64_t kSysutilBgmPlaybackPlay = 0x0141;
inline constexpr std::uint64_t kSysutilBgmPlaybackStop = 0x0142;
inline constexpr std::uint32_t kSysutilErrorValue = 0x8002b102;  // CELL_SYSUTIL_ERROR_VALUE
inline constexpr std::uint32_t kSysutilErrorBusy = 0x8002b105;   // CELL_SYSUTIL_ERROR_BUSY

// A unit of work that runs on the guest thread that calls cellSysutilCheckCallback, with that
// thread's context, and may call guest functions through callGuestFunction.
using SysutilCall = std::function<void(DalPpuContext*, std::uint8_t*)>;

// Queues `call` for the next cellSysutilCheckCallback (RPCS3 sysutil_register_cb). Call under the
// guest lock.
void queueSysutilCall(SysutilCall call);

// Queues func(status, param, userdata) for every callback registered right now, in slot order
// (RPCS3 sysutil_send_system_cmd). Returns how many were queued; DRAWING_BEGIN while one is open
// returns kSysutilErrorBusy and DRAWING_END without one returns -1, queueing nothing. Call under
// the guest lock.
std::int32_t queueSysutilEvent(std::uint64_t status, std::uint64_t param);

// Forgets the registered callbacks, the queue and the BGM state. Intended for tests.
void resetCellSysutil();

// Handlers of the cellMsgDialog imports (cellSysutil module): Open2, Close and the progress bar
// calls.
std::span<const HleEntry> cellMsgDialogHandlers();

// Sets what an auto-answered Yes/No dialog replies: "yes", "ok" (same as yes) or "no". Returns false
// for any other value. OK dialogs always answer OK; dialogs without buttons wait for
// cellMsgDialogClose.
bool setDialogAnswer(std::string_view answer);

// Delay before a dialog with buttons answers itself (default 200 ms).
void setDialogAnswerDelay(std::chrono::milliseconds delay);

// Forgets the open dialog and restores the answer and delay defaults. Intended for tests.
void resetCellMsgDialog();

// Handlers of the sys_fs module (cellFs*).
std::span<const HleEntry> cellFsHandlers();

// Handlers of the cellGcmSys module.
std::span<const HleEntry> cellGcmSysHandlers();

// The body of _cellGcmInitBody with a selectable local memory size (the import always passes the
// console's 0xF900000). Commits the RSX areas, maps the IO area and writes the context, control
// register and configuration. Returns kCellOk or a CellGcmError code. Intended for tests; the
// state is released with resetRsxState.
std::uint32_t gcmInitBody(std::uint8_t* base, std::uint32_t context_pp, std::uint32_t cmd_size,
                          std::uint32_t io_size, std::uint32_t io_address, std::uint32_t local_size);

// Forgets every initialized SPURS object and releases its thread group. Intended for tests.
void resetSpurs();

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
