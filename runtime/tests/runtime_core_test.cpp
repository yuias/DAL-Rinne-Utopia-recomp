// Self-checking unit tests for the runtime core library. No generated code is involved.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/boot.h"
#include "core/census.h"
#include "core/elf_loader.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/host_functions.h"
#include "core/log.h"
#include "core/param_sfo.h"
#include "core/threads.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/sys_ppu_thread.h"
#include "lv2/sys_spu.h"
#include "lv2/syscalls.h"
#include "lv2/vfs.h"
#include "lv2/sys_sync.h"
#include "rsx/fifo_parser.h"
#include "rsx/gcm_interrupts.h"
#include "rsx/rsx_state.h"
#include "rsx/rsx_thread.h"

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

void testIsCommittedAndRangeNames() {
  std::unique_ptr<dal::GuestMemory> memory(newMemory());
  CHECK(!memory->isCommitted(0x10000, 4));
  CHECK(memory->commit(0x10000, 0x2000));
  CHECK(memory->isCommitted(0x10000, 4));
  CHECK(memory->isCommitted(0x11ffc, 4));
  CHECK(memory->isCommitted(0x10ff0, 0x20));  // spans two pages
  CHECK(!memory->isCommitted(0x11ffe, 4));  // runs into the uncommitted page after
  CHECK(!memory->isCommitted(0x12000, 4));
  CHECK(!memory->isCommitted(0xFFFFFFFEu, 4));  // past the 32-bit space
  dal::GuestMemory unreserved;
  CHECK(!unreserved.isCommitted(0x10000, 4));

  CHECK(std::string_view(dal::guestRangeName(0x10000)) == "main");
  CHECK(std::string_view(dal::guestRangeName(0x0FFFFFFF)) == "main");
  CHECK(std::string_view(dal::guestRangeName(0x10000000)) == "rsx-context");
  CHECK(std::string_view(dal::guestRangeName(0x20000000)) == "user1m");
  CHECK(std::string_view(dal::guestRangeName(0x30000000)) == "user64k");
  CHECK(std::string_view(dal::guestRangeName(0xC0000000)) == "rsx-local");
  CHECK(std::string_view(dal::guestRangeName(0xD0000000)) == "stack");
  CHECK(dal::guestRangeName(0) == nullptr);
  CHECK(dal::guestRangeName(0x40000000) == nullptr);
  CHECK(dal::guestRangeName(0xE0000000) == nullptr);
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

// Builds a minimal big-endian ELF64 in memory: two PT_LOAD (the second with bss), PT_TLS and a
// process parameter block inside the first segment.
std::vector<std::uint8_t> syntheticElf(std::uint16_t machine) {
  std::vector<std::uint8_t> elf(0x320, 0);
  auto put = [&](std::size_t at, std::uint64_t value, int size) {
    for (int i = 0; i < size; i++) {
      elf[at + i] = static_cast<std::uint8_t>(value >> (8 * (size - 1 - i)));
    }
  };
  std::memcpy(elf.data(), "\x7f" "ELF", 4);
  elf[4] = 2;
  elf[5] = 2;
  put(18, machine, 2);
  put(24, 0x10080, 8);  // e_entry: the function descriptor
  put(32, 64, 8);
  put(54, 56, 2);
  put(56, 4, 2);

  auto phdr = [&](int index, std::uint32_t type, std::uint32_t flags, std::uint64_t offset,
                  std::uint64_t vaddr, std::uint64_t filesz, std::uint64_t memsz) {
    const std::size_t at = 64 + 56 * index;
    put(at, type, 4);
    put(at + 4, flags, 4);
    put(at + 8, offset, 8);
    put(at + 16, vaddr, 8);
    put(at + 24, vaddr, 8);
    put(at + 32, filesz, 8);
    put(at + 40, memsz, 8);
  };
  phdr(0, 1, 0x400005, 0x200, 0x10000, 0x100, 0x100);
  phdr(1, 1, 0x600006, 0x300, 0x30000, 0x20, 0x1000);
  phdr(2, 7, 4, 0x300, 0x30000, 4, 0x10);
  phdr(3, 0x60000001, 0, 0x2c0, 0x100c0, 0x20, 0x20);

  put(0x280, 0x10010, 4);  // descriptor at 0x10080: code address, then TOC
  put(0x284, 0x18000, 4);
  put(0x2c0, 0x20, 4);  // process parameter block
  put(0x2c4, 0x13bcc5f6, 4);
  put(0x2cc, 0x00430001, 4);
  put(0x2d0, 1001, 4);
  put(0x2d4, 0x8000, 4);
  put(0x2d8, 0x100000, 4);
  put(0x2dc, 7, 4);
  put(0x300, 0xcafef00d, 4);  // first word of the data segment
  return elf;
}

std::filesystem::path writeTempFile(const char* name, const std::vector<std::uint8_t>& bytes) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (std::string("dal_runtime_test_") + std::to_string(GetCurrentProcessId()) + name);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return path;
}

void testElfLoader() {
  dal::setLogSink([](std::string_view) {});
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());

  const auto bad_path = writeTempFile("_bad.elf", syntheticElf(62));
  const auto bad = dal::loadExecutable(bad_path);
  std::filesystem::remove(bad_path);
  CHECK(!bad.has_value());
  if (!bad) {
    CHECK(bad.error().find("e_machine") != std::string::npos);
  }

  const auto path = writeTempFile("_ok.elf", syntheticElf(21));
  const auto loaded = dal::loadExecutable(path);
  std::filesystem::remove(path);
  dal::setLogSink({});

  CHECK(loaded.has_value());
  if (!loaded) {
    std::fprintf(stderr, "load error: %s\n", loaded.error().c_str());
    return;
  }
  const dal::LoadedExecutable& exe = *loaded;
  CHECK(exe.entry_opd == 0x10080);
  CHECK(exe.entry_func == 0x10010);
  CHECK(exe.entry_toc == 0x18000);
  CHECK(exe.tls_vaddr == 0x30000 && exe.tls_filesz == 4 && exe.tls_memsz == 0x10);
  CHECK(exe.sdk_version == 0x00430001);
  CHECK(exe.primary_prio == 1001);
  CHECK(exe.primary_stacksize == 0x8000);
  CHECK(exe.malloc_pagesize == 0x100000);
  CHECK(exe.ppc_seg == 7);
  CHECK(exe.image_end == 0x40000);
  CHECK(exe.sha256.size() == 64);

  const std::uint8_t* base = p.memory.base();
  CHECK(dalRead32(base, 0x30000) == 0xcafef00d);
  CHECK(dalRead32(base, 0x30020) == 0 && dalRead32(base, 0x30ffc) == 0);  // bss
  // The image range is taken, so the first free block starts after it.
  CHECK(p.main.allocate(0x10000, 0x10000) == 0x40000);
}

DAL_PPU_FUNC(testFuncA) {
  (void)base;
  ctx->gpr[3] = 1;
}

DAL_PPU_FUNC(testFuncB) {
  (void)base;
  ctx->gpr[3] = 2;
}

std::size_t countContaining(const std::vector<std::string>& lines, std::string_view needle) {
  std::size_t n = 0;
  for (const std::string& line : lines) {
    n += line.find(needle) != std::string::npos ? 1 : 0;
  }
  return n;
}

void testDispatch() {
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();

  const DalFuncMapping mappings[] = {
      {0x10200, testFuncA}, {0x10210, testFuncB}, {0x10400, testFuncA}};
  dalRegisterFuncs(mappings, 3);
  CHECK(countContaining(lines, "dal: dispatch: 3 entries, span 0x00010200..0x00010404") == 1);

  CHECK(dalLookup(0x10200) == testFuncA);
  CHECK(dalLookup(0x10210) == testFuncB);
  CHECK(dalLookup(0x10400) == testFuncA);
  CHECK(dalLookup(0x10204) == nullptr);  // inside the span, no function
  CHECK(dalLookup(0x10201) == nullptr);  // misaligned
  CHECK(dalLookup(0x10211) == nullptr);
  CHECK(dalLookup(0x101fc) == nullptr);  // below the span
  CHECK(dalLookup(0x10404) == nullptr);  // above the span
  CHECK(dalLookup(0) == nullptr);
  CHECK(dalLookup(0xFFFFFFFCu) == nullptr);

  auto ctx = std::make_unique<DalPpuContext>();
  ctx->jump_target = 0x10210;
  dalDispatch(ctx.get(), nullptr);
  CHECK(ctx->gpr[3] == 2);
  ctx->jump_target = 0x10200;
  dalDispatch(ctx.get(), nullptr);
  CHECK(ctx->gpr[3] == 1);

  // Not strict: the unknown transfer returns, leaves the registers alone, and logs once per pair.
  ctx->gpr[3] = 77;
  ctx->trace_pos = 2;
  ctx->trace[0] = 0x10200;
  ctx->trace[1] = 0x10210;
  lines.clear();
  for (int i = 0; i < 3; i++) {
    ctx->jump_target = 0x10204;
    ctx->jump_site = 0x10300;
    dalDispatch(ctx.get(), nullptr);
  }
  CHECK(ctx->gpr[3] == 77);
  CHECK(countContaining(lines, "unknown target 0x00010204 from 0x00010300: inside the code span") ==
        1);
  CHECK(countContaining(lines, "trace (oldest -> newest): 00010200 00010210") == 1);

  ctx->jump_target = 0x20000;
  dalDispatch(ctx.get(), nullptr);
  CHECK(countContaining(lines, "unknown target 0x00020000 from 0x00010300: outside the code span") ==
        1);

  const std::string census = dal::formatCensus();
  CHECK(census.find("census: unknown targets 2 distinct\n  0x00010204 from 0x00010300: 3\n"
                    "  0x00020000 from 0x00010300: 1") != std::string::npos);

  dalRegisterFuncs(nullptr, 0);
  CHECK(dalLookup(0x10200) == nullptr);
  dal::setLogSink({});
  dal::resetCensus();
}

std::vector<dal::ThreadCensusEntry> fakeThreads() {
  using State = dal::ThreadCensusEntry::State;
  return {{0x01000000, "main_thread", State::Blocked, "sys_event_queue_receive"},
          {0x01000001, "worker", State::Exited, ""},
          {0x01000002, "idle", State::Running, ""}};
}

void testCensus() {
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();

  dal::censusSyscall(141, "sys_timer_usleep", true);
  dal::censusSyscall(988, nullptr, false);
  for (int i = 0; i < 119; i++) dal::censusSyscall(141, "sys_timer_usleep", true);
  dal::censusSyscall(988, nullptr, false);
  dal::censusSyscall(988, nullptr, false);
  dal::censusHle("sysPrxForUser", "sys_lwmutex_lock", true);
  dal::censusHle("cellSysutil", "cellSysutilRegisterCallback", false);
  dal::censusHle("sysPrxForUser", "sys_lwmutex_lock", true);
  dal::setThreadCensusProvider(&fakeThreads);

  CHECK(lines.size() == 4);
  CHECK(countContaining(lines, "dal: first sc 141 sys_timer_usleep") == 1);
  CHECK(countContaining(lines, "dal: first sc 988 ?") == 1);
  CHECK(countContaining(lines, "dal: first hle sysPrxForUser::sys_lwmutex_lock") == 1);
  CHECK(countContaining(lines, "dal: first hle cellSysutil::cellSysutilRegisterCallback") == 1);

  const std::string expected =
      "census: syscalls 123 calls, 2 distinct, 1 default-handled\n"
      "  sc 141 sys_timer_usleep: 120 [impl]\n"
      "  sc 988 ?: 3 [default]\n"
      "census: hle 3 calls, 2 distinct, 1 default-handled\n"
      "  sysPrxForUser::sys_lwmutex_lock: 2 [impl]\n"
      "  cellSysutil::cellSysutilRegisterCallback: 1 [default]\n"
      "census: unknown targets 0 distinct\n"
      "census: threads 3 created, 2 live\n"
      "  0x01000000 main_thread: blocked in sys_event_queue_receive\n"
      "  0x01000001 worker: exited\n"
      "  0x01000002 idle: running";
  const std::string actual = dal::formatCensus();
  CHECK(actual == expected);
  if (actual != expected) std::fprintf(stderr, "census was:\n%s\n", actual.c_str());

  lines.clear();
  dal::printCensus();
  CHECK(lines.size() == 11);
  CHECK(!lines.empty() && lines[0].find("census: syscalls 123 calls") != std::string::npos);

  dal::setLogSink({});
  dal::resetCensus();
}

void testSyscalls() {
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();

  const auto table = dal::syscallTable();
  CHECK(table.size() == 61);  // the 53 census numbers plus the implemented 22, 41, 43 and 103
  bool sorted = true;
  for (std::size_t i = 1; i < table.size(); i++) sorted = sorted && table[i - 1].number < table[i].number;
  CHECK(sorted);
  const dal::SyscallEntry* usleep_entry = dal::findSyscall(141);
  CHECK(usleep_entry != nullptr && std::string_view(usleep_entry->name) == "sys_timer_usleep");
  CHECK(usleep_entry != nullptr && usleep_entry->handler != nullptr);
  const dal::SyscallEntry* unnamed = dal::findSyscall(988);
  CHECK(unnamed != nullptr && unnamed->name == nullptr);
  CHECK(dal::findSyscall(999) == nullptr);

  auto ctx = std::make_unique<DalPpuContext>();
  ctx->gpr[11] = 181;  // sys_spu_thread_write_ls has no handler
  ctx->gpr[3] = 0x1234;
  dalSyscall(ctx.get(), nullptr, 0x10000);
  CHECK(ctx->gpr[3] == dal::kCellOk);
  dalSyscall(ctx.get(), nullptr, 0x10000);
  ctx->gpr[11] = 999;
  ctx->gpr[3] = 0x1234;
  dalSyscall(ctx.get(), nullptr, 0x10004);
  CHECK(ctx->gpr[3] == dal::kCellOk);

  CHECK(countContaining(lines, "dal: first sc 181 sys_spu_thread_write_ls") == 1);
  CHECK(countContaining(lines, "dal: first sc 999 ?") == 1);
  CHECK(countContaining(lines, "dal: default sc 181 sys_spu_thread_write_ls(0x1234") == 1);
  CHECK(countContaining(lines, "dal: default sc 999 ?(0x1234") == 1);

  const std::string census = dal::formatCensus();
  CHECK(census.find("census: syscalls 3 calls, 2 distinct, 2 default-handled") != std::string::npos);
  CHECK(census.find("  sc 181 sys_spu_thread_write_ls: 2 [default]") != std::string::npos);
  CHECK(census.find("  sc 999 ?: 1 [default]") != std::string::npos);

  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

DAL_PPU_FUNC(opdTarget) {
  (void)base;
  ctx->gpr[3] = ctx->gpr[2] + 1;  // proves r2 holds the descriptor's TOC
  ctx->gpr[4] = ctx->lr;
  ctx->gpr[2] = 0;
  ctx->lr = 0x7777;
}

void testHle() {
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();

  const DalImportInfo imports[] = {
      {0x00800000, 0xdeadbeef, "cellTestModule", "cellTestFirst"},
      {0x00800010, 0xcafef00d, "cellTestModule", "cellTestSecond"},
  };
  dal::installImports(imports, 2);
  CHECK(countContaining(lines, "dal: hle: 2 imports, 0 with handlers") == 1);

  auto ctx = std::make_unique<DalPpuContext>();
  for (int i = 0; i < 10; i++) {
    ctx->gpr[3] = 0x55;
    ctx->gpr[4] = 0x66;
    dalHleCall(ctx.get(), nullptr, 0);
    CHECK(ctx->gpr[3] == 0);
  }
  CHECK(countContaining(lines, "dal: default hle cellTestModule::cellTestFirst(0x55, 0x66,") == 8);
  CHECK(countContaining(lines, "dal: default hle cellTestModule::cellTestFirst(") == 8);
  CHECK(countContaining(lines, "dal: first hle cellTestModule::cellTestFirst") == 1);

  dal::setQuietHle(true);
  ctx->gpr[3] = 0x55;
  dalHleCall(ctx.get(), nullptr, 1);
  dal::setQuietHle(false);
  CHECK(ctx->gpr[3] == 0);
  CHECK(countContaining(lines, "cellTestSecond(") == 0);
  CHECK(countContaining(lines, "dal: first hle cellTestModule::cellTestSecond") == 1);

  const std::string census = dal::formatCensus();
  CHECK(census.find("census: hle 11 calls, 2 distinct, 2 default-handled") != std::string::npos);
  CHECK(census.find("  cellTestModule::cellTestFirst: 10 [default]") != std::string::npos);

  // callGuestOpd: the descriptor holds the entry address and the TOC.
  std::vector<std::uint8_t> memory(0x20000);
  std::uint8_t* base = memory.data();
  const DalFuncMapping mappings[] = {{0x10200, opdTarget}};
  dalRegisterFuncs(mappings, 1);
  dalWrite32(base, 0x18000, 0x10200);
  dalWrite32(base, 0x18004, 0xd0000);
  ctx->gpr[2] = 0x1111;
  ctx->lr = 0x2222;
  dal::callGuestOpd(ctx.get(), base, 0x18000);
  CHECK(ctx->gpr[3] == 0xd0001);
  CHECK(ctx->gpr[4] == 0);
  CHECK(ctx->gpr[2] == 0x1111);
  CHECK(ctx->lr == 0x2222);

  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

void testHleOutPointerHints() {
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  CHECK(dal::process().memory.reserve());

  // 0x500000 and 0x501000 are committed data, 0x700000 is not, 0x10200 is committed code.
  CHECK(dal::process().memory.commit(0x500000, 0x2000));
  CHECK(dal::process().memory.commit(0x10000, 0x1000));
  const DalFuncMapping mappings[] = {{0x10200, opdTarget}};
  dalRegisterFuncs(mappings, 1);

  const DalImportInfo imports[] = {
      {0x00800000, 0x11111111, "cellHintModule", "cellHintFirst"},
      {0x00800010, 0x22222222, "cellHintModule", "cellHintSecond"},
      {0x00800020, 0x33333333, "cellHintModule", "cellHintThird"},
  };
  dal::installImports(imports, 3);

  auto ctx = std::make_unique<DalPpuContext>();
  ctx->gpr[3] = 0;                 // null
  ctx->gpr[4] = 0x500000;          // candidate
  ctx->gpr[5] = 0x100000000ull | 0x500000;  // upper bits set
  ctx->gpr[6] = 0x700000;          // uncommitted
  ctx->gpr[7] = 0x10200;           // code address
  ctx->gpr[8] = 0x501000;          // candidate
  ctx->gpr[9] = 0x20;              // small integer, not committed
  dal::setQuietHle(true);          // the hint is printed anyway
  dalHleCall(ctx.get(), nullptr, 0);
  dalHleCall(ctx.get(), nullptr, 0);  // only the first default call scans
  dal::setQuietHle(false);
  CHECK(ctx->gpr[3] == dal::kCellOk);
  CHECK(countContaining(lines, "possible out-pointer") == 2);
  CHECK(countContaining(lines,
                        "dal: default hle cellHintModule::cellHintFirst: possible out-pointer "
                        "r4=0x00500000 (main)") == 1);
  CHECK(countContaining(lines, "r8=0x00501000 (main)") == 1);

  ctx->gpr[3] = 0x500000;
  for (int reg = 4; reg <= 10; reg++) ctx->gpr[reg] = 0;
  dalHleCall(ctx.get(), nullptr, 1);
  dalHleCall(ctx.get(), nullptr, 2);  // no candidates: no note
  CHECK(countContaining(lines, "cellHintSecond: possible out-pointer r3=0x00500000 (main)") == 1);

  const std::string census = dal::formatCensus();
  CHECK(census.find("  cellHintModule::cellHintFirst: 2 [default] ptr r4,r8\n") != std::string::npos);
  CHECK(census.find("  cellHintModule::cellHintSecond: 1 [default] ptr r3\n") != std::string::npos);
  CHECK(census.find("  cellHintModule::cellHintThird: 1 [default]\n") != std::string::npos);

  dal::process().memory.decommit(0x500000, 0x2000);
  dal::process().memory.decommit(0x10000, 0x1000);
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

DAL_PPU_FUNC(hostFunctionA) {
  (void)base;
  ctx->gpr[3] = ctx->gpr[3] + 100;
}

DAL_PPU_FUNC(hostFunctionB) {
  (void)base;
  ctx->gpr[3] = 0xB0B;
}

struct CallSnapshot {
  std::uint64_t r1 = 0, r2 = 0, lr = 0, back_chain = 0;
};
CallSnapshot g_call_snapshot;

DAL_PPU_FUNC(sumTarget) {
  g_call_snapshot = {ctx->gpr[1], ctx->gpr[2], ctx->lr,
                     dalRead64(base, static_cast<std::uint32_t>(ctx->gpr[1]))};
  ctx->gpr[3] = ctx->gpr[3] + ctx->gpr[4] + ctx->gpr[5];
  ctx->gpr[1] = 0;  // callee-side clobbers must not leak to the caller
  ctx->gpr[2] = 0;
  ctx->lr = 0x7777;
}

void testHostFunctionsAndCallGuestFunction() {
  dal::resetHostFunctions();
  CHECK(dal::process().memory.reserve());
  std::uint8_t* base = dal::process().memory.base();
  CHECK(dal::process().memory.commit(0x500000, 0x2000));
  CHECK(dal::process().memory.commit(0x10000, 0x10000));
  const DalFuncMapping mappings[] = {{0x10200, sumTarget}};
  dalRegisterFuncs(mappings, 1);

  CHECK(dalLookup(dal::kHostFunctionBase) == nullptr);  // nothing registered yet
  const std::uint32_t opd_a = dal::registerHostFunction("hostFunctionA", hostFunctionA);
  const std::uint32_t opd_b = dal::registerHostFunction("hostFunctionB", hostFunctionB);
  CHECK(opd_a != 0 && opd_b == opd_a + 8);
  CHECK(dal::process().memory.isCommitted(opd_a, 16));
  CHECK(dalRead32(base, opd_a) == dal::kHostFunctionBase);
  CHECK(dalRead32(base, opd_a + 4) == 0);
  CHECK(dalRead32(base, opd_b) == dal::kHostFunctionBase + 4);

  CHECK(dalLookup(dal::kHostFunctionBase) == hostFunctionA);
  CHECK(dalLookup(dal::kHostFunctionBase + 4) == hostFunctionB);
  CHECK(dalLookup(dal::kHostFunctionBase + 8) == nullptr);  // not registered
  CHECK(dalLookup(dal::kHostFunctionBase + 2) == nullptr);  // misaligned
  CHECK(dalLookup(dal::kHostFunctionBase - 4) == nullptr);
  CHECK(dalLookup(0xFFFFFFFCu) == nullptr);
  CHECK(dalLookup(0x10200) == sumTarget);  // the code table still resolves

  // Guest code reaches a host function through the OPD's entry word.
  auto ctx = std::make_unique<DalPpuContext>();
  ctx->gpr[3] = 5;
  ctx->jump_target = dalRead32(base, opd_a);
  dalDispatch(ctx.get(), base);
  CHECK(ctx->gpr[3] == 105);
  ctx->jump_target = dalRead32(base, opd_b);
  dalDispatch(ctx.get(), base);
  CHECK(ctx->gpr[3] == 0xB0B);

  // callGuestFunction: arguments in r3..r5, a lowered and aligned r1 with a back chain, and
  // r1/r2/lr restored afterwards.
  dalWrite32(base, 0x18000, 0x10200);
  dalWrite32(base, 0x18004, 0xd0000);
  ctx->gpr[1] = 0x500808;  // deliberately not 16-aligned
  ctx->gpr[2] = 0x1111;
  ctx->lr = 0x2222;
  const std::uint64_t result = dal::callGuestFunction(ctx.get(), base, 0x18000, {10, 20, 30});
  CHECK(result == 60);
  CHECK(g_call_snapshot.r1 == 0x500800 - 0x70);
  CHECK(g_call_snapshot.r2 == 0xd0000);
  CHECK(g_call_snapshot.lr == 0);
  CHECK(g_call_snapshot.back_chain == 0x500808);
  CHECK(ctx->gpr[1] == 0x500808 && ctx->gpr[2] == 0x1111 && ctx->lr == 0x2222);

  CHECK(dal::callGuestFunction(ctx.get(), base, opd_a, {7}) == 107);  // host function OPD
  CHECK(ctx->gpr[1] == 0x500808);

  CHECK(dal::argU32(ctx.get(), 0) == 107);
  ctx->gpr[5] = 0x1ffffffffull;
  CHECK(dal::argU32(ctx.get(), 2) == 0xffffffffu);

  std::memcpy(base + 0x500100, "hello\0world", 12);
  CHECK(dal::readGuestString(base, 0x500100, 64) == "hello");
  CHECK(dal::readGuestString(base, 0x500100, 3) == "hel");
  CHECK(dal::readGuestString(base, 0x500100, 0).empty());

  dalRegisterFuncs(nullptr, 0);
  dal::resetHostFunctions();
  dal::process().memory.decommit(0x500000, 0x2000);
  dal::process().memory.decommit(0x10000, 0x10000);
}

// Issues a syscall through the dispatcher and returns r3.
std::uint64_t syscall(std::uint32_t number, std::uint64_t r3 = 0, std::uint64_t r4 = 0,
                      std::uint64_t r5 = 0, std::uint64_t r6 = 0) {
  auto ctx = std::make_unique<DalPpuContext>();
  ctx->gpr[11] = number;
  ctx->gpr[3] = r3;
  ctx->gpr[4] = r4;
  ctx->gpr[5] = r5;
  ctx->gpr[6] = r6;
  dalSyscall(ctx.get(), dal::process().memory.base(), 0x10000);
  return ctx->gpr[3];
}

// lv2 errors reach the guest as sign-extended s32 values.
constexpr std::uint64_t err(std::uint32_t code) {
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(code)));
}

void testStartupSyscalls() {
  dal::setLogSink([](std::string_view) {});
  dal::resetCensus();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);

  // User memory size bookkeeping.
  const std::uint32_t used_before = p.user_memory_used;
  CHECK(syscall(352, scratch) == 0);
  CHECK(dalRead32(base, scratch) == p.user_memory_total);
  CHECK(dalRead32(base, scratch + 4) == p.user_memory_total - used_before);

  // Allocation: 1 MiB pages by default and for 0x400, 64 KiB pages for 0x200.
  CHECK(syscall(348, 0x100000, 0x400, scratch) == 0);
  const std::uint32_t a = dalRead32(base, scratch);
  CHECK(a >= dal::kUser1mRegion.begin && a < dal::kUser1mRegion.end && a % 0x100000 == 0);
  CHECK(p.user_memory_used == used_before + 0x100000);
  CHECK(syscall(352, scratch) == 0);
  CHECK(dalRead32(base, scratch + 4) == p.user_memory_total - used_before - 0x100000);
  CHECK(syscall(348, 0x200000, 0, scratch) == 0);
  const std::uint32_t b = dalRead32(base, scratch);
  CHECK(b >= dal::kUser1mRegion.begin && b != a);
  CHECK(syscall(348, 0x30000, 0x200, scratch) == 0);
  const std::uint32_t c = dalRead32(base, scratch);
  CHECK(c >= dal::kUser64kRegion.begin && c < dal::kUser64kRegion.end && c % 0x10000 == 0);
  dalWrite32(base, c, 0xdeadbeef);  // committed and writable

  // Error cases leave the bookkeeping alone.
  const std::uint32_t used = p.user_memory_used;
  CHECK(syscall(348, 0, 0x400, scratch) == err(dal::kCellEalign));
  CHECK(syscall(348, 0x100000, 0x123, scratch) == err(dal::kCellEinval));
  CHECK(syscall(348, 0x10000, 0x400, scratch) == err(dal::kCellEalign));
  CHECK(syscall(348, 0x180000, 0, scratch) == err(dal::kCellEalign));
  CHECK(syscall(348, 0xF000000, 0x400, scratch) == err(dal::kCellEnomem));  // above the total
  CHECK(syscall(348, 0x100000, 0x400, 0) == err(dal::kCellEfault));
  CHECK(p.user_memory_used == used);

  // Free: only allocation starts are accepted, once.
  CHECK(syscall(349, a + 0x10000) == err(dal::kCellEinval));
  CHECK(syscall(349, 0x1234) == err(dal::kCellEinval));
  CHECK(syscall(349, a) == 0);
  CHECK(syscall(349, a) == err(dal::kCellEinval));
  CHECK(syscall(349, b) == 0);
  CHECK(syscall(349, c) == 0);
  CHECK(p.user_memory_used == used_before);
  CHECK(syscall(348, 0x100000, 0x400, scratch) == 0 && dalRead32(base, scratch) == a);  // reuse
  CHECK(syscall(349, a) == 0);

  // tty: writes to channels outside 0..15 are dropped but still reported; reads fail.
  CHECK(syscall(403, 16, scratch + 0x100, 5, scratch) == 0);
  CHECK(dalRead32(base, scratch) == 5);
  CHECK(syscall(403, 0, scratch + 0x100, 0, scratch) == 0);
  CHECK(dalRead32(base, scratch) == 0);
  CHECK(syscall(403, 0, scratch + 0x100, 5, 0) == err(dal::kCellEfault));
  CHECK(syscall(402, 0, scratch + 0x100, 4, scratch) == err(dal::kCellEio));

  // Time base and sleeps (the sleeps wait on the guest lock, which the caller holds).
  CHECK(syscall(147) == dal::kTimebaseFrequency);
  {
    std::lock_guard<dal::GuestLock> guard(dal::guestLock());
    const auto start = std::chrono::steady_clock::now();
    CHECK(syscall(141, 20000) == 0);
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(15));
    CHECK(syscall(141, 0) == 0);  // yield
    CHECK(syscall(142, 0) == 0);
  }

  p.main.free(scratch);
  dal::setLogSink({});
  dal::resetCensus();
}

void testFileSystem() {
  namespace stdfs = std::filesystem;
  dal::setLogSink([](std::string_view) {});
  dal::resetLogOnce();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t path_a = scratch;
  const std::uint32_t buf = scratch + 0x2000;  // data buffer
  const std::uint32_t out = scratch + 0x3000;  // fd / nread / pos
  const std::uint32_t sb = scratch + 0x3100;   // CellFsStat
  const std::uint32_t ent = scratch + 0x3200;  // CellFsDirent
  const std::uint32_t big = scratch + 0x4000;

  auto put = [&](std::uint32_t addr, std::string_view text) {
    std::memcpy(base + addr, text.data(), text.size());
    base[addr + text.size()] = 0;
  };
  auto open = [&](std::string_view path, std::uint64_t flags) {
    put(path_a, path);
    return syscall(801, path_a, flags, out);
  };
  auto fdOut = [&] { return dalRead32(base, out); };
  auto hostFile = [](const stdfs::path& path, std::string_view data) {
    stdfs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << data;
  };
  auto hostRead = [](const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };

  const stdfs::path root =
      stdfs::temp_directory_path() / ("dal_vfs_test_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  stdfs::remove_all(root, ec);
  const stdfs::path game = root / "game";
  hostFile(game / "b.txt", "0123456789");
  hostFile(game / "PS3_GAME" / "USRDIR" / "a.bin", "usrdir");
  stdfs::create_directories(game / "sub");

  // Resolution rules.
  dal::configureVfs(game, std::nullopt);
  bool read_only = false;
  const auto resolved = dal::resolveGuestPath("/dev_bdvd/sub/../b.txt", read_only);
  CHECK(resolved && read_only && stdfs::equivalent(*resolved, game / "b.txt"));
  CHECK(!dal::resolveGuestPath("/dev_bdvd/../b.txt", read_only));
  CHECK(!dal::resolveGuestPath("/dev_bdvd/sub/../../x", read_only));
  CHECK(!dal::resolveGuestPath("/dev_bdvd/a:b", read_only));
  CHECK(!dal::resolveGuestPath("/dev_bdvd/NUL.txt", read_only));
  CHECK(!dal::resolveGuestPath("/dev_hdd0/x", read_only));  // no hdd directory configured
  CHECK(!dal::resolveGuestPath("/dev_usb000/x", read_only));
  CHECK(!dal::resolveGuestPath("relative", read_only));

  // open/read/lseek/close.
  CHECK(open("/dev_bdvd/b.txt", 0) == 0);
  const std::uint32_t fd = fdOut();
  CHECK(fd == 3);
  CHECK(syscall(802, fd, buf, 4, out) == 0 && dalRead64(base, out) == 4);
  CHECK(std::memcmp(base + buf, "0123", 4) == 0);
  CHECK(syscall(818, fd, 2, 0, out) == 0 && dalRead64(base, out) == 2);
  CHECK(syscall(802, fd, buf, 3, out) == 0 && dalRead64(base, out) == 3);
  CHECK(std::memcmp(base + buf, "234", 3) == 0);
  CHECK(syscall(818, fd, static_cast<std::uint64_t>(-2), 2, out) == 0 && dalRead64(base, out) == 8);
  CHECK(syscall(802, fd, buf, 100, out) == 0 && dalRead64(base, out) == 2);  // short read at the end
  CHECK(syscall(802, fd, buf, 100, out) == 0 && dalRead64(base, out) == 0);
  CHECK(syscall(818, fd, 0, 3, out) == err(dal::kCellEinval));
  CHECK(syscall(818, fd, static_cast<std::uint64_t>(-100), 0, out) == err(dal::kCellEinval));
  CHECK(syscall(802, fd, 0, 1, out) == err(dal::kCellEfault));
  CHECK(syscall(802, fd, buf, 1, 0) == err(dal::kCellEfault));
  CHECK(syscall(803, fd, buf, 1, out) == err(dal::kCellEbadf));  // opened read-only
  // fstat reports a read-only regular file on the disc.
  CHECK(syscall(809, fd, sb) == 0);
  CHECK(dalRead32(base, sb) == 0100444 && dalRead32(base, sb + 4) == 0xFFFFFFFFu);
  CHECK(dalRead64(base, sb + 36) == 10 && dalRead64(base, sb + 44) == 2048);
  CHECK(syscall(804, fd) == 0);
  CHECK(syscall(804, fd) == err(dal::kCellEbadf));
  CHECK(syscall(802, fd, buf, 1, out) == err(dal::kCellEbadf) && dalRead64(base, out) == 0);

  // Read-only enforcement on the disc.
  CHECK(open("/dev_bdvd/b.txt", 1) == err(dal::kCellEperm));
  CHECK(open("/dev_bdvd/b.txt", 02) == err(dal::kCellEperm));
  CHECK(open("/dev_bdvd/new.bin", 0101) == err(dal::kCellEperm));
  CHECK(open("/dev_bdvd/new.bin", 0) == err(dal::kCellEnoent));
  CHECK(open("/dev_bdvd/b.txt", 03) == err(dal::kCellEinval));
  put(path_a, "/dev_bdvd/x");
  CHECK(syscall(811, path_a, 0777) == err(dal::kCellErofs));
  put(path_a, "/dev_bdvd/b.txt");
  CHECK(syscall(814, path_a) == err(dal::kCellErofs));
  CHECK(stdfs::exists(game / "b.txt"));

  // Path errors.
  CHECK(open("/dev_bdvd/../b.txt", 0) == err(dal::kCellEnoent));
  CHECK(open("b.txt", 0) == err(dal::kCellEnoent));
  CHECK(open("/dev_usb000/b.txt", 0) == err(dal::kCellEnotmounted));
  CHECK(open("/dev_bdvd", 0) == err(dal::kCellEisdir));
  CHECK(open("/", 0) == err(dal::kCellEisdir));
  CHECK(open("/dev_bdvd/b.txt/x", 0) == err(dal::kCellEnotdir));
  CHECK(syscall(801, 0, 0, out) == err(dal::kCellEfault));
  CHECK(!p.memory.isCommitted(0x0D000000, 1));
  CHECK(syscall(801, 0x0D000000, 0, out) == err(dal::kCellEfault));
  std::memset(base + big, 'a', 1100);
  base[big] = '/';
  base[big + 1100] = 0;
  CHECK(syscall(801, big, 0, out) == err(dal::kCellEnametoolong));
  CHECK(open("/dev_hdd0/x", 0) == err(dal::kCellEnoent));  // not configured
  CHECK(open("/app_home/a.bin", 0) == 0);
  const std::uint32_t app_fd = fdOut();
  CHECK(syscall(802, app_fd, buf, 6, out) == 0 && std::memcmp(base + buf, "usrdir", 6) == 0);
  CHECK(syscall(804, app_fd) == 0);

  // stat.
  put(path_a, "/dev_bdvd/b.txt");
  CHECK(syscall(808, path_a, sb) == 0 && dalRead32(base, sb) == 0100444);
  CHECK(dalRead64(base, sb + 36) == 10);
  put(path_a, "/dev_bdvd/sub");
  CHECK(syscall(808, path_a, sb) == 0 && dalRead32(base, sb) == 040555);
  CHECK(dalRead64(base, sb + 36) == 2048);  // a directory reports its block size
  put(path_a, "/dev_bdvd/b.txt/");
  CHECK(syscall(808, path_a, sb) == err(dal::kCellEnotdir));
  put(path_a, "/dev_bdvd/missing");
  CHECK(syscall(808, path_a, sb) == err(dal::kCellEnoent));
  put(path_a, "/");
  CHECK(syscall(808, path_a, sb) == 0 && dalRead32(base, sb) == 040711);
  put(path_a, "/dev_usb000");
  CHECK(syscall(808, path_a, sb) == err(dal::kCellEnotmounted));

  // opendir/readdir: ".", "..", then names in byte order; at the end nread is 0 and the entry is
  // zeroed.
  put(path_a, "/dev_bdvd");
  CHECK(syscall(805, path_a, out) == 0);
  const std::uint32_t dir = fdOut();
  std::vector<std::string> names;
  std::vector<int> types;
  for (int i = 0; i < 10; i++) {
    std::memset(base + ent, 0xEE, 258);
    CHECK(syscall(806, dir, ent, out + 8) == 0);
    if (dalRead64(base, out + 8) == 0) {
      break;
    }
    CHECK(dalRead64(base, out + 8) == 258);
    names.emplace_back(reinterpret_cast<const char*>(base + ent + 2), base[ent + 1]);
    types.push_back(base[ent]);
  }
  CHECK((names == std::vector<std::string>{".", "..", "PS3_GAME", "b.txt", "sub"}));
  CHECK((types == std::vector<int>{1, 1, 1, 2, 1}));
  bool zeroed = true;
  for (int i = 0; i < 258; i++) {
    zeroed = zeroed && base[ent + i] == 0;
  }
  CHECK(zeroed);
  CHECK(syscall(806, dir, ent, out + 8) == 0 && dalRead64(base, out + 8) == 0);  // stays at the end
  CHECK(syscall(804, dir) == err(dal::kCellEbadf));  // a directory is not a file
  CHECK(syscall(807, dir) == 0);
  CHECK(syscall(807, dir) == err(dal::kCellEbadf));
  CHECK(syscall(806, dir, ent, out + 8) == err(dal::kCellEbadf));
  put(path_a, "/dev_bdvd/b.txt");
  CHECK(syscall(805, path_a, out) == err(dal::kCellEnotdir));
  put(path_a, "/dev_bdvd/missing");
  CHECK(syscall(805, path_a, out) == err(dal::kCellEnoent));
  put(path_a, "/");
  CHECK(syscall(805, path_a, out) == 0);  // the virtual root lists the mounted devices
  CHECK(syscall(807, fdOut()) == 0);

  // Writable device.
  const stdfs::path hdd = root / "hdd";
  dal::configureVfs(game, hdd);
  CHECK(stdfs::is_directory(hdd / "dev_hdd0") && stdfs::is_directory(hdd / "dev_hdd1"));
  CHECK(open("/dev_hdd0/out.bin", 0101) == err(dal::kCellEacces));  // direct child of /dev_hdd0
  CHECK(stdfs::is_directory(hdd / "dev_hdd0" / "game") && stdfs::is_directory(hdd / "dev_hdd0" / "tmp"));
  put(path_a, "/dev_hdd0/game");
  CHECK(syscall(811, path_a, 0777) == err(dal::kCellEexist));
  put(path_a, "/dev_hdd0/newdir");
  CHECK(syscall(811, path_a, 0777) == err(dal::kCellEacces));  // games cannot add to /dev_hdd0
  put(path_a, "/dev_hdd0/game/NEWGAME");
  CHECK(syscall(811, path_a, 0777) == err(dal::kCellEacces));
  put(path_a, "/dev_hdd0/tmp/data");
  CHECK(syscall(811, path_a, 0777) == 0);
  CHECK(syscall(811, path_a, 0777) == err(dal::kCellEexist));
  put(path_a, "/dev_hdd0/tmp/nope/x");
  CHECK(syscall(811, path_a, 0777) == err(dal::kCellEnoent));

  CHECK(open("/dev_hdd0/tmp/data/out.bin", 01101) == 0);  // WRONLY | CREAT | TRUNC
  const std::uint32_t wfd = fdOut();
  put(buf, "hello");
  CHECK(syscall(803, wfd, buf, 5, out) == 0 && dalRead64(base, out) == 5);
  CHECK(syscall(802, wfd, buf, 1, out) == err(dal::kCellEbadf));  // write-only
  CHECK(syscall(804, wfd) == 0);
  CHECK(hostRead(hdd / "dev_hdd0" / "tmp" / "data" / "out.bin") == "hello");
  CHECK(open("/dev_hdd0/tmp/data/out.bin", 0201 | 0100) == err(dal::kCellEexist));  // CREAT | EXCL
  CHECK(open("/dev_hdd0/tmp/data/out.bin", 02001) == 0);                           // WRONLY | APPEND
  const std::uint32_t afd = fdOut();
  put(buf, "!!");
  CHECK(syscall(803, afd, buf, 2, out) == 0 && dalRead64(base, out) == 2);
  CHECK(syscall(804, afd) == 0);
  CHECK(hostRead(hdd / "dev_hdd0" / "tmp" / "data" / "out.bin") == "hello!!");
  CHECK(open("/dev_hdd0/tmp/data/out.bin", 02) == 0);  // RDWR keeps the contents
  const std::uint32_t rfd = fdOut();
  CHECK(syscall(818, rfd, 1, 0, out) == 0);
  put(buf, "E");
  CHECK(syscall(803, rfd, buf, 1, out) == 0);
  CHECK(syscall(818, rfd, 0, 0, out) == 0 && syscall(802, rfd, buf + 0x100, 16, out) == 0);
  CHECK(dalRead64(base, out) == 7 && std::memcmp(base + buf + 0x100, "hEllo!!", 7) == 0);
  CHECK(syscall(809, rfd, sb) == 0 && dalRead32(base, sb) == 0100666 && dalRead64(base, sb + 36) == 7);
  CHECK(syscall(804, rfd) == 0);
  CHECK(open("/dev_hdd0/tmp/data/out.bin", 01000) == 0);  // RDONLY | TRUNC empties it
  CHECK(syscall(804, fdOut()) == 0);
  CHECK(hostRead(hdd / "dev_hdd0" / "tmp" / "data" / "out.bin").empty());
  put(path_a, "/dev_hdd0/tmp/data");
  CHECK(syscall(814, path_a) == err(dal::kCellEisdir));
  put(path_a, "/dev_hdd0/tmp/data/out.bin");
  CHECK(syscall(814, path_a) == 0);
  CHECK(syscall(814, path_a) == err(dal::kCellEnoent));
  CHECK(!stdfs::exists(hdd / "dev_hdd0" / "tmp" / "data" / "out.bin"));
  put(path_a, "/dev_hdd1/y");
  CHECK(syscall(811, path_a, 0777) == 0);
  put(path_a, "/dev_hdd1");
  CHECK(syscall(805, path_a, out) == 0);
  const std::uint32_t d1 = fdOut();
  for (int i = 0; i < 3; i++) {
    CHECK(syscall(806, d1, ent, out + 8) == 0 && dalRead64(base, out + 8) == 258);
  }
  // hdd1 repeats its last entry at the end instead of zeroing it.
  CHECK(syscall(806, d1, ent, out + 8) == 0 && dalRead64(base, out + 8) == 0);
  CHECK(base[ent + 1] == 1 && base[ent + 2] == 'y');
  CHECK(syscall(807, d1) == 0);

  // The id space: 252 objects starting at 3, then EMFILE.
  std::vector<std::uint32_t> fds;
  while (open("/dev_bdvd/b.txt", 0) == 0) {
    fds.push_back(fdOut());
  }
  CHECK(fds.size() == 252 && fds.front() == 3 && fds.back() == 254);
  CHECK(open("/dev_bdvd/b.txt", 0) == err(dal::kCellEmfile));
  put(path_a, "/dev_bdvd");
  CHECK(syscall(805, path_a, out) == err(dal::kCellEmfile));
  CHECK(syscall(804, fds[1]) == 0);
  CHECK(open("/dev_bdvd/b.txt", 0) == 0 && fdOut() == fds[1]);  // the lowest free id is reused
  for (const std::uint32_t id : fds) {
    syscall(804, id);
  }

  p.main.free(scratch);
  stdfs::remove_all(root, ec);
  dal::configureVfs({}, std::nullopt);
  dal::setLogSink({});
}

void testSysPrxForUser() {
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();

  constexpr const char* kModule = "sysPrxForUser";
  const DalImportInfo imports[] = {
      {0x00800000, 0xb2fcf2c8, kModule, "_sys_heap_create_heap"},
      {0x00800010, 0xaede4b03, kModule, "_sys_heap_delete_heap"},
      {0x00800020, 0x35168520, kModule, "_sys_heap_malloc"},
      {0x00800030, 0x44265c08, kModule, "_sys_heap_memalign"},
      {0x00800040, 0x8a561d92, kModule, "_sys_heap_free"},
      {0x00800050, 0x744680a2, kModule, "sys_initialize_tls"},
      {0x00800060, 0x8461e528, kModule, "sys_time_get_system_time"},
      {0x00800070, 0x45fe2fce, kModule, "_sys_spu_printf_initialize"},
      {0x00800080, 0x12345678, kModule, "sys_unknown"},
  };
  dal::installImports(imports, 9);
  CHECK(countContaining(lines, "dal: hle: 9 imports, 8 with handlers") == 1);
  CHECK(countContaining(lines, "is registered as") == 0);

  auto call = [&](std::uint32_t index, std::uint64_t r3 = 0, std::uint64_t r4 = 0,
                  std::uint64_t r5 = 0) {
    auto ctx = std::make_unique<DalPpuContext>();
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    dalHleCall(ctx.get(), base, index);
    return ctx->gpr[3];
  };

  // Heap: ids start at 1; blocks are 64 KiB aligned and come from the main region.
  CHECK(call(0, 0x1000) == 1);
  CHECK(call(0, 0x1000) == 2);
  const std::uint64_t block = call(2, 1, 0x100);
  CHECK(block != 0 && block % 0x10000 == 0);
  CHECK(block >= dal::kMainRegion.begin && block < dal::kMainRegion.end);
  const std::uint64_t aligned = call(3, 1, 0x40000, 0x10);
  CHECK(aligned != 0 && aligned % 0x40000 == 0);
  const std::uint64_t small_align = call(3, 1, 0x10, 0x10);
  CHECK(small_align != 0 && small_align % 0x10000 == 0);
  CHECK(call(4, 1, block) == 0);
  CHECK(call(2, 1, 0x100) == block);  // the freed block is reused
  CHECK(call(4, 1, block) == 0);
  CHECK(call(4, 1, aligned) == 0);
  CHECK(call(4, 1, small_align) == 0);
  CHECK(call(1, 2) == 0);
  CHECK(call(0, 0x1000) == 2);  // the deleted id is free again

  // sys_initialize_tls is a no-op once r13 is set (the boot did it natively).
  {
    auto ctx = std::make_unique<DalPpuContext>();
    ctx->gpr[13] = 0x00123456;
    ctx->gpr[3] = 0x01000000;
    ctx->gpr[4] = 0x0010245c;
    ctx->gpr[5] = 4;
    ctx->gpr[6] = 0x110;
    dalHleCall(ctx.get(), base, 5);
    CHECK(ctx->gpr[13] == 0x00123456);
    CHECK(ctx->gpr[3] == 0x01000000);
    CHECK(countContaining(lines, "dal: tls:") == 0);
  }

  const std::uint64_t t0 = call(6);
  CHECK(t0 != 0 && call(6) >= t0);
  CHECK(call(7, 0x55) == 0);
  CHECK(call(8, 0x55) == 0);
  CHECK(dal::sysPrxForUserHandlers().size() == 23);

  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

// A guest thread object that is not registered or started: it only gives a syscall caller an id.
std::unique_ptr<dal::GuestThread> makeCaller(std::uint32_t id) {
  return std::make_unique<dal::GuestThread>(id, dal::GuestThreadParams{}, 0, 0);
}

std::uint64_t syscallAs(dal::GuestThread& caller, std::uint32_t number, std::uint64_t r3 = 0,
                        std::uint64_t r4 = 0, std::uint64_t r5 = 0, std::uint64_t r6 = 0,
                        std::uint64_t r7 = 0) {
  DalPpuContext* ctx = &caller.context();
  ctx->gpr[11] = number;
  ctx->gpr[3] = r3;
  ctx->gpr[4] = r4;
  ctx->gpr[5] = r5;
  ctx->gpr[6] = r6;
  ctx->gpr[7] = r7;
  dalSyscall(ctx, dal::process().memory.base(), 0x10000);
  return ctx->gpr[3];
}

void testMutexAndCond() {
  dal::setLogSink([](std::string_view) {});
  dal::resetCensus();
  dal::resetSyncObjects();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t id_out = scratch, attr = scratch + 0x40, cond_out = scratch + 8;

  auto a = makeCaller(0x01000000);
  auto b = makeCaller(0x01000001);
  auto setMutexAttr = [&](std::uint32_t protocol, std::uint32_t recursive) {
    dalWrite32(base, attr, protocol);
    dalWrite32(base, attr + 4, recursive);
  };

  // Recursive mutex: the count survives nested locks, only the owner may unlock.
  setMutexAttr(2, 0x10);
  CHECK(syscallAs(*a, 100, id_out, attr) == 0);
  const std::uint32_t m = dalRead32(base, id_out);
  CHECK(m == 0x85000000);
  CHECK(syscallAs(*a, 102, m, 0) == 0);
  CHECK(syscallAs(*a, 102, m, 0) == 0);
  CHECK(syscallAs(*a, 103, m) == 0);
  CHECK(syscallAs(*b, 103, m) == err(dal::kCellEbusy));
  CHECK(syscallAs(*b, 104, m) == err(dal::kCellEperm));
  CHECK(syscallAs(*a, 101, m) == err(dal::kCellEbusy));
  CHECK(syscallAs(*a, 104, m) == 0);
  CHECK(syscallAs(*a, 104, m) == 0);
  CHECK(syscallAs(*b, 104, m) == err(dal::kCellEperm));  // still owned
  CHECK(syscallAs(*a, 104, m) == 0);
  CHECK(syscallAs(*a, 104, m) == err(dal::kCellEperm));  // now free
  CHECK(syscallAs(*a, 101, m) == 0);
  CHECK(syscallAs(*a, 101, m) == err(dal::kCellEsrch));
  CHECK(syscallAs(*a, 102, m, 0) == err(dal::kCellEsrch));

  // A non-recursive mutex reports a relock as a deadlock.
  setMutexAttr(1, 0x20);
  CHECK(syscallAs(*a, 100, id_out, attr) == 0);
  const std::uint32_t plain = dalRead32(base, id_out);
  CHECK(plain == 0x85000100);
  CHECK(syscallAs(*a, 102, plain, 0) == 0);
  CHECK(syscallAs(*a, 102, plain, 0) == err(dal::kCellEdeadlk));
  CHECK(syscallAs(*a, 103, plain) == err(dal::kCellEdeadlk));
  CHECK(syscallAs(*a, 104, plain) == 0);

  setMutexAttr(9, 0x20);
  CHECK(syscallAs(*a, 100, id_out, attr) == err(dal::kCellEinval));
  setMutexAttr(1, 0x21);
  CHECK(syscallAs(*a, 100, id_out, attr) == err(dal::kCellEinval));
  CHECK(syscallAs(*a, 100, 0, attr) == err(dal::kCellEfault));

  // Condition variable: the mutex must be held, and a timed-out wait still hands it back.
  CHECK(syscallAs(*a, 105, cond_out, 0x85000fff, attr) == err(dal::kCellEsrch));
  CHECK(syscallAs(*a, 105, cond_out, plain, attr) == 0);
  const std::uint32_t c = dalRead32(base, cond_out);
  CHECK(c == 0x86000000);
  CHECK(syscallAs(*a, 107, c, 0) == err(dal::kCellEperm));   // mutex not held
  CHECK(syscallAs(*a, 101, plain) == err(dal::kCellEperm));  // bound to a condition variable
  CHECK(syscallAs(*a, 108, c) == 0);                         // no waiters: dropped
  CHECK(syscallAs(*a, 109, c) == 0);
  {
    std::lock_guard<dal::GuestLock> guard(dal::guestLock());
    CHECK(syscallAs(*a, 102, plain, 0) == 0);
    CHECK(syscallAs(*a, 107, c, 1000) == err(dal::kCellEtimedout));
    CHECK(syscallAs(*b, 103, plain) == err(dal::kCellEbusy));  // reacquired by the waiter
    CHECK(syscallAs(*a, 104, plain) == 0);
  }
  setMutexAttr(2, 0x10);
  CHECK(syscallAs(*a, 100, id_out, attr) == 0);
  const std::uint32_t rec = dalRead32(base, id_out);
  CHECK(syscallAs(*a, 105, cond_out, rec, attr) == 0);
  const std::uint32_t rc = dalRead32(base, cond_out);
  {
    std::lock_guard<dal::GuestLock> guard(dal::guestLock());
    CHECK(syscallAs(*a, 102, rec, 0) == 0);
    CHECK(syscallAs(*a, 102, rec, 0) == 0);
    CHECK(syscallAs(*a, 107, rc, 1000) == err(dal::kCellEtimedout));
    CHECK(syscallAs(*a, 104, rec) == 0);  // the recursion count came back
    CHECK(syscallAs(*a, 104, rec) == 0);
    CHECK(syscallAs(*a, 104, rec) == err(dal::kCellEperm));
  }
  CHECK(syscallAs(*a, 106, rc) == 0);
  CHECK(syscallAs(*a, 106, rc) == err(dal::kCellEsrch));
  CHECK(syscallAs(*a, 101, rec) == 0);

  // Contention: a second host thread blocks on a held mutex and gets it after the unlock. The
  // worker holds the guest lock from the flag write until its blocking wait releases it, so seeing
  // the flag while holding the lock means the worker is waiting.
  CHECK(syscallAs(*a, 100, id_out, attr) == 0);
  const std::uint32_t contended = dalRead32(base, id_out);
  std::atomic<bool> b_ready{false};
  std::uint64_t b_result = ~0ull;
  {
    std::lock_guard<dal::GuestLock> guard(dal::guestLock());
    CHECK(syscallAs(*a, 102, contended, 0) == 0);
    std::thread worker([&] {
      std::lock_guard<dal::GuestLock> worker_guard(dal::guestLock());
      b_ready = true;
      b_result = syscallAs(*b, 102, contended, 0);
    });
    dal::guestLock().unlock();
    while (!b_ready) std::this_thread::yield();
    dal::guestLock().lock();
    CHECK(syscallAs(*a, 104, contended) == 0);
    dal::guestLock().unlock();
    worker.join();
    dal::guestLock().lock();
  }
  CHECK(b_result == 0);
  CHECK(syscallAs(*a, 104, contended) == err(dal::kCellEperm));
  CHECK(syscallAs(*b, 104, contended) == 0);

  // A waiter is woken by a signal and returns holding the mutex again.
  CHECK(syscallAs(*a, 105, cond_out, contended, attr) == 0);
  const std::uint32_t cv = dalRead32(base, cond_out);
  b_ready = false;
  b_result = ~0ull;
  {
    std::lock_guard<dal::GuestLock> guard(dal::guestLock());
    std::thread worker([&] {
      std::lock_guard<dal::GuestLock> worker_guard(dal::guestLock());
      syscallAs(*b, 102, contended, 0);
      b_ready = true;
      b_result = syscallAs(*b, 107, cv, 0);
    });
    dal::guestLock().unlock();
    while (!b_ready) std::this_thread::yield();
    dal::guestLock().lock();
    CHECK(syscallAs(*a, 102, contended, 0) == 0);  // free: the waiter released it
    CHECK(syscallAs(*a, 108, cv) == 0);
    CHECK(syscallAs(*a, 104, contended) == 0);
    dal::guestLock().unlock();
    worker.join();
    dal::guestLock().lock();
  }
  CHECK(b_result == 0);
  CHECK(syscallAs(*b, 104, contended) == 0);  // held by the waiter after the wait

  p.main.free(scratch);
  dal::resetSyncObjects();
  dal::setLogSink({});
  dal::resetCensus();
}

void testEventObjects() {
  dal::setLogSink([](std::string_view) {});
  dal::resetCensus();
  dal::resetSyncObjects();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t id_out = scratch, attr = scratch + 0x40, result = scratch + 0x80,
                      num = scratch + 0x90;

  auto a = makeCaller(0x01000000);
  auto b = makeCaller(0x01000001);
  // Waiting calls need the guest lock; the helpers below hand it over to the worker threads.
  std::lock_guard<dal::GuestLock> guard(dal::guestLock());

  // Event flag: attribute checks, then OR / AND waits with the clear modes.
  dalWrite32(base, attr, 1);             // protocol FIFO
  dalWrite32(base, attr + 20, 0x20000);  // multiple waiters
  CHECK(syscallAs(*a, 82, 0, attr, 0) == err(dal::kCellEfault));
  dalWrite32(base, attr + 20, 0x30000);
  CHECK(syscallAs(*a, 82, id_out, attr, 0) == err(dal::kCellEinval));
  dalWrite32(base, attr + 20, 0x20000);
  CHECK(syscallAs(*a, 82, id_out, attr, 0x5) == 0);
  const std::uint32_t flag = dalRead32(base, id_out);
  CHECK(flag == 0x98000000);
  CHECK(syscallAs(*a, 139, flag, result) == 0);
  CHECK(dalRead64(base, result) == 0x5);

  CHECK(syscallAs(*a, 85, flag, 0x4, dal::kWaitOr, result, 0) == 0);  // already set
  CHECK(dalRead64(base, result) == 0x5);
  CHECK(syscallAs(*a, 85, flag, 0x4, dal::kWaitOr | dal::kWaitClear, result, 1000) == 0);
  CHECK(dalRead64(base, result) == 0x5);  // the value before the clear
  CHECK(syscallAs(*a, 139, flag, result) == 0);
  CHECK(dalRead64(base, result) == 0x1);
  CHECK(syscallAs(*a, 87, flag, 0x6) == 0);
  CHECK(syscallAs(*a, 139, flag, result) == 0);
  CHECK(dalRead64(base, result) == 0x7);
  CHECK(syscallAs(*a, 85, flag, 0x3, dal::kWaitAnd | dal::kWaitClearAll, result, 1000) == 0);
  CHECK(dalRead64(base, result) == 0x7);
  CHECK(syscallAs(*a, 139, flag, result) == 0);
  CHECK(dalRead64(base, result) == 0);
  CHECK(syscallAs(*a, 87, flag, 0x3) == 0);
  CHECK(syscallAs(*a, 85, flag, 0x7, dal::kWaitAnd, result, 1000) == err(dal::kCellEtimedout));
  CHECK(dalRead64(base, result) == 0x3);  // the pattern at the time of the timeout
  CHECK(syscallAs(*a, 118, flag, 0x2) == 0);
  CHECK(syscallAs(*a, 139, flag, result) == 0);
  CHECK(dalRead64(base, result) == 0x2);
  CHECK(syscallAs(*a, 85, flag, 0x1, 0x3, result, 1000) == err(dal::kCellEinval));  // AND|OR
  CHECK(syscallAs(*a, 85, flag, 0x1, 0x4, result, 1000) == err(dal::kCellEinval));
  CHECK(syscallAs(*a, 85, 0x98000fff, 0x1, dal::kWaitOr, result, 1000) == err(dal::kCellEsrch));
  CHECK(syscallAs(*a, 139, 0x98000fff, result) == err(dal::kCellEsrch));
  CHECK(syscallAs(*a, 87, 0x98000fff, 1) == err(dal::kCellEsrch));

  // A waiter on another host thread is woken by a set and cancelled by a cancel. The worker holds
  // the guest lock from the flag write until its blocking wait releases it.
  std::atomic<bool> b_ready{false};
  std::uint64_t b_result = ~0ull;
  auto runWaiter = [&](std::uint32_t id, std::uint64_t pattern, std::uint32_t mode) {
    b_ready = false;
    b_result = ~0ull;
    std::thread worker([&, id, pattern, mode] {
      std::lock_guard<dal::GuestLock> worker_guard(dal::guestLock());
      b_ready = true;
      b_result = syscallAs(*b, 85, id, pattern, mode, result + 0x20, 0);
    });
    dal::guestLock().unlock();
    while (!b_ready) std::this_thread::yield();
    dal::guestLock().lock();
    return worker;
  };
  const auto finish = [&](std::thread& worker) {
    dal::guestLock().unlock();
    worker.join();
    dal::guestLock().lock();
  };
  {
    CHECK(syscallAs(*a, 118, flag, 0) == 0);
    std::thread worker = runWaiter(flag, 0x8, dal::kWaitOr | dal::kWaitClear);
    CHECK(syscallAs(*a, 83, flag) == err(dal::kCellEbusy));
    CHECK(syscallAs(*a, 87, flag, 0x1) == 0);  // does not match: still waiting
    CHECK(b_result == ~0ull);
    CHECK(syscallAs(*a, 87, flag, 0x8) == 0);
    finish(worker);
    CHECK(b_result == 0);
    CHECK(dalRead64(base, result + 0x20) == 0x9);
    CHECK(syscallAs(*a, 139, flag, result) == 0);
    CHECK(dalRead64(base, result) == 0x1);  // the waiter cleared its bit only

    std::thread canceled = runWaiter(flag, 0x10, dal::kWaitAnd);
    CHECK(syscallAs(*a, 132, flag, num) == 0);
    CHECK(dalRead32(base, num) == 1);
    finish(canceled);
    CHECK(b_result == err(dal::kCellEcanceled));
    CHECK(dalRead64(base, result + 0x20) == 0x1);
    CHECK(syscallAs(*a, 132, flag, num) == 0);
    CHECK(dalRead32(base, num) == 0);
    CHECK(syscallAs(*a, 132, 0x98000fff, num) == err(dal::kCellEsrch));
  }
  CHECK(syscallAs(*a, 83, flag) == 0);
  CHECK(syscallAs(*a, 83, flag) == err(dal::kCellEsrch));

  // A single-waiter flag refuses a second waiter.
  dalWrite32(base, attr + 20, 0x10000);
  CHECK(syscallAs(*a, 82, id_out, attr, 0) == 0);
  const std::uint32_t single = dalRead32(base, id_out);
  CHECK(single == 0x98000100);
  {
    std::thread worker = runWaiter(single, 0x1, dal::kWaitOr);
    CHECK(syscallAs(*a, 85, single, 0x2, dal::kWaitOr, result, 0) == err(dal::kCellEperm));
    CHECK(syscallAs(*a, 87, single, 0x1) == 0);
    finish(worker);
    CHECK(b_result == 0);
  }
  CHECK(syscallAs(*a, 83, single) == 0);

  // Event queue and port: send then receive delivers source/data1..3 in r4..r7.
  const std::uint32_t queue_id = scratch + 0xa0, port_id = scratch + 0xb0,
                      sys_event = scratch + 0xc0;
  dalWrite32(base, attr, 2);      // protocol PRIORITY
  dalWrite32(base, attr + 4, 1);  // PPU queue
  CHECK(syscallAs(*a, 128, queue_id, attr, 0, 0) == err(dal::kCellEinval));    // size 0
  CHECK(syscallAs(*a, 128, queue_id, attr, 0, 128) == err(dal::kCellEinval));  // size 128
  dalWrite32(base, attr + 4, 3);
  CHECK(syscallAs(*a, 128, queue_id, attr, 0, 2) == err(dal::kCellEinval));  // type
  dalWrite32(base, attr + 4, 1);
  CHECK(syscallAs(*a, 128, queue_id, attr, 0, 2) == 0);
  const std::uint32_t queue = dalRead32(base, queue_id);
  CHECK(queue == 0x8d000000);
  CHECK(syscallAs(*a, 134, port_id, 2, 0) == err(dal::kCellEinval));
  CHECK(syscallAs(*a, 134, port_id, 1, 0) == 0);
  const std::uint32_t port = dalRead32(base, port_id);
  CHECK(port == 0x0e000000);
  CHECK(syscallAs(*a, 138, port, 1, 2, 3) == err(dal::kCellEnotconn));
  CHECK(syscallAs(*a, 136, port, 0x8d000fff) == err(dal::kCellEsrch));
  CHECK(syscallAs(*a, 136, port, queue) == 0);
  CHECK(syscallAs(*a, 136, port, queue) == err(dal::kCellEisconn));
  CHECK(syscallAs(*a, 135, port) == err(dal::kCellEisconn));

  CHECK(syscallAs(*a, 138, port, 0x11, 0x22, 0x33) == 0);
  CHECK(syscallAs(*a, 138, port, 0x44, 0x55, 0x66) == 0);
  CHECK(syscallAs(*a, 138, port, 0x77, 0x88, 0x99) == err(dal::kCellEbusy));  // queue full
  CHECK(syscallAs(*a, 130, queue, sys_event, 1000) == 0);
  DalPpuContext* actx = &a->context();
  CHECK(actx->gpr[4] == ((std::uint64_t{1} << 32) | port));  // unnamed port: pid << 32 | id
  CHECK(actx->gpr[5] == 0x11 && actx->gpr[6] == 0x22 && actx->gpr[7] == 0x33);
  CHECK(dalRead64(base, sys_event) == 0);  // the event pointer is never written
  CHECK(syscallAs(*a, 133, queue) == 0);   // drain
  CHECK(syscallAs(*a, 130, queue, sys_event, 1000) == err(dal::kCellEtimedout));

  // A named port reports its name; a blocked receiver gets the event directly.
  CHECK(syscallAs(*a, 137, port) == 0);
  CHECK(syscallAs(*a, 137, port) == err(dal::kCellEnotconn));
  CHECK(syscallAs(*a, 135, port) == 0);
  CHECK(syscallAs(*a, 134, port_id, 1, 0xabcd) == 0);
  const std::uint32_t named = dalRead32(base, port_id);
  CHECK(syscallAs(*a, 136, named, queue) == 0);
  {
    std::atomic<bool> receiving{false};
    std::thread worker([&] {
      std::lock_guard<dal::GuestLock> worker_guard(dal::guestLock());
      receiving = true;
      b_result = syscallAs(*b, 130, queue, sys_event, 0);
    });
    dal::guestLock().unlock();
    while (!receiving) std::this_thread::yield();
    dal::guestLock().lock();
    CHECK(syscallAs(*a, 129, queue, 0) == err(dal::kCellEbusy));  // a receiver waits
    CHECK(syscallAs(*a, 138, named, 7, 8, 9) == 0);
    dal::guestLock().unlock();
    worker.join();
    dal::guestLock().lock();
  }
  CHECK(b_result == 0);
  DalPpuContext* bctx = &b->context();
  CHECK(bctx->gpr[4] == 0xabcd && bctx->gpr[5] == 7 && bctx->gpr[6] == 8 && bctx->gpr[7] == 9);

  // Forced destroy cancels receivers; the port is then disconnected.
  {
    std::atomic<bool> receiving{false};
    std::thread worker([&] {
      std::lock_guard<dal::GuestLock> worker_guard(dal::guestLock());
      receiving = true;
      b_result = syscallAs(*b, 130, queue, sys_event, 0);
    });
    dal::guestLock().unlock();
    while (!receiving) std::this_thread::yield();
    dal::guestLock().lock();
    CHECK(syscallAs(*a, 129, queue, 2) == err(dal::kCellEinval));
    CHECK(syscallAs(*a, 129, queue, 1) == 0);
    dal::guestLock().unlock();
    worker.join();
    dal::guestLock().lock();
  }
  CHECK(b_result == err(dal::kCellEcanceled));
  CHECK(syscallAs(*a, 129, queue, 0) == err(dal::kCellEsrch));
  CHECK(syscallAs(*a, 130, queue, sys_event, 1000) == err(dal::kCellEsrch));
  CHECK(syscallAs(*a, 138, named, 1, 2, 3) == err(dal::kCellEnotconn));
  CHECK(syscallAs(*a, 135, named) == 0);
  CHECK(syscallAs(*a, 135, named) == err(dal::kCellEsrch));

  p.main.free(scratch);
  dal::resetSyncObjects();
  dal::setLogSink({});
  dal::resetCensus();
}

void testSpuAndSysmodule() {
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetSyncObjects();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t queue_id_out = scratch, attr = scratch + 0x40, spup = scratch + 0x80;

  auto a = makeCaller(0x01000000);
  std::lock_guard<dal::GuestLock> guard(dal::guestLock());

  // sys_spu_initialize: more than 5 raw SPUs is invalid.
  CHECK(syscallAs(*a, 169, 6, 5) == 0);
  CHECK(syscallAs(*a, 169, 6, 6) == err(dal::kCellEinval));
  CHECK(syscallAs(*a, 169, 0, 0) == 0);

  dalWrite32(base, attr, 1);      // protocol FIFO
  dalWrite32(base, attr + 4, 2);  // SPU queue
  CHECK(syscallAs(*a, 128, queue_id_out, attr, 0, 2) == 0);
  const std::uint32_t queue = dalRead32(base, queue_id_out);

  // sc 251: req 0 is invalid, an unknown group or queue is ESRCH, the port is the lowest
  // requested bit.
  dal::resetSpuThreadGroups();
  CHECK(syscallAs(*a, 251, 0x04000100, queue, 4, spup) == err(dal::kCellEsrch));
  CHECK(dal::createSpuThreadGroup() == 0x04000100);
  CHECK(!dal::spuThreadGroupExists(0x04000101));
  dalWrite8(base, spup, 0xee);
  CHECK(syscallAs(*a, 251, 0x04000100, queue, 0, spup) == err(dal::kCellEinval));
  CHECK(syscallAs(*a, 251, 0x04000100, 0x8d000fff, 4, spup) == err(dal::kCellEsrch));
  CHECK(dalRead8(base, spup) == 0xee);
  CHECK(syscallAs(*a, 251, 0x04000100, queue, 4, spup) == 0);
  CHECK(dalRead8(base, spup) == 2);
  CHECK(syscallAs(*a, 251, 0x04000100, queue, 0x28, spup) == 0);
  CHECK(dalRead8(base, spup) == 3);
  CHECK(syscallAs(*a, 251, 0x04000100, queue, std::uint64_t{1} << 63, spup) == 0);
  CHECK(dalRead8(base, spup) == 63);
  CHECK(syscallAs(*a, 251, 0x04000100, queue, 4, 0) == err(dal::kCellEfault));

  const std::string census = dal::formatCensus();
  CHECK(census.find("sc 169 sys_spu_initialize: 3 [impl]") != std::string::npos);
  CHECK(census.find("sc 251 sys_spu_thread_group_connect_event_all_threads: 7 [impl]") !=
        std::string::npos);

  // cellSysmodule: any id succeeds; the first load and unload of each id is logged once.
  const DalImportInfo imports[] = {
      {0x00800000, 0x32267a31, "cellSysmodule", "cellSysmoduleLoadModule"},
      {0x00800010, 0x112a5ee9, "cellSysmodule", "cellSysmoduleUnloadModule"},
  };
  dal::installImports(imports, 2);
  DalPpuContext* ctx = &a->context();
  for (int i = 0; i < 2; i++) {
    ctx->gpr[3] = 0xf;
    dalHleCall(ctx, base, 0);
    CHECK(ctx->gpr[3] == 0);
  }
  ctx->gpr[3] = 0x10001;  // only the low 16 bits are the id
  dalHleCall(ctx, base, 0);
  CHECK(ctx->gpr[3] == 0);
  ctx->gpr[3] = 0xf;
  dalHleCall(ctx, base, 1);
  CHECK(ctx->gpr[3] == 0);
  CHECK(countContaining(lines, "dal: sysmodule: load 0x000f") == 1);
  CHECK(countContaining(lines, "dal: sysmodule: load 0x0001") == 1);
  CHECK(countContaining(lines, "dal: sysmodule: unload 0x000f") == 1);
  CHECK(countContaining(lines, "dal: default hle") == 0);
  CHECK(dal::formatCensus().find("cellSysmodule::cellSysmoduleLoadModule: 3 [impl]") !=
        std::string::npos);

  dal::resetSpuThreadGroups();
  p.main.free(scratch);
  dal::resetSyncObjects();
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

// In-memory command buffer: words are keyed by IO offset, unlisted offsets are unmapped.
class FakeSink : public dal::RsxSink {
 public:
  bool readIo(std::uint32_t io, std::uint32_t& word) override {
    const auto it = words.find(io);
    if (it == words.end()) return false;
    word = it->second;
    return true;
  }
  bool write(std::uint32_t method, std::uint32_t value) override {
    ++attempts;
    if (refuse > 0) {
      --refuse;
      return false;
    }
    writes.emplace_back(method, value);
    return true;
  }
  void put(std::uint32_t at, std::initializer_list<std::uint32_t> list) {
    for (const std::uint32_t word : list) {
      words[at] = word;
      at += 4;
    }
  }

  std::map<std::uint32_t, std::uint32_t> words;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> writes;
  int refuse = 0;  // the next writes return false
  int attempts = 0;
};

using Writes = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

void testParamSfo() {
  dal::ParamSfo sfo;
  sfo.ints["PARENTAL_LEVEL"] = 5;
  sfo.ints["RESOLUTION"] = 0x1d;
  sfo.strings["CATEGORY"] = "DG";
  sfo.strings["TITLE_ID"] = "BLJM61048";
  sfo.strings["TITLE"] = "Sample title";
  const std::vector<std::uint8_t> bytes = dal::serializeParamSfo(sfo);
  CHECK(bytes.size() > 20 && bytes[0] == 0 && bytes[1] == 'P' && bytes[2] == 'S' && bytes[3] == 'F');
  const auto parsed = dal::parseParamSfo(bytes);
  CHECK(parsed && parsed->ints == sfo.ints && parsed->strings == sfo.strings);
  CHECK(parsed && parsed->ints.find("SOUND_FORMAT") == parsed->ints.end());  // missing key

  std::vector<std::uint8_t> bad = bytes;
  bad[1] = 'X';
  CHECK(!dal::parseParamSfo(bad));
  CHECK(!dal::parseParamSfo(std::span<const std::uint8_t>(bytes.data(), 10)));
  CHECK(!dal::parseParamSfo(std::span<const std::uint8_t>(bytes.data(), bytes.size() - 8)));

  // A raw-format (0x0004) string that fills its length without a terminator.
  const std::vector<std::uint8_t> raw = {0, 'P', 'S', 'F', 1, 1, 0, 0, 36, 0, 0, 0, 40, 0, 0, 0, 1, 0, 0, 0,
                                         0, 0, 4, 0, 4, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,
                                         'K', 0, 0, 0, 'a', 'b', 'c', 'd'};
  const auto raw_parsed = dal::parseParamSfo(raw);
  CHECK(raw_parsed && raw_parsed->strings.at("K") == "abcd");
}

void testCellGame() {
  namespace stdfs = std::filesystem;
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetCellGame();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t name = scratch, size = scratch + 0x100, info = scratch + 0x200,
                      usrdir = scratch + 0x300, init = scratch + 0x400;

  const stdfs::path root =
      stdfs::temp_directory_path() / ("dal_game_test_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  stdfs::remove_all(root, ec);
  const stdfs::path game = root / "game";
  stdfs::create_directories(game / "PS3_GAME" / "USRDIR");
  dal::ParamSfo disc;
  disc.strings["CATEGORY"] = "DG";
  disc.strings["TITLE_ID"] = "BLJM61048";
  {
    const auto bytes = dal::serializeParamSfo(disc);
    std::ofstream(game / "PS3_GAME" / "PARAM.SFO", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  dal::configureVfs(game, root / "hdd");

  enum Import : std::uint32_t { Create, Permit, ErrorDialog, DataCheck, ImportCount };
  const DalImportInfo imports[] = {
      {0x00800000, 0x42a2e133, "cellGame", "cellGameCreateGameData"},
      {0x00800010, 0x70acec67, "cellGame", "cellGameContentPermit"},
      {0x00800020, 0xb0a1f8c6, "cellGame", "cellGameContentErrorDialog"},
      {0x00800030, 0xdb9819f3, "cellGame", "cellGameDataCheck"},
  };
  dal::installImports(imports, ImportCount);
  auto ctx = std::make_unique<DalPpuContext>();
  auto call = [&](Import which, std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    ctx->gpr[3] = a;
    ctx->gpr[4] = b;
    ctx->gpr[5] = c;
    dalHleCall(ctx.get(), base, which);
    return static_cast<std::uint32_t>(ctx->gpr[3]);
  };
  auto put = [&](std::uint32_t addr, std::string_view text) {
    std::memcpy(base + addr, text.data(), text.size());
    base[addr + text.size()] = 0;
  };
  auto get = [&](std::uint32_t addr) { return std::string(reinterpret_cast<const char*>(base + addr)); };
  constexpr std::uint32_t kParam = 0x8002cb07, kFailure = 0x8002cb22, kBusy = 0x8002cb23,
                          kBroken = 0x8002cb05, kExist = 0x8002cb26;

  // Disc: the content directory is the disc itself.
  CHECK(call(DataCheck, 1, 0, size) == 0);
  CHECK(dalRead32(base, size) == 40 * 1024 * 1024 - 256);
  CHECK(static_cast<std::int32_t>(dalRead32(base, size + 4)) == -1 && dalRead32(base, size + 8) == 0);
  CHECK(call(Permit, info, usrdir, 0) == 0);
  CHECK(get(info) == "/dev_bdvd/PS3_GAME" && get(usrdir) == "/dev_bdvd/PS3_GAME/USRDIR");
  CHECK(call(Permit, info, usrdir, 0) == kFailure);  // no outstanding check

  // Argument validation and the busy state.
  CHECK(call(DataCheck, 0, name, 0) == kParam);
  CHECK(call(DataCheck, 4, name, 0) == kParam);
  CHECK(call(DataCheck, 3, 0, 0) == kParam);
  CHECK(call(DataCheck, 1, 0, 0) == 0);
  CHECK(call(DataCheck, 1, 0, 0) == kBusy);
  CHECK(call(Permit, 0, usrdir, 0) == kParam);
  CHECK(call(Permit, info, usrdir, 0) == 0);

  // Game data that does not exist yet: check, create, permit moves it into place.
  put(name, "TESTDATA");
  CHECK(call(DataCheck, 3, name, size) == 2);
  CHECK(dalRead32(base, size + 4) == 0);
  put(init + 0, "My title");
  put(init + 128, "TESTID001");
  put(init + 140, "01.00");
  CHECK(call(Create, init, info, usrdir) == 0);
  const std::string temp_dir = get(info);
  CHECK(temp_dir.starts_with("/dev_hdd0/game/_GDATA_") && get(usrdir) == temp_dir + "/USRDIR");
  const stdfs::path hdd_game = root / "hdd" / "dev_hdd0" / "game";
  CHECK(stdfs::is_directory(hdd_game / temp_dir.substr(15) / "USRDIR"));
  CHECK(call(Permit, info, usrdir, 0) == 0);
  CHECK(get(info) == "/dev_hdd0/game/TESTDATA" && get(usrdir) == "/dev_hdd0/game/TESTDATA/USRDIR");
  CHECK(stdfs::is_directory(hdd_game / "TESTDATA" / "USRDIR"));
  CHECK(!stdfs::exists(hdd_game / temp_dir.substr(15)));
  const auto written = dal::readParamSfoFile(hdd_game / "TESTDATA" / "PARAM.SFO");
  CHECK(written && written->strings.at("CATEGORY") == "GD" && written->strings.at("TITLE") == "My title" &&
        written->strings.at("TITLE_ID") == "TESTID001" && written->strings.at("VERSION") == "01.00");

  // The data now exists: no creation, a category mismatch is reported as broken.
  CHECK(call(DataCheck, 3, name, size) == 0);
  CHECK(static_cast<std::int32_t>(dalRead32(base, size + 4)) == -1);
  CHECK(call(Create, init, info, usrdir) == kExist);
  CHECK(call(Permit, info, usrdir, 0) == 0);
  CHECK(get(usrdir) == "/dev_hdd0/game/TESTDATA/USRDIR");
  CHECK(call(DataCheck, 2, name, 0) == kBroken);
  CHECK(call(DataCheck, 3, name, 0) == 0);  // the broken check left no outstanding state
  CHECK(call(Permit, info, usrdir, 0) == 0);

  // CreateGameData needs an outstanding game data check.
  CHECK(call(Create, init, info, usrdir) == kFailure);
  CHECK(call(Create, 0, info, usrdir) == kParam);

  // Permit for a missing directory returns empty paths.
  put(name, "NOSUCHDIR");
  CHECK(call(DataCheck, 2, name, 0) == 2);
  CHECK(call(Permit, info, usrdir, 0) == 0);
  CHECK(get(info).empty() && get(usrdir).empty());

  // Error dialog: only the continuing types return.
  CHECK(call(ErrorDialog, 0, 0, 0) == 0);
  CHECK(call(ErrorDialog, 2, 1024, name) == 0);
  CHECK(call(ErrorDialog, 7, 0, 0) == kParam);

  dal::resetCellGame();
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
  stdfs::remove_all(root, ec);
}

void testCellFs() {
  namespace stdfs = std::filesystem;
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t path_a = scratch, buf = scratch + 0x2000, out = scratch + 0x3000,
                      sb = scratch + 0x3100, arg = scratch + 0x3200;

  const stdfs::path root =
      stdfs::temp_directory_path() / ("dal_cellfs_test_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  stdfs::remove_all(root, ec);
  const stdfs::path game = root / "game";
  stdfs::create_directories(game / "sub");
  std::ofstream(game / "b.txt", std::ios::binary) << "0123456789";
  dal::configureVfs(game, root / "hdd");

  enum Import : std::uint32_t { Read, Close, Open, Stat, Unlink, Write, SdataOpen, Mkdir, Fstat, ImportCount };
  const DalImportInfo imports[] = {
      {0x00800000, 0x0d5b4a14, "sys_fs", "cellFsReadWithOffset"},
      {0x00800010, 0x2cb51f0d, "sys_fs", "cellFsClose"},
      {0x00800020, 0x718bf5f8, "sys_fs", "cellFsOpen"},
      {0x00800030, 0x7de6dced, "sys_fs", "cellFsStat"},
      {0x00800040, 0x7f4677a8, "sys_fs", "cellFsUnlink"},
      {0x00800050, 0x8cb722d5, "sys_fs", "cellFsWriteWithOffset"},
      {0x00800060, 0xb1840b53, "sys_fs", "cellFsSdataOpen"},
      {0x00800070, 0xba901fe6, "sys_fs", "cellFsMkdir"},
      {0x00800080, 0xef3efa34, "sys_fs", "cellFsFstat"},
  };
  dal::installImports(imports, ImportCount);
  auto ctx = std::make_unique<DalPpuContext>();
  auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
                  std::uint64_t d = 0, std::uint64_t e = 0) {
    ctx->gpr[3] = a;
    ctx->gpr[4] = b;
    ctx->gpr[5] = c;
    ctx->gpr[6] = d;
    ctx->gpr[7] = e;
    dalHleCall(ctx.get(), base, which);
    return ctx->gpr[3];
  };
  auto put = [&](std::uint32_t addr, std::string_view text) {
    std::memcpy(base + addr, text.data(), text.size());
    base[addr + text.size()] = 0;
  };
  auto hostRead = [](const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };

  // Open, positional read, close. The file position is not moved by ReadWithOffset.
  put(path_a, "/dev_bdvd/b.txt");
  CHECK(call(Open, path_a, 0, out, 0, 0) == 0);
  const std::uint32_t fd = dalRead32(base, out);
  CHECK(fd == 3);
  CHECK(call(Open, path_a, 0, 0, 0, 0) == err(dal::kCellEfault));
  CHECK(call(Read, fd, 6, buf, 3, out) == 0 && dalRead64(base, out) == 3);
  CHECK(std::memcmp(base + buf, "678", 3) == 0);
  CHECK(call(Read, fd, 8, buf, 100, out) == 0 && dalRead64(base, out) == 2);  // short at the end
  CHECK(call(Read, fd, 50, buf, 4, out) == 0 && dalRead64(base, out) == 0);   // past the end
  CHECK(call(Read, fd, 0, buf, 2, 0) == 0);                                  // nread is optional
  CHECK(call(Read, fd, 0, 0, 2, out) == err(dal::kCellEfault) && dalRead64(base, out) == 0);
  // sys_fs_read continues from the start: the position never moved.
  CHECK(syscall(802, fd, buf, 4, out) == 0 && std::memcmp(base + buf, "0123", 4) == 0);
  CHECK(call(Read, fd, 0, buf, 1, out) == 0 && syscall(802, fd, buf + 8, 2, out) == 0);
  CHECK(std::memcmp(base + buf + 8, "45", 2) == 0);
  CHECK(call(Read, 99, 0, buf, 1, out) == err(dal::kCellEbadf) && dalRead64(base, out) == 0);
  CHECK(call(Read, 0, 0, buf, 1, out) == err(dal::kCellEbadf));
  CHECK(call(Write, fd, 0, buf, 1, out) == err(dal::kCellEbadf));  // opened read-only

  // Stat of a file and of a directory, fstat of the open file.
  CHECK(call(Stat, path_a, sb) == 0 && dalRead32(base, sb) == 0100444 && dalRead64(base, sb + 36) == 10);
  CHECK(call(Stat, path_a, 0) == err(dal::kCellEfault));
  put(path_a, "/dev_bdvd/sub");
  CHECK(call(Stat, path_a, sb) == 0 && dalRead32(base, sb) == 040555);
  put(path_a, "/dev_bdvd/none");
  CHECK(call(Stat, path_a, sb) == err(dal::kCellEnoent));
  CHECK(call(Fstat, fd, sb) == 0 && dalRead32(base, sb) == 0100444 && dalRead64(base, sb + 36) == 10);
  CHECK(call(Fstat, 99, sb) == err(dal::kCellEbadf));
  CHECK(call(Close, fd) == 0);
  CHECK(call(Close, fd) == err(dal::kCellEbadf));

  // Mkdir, positional write and Unlink under the hdd directory.
  put(path_a, "/dev_hdd0/tmp/d");
  CHECK(call(Mkdir, path_a, 0777) == 0);
  CHECK(call(Mkdir, path_a, 0777) == err(dal::kCellEexist));
  CHECK(stdfs::is_directory(root / "hdd" / "dev_hdd0" / "tmp" / "d"));
  put(path_a, "/dev_hdd0/tmp/d/f.bin");
  CHECK(call(Open, path_a, 0102, out, 0, 0) == 0);  // RDWR | CREAT
  const std::uint32_t wfd = dalRead32(base, out);
  put(buf, "abcdef");
  CHECK(call(Write, wfd, 0, buf, 6, out) == 0 && dalRead64(base, out) == 6);
  put(buf + 0x10, "XY");
  CHECK(call(Write, wfd, 2, buf + 0x10, 2, out) == 0 && dalRead64(base, out) == 2);
  CHECK(call(Write, wfd, 0, 0, 2, out) == err(dal::kCellEfault) && dalRead64(base, out) == 0);
  // The file position stayed at 0, so a plain write lands at the start.
  put(buf + 0x20, "Z");
  CHECK(syscall(803, wfd, buf + 0x20, 1, out) == 0);
  CHECK(call(Fstat, wfd, sb) == 0 && dalRead64(base, sb + 36) == 6);
  CHECK(call(Close, wfd) == 0);
  CHECK(hostRead(root / "hdd" / "dev_hdd0" / "tmp" / "d" / "f.bin") == "ZbXYef");
  CHECK(call(Unlink, path_a) == 0);
  CHECK(call(Unlink, path_a) == err(dal::kCellEnoent));
  CHECK(!stdfs::exists(root / "hdd" / "dev_hdd0" / "tmp" / "d" / "f.bin"));
  put(path_a, "/dev_bdvd/b.txt");
  CHECK(call(Unlink, path_a) == err(dal::kCellErofs));
  CHECK(call(Write, 3, 0, buf, 1, out) == err(dal::kCellEbadf));

  // cellFsSdataOpen: argument validation, then ENOENT with nothing written to *fd.
  dalWrite32(base, out, 0xDEADBEEF);
  CHECK(call(SdataOpen, path_a, 1, out) == err(dal::kCellEinval));
  CHECK(call(SdataOpen, path_a, 0, 0) == err(dal::kCellEfault));
  CHECK(call(SdataOpen, 0, 0, out) == err(dal::kCellEfault));
  CHECK(call(SdataOpen, path_a, 0, out) == err(dal::kCellEnoent));
  CHECK(call(SdataOpen, path_a, 0, out) == err(dal::kCellEnoent));
  CHECK(dalRead32(base, out) == 0xDEADBEEF);
  int sdata_lines = 0;
  for (const std::string& line : lines) {
    sdata_lines += line.find("sdata open not supported: /dev_bdvd/b.txt") != std::string::npos;
  }
  CHECK(sdata_lines == 1);  // logged once per path
  // A plain open through the sdata argument (0x180, 0x10) takes the same route.
  dalWrite32(base, arg, 0x180);
  dalWrite32(base, arg + 4, 0x10);
  CHECK(call(Open, path_a, 0, out, arg, 8) == err(dal::kCellEnoent));
  CHECK(dalRead32(base, out) == 0xDEADBEEF);

  // With a cache directory the decrypted copy at the same relative path is served read-only.
  stdfs::create_directories(root / "cache");
  std::ofstream(root / "cache" / "b.txt", std::ios::binary) << "decrypted!";
  dal::configureSdataCache(root / "cache");
  CHECK(call(SdataOpen, path_a, 0, out) == 0);
  const std::uint32_t sfd = dalRead32(base, out);
  CHECK(sfd != 0xDEADBEEF);
  CHECK(call(Fstat, sfd, sb) == 0 && dalRead64(base, sb + 36) == 10 && dalRead32(base, sb) == 0100444);
  CHECK(call(Read, sfd, 2, buf, 4, out) == 0 && dalRead64(base, out) == 4 && std::memcmp(base + buf, "cryp", 4) == 0);
  CHECK(call(Write, sfd, 0, buf, 1, out) == err(dal::kCellEbadf));
  CHECK(call(Close, sfd) == 0);
  CHECK(call(Open, path_a, 0, out, arg, 8) == 0);  // the sdata marker takes the same route
  CHECK(call(Close, dalRead32(base, out)) == 0);
  put(path_a, "/dev_bdvd/sub/nothing.sdat");
  dalWrite32(base, out, 0xDEADBEEF);
  CHECK(call(SdataOpen, path_a, 0, out) == err(dal::kCellEnoent) && dalRead32(base, out) == 0xDEADBEEF);
  put(path_a, "/dev_hdd0/tmp/x.sdat");  // only the game directory is mapped
  CHECK(call(SdataOpen, path_a, 0, out) == err(dal::kCellEnoent));
  dal::configureSdataCache(std::nullopt);

  p.main.free(scratch);
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
  stdfs::remove_all(root, ec);
  dal::configureVfs({}, std::nullopt);
}

void testFifoParser() {
  using dal::FifoParser;
  using dal::FifoStatus;
  constexpr std::uint32_t kBase = 0x1000;

  // Increment method: registers advance by 4 per argument. 0x4feac and 0x80064 are the headers
  // the cellGcmSys flip functions write.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0x1d6c | (3u << 18), 10, 11, 12, 0x4feac, 2, 0x80064, 0x50, 0x77});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, kBase + 36, 100, sink) == FifoStatus::Idle);
    CHECK(get == kBase + 36);
    CHECK((sink.writes == Writes{{0x1d6c, 10}, {0x1d70, 11}, {0x1d74, 12}, {0xfeac, 2}, {0x64, 0x50}, {0x68, 0x77}}));
    CHECK(parser.headersParsed() == 3);
    CHECK(parser.run(get, kBase + 36, 100, sink) == FifoStatus::Idle && sink.writes.size() == 6);
  }

  // Non-increment method: every argument goes to the same register. The low bits of put are ignored.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0x40000000u | 0x1800 | (3u << 18), 1, 2, 3, 0x1800 | (1u << 18), 9});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, kBase + 24 + 3, 100, sink) == FifoStatus::Idle && get == kBase + 24);
    CHECK((sink.writes == Writes{{0x1800, 1}, {0x1800, 2}, {0x1800, 3}, {0x1800, 9}}));
  }

  // The method register wraps at 16 bits.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0xfffcu | (2u << 18), 5, 6});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, kBase + 12, 100, sink) == FifoStatus::Idle);
    CHECK((sink.writes == Writes{{0xfffc, 5}, {0x0000, 6}}));
  }

  // NOPs: an all-zero word and a zero-count header of either kind write nothing.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0, 0x40000000u | 0x100, 0x100, 0x1800 | (1u << 18), 4});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, kBase + 20, 100, sink) == FifoStatus::Idle);
    CHECK((sink.writes == Writes{{0x1800, 4}}) && parser.headersParsed() == 1);
  }

  // Old and new jumps move get; put is only compared against get.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0x20000000u | 0x2000});
    sink.put(0x2000, {0x00003000u | 1});
    sink.put(0x3000, {0x1800 | (1u << 18), 7});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, 0x3008, 100, sink) == FifoStatus::Idle && get == 0x3008);
    CHECK((sink.writes == Writes{{0x1800, 7}}));
  }

  // A jump target is only validated when its word is read.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0x20000000u | 0x1ffffffc});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, 0x40, 100, sink) == FifoStatus::Fault);  // 0x1ffffffc is unmapped
    CHECK(get == 0x1ffffffc);
  }

  // Call and return: the return lands after the call word; the same call word can be reused.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0x2000u | 2, 0x1800 | (1u << 18), 100, 0x2000u | 2});
    sink.put(0x2000, {0x1800 | (1u << 18), 200, 0x00020000});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, kBase + 16, 100, sink) == FifoStatus::Idle && get == kBase + 16);
    CHECK((sink.writes == Writes{{0x1800, 200}, {0x1800, 100}, {0x1800, 200}}));
  }

  // A call inside a subroutine and a return without a call are faults that leave get on the word.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0x2000u | 2});
    sink.put(0x2000, {0x3000u | 2});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, 0x4000, 100, sink) == FifoStatus::Fault);
    CHECK(get == 0x2000 && parser.lastFault().find("inside a subroutine") != std::string_view::npos);
    CHECK(parser.run(get, 0x4000, 100, sink) == FifoStatus::Fault && get == 0x2000);  // sticky
    parser.reset();
    CHECK(parser.lastFault().empty());
    get = kBase;
    CHECK(parser.run(get, 0x4000, 100, sink) == FifoStatus::Fault && get == 0x2000);  // the nested call again

    FakeSink sink2;
    FifoParser parser2;
    sink2.put(kBase, {0x00020000});
    get = kBase;
    CHECK(parser2.run(get, kBase + 4, 100, sink2) == FifoStatus::Fault && get == kBase);
    CHECK(parser2.lastFault().find("without a call") != std::string_view::npos);
  }

  // Headers that are neither a method nor flow control: no write, get stays on the header.
  for (const std::uint32_t bad : {0x80000000u, 0xa0001800u | (1u << 18), 0x00010000u, 0x00030000u, 0x00060000u,
                                  0x60000000u | 0x1800}) {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0x1800 | (1u << 18), 1, bad});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, kBase + 12, 100, sink) == FifoStatus::Fault);
    CHECK(get == kBase + 8 && sink.writes.size() == 1);
    CHECK(parser.lastFault().find("invalid header") != std::string_view::npos);
  }

  // Unaligned get.
  {
    FakeSink sink;
    FifoParser parser;
    std::uint32_t get = kBase + 2;
    CHECK(parser.run(get, kBase + 16, 100, sink) == FifoStatus::Fault && get == kBase + 2);
  }

  // A refused write stops with Waiting and the next run retries the same write before reading on.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0x68 | (2u << 18), 0x11, 0x22, 0x1800 | (1u << 18), 3});
    sink.refuse = 2;
    std::uint32_t get = kBase;
    CHECK(parser.run(get, kBase + 20, 100, sink) == FifoStatus::Waiting);
    CHECK(get == kBase + 8 && sink.writes.empty());  // the argument is consumed, the write is kept
    CHECK(parser.run(get, kBase + 20, 100, sink) == FifoStatus::Waiting && sink.attempts == 2);
    CHECK(parser.run(get, kBase + 20, 100, sink) == FifoStatus::Idle && get == kBase + 20);
    CHECK((sink.writes == Writes{{0x68, 0x11}, {0x6c, 0x22}, {0x1800, 3}}));
    CHECK(sink.attempts == 5);
  }

  // Budget counts words read; the method register keeps advancing across runs.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0x1d6c | (5u << 18), 1, 2, 3, 4, 5, 0x1800 | (1u << 18), 6});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, kBase + 32, 3, sink) == FifoStatus::Budget && get == kBase + 12);
    CHECK((sink.writes == Writes{{0x1d6c, 1}, {0x1d70, 2}}));
    CHECK(parser.run(get, kBase + 32, 100, sink) == FifoStatus::Idle && get == kBase + 32);
    CHECK((sink.writes == Writes{{0x1d6c, 1}, {0x1d70, 2}, {0x1d74, 3}, {0x1d78, 4}, {0x1d7c, 5}, {0x1800, 6}}));
    CHECK(parser.run(get, kBase + 32, 0, sink) == FifoStatus::Idle);  // nothing left: Idle wins over Budget
    sink.put(kBase + 32, {0x1800 | (1u << 18), 8});
    CHECK(parser.run(get, kBase + 40, 0, sink) == FifoStatus::Budget && get == kBase + 32);
  }

  // put in the middle of a method: Idle with the arguments still owed, then it resumes.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0x1d6c | (3u << 18), 1, 2, 3});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, kBase + 8, 100, sink) == FifoStatus::Idle && get == kBase + 8);
    CHECK((sink.writes == Writes{{0x1d6c, 1}}));
    CHECK(parser.run(get, kBase + 16, 100, sink) == FifoStatus::Idle && get == kBase + 16);
    CHECK((sink.writes == Writes{{0x1d6c, 1}, {0x1d70, 2}, {0x1d74, 3}}));
  }

  // Unmapped reads: get stays on the unreadable word, the fault is sticky.
  {
    FakeSink sink;
    FifoParser parser;
    std::uint32_t get = 0x5000;
    CHECK(parser.run(get, 0x5010, 100, sink) == FifoStatus::Fault && get == 0x5000);
    CHECK(parser.lastFault().find("unmapped") != std::string_view::npos);

    FakeSink sink2;
    FifoParser parser2;
    sink2.put(kBase, {0x1800 | (2u << 18), 1});  // the second argument is unmapped
    get = kBase;
    CHECK(parser2.run(get, kBase + 12, 100, sink2) == FifoStatus::Fault && get == kBase + 8);
    CHECK((sink2.writes == Writes{{0x1800, 1}}));
  }

  // Jumping to itself parks the puller as Idle without moving get; rewriting the word resumes.
  {
    FakeSink sink;
    FifoParser parser;
    sink.put(kBase, {0, 0x20000000u | (kBase + 4)});
    std::uint32_t get = kBase;
    CHECK(parser.run(get, 0x9000, 100, sink) == FifoStatus::Idle && get == kBase + 4);
    CHECK(parser.run(get, 0x9000, 100, sink) == FifoStatus::Idle && get == kBase + 4);
    sink.put(kBase + 4, {0x1800 | (1u << 18), 5, 0x00000001u | 0x4000});
    sink.put(0x4000, {0x1800 | (1u << 18), 6});
    CHECK(parser.run(get, 0x4008, 100, sink) == FifoStatus::Idle && get == 0x4008);
    CHECK((sink.writes == Writes{{0x1800, 5}, {0x1800, 6}}));
  }
}

std::vector<std::pair<dal::RsxInterrupt, std::uint32_t>> g_interrupts;
void recordInterrupt(dal::RsxInterrupt kind, std::uint32_t arg) { g_interrupts.emplace_back(kind, arg); }

dal::RsxCensus fakeRsxCensus() {
  return {120, 3, {{0x1d6c, 5, "NV4097_SET_SEMAPHORE_OFFSET"}, {0x1808, 9, ""}, {0x1800, 5, ""}}};
}

dal::RsxCensus fakeRsxCensusWithHandlerCalls() {
  dal::RsxCensus census = fakeRsxCensus();
  census.vblank_handler_runs = 600;
  census.user_handler_runs = 2;
  return census;
}

void testRsxConsumer() {
  using dal::FifoStatus;
  using Interrupts = std::vector<std::pair<dal::RsxInterrupt, std::uint32_t>>;
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetRsxState();
  dal::setRsxInterruptHook(nullptr);
  g_interrupts.clear();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  const std::uint32_t area = p.user1m.allocate(0x400000, 0x100000);
  CHECK(scratch != 0 && area != 0);
  CHECK(dal::gcmInitBody(base, scratch, 0x10000, 0x100000, area, 0x200000) == 0);

  constexpr std::uint32_t kControl = 0x10100040, kLabels = 0x10300000, kDevice = 0x10000000;
  constexpr std::uint32_t kStart = 0x1000;
  dal::RsxConsumer consumer(base);
  std::uint32_t at = kStart;
  const auto emit = [&](std::uint32_t method, std::initializer_list<std::uint32_t> args) {
    dalWrite32(base, area + at, method | (static_cast<std::uint32_t>(args.size()) << 18));
    at += 4;
    for (const std::uint32_t arg : args) {
      dalWrite32(base, area + at, arg);
      at += 4;
    }
  };
  const auto run = [&] {
    dalWrite32(base, kControl, at);
    return consumer.pump(100);
  };
  const auto label = [&](std::uint32_t offset) { return dalRead32(base, kLabels + offset); };

  // Back-end release swaps the red and blue bytes, texture-read release stores the value as is;
  // an unaligned offset is ignored.
  emit(dal::kMethodNv4097SetSemaphoreOffset, {0x20});
  emit(dal::kMethodNv4097BackEndWriteSemaphoreRelease, {0x11223344});
  CHECK(run() == FifoStatus::Idle);
  CHECK(label(0x20) == 0x11443322);
  CHECK(dalRead32(base, kControl + 4) == at);  // get follows put
  emit(dal::kMethodNv4097SetSemaphoreOffset, {0x40});
  emit(dal::kMethodNv4097TextureReadSemaphoreRelease, {0xAABBCCDD});
  emit(dal::kMethodNv4097SetSemaphoreOffset, {0x14});
  emit(dal::kMethodNv4097TextureReadSemaphoreRelease, {0x99});
  CHECK(run() == FifoStatus::Idle);
  CHECK(label(0x40) == 0xAABBCCDD && label(0x14) == 0x1337BABE);
  CHECK(countContaining(lines, "unaligned offset 0x14") == 1);
  CHECK(!consumer.hasWork(at) && consumer.hasWork(at + 4));

  // The reference value goes to the control register.
  emit(dal::kMethodNv406eSetReference, {0x1234});
  CHECK(run() == FifoStatus::Idle && dalRead32(base, kControl + 8) == 0x1234);

  // An acquire that is not satisfied keeps get at its header; it completes once the label matches.
  const std::uint32_t acquire_at = at;
  emit(dal::kMethodNv406eSemaphoreOffset, {0x80, 0x5555});
  CHECK(run() == FifoStatus::Waiting);
  CHECK(dalRead32(base, kControl + 4) == acquire_at && consumer.hasWork(at));
  CHECK(run() == FifoStatus::Waiting);
  dalWrite32(base, kLabels + 0x80, 0x5555);
  CHECK(run() == FifoStatus::Idle && dalRead32(base, kControl + 4) == at);

  // NV406E release writes through the semaphore DMA; a zero into the device ready word becomes 1.
  emit(dal::kMethodNv406eSemaphoreOffset, {0x84});
  emit(dal::kMethodNv406eSemaphoreRelease, {0x777});
  dalWrite32(base, kDevice + 0x30, 5);
  emit(dal::kMethodNv406eSetContextDmaSemaphore, {0x56616661});
  emit(dal::kMethodNv406eSemaphoreOffset, {0x30});
  emit(dal::kMethodNv406eSemaphoreRelease, {0});
  CHECK(run() == FifoStatus::Idle);
  CHECK(label(0x84) == 0x777 && dalRead32(base, kDevice + 0x30) == 1);

  // Reports carry an increasing timer in nanoseconds and a zero value.
  emit(dal::kMethodNv4097GetReport, {0x00000010});
  CHECK(run() == FifoStatus::Idle);
  const std::uint32_t report = kLabels + 0x1400;
  const std::uint64_t first = dalRead64(base, report + 0x10);
  CHECK(first != 0 && dalRead32(base, report + 0x18) == 0 && dalRead32(base, report + 0x1c) == 0);
  CHECK(dalRead64(base, report + 0x20) == 0xFFFFFFFFFFFFFFFF);
  emit(dal::kMethodNv4097GetReport, {0x00000020});
  CHECK(run() == FifoStatus::Idle && dalRead64(base, report + 0x20) > first);

  // A flip with vsync waits for a VBlank; it clears the flip label and notifies the hook.
  dal::RsxState& state = dal::rsxState();
  state.flip_status = dal::kRsxFlipStatusWaiting;
  dalWrite32(base, kLabels + 0x10, 0x1111);
  dal::setRsxInterruptHook(&recordInterrupt);
  emit(dal::kMethodGcmFlipCommand, {1});
  CHECK(run() == FifoStatus::Waiting && consumer.census().flips == 0);
  CHECK(state.flip_status == dal::kRsxFlipStatusWaiting && label(0x10) == 0x1111);
  consumer.vblankTick();
  CHECK(state.vblank_count == 1);
  CHECK(consumer.pump(100) == FifoStatus::Idle);
  CHECK(consumer.census().flips == 1 && state.flip_status == dal::kRsxFlipStatusDone && label(0x10) == 0);
  CHECK((g_interrupts == Interrupts{{dal::RsxInterrupt::VBlank, 1}, {dal::RsxInterrupt::Flip, 1}}));
  CHECK(countContaining(lines, "dal: rsx: first flip") == 1);
  // Without vsync it completes at once.
  state.requested_vsync = false;
  emit(dal::kMethodGcmFlipCommand, {2});
  CHECK(run() == FifoStatus::Idle && consumer.census().flips == 2);

  // User commands reach the hook; without one they are only logged.
  g_interrupts.clear();
  emit(dal::kMethodGcmSetUserCommand, {7});
  emit(dal::kMethodGcmSetUserCommand + 4, {9});
  CHECK(run() == FifoStatus::Idle);
  CHECK((g_interrupts == Interrupts{{dal::RsxInterrupt::User, 7}, {dal::RsxInterrupt::User, 9}}));
  dal::setRsxInterruptHook(nullptr);
  emit(dal::kMethodGcmSetUserCommand, {3});
  CHECK(run() == FifoStatus::Idle);
  CHECK(countContaining(lines, "pending user command cause 3") == 1);

  // The census counts completed writes by register: the waiting flip counts once.
  const dal::RsxCensus census = consumer.census();
  const auto countOf = [&](std::uint32_t method) -> std::uint64_t {
    for (const auto& m : census.methods) {
      if (m.method == method) return m.count;
    }
    return 0;
  };
  CHECK(countOf(dal::kMethodGcmFlipCommand) == 2 && countOf(dal::kMethodGcmSetUserCommand) == 2);
  CHECK(countOf(dal::kMethodNv4097SetSemaphoreOffset) == 3 && countOf(dal::kMethodNv4097GetReport) == 2);
  CHECK(countOf(0x1234) == 0 && census.headers > 10);

  // A jump into unmapped IO space faults once; after that nothing is consumed.
  dalWrite32(base, area + at, 0x20000000 | 0x0F000000);
  at += 4;
  CHECK(run() == FifoStatus::Fault && consumer.faulted());
  CHECK(countContaining(lines, "command fifo fault") == 1);
  CHECK(run() == FifoStatus::Fault && countContaining(lines, "command fifo fault") == 1);
  CHECK(!consumer.hasWork(at + 4));

  // The census section is printed only while a provider is registered.
  CHECK(dal::formatCensus().find("census: rsx") == std::string::npos);
  dal::setRsxCensusProvider(&fakeRsxCensus);
  const std::string text = dal::formatCensus();
  CHECK(text.find("census: rsx 120 headers, 3 distinct methods, 3 flips\n"
                  "  method 0x1808: 9\n"
                  "  method 0x1800: 5\n"
                  "  method 0x1d6c NV4097_SET_SEMAPHORE_OFFSET: 5\n") != std::string::npos);
  CHECK(text.find("interrupt handler calls") == std::string::npos);
  dal::setRsxCensusProvider(&fakeRsxCensusWithHandlerCalls);
  CHECK(dal::formatCensus().find("census: rsx 120 headers, 3 distinct methods, 3 flips\n"
                                 "  interrupt handler calls: vblank 600, flip 0, user 2\n"
                                 "  method 0x1808: 9\n") != std::string::npos);

  dal::setRsxInterruptHook(nullptr);
  dal::resetRsxState();
  p.user1m.free(area);
  p.main.free(scratch);
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

void testCellGcmSys() {
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetRsxState();
  dal::resetHostFunctions();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  const std::uint32_t area = p.user1m.allocate(0x400000, 0x100000);
  CHECK(scratch != 0 && area != 0 && (area & 0xfffff) == 0);
  const std::uint32_t out = scratch, config = scratch + 0x100, stack = scratch + 0x9000;
  const std::uint32_t io_ea = area, io_size = 0x100000, local_size = 0x200000;
  constexpr std::uint32_t kFailure = 0x802100ff, kNoIoPageTable = 0x80210001, kInval = 0x80010002;

  enum Import : std::uint32_t {
    TiledPitch, InitBody, AddressToOffset, Func15, TimeStamp, CommandWords, SegmentWords, FifoSize,
    MapMain, ControlRegister, GetConfig, UnmapEa, SetUserHandler, SetFlipCommand, BindTile, SetFlipMode,
    GetLastFlipTime, BindZcull, SetFlipHandler, SetDisplayBuffer, UnbindZcull, SetVBlankHandler,
    ResetFlipStatus, SetTileInfo, FlipWithLabel, UnbindTile, ImportCount
  };
  const DalImportInfo imports[] = {
      {0x00800000, 0x055bd74d, "cellGcmSys", "cellGcmGetTiledPitchSize"},
      {0x00800010, 0x15bae46b, "cellGcmSys", "_cellGcmInitBody"},
      {0x00800020, 0x21ac3697, "cellGcmSys", "cellGcmAddressToOffset"},
      {0x00800030, 0x3a33c1fd, "cellGcmSys", "_cellGcmFunc15"},
      {0x00800040, 0x5a41c10f, "cellGcmSys", "cellGcmGetTimeStamp"},
      {0x00800050, 0x5e2ee0f0, "cellGcmSys", "cellGcmGetDefaultCommandWordSize"},
      {0x00800060, 0x8cdf8c70, "cellGcmSys", "cellGcmGetDefaultSegmentWordSize"},
      {0x00800070, 0x9ba451e4, "cellGcmSys", "cellGcmSetDefaultFifoSize"},
      {0x00800080, 0xa114ec67, "cellGcmSys", "cellGcmMapMainMemory"},
      {0x00800090, 0xa547adde, "cellGcmSys", "cellGcmGetControlRegister"},
      {0x008000a0, 0xe315a0b2, "cellGcmSys", "cellGcmGetConfiguration"},
      {0x008000b0, 0xefd00f54, "cellGcmSys", "cellGcmUnmapEaIoAddress"},
      {0x008000c0, 0x06edea9e, "cellGcmSys", "cellGcmSetUserHandler"},
      {0x008000d0, 0x21397818, "cellGcmSys", "_cellGcmSetFlipCommand"},
      {0x008000e0, 0x4524cccd, "cellGcmSys", "cellGcmBindTile"},
      {0x008000f0, 0x4ae8d215, "cellGcmSys", "cellGcmSetFlipMode"},
      {0x00800100, 0x63387071, "cellGcmSys", "cellGcmGetLastFlipTime"},
      {0x00800110, 0x9dc04436, "cellGcmSys", "cellGcmBindZcull"},
      {0x00800120, 0xa41ef7e8, "cellGcmSys", "cellGcmSetFlipHandler"},
      {0x00800130, 0xa53d12ae, "cellGcmSys", "cellGcmSetDisplayBuffer"},
      {0x00800140, 0xa75640e8, "cellGcmSys", "cellGcmUnbindZcull"},
      {0x00800150, 0xa91b0402, "cellGcmSys", "cellGcmSetVBlankHandler"},
      {0x00800160, 0xb2e761d4, "cellGcmSys", "cellGcmResetFlipStatus"},
      {0x00800170, 0xbd100dbc, "cellGcmSys", "cellGcmSetTileInfo"},
      {0x00800180, 0xd8f88e1a, "cellGcmSys", "_cellGcmSetFlipCommandWithWaitLabel"},
      {0x00800190, 0xd9b7653e, "cellGcmSys", "cellGcmUnbindTile"},
  };
  dal::installImports(imports, ImportCount);
  auto a = makeCaller(0x01000000);
  std::lock_guard<dal::GuestLock> guard(dal::guestLock());
  DalPpuContext* ctx = &a->context();
  ctx->gpr[1] = stack;
  const auto call = [&](std::uint32_t index, std::initializer_list<std::uint64_t> args) {
    int reg = 3;
    for (const std::uint64_t value : args) ctx->gpr[reg++] = value;
    dalHleCall(ctx, base, index);
    return static_cast<std::uint32_t>(ctx->gpr[3]);
  };
  const auto offsetOf = [&](std::uint32_t address, std::uint32_t& offset) {
    dalWrite32(base, out, 0xdeadbeef);
    const std::uint32_t result = call(AddressToOffset, {address, out});
    offset = dalRead32(base, out);
    return result;
  };

  // Before initialisation the configuration is zero and nothing is mappable.
  std::memset(base + config, 0xaa, 0x20);
  call(GetConfig, {config});
  CHECK(dalRead32(base, config) == 0 && dalRead32(base, config + 8) == 0 && dalRead32(base, config + 0x14) == 0);
  std::uint32_t offset = 0;
  CHECK(offsetOf(0xc0000000, offset) == kFailure && offset == 0xdeadbeef);
  CHECK(call(TiledPitch, {0x1400}) == 0x1400);

  // Handlers are ignored and the zcull request is refused before initialisation.
  call(SetFlipHandler, {0x1234});
  CHECK(dal::rsxState().flip_handler == 0);
  CHECK(call(BindZcull, {0, 0x1000, 0x500, 0x300, 0, 2, 0, 0}) == kInval);

  // A failing init releases what it committed and can be retried.
  CHECK(dal::gcmInitBody(base, out, 0x10000, io_size, io_ea + 0x1000, local_size) == kFailure);
  CHECK(!dal::rsxState().initialized && !p.memory.isCommitted(dal::kRsxDeviceAddr, 4));
  CHECK(dal::gcmInitBody(base, out, 0x10000, io_size, 0x2f000000, local_size) == kFailure);  // uncommitted

  CHECK(dal::gcmInitBody(base, out, 0x10000, io_size, io_ea, local_size) == 0);
  CHECK(dal::gcmInitBody(base, out, 0x10000, io_size, io_ea, local_size) == kFailure);  // second init
  const dal::RsxState& state = dal::rsxState();
  const std::uint32_t callback = state.wrap_callback;
  CHECK(callback != 0);
  CHECK(dalRead32(base, out) == 0x10000000);
  CHECK(dalRead32(base, 0x10000000) == io_ea + 0x1000);
  CHECK(dalRead32(base, 0x10000004) == io_ea + 0x8000 - 4);
  CHECK(dalRead32(base, 0x10000008) == io_ea + 0x1000);
  CHECK(dalRead32(base, 0x1000000c) == callback);
  CHECK(dalRead32(base, 0x10000030) == 1);
  CHECK(dalRead32(base, 0x10100040) == 0x1000 && dalRead32(base, 0x10100044) == 0x1000);
  CHECK(dalRead32(base, 0x10100048) == 0xffffffff);
  CHECK(dalRead32(base, 0x10200008) == local_size && dalRead32(base, 0x10200010) == 500000000);
  CHECK(dalRead32(base, 0x1020002c) == 0x1000 && dalRead32(base, 0x10200034) == 0x1400);
  CHECK(dalRead32(base, 0x10300000) == 0x1337c0d3 && dalRead32(base, 0x1030000c) == 0x1337f001);
  CHECK(dalRead32(base, 0x10300ffc) == 0x1337f001);
  CHECK(dalRead64(base, 0x10301000) == ~std::uint64_t{0} && dalRead64(base, 0x10301008) == 0);
  CHECK(dalRead64(base, 0x10301400) == ~std::uint64_t{0} && dalRead32(base, 0x10301408) == 0);
  CHECK(dalRead32(base, 0x1030140c) == 0xffffffff);
  CHECK(p.memory.isCommitted(0xc0000000, local_size) && !p.memory.isCommitted(0xc0000000 + local_size, 0x1000));

  std::memset(base + config, 0xaa, 0x20);
  call(GetConfig, {config});
  CHECK(dalRead32(base, config + 0x00) == 0xc0000000 && dalRead32(base, config + 0x04) == io_ea);
  CHECK(dalRead32(base, config + 0x08) == local_size && dalRead32(base, config + 0x0c) == io_size);
  CHECK(dalRead32(base, config + 0x10) == 650000000 && dalRead32(base, config + 0x14) == 500000000);
  CHECK(dalRead8(base, config + 0x18) == 0xaa);

  CHECK(call(ControlRegister, {}) == 0x10100040);
  CHECK(call(CommandWords, {}) == 0x400 && call(SegmentWords, {}) == 0x100);
  CHECK(call(FifoSize, {0x20000, 0x1000}) == 0);
  ctx->gpr[3] = 0x1234;
  call(Func15, {0x10000000});
  CHECK(ctx->gpr[3] == 0x10000000);  // void: r3 is not written
  CHECK(call(TimeStamp, {0}) == 0xffffffff && ctx->gpr[3] == ~std::uint64_t{0});
  CHECK(call(TimeStamp, {2047}) == 0xffffffff);
  CHECK(call(TimeStamp, {2048}) == 0 && ctx->gpr[3] == 0);

  // cellGcmGetTiledPitchSize
  CHECK(call(TiledPitch, {0}) == 0 && call(TiledPitch, {1}) == 0x200);
  CHECK(call(TiledPitch, {0x200}) == 0x200 && call(TiledPitch, {0x201}) == 0x300);
  CHECK(call(TiledPitch, {0x1400}) == 0x1400 && call(TiledPitch, {0x10000}) == 0x10000);
  CHECK(call(TiledPitch, {0x10001}) == 0);

  // The IO area mapped at init: io page 0. cellGcmMapMainMemory takes the first free pages.
  CHECK(offsetOf(io_ea + 8, offset) == 0 && offset == 8);
  const std::uint32_t ea1 = io_ea + 0x100000, ea2 = io_ea + 0x200000;
  dalWrite32(base, out, 0);
  CHECK(call(MapMain, {ea1, 0x100000, out}) == 0 && dalRead32(base, out) == 0x100000);
  CHECK(call(MapMain, {ea2, 0x200000, out}) == 0 && dalRead32(base, out) == 0x200000);
  CHECK(offsetOf(ea1 + 0x1234, offset) == 0 && offset == 0x101234);
  CHECK(offsetOf(ea2 + 0x1fffff, offset) == 0 && offset == 0x3fffff);
  std::uint32_t ea = 0;
  CHECK(dal::rsxIoToEa(0x101234, ea) && ea == ea1 + 0x1234);
  CHECK(!dal::rsxIoToEa(0x400000, ea) && !dal::rsxIoToEa(0x10000000, ea));

  // Rejected mappings write nothing.
  dalWrite32(base, out, 0xdeadbeef);
  CHECK(call(MapMain, {ea1, 0, out}) == kFailure);
  CHECK(call(MapMain, {ea1 + 0x1000, 0x100000, out}) == kFailure);
  CHECK(call(MapMain, {ea1, 0x180000, out}) == kFailure);
  CHECK(call(MapMain, {0x2f000000, 0x100000, out}) == kInval);  // not committed guest memory
  CHECK(call(MapMain, {ea1, 0x10000000, out}) == kNoIoPageTable);
  CHECK(dalRead32(base, out) == 0xdeadbeef);

  // Local memory, unmapped addresses and the end of the local range.
  CHECK(offsetOf(0xc0000000, offset) == 0 && offset == 0);
  CHECK(offsetOf(0xc0000000 + local_size - 4, offset) == 0 && offset == local_size - 4);
  CHECK(offsetOf(0xc0000000 + local_size, offset) == kFailure && offset == 0xdeadbeef);
  CHECK(offsetOf(0xcff00000, offset) == kFailure);
  CHECK(offsetOf(0x00010000, offset) == kFailure);

  // cellGcmUnmapEaIoAddress frees the whole mapping that starts at the page of `ea` (low bits are
  // ignored); an address inside a larger mapping is not its start.
  CHECK(call(UnmapEa, {ea2 + 0x100000}) == kFailure);
  CHECK(call(UnmapEa, {ea2 + 0x1234}) == 0);
  CHECK(offsetOf(ea2, offset) == kFailure && offsetOf(ea2 + 0x100000, offset) == kFailure);
  CHECK(call(UnmapEa, {ea2}) == kFailure);
  CHECK(call(UnmapEa, {0xc0000000}) == kFailure);
  CHECK(call(UnmapEa, {ea1}) == 0);
  CHECK(call(MapMain, {ea2 + 0x100000, 0x100000, out}) == 0 && dalRead32(base, out) == 0x100000);
  CHECK(offsetOf(ea2 + 0x100000, offset) == 0 && offset == 0x100000);

  // The wrap callback flushes put, jumps to the next fragment and wraps around after the last.
  const std::uint32_t context = dal::kRsxDeviceAddr;
  const auto wrap = [&] { return dal::callGuestFunction(ctx, base, callback, {context, 0}); };
  dalWrite32(base, context + 8, io_ea + 0x1010);
  dalWrite32(base, 0x10100044, 0x1000);
  CHECK(wrap() == 0);
  CHECK(dalRead32(base, 0x10100040) == 0x1010);
  CHECK(dalRead32(base, io_ea + 0x1010) == (0x20000000 | 0x8000));
  CHECK(dalRead32(base, context) == io_ea + 0x8000 && dalRead32(base, context + 8) == io_ea + 0x8000);
  CHECK(dalRead32(base, context + 4) == io_ea + 0x10000 - 4);
  CHECK(ctx->gpr[1] == stack);

  dalWrite32(base, 0x10100044, 0x9000);  // the consumer is past the new fragment
  dalWrite32(base, context + 8, io_ea + 0x8040);
  CHECK(wrap() == 0);
  CHECK(dalRead32(base, 0x10100040) == 0x8040);
  CHECK(dalRead32(base, io_ea + 0x8040) == (0x20000000 | 0x1000));
  CHECK(dalRead32(base, context) == io_ea + 0x1000 && dalRead32(base, context + 8) == io_ea + 0x1000);
  CHECK(dalRead32(base, context + 4) == io_ea + 0x8000 - 4);

  // With get inside the new fragment the callback waits until the consumer moves it.
  dalWrite32(base, 0x10100044, 0x8010);
  dalWrite32(base, context + 8, io_ea + 0x1020);
  std::thread consumer([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::lock_guard<dal::GuestLock> lock(dal::guestLock());
    dalWrite32(dal::process().memory.base(), 0x10100044, 0x1020);
    dal::rsxGetAdvanced().notify_all();
  });
  const auto started = std::chrono::steady_clock::now();
  CHECK(wrap() == 0);
  CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(80));
  CHECK(dalRead32(base, context) == io_ea + 0x8000);
  consumer.join();

  // Display buffers, flip mode and status, handlers.
  CHECK(call(SetDisplayBuffer, {1, 0x100, 0x2000, 1280, 720}) == 0);
  CHECK(state.display_count == 2 && state.display[1].offset == 0x100 && state.display[1].pitch == 0x2000);
  CHECK(state.display[1].width == 1280 && state.display[1].height == 720);
  CHECK(call(SetDisplayBuffer, {8, 1, 2, 3, 4}) == kFailure && state.display_count == 2);
  CHECK(call(SetDisplayBuffer, {0x101, 1, 2, 3, 4}) == 0 && state.display[1].offset == 1);  // 8-bit id
  call(SetFlipMode, {1});
  CHECK(!state.requested_vsync);
  call(SetFlipMode, {2});
  CHECK(state.requested_vsync);
  CHECK(state.flip_status == dal::kRsxFlipStatusDone);
  call(ResetFlipStatus, {});
  CHECK(state.flip_status == dal::kRsxFlipStatusWaiting);
  call(GetLastFlipTime, {});
  CHECK(ctx->gpr[3] == state.last_flip_time);
  call(SetFlipHandler, {0x1111});
  call(SetVBlankHandler, {0x2222});
  call(SetUserHandler, {0x3333});
  CHECK(state.flip_handler == 0x1111 && state.vblank_handler == 0x2222 && state.user_handler == 0x3333);

  // Old-API flip commands: header = method | count << 18 at `current`, no put update.
  const std::uint32_t cmd = io_ea + 0x2000;
  dalWrite32(base, context + 8, cmd);
  dalWrite32(base, 0x10100040, 0x4444);
  CHECK(call(SetFlipCommand, {context, 3}) == context);  // void: r3 is not written
  CHECK(dalRead32(base, cmd) == 0x4feac && dalRead32(base, cmd + 4) == 3);
  CHECK(dalRead32(base, context + 8) == cmd + 8 && dalRead32(base, 0x10100040) == 0x4444);
  dalWrite32(base, cmd + 8, 0xdeadbeef);
  call(SetFlipCommand, {context, 8});
  CHECK(dalRead32(base, cmd + 8) == 0xdeadbeef && dalRead32(base, context + 8) == cmd + 8);
  call(FlipWithLabel, {context, 2, 5, 0x77});
  CHECK(dalRead32(base, cmd + 8) == 0x80064 && dalRead32(base, cmd + 12) == 0x50 && dalRead32(base, cmd + 16) == 0x77);
  CHECK(dalRead32(base, cmd + 20) == 0x4feac && dalRead32(base, cmd + 24) == 2);
  CHECK(dalRead32(base, context + 8) == cmd + 28);
  call(FlipWithLabel, {context, 9, 1, 2});  // the label wait is written, the flip is refused
  CHECK(dalRead32(base, cmd + 28) == 0x80064 && dalRead32(base, context + 8) == cmd + 40);
  CHECK(countContaining(lines, "display buffer id 8 is out of range") == 1);
  CHECK(countContaining(lines, "display buffer id 9 is out of range") == 1);

  // Tiles: validation order and error codes of RPCS3.
  constexpr std::uint32_t kInvalidEnum = 0x80210002, kInvalidValue = 0x80210003, kInvalidAlign = 0x80210004;
  CHECK(call(SetTileInfo, {5, 0, 0, 0x3a0000, 0x1400, 7, 0, 0}) == 0);
  CHECK(state.tiles[5].size == 0x3a0000 && state.tiles[5].pitch == 0x1400 && state.tiles[5].comp == 7);
  CHECK(!state.tiles[5].bound);
  CHECK(call(BindTile, {5}) == 0 && state.tiles[5].bound);
  CHECK(call(UnbindTile, {5}) == 0 && !state.tiles[5].bound);
  CHECK(call(BindTile, {15}) == kInvalidValue && call(UnbindTile, {15}) == kInvalidValue);
  CHECK(call(SetTileInfo, {15, 0, 0, 0x10000, 0x100, 0, 0, 0}) == kInvalidValue);
  CHECK(call(SetTileInfo, {1, 0, 0, 0x10000, 0x100, 0, 2048, 0}) == kInvalidValue);
  CHECK(call(SetTileInfo, {1, 0, 0, 0x10000, 0x100, 0, 0, 4}) == kInvalidValue);
  CHECK(call(SetTileInfo, {1, 0, 0x10, 0x10000, 0x100, 0, 0, 0}) == kInvalidAlign);
  CHECK(call(SetTileInfo, {1, 0, 0, 0x10001, 0x100, 0, 0, 0}) == kInvalidAlign);
  CHECK(call(SetTileInfo, {1, 0, 0, 0x10000, 0x80, 0, 0, 0}) == kInvalidAlign);
  CHECK(call(SetTileInfo, {1, 2, 0, 0x10000, 0x100, 0, 0, 0}) == kInvalidEnum);
  CHECK(call(SetTileInfo, {1, 0, 0, 0x10000, 0x100, 6, 0, 0}) == kInvalidEnum);
  CHECK(call(SetTileInfo, {1, 0, 0, 0x10000, 0x100, 13, 0, 0}) == kInvalidEnum);
  CHECK(call(SetTileInfo, {1, 1, 0, 0x10000, 0x100, 12, 2047, 3}) == 0 && state.tiles[1].bank == 3);
  CHECK(call(SetTileInfo, {0x101, 0, 0, 0x10000, 0x100, 0, 0, 0}) == 0);  // 8-bit index: tile 1

  // Zcull: the last four arguments (zCullFormat, sFunc, sRef, sMask) come from the stack.
  const auto bindZcull = [&](std::uint32_t index, std::uint32_t offset, std::uint32_t width, std::uint32_t height,
                             std::uint32_t cull_start) {
    dalWrite64(base, stack + 0x70, 1);
    dalWrite64(base, stack + 0x78, 3);
    dalWrite64(base, stack + 0x80, 0x41);
    dalWrite64(base, stack + 0x88, 0xff);
    return call(BindZcull, {index, offset, width, height, cull_start, 2, 0, 0});
  };
  CHECK(bindZcull(0, 0x100000, 0x500, 0x300, 0x1000) == 0);
  const dal::RsxZcull& zcull = state.zculls[0];
  CHECK(zcull.bound && zcull.offset == 0x100000 && zcull.width == 0x500 && zcull.height == 0x300);
  CHECK(zcull.cull_start == 0x1000 && zcull.z_format == 2 && zcull.aa_format == 0 && zcull.zcull_dir == 0);
  CHECK(zcull.zcull_format == 1 && zcull.s_func == 3 && zcull.s_ref == 0x41 && zcull.s_mask == 0xff);
  CHECK(bindZcull(1, 0x100000, 0x500, 0x300, 0x1234) == 0 && state.zculls[1].cull_start == 0x1000);
  CHECK(bindZcull(8, 0x100000, 0x500, 0x300, 0) == 0xffffffef);
  CHECK(bindZcull(2, 0x100000, 0, 0x300, 0) == kInval);                // empty region
  CHECK(bindZcull(2, 0x100000, 0x500, 0x300, 0x300000) == kInval);     // beyond the 3 MiB zcull RAM
  CHECK(bindZcull(2, local_size, 0x500, 0x300, 0) == kInval);          // offset outside local memory
  CHECK(!state.zculls[2].bound);
  CHECK(call(UnbindZcull, {0}) == 0 && !zcull.bound);
  CHECK(call(UnbindZcull, {8}) == kInvalidValue);

  CHECK(countContaining(lines, "dal: default hle") == 0);
  CHECK(dal::formatCensus().find("cellGcmSys::cellGcmMapMainMemory: 8 [impl]") != std::string::npos);

  dal::resetRsxState();
  CHECK(!p.memory.isCommitted(0xc0000000, 4) && !dal::rsxState().initialized);
  dal::resetHostFunctions();
  p.user1m.free(area);
  p.main.free(scratch);
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

void testSpurs() {
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetSyncObjects();
  dal::resetSpuThreadGroups();
  dal::resetSpurs();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t attr = scratch, spurs = scratch + 0x4000, out = scratch + 0x8000;
  const std::uint32_t queue_id_out = scratch + 0x8100, queue_attr = scratch + 0x8140;
  const std::uint32_t spup = scratch + 0x8180;
  constexpr std::uint32_t kErrNull = 0x80410711, kErrAlign = 0x80410710, kErrInval = 0x80410702;

  const DalImportInfo imports[] = {
      {0x00800000, 0x95180230, "cellSpurs", "_cellSpursAttributeInitialize"},
      {0x00800010, 0x30aa96c4, "cellSpurs", "cellSpursInitializeWithAttribute2"},
      {0x00800020, 0x39c173fb, "cellSpurs", "cellSpursGetSpuThreadGroupId"},
      {0x00800030, 0xca4c4600, "cellSpurs", "cellSpursFinalize"},
  };
  dal::installImports(imports, 4);
  auto a = makeCaller(0x01000000);
  std::lock_guard<dal::GuestLock> guard(dal::guestLock());
  DalPpuContext* ctx = &a->context();
  const auto call = [&](std::uint32_t index, std::initializer_list<std::uint64_t> args) {
    int reg = 3;
    for (const std::uint64_t value : args) ctx->gpr[reg++] = value;
    dalHleCall(ctx, base, index);
    return static_cast<std::uint32_t>(ctx->gpr[3]);
  };

  // _cellSpursAttributeInitialize: the struct is zeroed, then the seven fields are stored.
  std::memset(base + attr, 0xaa, 512);
  CHECK(call(0, {0, 1, 0x330000, 5, 100, 1000, 1}) == kErrNull);
  CHECK(call(0, {attr + 4, 1, 0x330000, 5, 100, 1000, 1}) == kErrAlign);
  CHECK(dalRead8(base, attr) == 0xaa);  // a rejected call writes nothing
  CHECK(call(0, {attr, 1, 0x330000, 5, 100, 1000, 1}) == 0);
  CHECK(dalRead32(base, attr + 0x00) == 1);
  CHECK(dalRead32(base, attr + 0x04) == 0x330000);
  CHECK(dalRead32(base, attr + 0x08) == 5);
  CHECK(dalRead32(base, attr + 0x0c) == 100);
  CHECK(dalRead32(base, attr + 0x10) == 1000);
  CHECK(dalRead8(base, attr + 0x14) == 1);
  CHECK(dalRead32(base, attr + 0x24) == 0 && dalRead32(base, attr + 0x28) == 0);
  CHECK(dalRead8(base, attr + 0x1ff) == 0);
  CHECK(call(0, {attr, 2, 0x330000, 6, 3, 4, 0}) == 0);
  CHECK(dalRead8(base, attr + 0x14) == 0);

  // cellSpursInitializeWithAttribute2: argument checks first.
  CHECK(call(1, {spurs, 0}) == kErrNull);
  CHECK(call(1, {spurs, attr + 4}) == kErrAlign);
  CHECK(call(1, {0, attr}) == kErrNull);
  CHECK(call(1, {spurs + 0x80 + 8, attr}) == kErrAlign);
  CHECK(call(1, {spurs + 0x10, attr}) == kErrAlign);
  dalWrite32(base, attr, 3);
  CHECK(call(1, {spurs, attr}) == kErrInval);  // revision > 2
  dalWrite32(base, attr, 2);
  dalWrite32(base, attr + 0x24, 16);
  CHECK(call(1, {spurs, attr}) == kErrInval);  // prefix longer than 15 bytes
  dalWrite32(base, attr + 0x24, 3);
  std::memcpy(base + attr + 0x15, "abc", 3);
  dalWrite8(base, attr + 0x14, 1);  // exitIfNoWork
  std::memset(base + spurs, 0x55, 0x2000);
  CHECK(call(1, {spurs, attr}) == 0);

  CHECK(dalRead32(base, spurs + 0xda0) == 2);          // revision
  CHECK(dalRead32(base, spurs + 0xda4) == 0x330000);   // sdkVersion
  CHECK(dalRead32(base, spurs + 0xd80) == 5);          // exit-if-no-work | second version
  CHECK(dalRead32(base, spurs + 0xd84) == 3);          // spuPriority
  CHECK(dalRead32(base, spurs + 0xd88) == 4);          // ppuPriority
  CHECK(dalRead8(base, spurs + 0x76) == 6);            // nSpus
  CHECK(dalRead8(base, spurs + 0x74) == 0xc0);         // 32 workloads | exit if no work
  CHECK(dalRead8(base, spurs + 0xd9b) == 3);           // prefixSize
  CHECK(std::memcmp(base + spurs + 0xd8c, "abc", 3) == 0);
  CHECK(dalRead64(base, spurs + 0xd20) == 0xffffffffull);
  CHECK(dalRead8(base, spurs + 0x1fff) == 0 && dalRead8(base, spurs + 0x500) == 0);  // zeroed
  const std::uint32_t group = dalRead32(base, spurs + 0xd30);
  CHECK(group == 0x04000100);
  CHECK(dal::spuThreadGroupExists(group));
  CHECK(countContaining(lines, "dal: spurs: initialized") == 1);

  // cellSpursGetSpuThreadGroupId writes the id.
  dalWrite32(base, out, 0);
  CHECK(call(2, {spurs, 0}) == kErrNull);
  CHECK(call(2, {0, out}) == kErrNull);
  CHECK(call(2, {spurs + 8, out}) == kErrAlign);
  CHECK(call(2, {spurs, out}) == 0);
  CHECK(dalRead32(base, out) == group);

  // sc 251 accepts the SPURS group id and rejects ids that were never handed out.
  dalWrite32(base, queue_attr, 1);
  dalWrite32(base, queue_attr + 4, 2);
  CHECK(syscallAs(*a, 128, queue_id_out, queue_attr, 0, 2) == 0);
  const std::uint32_t queue = dalRead32(base, queue_id_out);
  CHECK(syscallAs(*a, 251, group, queue, 1, spup) == 0);
  CHECK(syscallAs(*a, 251, group + 0x100, queue, 1, spup) == err(dal::kCellEsrch));

  // A second instance gets the next id; finalize releases only its own group.
  const std::uint32_t spurs2 = scratch + 0x2000;
  CHECK(call(1, {spurs2, attr}) == 0);
  CHECK(dalRead32(base, spurs2 + 0xd30) == group + 0x100);
  CHECK(call(3, {0}) == kErrNull);
  CHECK(call(3, {spurs2 + 8}) == kErrAlign);
  CHECK(call(3, {spurs2}) == 0);
  CHECK(!dal::spuThreadGroupExists(group + 0x100));
  CHECK(dal::spuThreadGroupExists(group));
  CHECK(call(3, {spurs}) == 0);
  CHECK(!dal::spuThreadGroupExists(group));
  CHECK(syscallAs(*a, 251, group, queue, 1, spup) == err(dal::kCellEsrch));

  const std::string census = dal::formatCensus();
  CHECK(census.find("cellSpurs::cellSpursInitializeWithAttribute2: ") != std::string::npos);
  CHECK(countContaining(lines, "dal: default hle") == 0);

  dal::resetSpurs();
  dal::resetSpuThreadGroups();
  p.main.free(scratch);
  dal::resetSyncObjects();
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

void testLwmutex() {
  dal::setLogSink([](std::string_view) {});
  dal::resetCensus();
  dal::resetSyncObjects();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t lw = scratch, lw2 = scratch + 0x20, attr = scratch + 0x40;

  constexpr const char* kModule = "sysPrxForUser";
  const DalImportInfo imports[] = {
      {0x00800000, 0x2f85c0ef, kModule, "sys_lwmutex_create"},
      {0x00800010, 0xc3476d0c, kModule, "sys_lwmutex_destroy"},
      {0x00800020, 0x1573dc3f, kModule, "sys_lwmutex_lock"},
      {0x00800030, 0xaeb78725, kModule, "sys_lwmutex_trylock"},
      {0x00800040, 0x1bc200f4, kModule, "sys_lwmutex_unlock"},
  };
  dal::installImports(imports, 5);
  enum { kCreate, kDestroy, kLock, kTryLock, kUnlock };
  auto call = [&](dal::GuestThread& caller, std::uint32_t index, std::uint64_t r3,
                  std::uint64_t r4 = 0) {
    DalPpuContext* ctx = &caller.context();
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    dalHleCall(ctx, base, index);
    return ctx->gpr[3];
  };
  auto a = makeCaller(0x01000000);
  auto b = makeCaller(0x01000001);

  dalWrite32(base, attr, 2);
  dalWrite32(base, attr + 4, 0x10);
  CHECK(call(*a, kCreate, lw, attr) == 0);
  CHECK(dalRead32(base, lw) == dal::kLwmutexFree);
  CHECK(dalRead32(base, lw + 8) == 0x12);
  CHECK(dalRead32(base, lw + 16) == 0x95000000);

  CHECK(call(*a, kLock, lw, 0) == 0);
  CHECK(dalRead32(base, lw) == 0x01000000);
  CHECK(call(*a, kLock, lw, 0) == 0);  // recursive
  CHECK(dalRead32(base, lw + 12) == 1);
  CHECK(call(*a, kTryLock, lw) == 0);
  CHECK(dalRead32(base, lw + 12) == 2);
  CHECK(call(*b, kTryLock, lw) == err(dal::kCellEbusy));
  CHECK(call(*b, kUnlock, lw) == err(dal::kCellEperm));
  CHECK(call(*a, kDestroy, lw) == err(dal::kCellEbusy));
  CHECK(call(*b, kDestroy, lw) == err(dal::kCellEbusy));
  CHECK(call(*a, kUnlock, lw) == 0);
  CHECK(call(*a, kUnlock, lw) == 0);
  CHECK(dalRead32(base, lw + 12) == 0);
  CHECK(dalRead32(base, lw) == 0x01000000);
  CHECK(call(*a, kUnlock, lw) == 0);
  CHECK(dalRead32(base, lw) == dal::kLwmutexFree);

  // Contention: the second thread blocks until the owner unlocks, and the waiter count shows in
  // the guest struct meanwhile.
  CHECK(call(*a, kLock, lw, 0) == 0);
  std::atomic<bool> b_ready{false};
  std::uint64_t b_result = ~0ull;
  {
    std::lock_guard<dal::GuestLock> guard(dal::guestLock());
    std::thread worker([&] {
      std::lock_guard<dal::GuestLock> worker_guard(dal::guestLock());
      b_ready = true;
      b_result = call(*b, kLock, lw, 0);
    });
    dal::guestLock().unlock();
    while (!b_ready) std::this_thread::yield();
    dal::guestLock().lock();
    CHECK(dalRead32(base, lw + 4) == 1);
    CHECK(call(*a, kUnlock, lw) == 0);
    dal::guestLock().unlock();
    worker.join();
    dal::guestLock().lock();
  }
  CHECK(b_result == 0);
  CHECK(dalRead32(base, lw) == 0x01000001);
  CHECK(dalRead32(base, lw + 4) == 0);
  CHECK(call(*b, kUnlock, lw) == 0);

  // A timed lock on a held lwmutex gives up with ETIMEDOUT.
  CHECK(call(*a, kLock, lw, 0) == 0);
  {
    std::lock_guard<dal::GuestLock> guard(dal::guestLock());
    CHECK(call(*b, kLock, lw, 1000) == err(dal::kCellEtimedout));
  }
  CHECK(call(*a, kUnlock, lw) == 0);

  // Non-recursive: relock is EDEADLK; a destroyed lwmutex is dead.
  dalWrite32(base, attr, 1);
  dalWrite32(base, attr + 4, 0x20);
  CHECK(call(*a, kCreate, lw2, attr) == 0);
  CHECK(call(*a, kLock, lw2, 0) == 0);
  CHECK(call(*a, kLock, lw2, 0) == err(dal::kCellEdeadlk));
  CHECK(call(*a, kUnlock, lw2) == 0);
  CHECK(call(*a, kDestroy, lw2) == 0);
  CHECK(dalRead32(base, lw2) == dal::kLwmutexDead);
  CHECK(call(*a, kLock, lw2, 0) == err(dal::kCellEinval));
  CHECK(call(*a, kTryLock, lw2) == err(dal::kCellEinval));

  dalWrite32(base, attr + 4, 0x30);
  CHECK(call(*a, kCreate, lw2, attr) == err(dal::kCellEinval));
  dalWrite32(base, attr, 3);
  dalWrite32(base, attr + 4, 0x10);
  CHECK(call(*a, kCreate, lw2, attr) == err(dal::kCellEinval));

  // The process-internal objects take the first ids of their kinds.
  dal::resetSyncObjects();
  dal::createProcessSyncObjects(base);
  const dal::ProcessSyncObjects& objects = dal::processSyncObjects();
  CHECK(dalRead32(base, objects.atexit_lwmutex + 16) == 0x95000000);
  CHECK(dalRead32(base, objects.prx_lwmutex + 16) == 0x95000100);
  CHECK(objects.once_mutex == 0x85000000 && objects.exit_mutex == 0x85000100);

  p.main.free(scratch);
  dal::resetSyncObjects();
  dal::setLogSink({});
  dal::resetCensus();
}

// What the entry function of the guest threads below saw; written under the guest lock.
std::uint64_t g_entry_arg, g_entry_r2, g_entry_r4, g_entry_r13;
std::uint32_t g_entry_runs, g_once_runs;

DAL_PPU_FUNC(ppuThreadEntry) {
  g_entry_runs++;
  g_entry_arg = ctx->gpr[3];
  g_entry_r2 = ctx->gpr[2];
  g_entry_r4 = ctx->gpr[4];
  g_entry_r13 = ctx->gpr[13];
  if (ctx->gpr[3] == 5) {
    ctx->gpr[3] = 0x77;
    dalHleCall(ctx, base, 3);  // sys_ppu_thread_exit: does not return
    return;
  }
  ctx->gpr[3] += 100;  // a C return ends the thread with r3 as the value
}

DAL_PPU_FUNC(ppuOnceInit) {
  (void)ctx;
  (void)base;
  g_once_runs++;
}

void testPpuThreads() {
  dal::setLogSink([](std::string_view) {});
  dal::resetCensus();
  dal::resetSyncObjects();
  dal::resetPpuThreadState();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t id_out = scratch, opd_entry = scratch + 0x10, opd_init = scratch + 0x20;
  const std::uint32_t once_ctrl = scratch + 0x30, name = scratch + 0x40, join_out = scratch + 0x80;
  const std::uint32_t tls_image = scratch + 0x100;

  const DalFuncMapping mappings[] = {{0x10200, ppuThreadEntry}, {0x10210, ppuOnceInit}};
  dalRegisterFuncs(mappings, 2);
  dalWrite32(base, opd_entry, 0x10200);
  dalWrite32(base, opd_entry + 4, 0x1234);
  dalWrite32(base, opd_init, 0x10210);
  dalWrite32(base, opd_init + 4, 0x4321);

  constexpr const char* kModule = "sysPrxForUser";
  const DalImportInfo imports[] = {
      {0x00800000, 0x24a1ea07, kModule, "sys_ppu_thread_create"},
      {0x00800010, 0x350d454e, kModule, "sys_ppu_thread_get_id"},
      {0x00800020, 0xa3e3be68, kModule, "sys_ppu_thread_once"},
      {0x00800030, 0xaff080a4, kModule, "sys_ppu_thread_exit"},
  };
  dal::installImports(imports, 4);
  enum { kCreate, kGetId, kOnce };
  auto call = [&](dal::GuestThread& caller, std::uint32_t index, std::uint64_t r3 = 0,
                  std::uint64_t r4 = 0, std::uint64_t r5 = 0, std::uint64_t r6 = 0,
                  std::uint64_t r7 = 0, std::uint64_t r8 = 0, std::uint64_t r9 = 0) {
    DalPpuContext* ctx = &caller.context();
    ctx->gpr[3] = r3;
    ctx->gpr[4] = r4;
    ctx->gpr[5] = r5;
    ctx->gpr[6] = r6;
    ctx->gpr[7] = r7;
    ctx->gpr[8] = r8;
    ctx->gpr[9] = r9;
    dalHleCall(ctx, base, index);
    return ctx->gpr[3];
  };

  // The guest lock is held throughout: blocking calls release it while they wait.
  std::lock_guard<dal::GuestLock> guard(dal::guestLock());
  auto caller = makeCaller(0x0e000000);

  dalWrite32(base, tls_image, 0xCAFEF00D);
  auto boot_ctx = std::make_unique<DalPpuContext>();
  CHECK(dal::initializeTls(boot_ctx.get(), base, 0x01000000, tls_image, 4, 0x20) != 0);
  const std::uint32_t probe = dal::allocateTls();
  dal::freeTls(probe);
  CHECK(probe != 0);

  // Creation errors give the TLS block back.
  CHECK(call(*caller, kCreate, id_out, 0, 1, 500, 0x10000, 1, 0) == err(dal::kCellEfault));
  CHECK(call(*caller, kCreate, id_out, opd_entry, 1, 3072, 0x10000, 1, 0) == err(dal::kCellEinval));
  CHECK(call(*caller, kCreate, id_out, opd_entry, 1, static_cast<std::uint64_t>(-1), 0x10000, 1,
             0) == err(dal::kCellEinval));
  CHECK(call(*caller, kCreate, id_out, opd_entry, 1, 500, 0x10000, 3, 0) == err(dal::kCellEperm));
  CHECK(call(*caller, kCreate, 0, opd_entry, 1, 500, 0x10000, 1, 0) == err(dal::kCellEnomem));
  const std::uint32_t probe2 = dal::allocateTls();
  dal::freeTls(probe2);
  CHECK(probe2 == probe);
  CHECK(g_entry_runs == 0);

  // A joinable thread starts at the descriptor with r3 = arg, r4 = 0, r2 = TOC and its own r13.
  const char long_name[] = "a-thread-name-that-is-far-longer-than-27-characters";
  std::memcpy(base + name, long_name, sizeof(long_name));
  CHECK(call(*caller, kCreate, id_out, opd_entry, 7, 500, 0x10000, 1, name) == 0);
  const auto id = static_cast<std::uint32_t>(dalRead64(base, id_out));
  dal::GuestThread* thread = dal::findThread(id);
  CHECK(thread != nullptr && thread->id() == id);
  if (thread != nullptr) {
    CHECK(thread->name() == std::string(long_name, 27));
    CHECK(thread->joinable() && thread->priority() == 500 && thread->stackSize() == 0x10000);
    CHECK(thread->tlsR13() == probe);
    CHECK(thread->context().gpr[13] == probe && thread->context().gpr[2] == 0x1234);
    CHECK(std::memcmp(base + thread->stackAddr(), "STACKGRD", 8) == 0);
  }
  CHECK(syscallAs(*caller, 44, id, join_out) == 0);
  CHECK(dalRead64(base, join_out) == 107);
  CHECK(g_entry_runs == 1 && g_entry_arg == 7 && g_entry_r4 == 0 && g_entry_r2 == 0x1234);
  CHECK(g_entry_r13 == probe);
  CHECK(syscallAs(*caller, 44, id, join_out) == err(dal::kCellEsrch));  // already joined
  CHECK(syscallAs(*caller, 48, id, join_out) == err(dal::kCellEsrch));

  // Leaving through sys_ppu_thread_exit returns the TLS block and reports the exit value.
  CHECK(call(*caller, kCreate, id_out, opd_entry, 5, 500, 0x4000, 1, 0) == 0);
  const auto id2 = static_cast<std::uint32_t>(dalRead64(base, id_out));
  CHECK(id2 == id + 1);
  CHECK(syscallAs(*caller, 44, id2, join_out) == 0);
  CHECK(dalRead64(base, join_out) == 0x77);
  const std::uint32_t probe3 = dal::allocateTls();
  dal::freeTls(probe3);
  CHECK(probe3 == probe);
  CHECK(syscallAs(*caller, 44, id2, 0) == err(dal::kCellEsrch));

  // A detached thread cannot be joined; a missing result pointer is reported after the join.
  CHECK(call(*caller, kCreate, id_out, opd_entry, 1, 500, 0x4000, 0, 0) == 0);
  const auto id3 = static_cast<std::uint32_t>(dalRead64(base, id_out));
  CHECK(syscallAs(*caller, 44, id3, join_out) == err(dal::kCellEinval));
  CHECK(call(*caller, kCreate, id_out, opd_entry, 2, 500, 0x4000, 1, 0) == 0);
  const auto id4 = static_cast<std::uint32_t>(dalRead64(base, id_out));
  CHECK(syscallAs(*caller, 44, id4, 0) == err(dal::kCellEfault));
  while (dal::findThread(id3)->state() != dal::ThreadState::Exited) {
    syscallAs(*caller, 141, 1000);  // sleeping releases the guest lock
  }
  CHECK(syscallAs(*caller, 47, id3, 100) == err(dal::kCellEsrch));  // detached and gone
  CHECK(g_entry_runs == 4);

  // The interrupt flag creates the thread without starting it.
  CHECK(call(*caller, kCreate, id_out, opd_entry, 1, 500, 0x4000, 2, 0) == 0);
  const auto id5 = static_cast<std::uint32_t>(dalRead64(base, id_out));
  CHECK(dal::findThread(id5) != nullptr &&
        dal::findThread(id5)->state() == dal::ThreadState::Created);
  CHECK(g_entry_runs == 4);

  // join: self and unknown ids.
  CHECK(syscallAs(*caller, 44, 0x0e000000, join_out) == err(dal::kCellEdeadlk));
  CHECK(syscallAs(*caller, 44, 0x0e0000f0, join_out) == err(dal::kCellEsrch));

  // Priority is stored; the caller is not registered, so it is found through its context.
  CHECK(syscallAs(*caller, 47, 0x0e000000, 1234) == 0);
  CHECK(syscallAs(*caller, 48, 0x0e000000, join_out) == 0);
  CHECK(dalRead32(base, join_out) == 1234);
  CHECK(syscallAs(*caller, 47, 0x0e000000, 3072) == err(dal::kCellEinval));
  CHECK(syscallAs(*caller, 47, 0x0e000000, static_cast<std::uint64_t>(-1)) ==
        err(dal::kCellEinval));
  CHECK(syscallAs(*caller, 47, 0x0e0000f0, 100) == err(dal::kCellEsrch));
  CHECK(syscallAs(*caller, 48, 0x0e0000f0, join_out) == err(dal::kCellEsrch));
  CHECK(syscallAs(*caller, 47, id5, 77) == 0);
  CHECK(dal::findThread(id5)->priority() == 77);

  // yield: CELL_CANCEL when no other thread waits for the lock.
  CHECK(syscallAs(*caller, 43) == 1);

  // sys_ppu_thread_get_id writes the id of the calling thread as a BE u64.
  dalWrite64(base, id_out, ~0ull);
  CHECK(call(*caller, kGetId, id_out) == 0);
  CHECK(dalRead64(base, id_out) == 0x0e000000);

  // sys_ppu_thread_once runs the initialiser once and leaves the mutex unlocked.
  dalWrite32(base, once_ctrl, dal::kOnceInit);
  call(*caller, kOnce, once_ctrl, opd_init);
  call(*caller, kOnce, once_ctrl, opd_init);
  CHECK(g_once_runs == 1 && dalRead32(base, once_ctrl) == dal::kDoneInit);
  CHECK(dal::mutexTryLock(&caller->context(), dal::processSyncObjects().once_mutex) == 0);
  CHECK(dal::mutexUnlock(&caller->context(), dal::processSyncObjects().once_mutex) == 0);

  p.main.free(scratch);
  dalRegisterFuncs(nullptr, 0);
  dal::resetSyncObjects();
  dal::resetPpuThreadState();
  dal::setLogSink({});
  dal::resetCensus();
}

struct HandlerCall {
  char tag;
  std::uint64_t r3;
  bool on_service_thread;
};
std::vector<HandlerCall> g_handler_calls;  // written under the guest lock
std::condition_variable_any g_handler_cv;

void recordHandlerCall(char tag, DalPpuContext* ctx) {
  const dal::GuestThread* thread = dal::currentThread();
  g_handler_calls.push_back({tag, ctx->gpr[3],
                             thread != nullptr && thread->service() && thread->name() == "_gcm_intr_thread" &&
                                 ctx->gpr[13] == thread->tlsR13() && ctx->gpr[13] != 0});
  g_handler_cv.notify_all();
}

DAL_PPU_FUNC(gcmVblankHandler) {
  (void)base;
  recordHandlerCall('v', ctx);
}

DAL_PPU_FUNC(gcmUserHandler) {
  (void)base;
  recordHandlerCall('u', ctx);
}

// Waits with the guest lock released until `done` holds; false after five seconds.
template <class Pred>
bool waitUntil(Pred done) {
  return dal::blockingWait(g_handler_cv, done, "test wait", std::chrono::seconds(5));
}

// Lets other guest threads run for a moment.
void pauseUnlocked() {
  std::condition_variable_any cv;
  dal::blockingWait(cv, [] { return false; }, "test pause", std::chrono::milliseconds(2));
}

void testGcmInterruptThread() {
  dal::setLogSink([](std::string_view) {});
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetSyncObjects();
  dal::resetRsxState();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t opd_vblank = scratch, opd_user = scratch + 0x10, tls_image = scratch + 0x100;
  const DalFuncMapping mappings[] = {{0x10400, gcmVblankHandler}, {0x10410, gcmUserHandler}};
  dalRegisterFuncs(mappings, 2);
  dalWrite32(base, opd_vblank, 0x10400);
  dalWrite32(base, opd_vblank + 4, 0x5555);
  dalWrite32(base, opd_user, 0x10410);
  dalWrite32(base, opd_user + 4, 0x6666);
  g_handler_calls.clear();

  std::lock_guard<dal::GuestLock> guard(dal::guestLock());
  auto boot_ctx = std::make_unique<DalPpuContext>();
  dalWrite32(base, tls_image, 0xCAFEF00D);
  CHECK(dal::initializeTls(boot_ctx.get(), base, 0x01000000, tls_image, 4, 0x20) != 0);

  dal::RsxState& state = dal::rsxState();
  state.vblank_handler = opd_vblank;
  state.user_handler = opd_user;  // the flip handler stays unset, like the game's

  const std::uint32_t id = dal::startGcmInterruptThread();
  CHECK(id != 0 && dal::startGcmInterruptThread() == id);
  const dal::GuestThread* thread = dal::findThread(id);
  CHECK(thread != nullptr && thread->service() && thread->name() == "_gcm_intr_thread");
  CHECK(thread != nullptr && thread->priority() == 1 && thread->stackSize() == 0x4000);

  // The thread cannot run while this test holds the guest lock, so everything below queues up:
  // user commands and flips are all delivered in order, vblanks coalesce, a missing handler drops.
  dal::postGcmInterrupt(dal::RsxInterrupt::User, 7);
  dal::postGcmInterrupt(dal::RsxInterrupt::VBlank, 1);
  dal::postGcmInterrupt(dal::RsxInterrupt::VBlank, 1);
  dal::postGcmInterrupt(dal::RsxInterrupt::Flip, 1);  // no flip handler
  dal::postGcmInterrupt(dal::RsxInterrupt::User, 8);
  dal::postGcmInterrupt(dal::RsxInterrupt::VBlank, 1);
  CHECK(waitUntil([] { return g_handler_calls.size() >= 3; }));
  CHECK(g_handler_calls.size() == 3);
  if (g_handler_calls.size() == 3) {
    CHECK(g_handler_calls[0].tag == 'u' && g_handler_calls[0].r3 == 7);
    CHECK(g_handler_calls[1].tag == 'v' && g_handler_calls[1].r3 == 1);
    CHECK(g_handler_calls[2].tag == 'u' && g_handler_calls[2].r3 == 8);
    CHECK(g_handler_calls[0].on_service_thread && g_handler_calls[1].on_service_thread);
  }

  // Once the queue is empty a new tick is accepted again, and the thread idles as a service thread.
  dal::postGcmInterrupt(dal::RsxInterrupt::VBlank, 1);
  CHECK(waitUntil([] { return g_handler_calls.size() >= 4; }));
  for (int i = 0; i < 2500 && thread->state() != dal::ThreadState::Blocked; i++) pauseUnlocked();
  CHECK(thread->state() == dal::ThreadState::Blocked);
  const dal::GcmInterruptCounts counts = dal::gcmInterruptCounts();
  CHECK(counts.vblank == 2 && counts.flip == 0 && counts.user == 2);
  CHECK(dal::formatCensus().find("_gcm_intr_thread: blocked in gcm interrupt wait [service]") !=
        std::string::npos);

  dal::stopGcmInterruptThread();
  CHECK(thread->state() == dal::ThreadState::Exited);
  CHECK(dal::gcmInterruptCounts().vblank == 0);

  dalRegisterFuncs(nullptr, 0);
  dal::resetHostFunctions();
  dal::resetRsxState();
  dal::resetSyncObjects();
  p.main.free(scratch);
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

void testJudgeThreads() {
  using dal::ThreadHealth;
  using dal::ThreadState;
  using dal::ThreadVerdict;
  const auto stalled = [](bool service) {
    return ThreadHealth{ThreadState::Blocked, true, 0, service};
  };
  const ThreadHealth running{ThreadState::Running, false, 0, false};
  const ThreadHealth exited{ThreadState::Exited, false, 0, false};
  std::size_t count = 0;

  // Blocked for under two seconds is not a stall.
  CHECK(dal::judgeThreads({ThreadHealth{ThreadState::Blocked, true, 9000, false}}, 10000) == ThreadVerdict::Ok);
  CHECK(dal::judgeThreads({stalled(false)}, 10000, &count) == ThreadVerdict::Stall && count == 1);
  // A timed wait never counts.
  CHECK(dal::judgeThreads({ThreadHealth{ThreadState::Blocked, false, 0, false}}, 10000) == ThreadVerdict::Ok);
  // A service thread, however it waits, neither keeps the process alive nor hides a stall.
  const ThreadHealth running_service{ThreadState::Running, false, 0, true};
  CHECK(dal::judgeThreads({stalled(false), running_service}, 10000) == ThreadVerdict::Stall);
  CHECK(dal::judgeThreads({stalled(false), stalled(true)}, 10000, &count) == ThreadVerdict::Stall && count == 1);
  CHECK(dal::judgeThreads({stalled(false), running}, 10000) == ThreadVerdict::Ok);
  CHECK(dal::judgeThreads({exited, stalled(true)}, 10000) == ThreadVerdict::NoThreads);
  CHECK(dal::judgeThreads({}, 10000) == ThreadVerdict::NoThreads);
}

// Waiters get the lock in the order they queued, whatever the host scheduler does.
void testGuestLockFifo() {
  dal::GuestLock lock;
  std::vector<int> order;  // guarded by `lock`

  lock.lock();
  CHECK(!lock.hasWaiters());

  std::vector<std::thread> threads;
  for (int id = 1; id <= 3; id++) {
    threads.emplace_back([&lock, &order, id] {
      lock.lock();
      order.push_back(id);
      lock.unlock();
    });
    // The next thread starts only after this one has its place in the queue.
    while (lock.waiterCount() < static_cast<std::size_t>(id)) {
      std::this_thread::yield();
    }
  }
  CHECK(lock.hasWaiters());
  lock.unlock();
  for (std::thread& t : threads) t.join();

  CHECK(order == (std::vector<int>{1, 2, 3}));
  CHECK(!lock.hasWaiters());
}

void testTimebase() {
  auto ctx = std::make_unique<DalPpuContext>();
  LARGE_INTEGER frequency, t0, t1;
  QueryPerformanceFrequency(&frequency);

  const std::uint64_t first = dalMftb(ctx.get());
  QueryPerformanceCounter(&t0);
  const std::uint64_t tb0 = dalMftb(ctx.get());
  Sleep(100);
  const std::uint64_t tb1 = dalMftb(ctx.get());
  QueryPerformanceCounter(&t1);

  CHECK(tb0 >= first);
  CHECK(tb1 > tb0);
  // The rate is measured against the host clock over the same interval, so Sleep jitter cancels.
  const double seconds = static_cast<double>(t1.QuadPart - t0.QuadPart) / frequency.QuadPart;
  const double rate = static_cast<double>(tb1 - tb0) / seconds;
  const bool rate_ok = rate > 79800000.0 * 0.95 && rate < 79800000.0 * 1.05;
  CHECK(rate_ok);
  if (!rate_ok) std::fprintf(stderr, "timebase rate %.0f Hz\n", rate);
  CHECK(dal::kTimebaseFrequency == 79800000);

  const std::uint64_t us0 = dal::systemTimeUs();
  Sleep(20);
  CHECK(dal::systemTimeUs() >= us0 + 15000);
}

void testReservationAndDcbz() {
  std::unique_ptr<dal::GuestMemory> memory(newMemory());
  std::uint8_t* base = memory->base();
  CHECK(memory->commit(0x20000, 0x1000));
  auto ctx = std::make_unique<DalPpuContext>();

  dalWrite32(base, 0x20010, 5);
  CHECK(dalLwarx(ctx.get(), base, 0x20010) == 5);
  CHECK(dalStwcx(ctx.get(), base, 0x20010, 6));
  CHECK(dalRead32(base, 0x20010) == 6);
  CHECK(!dalStwcx(ctx.get(), base, 0x20010, 7));  // the reservation is gone
  CHECK(dalRead32(base, 0x20010) == 6);

  dalLwarx(ctx.get(), base, 0x20010);
  dalWrite32(base, 0x20010, 9);  // another thread changed the value
  CHECK(!dalStwcx(ctx.get(), base, 0x20010, 7));
  CHECK(dalRead32(base, 0x20010) == 9);

  dalLwarx(ctx.get(), base, 0x20010);
  CHECK(!dalStwcx(ctx.get(), base, 0x20014, 7));  // different address
  CHECK(!dalStwcx(ctx.get(), base, 0x20010, 7));  // the failure also cleared it

  dalWrite64(base, 0x20020, 0x1122334455667788ull);
  CHECK(dalLdarx(ctx.get(), base, 0x20020) == 0x1122334455667788ull);
  CHECK(dalStdcx(ctx.get(), base, 0x20020, 3));
  CHECK(dalRead64(base, 0x20020) == 3);
  CHECK(!dalStdcx(ctx.get(), base, 0x20020, 4));

  std::memset(base + 0x20000, 0xAB, 0x300);
  dalDcbz(ctx.get(), base, 0x20085);  // any address inside the line
  CHECK(base[0x2007f] == 0xAB);
  CHECK(base[0x20100] == 0xAB);
  bool line_zero = true;
  for (int i = 0x80; i < 0x100; i++) line_zero = line_zero && base[0x20000 + i] == 0;
  CHECK(line_zero);

  dalSync(ctx.get(), kDalSyncHeavy);
  ctx->fpscr = 0;
  dalMtfsf(ctx.get(), 0x80, 0xF0000000u);
  CHECK(dalMffs(ctx.get()) == 0xF0000000u);
  dalMtfsb(ctx.get(), 31, 1);
  CHECK(dalMffs(ctx.get()) == 0xF0000001u);
  dalMtfsb(ctx.get(), 0, 0);
  CHECK(dalMffs(ctx.get()) == 0x70000001u);
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

void testStrictDispatchChild() {
  const ChildResult r = runChild(L"--strict-dispatch-child");
  echoChild("strict dispatch child", r);
  CHECK(r.exit_code == 3);
  CHECK(contains(r, "dal: stop: fatal unknown target: target 0x00010204 from 0x00010300"));
  CHECK(contains(r, "census: unknown targets 1 distinct"));
}

void testUnsortedTableChild() {
  const ChildResult r = runChild(L"--unsorted-table-child");
  echoChild("unsorted table child", r);
  CHECK(r.exit_code == 3);
  CHECK(contains(r, "dal: stop: fatal internal error: function table: 0x00010200 follows "
                    "0x00010210 (unsorted or duplicate)"));
}

void testGuestStackWalk() {
  std::unique_ptr<dal::GuestMemory> memory(newMemory());
  dal::RegionAllocator stack(*memory, dal::kStackRegion);
  const std::uint32_t lo = stack.allocate(0x1000, 0x1000);
  std::uint8_t* base = memory->base();

  const DalFuncMapping mappings[] = {
      {0x10200, testFuncA}, {0x10210, testFuncB}, {0x10300, testFuncA}};
  dalRegisterFuncs(mappings, 3);
  CHECK(dal::findOwningFunction(0x10200) == 0x10200);
  CHECK(dal::findOwningFunction(0x1020c) == 0x10200);
  CHECK(dal::findOwningFunction(0x10214) == 0x10210);
  CHECK(dal::findOwningFunction(0x101fc) == 0);
  CHECK(dal::findOwningFunction(0x10304) == 0);  // past the last entry's slot

  // Chain sp0 -> sp1 -> sp2 -> 0. The LR for the frame at spN is saved at (back chain + 16).
  const std::uint32_t sp0 = lo + 0x100, sp1 = lo + 0x200, sp2 = lo + 0x300;
  dalWrite64(base, sp0, sp1);
  dalWrite64(base, sp1, sp2);
  dalWrite64(base, sp1 + 16, 0x10214);  // return into the middle of fn 0x10210
  dalWrite64(base, sp2, 0);
  dalWrite64(base, sp2 + 16, 0x10208);

  DalPpuContext ctx{};
  ctx.gpr[1] = sp0;
  dal::GuestFrame frames[dal::kMaxGuestFrames];
  std::size_t n = dal::walkGuestStack(ctx, base, frames, dal::kMaxGuestFrames);
  CHECK(n == 2);
  if (n == 2) {
    CHECK(frames[0].lr == 0x10214 && frames[0].function == 0x10210);
    CHECK(frames[1].lr == 0x10208 && frames[1].function == 0x10200);
  }

  CHECK(dal::walkGuestStack(ctx, base, frames, 1) == 1);

  // A cycle, a link that does not ascend, a link outside the stack region, a link into an
  // uncommitted page and a misaligned link all end the walk without touching bad memory.
  dalWrite64(base, sp2, sp0);
  CHECK(dal::walkGuestStack(ctx, base, frames, dal::kMaxGuestFrames) == 2);
  // The first frame is still good; only the link behind it is bad.
  dalWrite64(base, sp1, 0x00010000);
  CHECK(dal::walkGuestStack(ctx, base, frames, dal::kMaxGuestFrames) == 1);
  dalWrite64(base, sp1, lo + 0x1000);  // the guard page above the allocation
  CHECK(dal::walkGuestStack(ctx, base, frames, dal::kMaxGuestFrames) == 1);
  dalWrite64(base, sp1, sp1 + 4);
  CHECK(dal::walkGuestStack(ctx, base, frames, dal::kMaxGuestFrames) == 1);
  dalWrite64(base, sp0, 0x00010000);
  CHECK(dal::walkGuestStack(ctx, base, frames, dal::kMaxGuestFrames) == 0);
  ctx.gpr[1] = 0;
  CHECK(dal::walkGuestStack(ctx, base, frames, dal::kMaxGuestFrames) == 0);
  ctx.gpr[1] = 0x1'0000'0000ull + sp0;
  CHECK(dal::walkGuestStack(ctx, base, frames, dal::kMaxGuestFrames) == 0);
}

// Children: each ends the process through the stop path under test.

int strictDispatchChild() {
  dal::installProcessSettings();
  dal::installCensus();
  dal::process().strict = true;
  const DalFuncMapping mappings[] = {{0x10200, testFuncA}};
  dalRegisterFuncs(mappings, 1);
  auto ctx = std::make_unique<DalPpuContext>();
  ctx->jump_target = 0x10204;
  ctx->jump_site = 0x10300;
  dalDispatch(ctx.get(), nullptr);
  return 99;
}

int unsortedTableChild() {
  dal::installProcessSettings();
  const DalFuncMapping mappings[] = {{0x10210, testFuncA}, {0x10200, testFuncB}};
  dalRegisterFuncs(mappings, 2);
  return 99;
}

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
    if (mode == "--strict-dispatch-child") return strictDispatchChild();
    if (mode == "--unsorted-table-child") return unsortedTableChild();
  }

  dal::installProcessSettings();
  testLogLevelsAndPrefix();
  testLogOnce();
  testGuestMemoryRoundTrip();
  testIsCommittedAndRangeNames();
  testTopOfAddressSpace();
  testAllocatorFirstFit();
  testAllocatorMarkUsedAndExhaustion();
  testStackGuards();
  testProcessSingleton();
  testElfLoader();
  testDispatch();
  testCensus();
  testSyscalls();
  testHle();
  testHleOutPointerHints();
  testHostFunctionsAndCallGuestFunction();
  testStartupSyscalls();
  testFileSystem();
  testSysPrxForUser();
  testMutexAndCond();
  testEventObjects();
  testSpuAndSysmodule();
  testSpurs();
  testCellGcmSys();
  testParamSfo();
  testCellGame();
  testCellFs();
  testFifoParser();
  testRsxConsumer();
  testLwmutex();
  testPpuThreads();
  testGcmInterruptThread();
  testJudgeThreads();
  testTimebase();
  testGuestLockFifo();
  testReservationAndDcbz();
  testGuestStackWalk();
  testFaultChild();
  testOverflowChild();
  testStopChild();
  testStrictDispatchChild();
  testUnsortedTableChild();
  std::printf("runtime core tests: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed != 0 ? 1 : 0;
}
