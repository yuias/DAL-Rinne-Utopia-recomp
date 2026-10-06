#include "core/fatal.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <crtdbg.h>

namespace dal {
namespace {

std::atomic<CensusPrinterFn> g_census_printer{nullptr};
std::atomic<bool> g_stopping{false};
thread_local const DalPpuContext* t_fatal_context = nullptr;
thread_local bool t_in_stop = false;

// Takes the single right to end the process. A second host thread parks until the first one
// calls _Exit. A thread that faults again while stopping must not wait for itself.
void enterStop() {
  if (t_in_stop) {
    std::_Exit(static_cast<int>(ExitCode::Fatal));
  }
  t_in_stop = true;
  if (g_stopping.exchange(true)) {
    for (;;) {
      Sleep(INFINITE);
    }
  }
}

[[noreturn]] void finishStop(ExitCode code) {
  if (CensusPrinterFn printer = g_census_printer.load()) {
    printer();
  }
  std::fflush(stdout);
  flushLog();
  std::fflush(stderr);
  std::_Exit(static_cast<int>(code));
}

void dumpContext(const DalPpuContext& ctx) {
  logInfo("registers:");
  for (int row = 0; row < 32; row += 4) {
    logInfo("  r{:<2} {:016x}  r{:<2} {:016x}  r{:<2} {:016x}  r{:<2} {:016x}", row, ctx.gpr[row],
            row + 1, ctx.gpr[row + 1], row + 2, ctx.gpr[row + 2], row + 3, ctx.gpr[row + 3]);
  }
  logInfo("  lr {:016x}  ctr {:016x}  cr {:08x}  xer {:08x}", ctx.lr, ctx.ctr, dalMfcr(&ctx),
          static_cast<std::uint32_t>(dalMfxer(&ctx)));

  const std::uint32_t count = ctx.trace_pos < kDalTraceSize ? ctx.trace_pos : kDalTraceSize;
  logInfo("trace (oldest -> newest, {} entries):", count);
  std::string line;
  for (std::uint32_t i = 0; i < count; i++) {
    // Entry i of the logical sequence ending at trace_pos - 1.
    const std::uint32_t slot = (ctx.trace_pos - count + i) & (kDalTraceSize - 1);
    line += std::format(" {:08x}", ctx.trace[slot]);
    if ((i & 7) == 7 || i + 1 == count) {
      logInfo("{}", line);
      line.clear();
    }
  }
}

}  // namespace

std::string_view fatalKindName(FatalKind kind) {
  switch (kind) {
    case FatalKind::UnhandledInstruction:
      return "unhandled instruction";
    case FatalKind::Trap:
      return "trap";
    case FatalKind::GuestFault:
      return "guest fault";
    case FatalKind::UnknownTarget:
      return "unknown target";
    case FatalKind::Internal:
      break;
  }
  return "internal error";
}

void setCensusPrinter(CensusPrinterFn fn) { g_census_printer.store(fn); }

void setFatalContext(const DalPpuContext* ctx) { t_fatal_context = ctx; }

void fatal(FatalKind kind, std::string_view message) {
  enterStop();
  logInfo("dal: stop: fatal {}: {}", fatalKindName(kind), message);
  if (t_fatal_context != nullptr) {
    dumpContext(*t_fatal_context);
  }
  finishStop(ExitCode::Fatal);
}

void stopProcess(ExitCode code, std::string_view line) {
  enterStop();
  logInfo("{}", line);
  finishStop(code);
}

void installProcessSettings() {
  // abort() would otherwise show the debug CRT dialog or hand the process to Windows Error
  // Reporting; with both off it just terminates.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _set_error_mode(_OUT_TO_STDERR);
  // The error-mode flags also cover faults in the loader and unhandled exceptions, which would
  // otherwise raise the "has stopped working" box.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _DEBUG
  for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
  }
#endif
}

}  // namespace dal
