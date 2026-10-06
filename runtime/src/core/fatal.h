#pragma once

#include <string_view>

#include "dal/ppu_context.h"

namespace dal {

enum class ExitCode : int {
  ProcessExit = 0,
  Usage = 2,
  Fatal = 3,
  LoadError = 4,
  Watchdog = 5,
  Stall = 6,
  NoThreads = 7,
};

enum class FatalKind {
  UnhandledInstruction,
  Trap,
  GuestFault,
  UnknownTarget,
  Internal,
};

std::string_view fatalKindName(FatalKind kind);

// Registered by the census code so this file does not depend on it. Called exactly once, on the
// way out of every stop path. Without a printer nothing is printed.
using CensusPrinterFn = void (*)();
void setCensusPrinter(CensusPrinterFn fn);

// Context of the guest thread running on the calling host thread (nullptr on other threads);
// fatal() dumps its registers and trace ring. The thread layer sets it when a guest thread starts.
void setFatalContext(const DalPpuContext* ctx);

// Logs the stop line, the registers and trace ring of the current guest thread, and the census,
// then ends the process with ExitCode::Fatal. Only the first caller proceeds; others block forever.
[[noreturn]] void fatal(FatalKind kind, std::string_view message);

// Like fatal() without the register dump. `line` is the complete stop line, e.g.
// "dal: stop: watchdog 20 s".
[[noreturn]] void stopProcess(ExitCode code, std::string_view line);

// Keeps failures from raising modal dialogs (CRT abort/assert/report boxes, critical-error and
// fault boxes) so that a headless run always ends with an exit code. Call first thing in main().
void installProcessSettings();

}  // namespace dal
