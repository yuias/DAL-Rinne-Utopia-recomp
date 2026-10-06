// Self-checking unit tests for the runtime core library. No generated code is involved.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "dal/ppu_context.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

int g_passed;
int g_failed;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (cond) {                                                              \
      g_passed++;                                                            \
    } else {                                                                 \
      g_failed++;                                                            \
      std::fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
    }                                                                        \
  } while (0)

std::optional<std::uint32_t> g_thread_id;

std::optional<std::uint32_t> fakeThreadId() { return g_thread_id; }

void testLogLevelsAndPrefix() {
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::setGuestThreadIdProvider(&fakeThreadId);

  g_thread_id = std::nullopt;
  dal::logInfo("value {}", 7);
  g_thread_id = 0x01000000u;
  dal::logWarn("w {:#x}", 255u);
  dal::logError("e");

  dal::setLogSink({});
  dal::setGuestThreadIdProvider(nullptr);

  CHECK(lines.size() == 3);
  if (lines.size() == 3) {
    CHECK(lines[0] == "[host] value 7");
    CHECK(lines[1] == "[t 01000000] warn: w 0xff");
    CHECK(lines[2] == "[t 01000000] error: e");
  }
}

void testLogOnce() {
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetLogOnce();

  for (int i = 0; i < 3; i++) {
    dal::logOnce("key-a", "a {}", i);
  }
  dal::logOnce("key-b", "b");
  dal::logOnce(dal::LogLevel::Warn, "key-b", "b again");
  dal::logOnce(dal::LogLevel::Warn, "key-c", "c");

  dal::resetLogOnce();
  dal::logOnce("key-a", "a after reset");

  dal::setLogSink({});

  CHECK(lines.size() == 4);
  if (lines.size() == 4) {
    CHECK(lines[0] == "[host] a 0");
    CHECK(lines[1] == "[host] b");
    CHECK(lines[2] == "[host] warn: c");
    CHECK(lines[3] == "[host] a after reset");
  }
}

constexpr std::uint64_t kGuestSpace = 1ull << 32;

dal::GuestMemory* newMemory() {
  auto* memory = new dal::GuestMemory;
  if (!memory->reserve()) {
    std::fprintf(stderr, "cannot reserve guest memory\n");
    std::exit(1);
  }
  return memory;
}

DWORD queryState(const dal::GuestMemory& memory, std::uint64_t guest_addr) {
  MEMORY_BASIC_INFORMATION info{};
  VirtualQuery(memory.base() + guest_addr, &info, sizeof info);
  return info.State;
}

void testGuestMemoryRoundTrip() {
  std::unique_ptr<dal::GuestMemory> memory(newMemory());
  std::uint8_t* base = memory->base();

  CHECK(base != nullptr);
  CHECK(queryState(*memory, 0x10000) == MEM_RESERVE);
  CHECK(memory->commit(0x10001, 0x10));  // rounds out to the whole page
  CHECK(queryState(*memory, 0x10000) == MEM_COMMIT);
  CHECK(queryState(*memory, 0x11000) == MEM_RESERVE);
  CHECK(dalRead32(base, 0x10100) == 0);  // zero-filled

  dalWrite32(base, 0x10100, 0x11223344u);
  CHECK(base[0x10100] == 0x11 && base[0x10101] == 0x22 && base[0x10102] == 0x33 &&
        base[0x10103] == 0x44);
  CHECK(dalRead32(base, 0x10100) == 0x11223344u);
  CHECK(dalRead16(base, 0x10101) == 0x2233);

  // Commit keeps contents, decommit discards them.
  CHECK(memory->commit(0x10000, 0x1000));
  CHECK(dalRead32(base, 0x10100) == 0x11223344u);
  memory->decommit(0x10000, 0x1000);
  CHECK(queryState(*memory, 0x10000) == MEM_RESERVE);
  CHECK(memory->commit(0x10000, 0x1000));
  CHECK(dalRead32(base, 0x10100) == 0);

  CHECK(memory->containsHost(base));
  CHECK(memory->containsHost(base + kGuestSpace + kDalGuestGuardSize - 1));
  CHECK(!memory->containsHost(base + kGuestSpace + kDalGuestGuardSize));
  CHECK(!memory->containsHost(base - 1));
}

void testTopOfAddressSpace() {
  std::unique_ptr<dal::GuestMemory> memory(newMemory());
  std::uint8_t* base = memory->base();

  CHECK(memory->commit(0xFFFFF000u, 0x1000));
  CHECK(!memory->commit(0xFFFFFFFCu, 8));  // would run past the 32-bit space
  dalWrite64(base, 0xFFFFFFF8u, 0x0102030405060708ull);
  CHECK(dalRead64(base, 0xFFFFFFF8u) == 0x0102030405060708ull);
  dalWrite32(base, 0xFFFFFFFCu, 0xA1B2C3D4u);
  CHECK(dalRead32(base, 0xFFFFFFFCu) == 0xA1B2C3D4u);

  // An 8-byte access at 0xFFFFFFFC spills into the guard tail. The tail belongs to the same
  // reservation and stays inaccessible, so it faults instead of touching foreign memory.
  MEMORY_BASIC_INFORMATION tail{};
  VirtualQuery(base + kGuestSpace, &tail, sizeof tail);
  CHECK(tail.AllocationBase == base);
  CHECK(tail.State == MEM_RESERVE);
  CHECK(tail.RegionSize >= static_cast<SIZE_T>(kDalGuestGuardSize));
}

void testAllocatorFirstFit() {
  std::unique_ptr<dal::GuestMemory> memory(newMemory());
  dal::RegionAllocator alloc(*memory, dal::kMainRegion);

  const std::uint32_t a = alloc.allocate(0x1000, 0);
  const std::uint32_t b = alloc.allocate(0x10001, 0);
  const std::uint32_t c = alloc.allocate(0x10000, 0x80000);
  CHECK(a == 0x10000);
  CHECK(b == 0x20000);  // 0x10001 rounds up to two 64 KiB granules
  CHECK(c == 0x80000);  // alignment leaves 0x40000..0x80000 free
  CHECK(queryState(*memory, a) == MEM_COMMIT);

  // First fit takes the hole before the end of the region.
  const std::uint32_t d = alloc.allocate(0x10000, 0);
  CHECK(d == 0x40000);

  CHECK(alloc.free(b) == 0x20000);
  CHECK(alloc.free(b) == 0);        // already freed
  CHECK(alloc.free(0x10010) == 0);  // not an allocation start
  CHECK(queryState(*memory, b) == MEM_RESERVE);
  CHECK(alloc.allocate(0x20000, 0) == b);  // reuse of the freed hole
  CHECK(dalRead32(memory->base(), b) == 0);

  CHECK(alloc.allocate(0, 0) == 0);
  CHECK(alloc.allocate(0x1000, 0x30000) == 0);  // not a power of two

  // Memory returned by a reuse is zero again.
  dalWrite32(memory->base(), a, 0xFFFFFFFFu);
  CHECK(alloc.free(a) == 0x10000);
  CHECK(alloc.allocate(0x10000, 0) == a);
  CHECK(dalRead32(memory->base(), a) == 0);
}

void testAllocatorMarkUsedAndExhaustion() {
  std::unique_ptr<dal::GuestMemory> memory(newMemory());
  const dal::GuestRegion tiny{0x10000, 0x50000, 0x10000, 0, "tiny"};
  dal::RegionAllocator alloc(*memory, tiny);

  alloc.markUsed(0x20000, 0x10000);
  CHECK(alloc.allocate(0x10000, 0) == 0x10000);
  CHECK(alloc.allocate(0x10000, 0) == 0x30000);
  CHECK(alloc.allocate(0x10000, 0) == 0x40000);
  CHECK(alloc.allocate(0x10000, 0) == 0);  // exhausted
  CHECK(alloc.allocate(0x40000, 0) == 0);  // larger than the region
  CHECK(alloc.allocate(0xFFFFFFFFu, 0) == 0);
  CHECK(alloc.free(0x20000) == 0x10000);  // marked ranges are freeable
  CHECK(alloc.allocate(0x10000, 0) == 0x20000);

  // Marking outside the region or over an existing range is rejected.
  dal::setLogSink([](std::string_view) {});
  alloc.markUsed(0x60000, 0x1000);
  alloc.markUsed(0x30000, 0x1000);
  dal::setLogSink({});
  CHECK(alloc.free(0x60000) == 0);
  CHECK(alloc.free(0x30000) == 0x10000);
}

void testStackGuards() {
  std::unique_ptr<dal::GuestMemory> memory(newMemory());
  dal::RegionAllocator stack(*memory, dal::kStackRegion);

  const std::uint32_t a = stack.allocate(0x10000, 0x1000);
  CHECK(a == 0xD0001000);
  CHECK(queryState(*memory, a - 0x1000) == MEM_RESERVE);      // guard below
  CHECK(queryState(*memory, a) == MEM_COMMIT);
  CHECK(queryState(*memory, a + 0xF000) == MEM_COMMIT);
  CHECK(queryState(*memory, a + 0x10000) == MEM_RESERVE);     // guard above

  const std::uint32_t b = stack.allocate(0x1000, 0x1000);
  CHECK(b == 0xD0013000);  // the upper guard of a, then the lower guard of b
  CHECK(queryState(*memory, b - 0x1000) == MEM_RESERVE);
  CHECK(stack.allocate(0xE0000000u - 0xD0000000u, 0x1000) == 0);
  CHECK(stack.free(a) == 0x10000);
  CHECK(stack.allocate(0x2000, 0x1000) == a);
}

void testProcessSingleton() {
  dal::Process& p = dal::process();
  CHECK(&p == &dal::process());
  CHECK(p.main.region().begin == dal::kMainRegion.begin);
  CHECK(p.stack.region().guard == 0x1000);
  CHECK(p.user_memory_total == 0xD500000);
  CHECK(!p.strict);
}

struct ChildResult {
  DWORD exit_code = 0xFFFFFFFF;
  std::string output;
};

// Runs this executable again with `arg`, capturing stdout and stderr together.
ChildResult runChild(const wchar_t* arg) {
  ChildResult result;
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring command = std::wstring(L"\"") + exe + L"\" " + arg;

  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  CreatePipe(&read_end, &write_end, &sa, 0);
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = INVALID_HANDLE_VALUE;
  si.hStdOutput = write_end;
  si.hStdError = write_end;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(read_end);
    CloseHandle(write_end);
    return result;
  }
  CloseHandle(write_end);

  char buffer[4096];
  DWORD got = 0;
  while (ReadFile(read_end, buffer, sizeof buffer, &got, nullptr) && got != 0) {
    result.output.append(buffer, got);
  }
  WaitForSingleObject(pi.hProcess, 30000);
  GetExitCodeProcess(pi.hProcess, &result.exit_code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(read_end);
  return result;
}

void echoChild(const char* name, const ChildResult& result) {
  std::printf("--- %s: exit code %lu\n", name, static_cast<unsigned long>(result.exit_code));
  std::printf("%s", result.output.c_str());
  std::printf("--- end %s\n", name);
  std::fflush(stdout);
}

bool contains(const ChildResult& result, std::string_view needle) {
  return result.output.find(needle) != std::string::npos;
}

void testFaultChild() {
  const ChildResult r = runChild(L"--fault-child");
  echoChild("fault child", r);
  CHECK(r.exit_code == 3);
  CHECK(contains(r, "dal: stop: fatal guest fault: write to guest address 0x00200000"));
  CHECK(contains(r, "r0  0000000000000000  r1  0000000000000000  r2  0000000000000000  "
                    "r3  00000000deadbeef"));
  CHECK(contains(r, "trace (oldest -> newest, 3 entries):"));
  CHECK(contains(r, " 00010000 00010100 00010200"));
  CHECK(contains(r, "census: test printer ran"));
}

void testOverflowChild() {
  const ChildResult r = runChild(L"--overflow-child");
  echoChild("overflow child", r);
  CHECK(r.exit_code == 3);
  CHECK(contains(r, "dal: stop: fatal guest fault: host stack overflow"));
}

void testStopChild() {
  const ChildResult r = runChild(L"--stop-child");
  echoChild("stop child", r);
  CHECK(r.exit_code == 5);
  CHECK(contains(r, "dal: stop: watchdog 5 s"));
  CHECK(!contains(r, "registers:"));
  CHECK(contains(r, "census: test printer ran"));
}

// Children: each ends the process through the stop path under test.

void childCensus() { std::fprintf(stderr, "census: test printer ran\n"); }

DalPpuContext g_child_ctx;

int faultChild() {
  dal::installProcessSettings();
  dal::installFaultHandler();
  dal::process().memory.reserve();
  dal::setCensusPrinter(&childCensus);
  g_child_ctx.gpr[3] = 0xdeadbeefull;
  g_child_ctx.trace_pos = 3;
  g_child_ctx.trace[0] = 0x00010000;
  g_child_ctx.trace[1] = 0x00010100;
  g_child_ctx.trace[2] = 0x00010200;
  dal::setFatalContext(&g_child_ctx);

  // The page was never committed.
  *reinterpret_cast<volatile std::uint8_t*>(dal::process().memory.base() + 0x00200000) = 1;
  return 99;
}

// The volatile limit keeps the compiler from proving the recursion infinite and removing it.
volatile int g_recursion_limit = 0x7FFFFFFF;

DAL_NOINLINE int recurse(int depth) {
  volatile char pad[512];
  pad[0] = static_cast<char>(depth);
  return depth < g_recursion_limit ? recurse(depth + 1) + pad[0] : 0;
}

int overflowChild() {
  dal::installProcessSettings();
  dal::installFaultHandler();
  dal::process().memory.reserve();
  return recurse(0);
}

int stopChild() {
  dal::installProcessSettings();
  dal::setCensusPrinter(&childCensus);
  dal::stopProcess(dal::ExitCode::Watchdog, "dal: stop: watchdog 5 s");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string_view mode = argv[1];
    if (mode == "--fault-child") return faultChild();
    if (mode == "--overflow-child") return overflowChild();
    if (mode == "--stop-child") return stopChild();
  }

  dal::installProcessSettings();
  testLogLevelsAndPrefix();
  testLogOnce();
  testGuestMemoryRoundTrip();
  testTopOfAddressSpace();
  testAllocatorFirstFit();
  testAllocatorMarkUsedAndExhaustion();
  testStackGuards();
  testProcessSingleton();
  testFaultChild();
  testOverflowChild();
  testStopChild();
  std::printf("runtime core tests: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed != 0 ? 1 : 0;
}
