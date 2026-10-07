// Self-checking unit tests for the runtime core library. No generated code is involved.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <csetjmp>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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
#include "png.h"
#include "lv2/cell_error.h"
#include "lv2/sys_ppu_thread.h"
#include "lv2/sys_spu.h"
#include "lv2/syscalls.h"
#include "lv2/vfs.h"
#include "lv2/vfs_config.h"
#include "lv2/sys_sync.h"
#include "rsx/fifo_parser.h"
#include "rsx/gcm_interrupts.h"
#include "rsx/rsx_census.h"
#include "rsx/rsx_frame.h"
#include "rsx/rsx_registers.h"
#include "rsx/rsx_state.h"
#include "rsx/rsx_thread.h"
#include "zlib.h"

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

// Parsing, composition and installation of the VFS configuration file, and file overrides through
// the guest-facing open calls.
void testVfsConfig() {
  namespace stdfs = std::filesystem;
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();

  const stdfs::path root =
      stdfs::temp_directory_path() / ("dal_vfs_config_test_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  stdfs::remove_all(root, ec);
  auto hostFile = [](const stdfs::path& path, std::string_view data) {
    stdfs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << data;
  };
  const stdfs::path cfg = root / "cfg";
  hostFile(cfg / "game" / "PS3_GAME" / "USRDIR" / "Exi.sdat", "encrypted");
  hostFile(cfg / "game" / "b.txt", "0123456789");
  hostFile(cfg / "plain" / "Exi.bin", "plain-bytes");
  hostFile(cfg / "plain" / "other.txt", "other");
  hostFile(root / "cache" / "PS3_GAME" / "USRDIR" / "Exi.sdat", "from-cache");

  // A full file: relative host paths resolve against the base directory.
  const auto full = dal::parseVfsConfig(R"({
    "version": 1,
    "game_dir": "game",
    "hdd_dir": "hdd",
    "sdata_cache": "../cache",
    "mounts": {"/dev_usb000": {"host": "usb", "block_size": 4096},
               "/dev_hdd2": {"host": "hdd2", "read_only": true}},
    "files": {"/dev_bdvd/PS3_GAME/USRDIR/Exi.sdat": "plain/Exi.bin",
              "/dev_hdd0/tmp/w.bin": {"host": "plain/w.bin", "read_only": false},
              "/dev_bdvd/other.txt": {"host": "plain/other.txt"}}
  })", cfg);
  CHECK(full.has_value());
  if (full) {
    CHECK(full->warnings.empty());
    CHECK(full->mounts.size() == 6);
    auto find = [&](std::string_view name) -> const dal::VfsMount* {
      for (const dal::VfsMount& m : full->mounts) {
        if (m.name == name) return &m;
      }
      return nullptr;
    };
    const dal::VfsMount* bdvd = find("dev_bdvd");
    const dal::VfsMount* app = find("app_home");
    const dal::VfsMount* hdd0 = find("dev_hdd0");
    const dal::VfsMount* usb = find("dev_usb000");
    const dal::VfsMount* hdd2 = find("dev_hdd2");
    CHECK(bdvd && bdvd->read_only && bdvd->host == cfg / "game");
    CHECK(app && app->read_only && app->host == cfg / "game" / "PS3_GAME" / "USRDIR");
    CHECK(hdd0 && !hdd0->read_only && hdd0->host == cfg / "hdd" / "dev_hdd0");
    CHECK(usb && !usb->read_only && usb->block_size == 4096u && usb->host == cfg / "usb");
    CHECK(hdd2 && hdd2->read_only && !hdd2->block_size);
    CHECK(full->sdata_cache == root / "cache");
    CHECK(full->files.size() == 3);
    for (const dal::VfsFileOverride& f : full->files) {
      if (f.guest == "/dev_bdvd/PS3_GAME/USRDIR/Exi.sdat") {
        CHECK(f.read_only && f.host == cfg / "plain" / "Exi.bin");
      } else if (f.guest == "/dev_hdd0/tmp/w.bin") {
        CHECK(!f.read_only);
      }
    }
  }

  // Unknown keys warn and do not fail.
  const auto unknown = dal::parseVfsConfig(R"({"version": 1, "colour": 1,
      "mounts": {"/dev_usb000": {"host": "usb", "mode": "x"}},
      "files": {"/dev_bdvd/a": {"host": "a", "extra": 1}}})", cfg);
  CHECK(unknown.has_value() && unknown->warnings.size() == 3);
  if (unknown) {
    CHECK(unknown->warnings[0].find("colour") != std::string::npos);
  }
  CHECK(dal::parseVfsConfig(R"({"version": 1})", cfg).has_value());

  // Errors, each with a message that names the problem.
  auto expectError = [&](std::string_view text, std::string_view needle) {
    const auto parsed = dal::parseVfsConfig(text, cfg);
    CHECK(!parsed.has_value());
    if (parsed) {
      std::printf("  expected an error containing \"%.*s\"\n", static_cast<int>(needle.size()), needle.data());
    } else {
      const bool found = parsed.error().find(needle) != std::string::npos;
      CHECK(found);
      if (!found) {
        std::printf("  error was: %s\n", parsed.error().c_str());
      }
    }
  };
  expectError("{", "invalid JSON");
  expectError("[]", "top level");
  expectError(R"({})", "\"version\" is missing");
  expectError(R"({"version": 2})", "unsupported");
  expectError(R"({"version": "1"})", "unsupported");
  expectError(R"({"version": 1, "mounts": []})", "\"mounts\" must be an object");
  expectError(R"({"version": 1, "mounts": {"dev_usb000": {"host": "x"}}})", "not absolute");
  expectError(R"({"version": 1, "mounts": {"/dev_usb000/": {"host": "x"}}})", "not normalised");
  expectError(R"({"version": 1, "mounts": {"/dev_usb000/../x": {"host": "x"}}})", "not normalised");
  expectError(R"({"version": 1, "mounts": {"/dev_hdd0/game": {"host": "x"}}})", "overlap");
  expectError(R"({"version": 1, "mounts": {"/dev_usb000": {}}})", "\"host\" is missing");
  expectError(R"({"version": 1, "mounts": {"/dev_usb000": {"host": ""}}})", "empty");
  expectError(R"({"version": 1, "mounts": {"/dev_usb000": {"host": "x", "read_only": 1}}})", "read_only");
  expectError(R"({"version": 1, "mounts": {"/dev_usb000": {"host": "x", "block_size": 1000}}})", "block_size");
  expectError(R"({"version": 1, "mounts": {"/dev_usb000": {"host": "x"}, "/dev_usb000": {"host": "y"}}})",
              "duplicate key");
  expectError(R"({"version": 1, "game_dir": "g", "mounts": {"/dev_bdvd": {"host": "x"}}})", "mounted twice");
  expectError(R"({"version": 1, "hdd_dir": "g", "mounts": {"/dev_hdd1": {"host": "x"}}})", "mounted twice");
  expectError(R"({"version": 1, "files": {"/dev_bdvd": "x"}})", "at least 2");
  expectError(R"({"version": 1, "files": {"/dev_bdvd/a/./b": "x"}})", "not normalised");
  expectError(R"({"version": 1, "files": {"dev_bdvd/a": "x"}})", "not absolute");
  expectError(R"({"version": 1, "files": {"/dev_bdvd/a": 3}})", "host path string");
  expectError(R"({"version": 1, "files": {"/dev_bdvd/a": "x", "/dev_bdvd/a/b": "y"}})", "below");
  expectError(R"({"version": 1, "sdata_cache": 4})", "path string");

  // Composition: command-line options win over the file, which wins over the defaults.
  const auto parsed_base = dal::parseVfsConfig(R"({"version": 1, "game_dir": "game", "hdd_dir": "hdd",
      "sdata_cache": "../cache", "mounts": {"/dev_hdd2": {"host": "hdd2"}}})", cfg);
  CHECK(parsed_base.has_value());
  const dal::VfsConfigFile* file = parsed_base ? &*parsed_base : nullptr;
  auto hostOf = [](const dal::VfsLayout& layout, std::string_view name) {
    for (const dal::VfsMount& m : layout.mounts) {
      if (m.name == name) return m.host;
    }
    return stdfs::path();
  };
  const auto from_file = dal::composeVfsLayout(file, {});
  CHECK(from_file.has_value());
  if (from_file) {
    CHECK(hostOf(*from_file, "dev_bdvd") == cfg / "game");
    CHECK(hostOf(*from_file, "dev_hdd1") == cfg / "hdd" / "dev_hdd1");
    CHECK(from_file->sdata_cache == root / "cache");
  }
  dal::VfsCliOptions cli;
  cli.game_dir = cfg / "plain";
  cli.hdd_dir = root / "cli_hdd";
  cli.sdata_cache = root / "cli_cache";
  const auto merged = dal::composeVfsLayout(file, cli);
  CHECK(merged.has_value());
  if (merged) {
    CHECK(merged->mounts.size() == 5);  // the options replaced four mounts, the extra one stays
    CHECK(hostOf(*merged, "dev_bdvd") == *cli.game_dir);
    CHECK(hostOf(*merged, "app_home") == *cli.game_dir / "PS3_GAME" / "USRDIR");
    CHECK(hostOf(*merged, "dev_hdd0") == *cli.hdd_dir / "dev_hdd0");
    CHECK(hostOf(*merged, "dev_hdd2") == cfg / "hdd2");
    CHECK(merged->sdata_cache == cli.sdata_cache);
  }
  // Without a file the options alone give the default layout.
  dal::VfsCliOptions game_only;
  game_only.game_dir = cfg / "game";
  const auto only_cli = dal::composeVfsLayout(nullptr, game_only);
  CHECK(only_cli.has_value() && only_cli->mounts.size() == 2 && !only_cli->sdata_cache);
  CHECK(dal::composeVfsLayout(nullptr, {}).has_value());  // no mounts at all is valid here
  // A game directory without PS3_GAME/USRDIR still composes: the derived /app_home may be absent.
  CHECK(!stdfs::exists(cfg / "plain" / "PS3_GAME"));
  dal::VfsCliOptions bare_game;
  bare_game.game_dir = cfg / "plain";
  CHECK(dal::composeVfsLayout(nullptr, bare_game).has_value());

  // Host checks: a read-only mount and an override file must exist; writable mounts may not overlap.
  auto composeError = [&](std::string_view text, std::string_view needle) {
    const auto parsed = dal::parseVfsConfig(text, cfg);
    CHECK(parsed.has_value());
    if (!parsed) return;
    const auto composed = dal::composeVfsLayout(&*parsed, {});
    CHECK(!composed.has_value());
    CHECK(!composed && composed.error().find(needle) != std::string::npos);
  };
  composeError(R"({"version": 1, "game_dir": "missing"})", "does not exist");
  composeError(R"({"version": 1, "mounts": {"/app_home": {"host": "plain/none"}}})", "does not exist");
  composeError(R"({"version": 1, "files": {"/dev_bdvd/a": "plain/missing"}})", "does not exist");
  composeError(R"({"version": 1, "files": {"/dev_bdvd/a": "plain"}})", "not a regular file");
  composeError(R"({"version": 1, "files": {"/dev_bdvd/a": {"host": "nodir/f", "read_only": false}}})",
               "does not exist");
  composeError(R"({"version": 1, "mounts": {"/dev_usb000": {"host": "usb"}, "/dev_usb001": {"host": "usb/inner"}}})",
               "overlap");
  composeError(R"({"version": 1, "mounts": {"/dev_hdd0": {"host": "plain"}, "/dev_bdvd": {"host": "plain"}}})",
               "overlap");
  // Two read-only mounts may share a directory.
  const auto shared = dal::parseVfsConfig(
      R"({"version": 1, "mounts": {"/dev_bdvd": {"host": "game"}, "/app_home": {"host": "game/PS3_GAME/USRDIR"}}})",
      cfg);
  CHECK(shared && dal::composeVfsLayout(&*shared, {}).has_value());

  // loadVfsConfig reads a file and resolves against its directory; errors name the file.
  hostFile(cfg / "vfs.json", R"({"version": 1, "game_dir": "game"})");
  const auto loaded = dal::loadVfsConfig(cfg / "vfs.json");
  CHECK(loaded && loaded->mounts.size() == 2 && loaded->mounts[0].host == cfg / "game");
  const auto no_file = dal::loadVfsConfig(cfg / "none.json");
  CHECK(!no_file && no_file.error().find("none.json") != std::string::npos);
  hostFile(cfg / "bad.json", R"({"version": 9})");
  const auto bad_file = dal::loadVfsConfig(cfg / "bad.json");
  CHECK(!bad_file && bad_file.error().find("bad.json") != std::string::npos);

  // Install a file's layout and use it through the guest-facing calls.
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t path_a = scratch, buf = scratch + 0x2000, out = scratch + 0x3000,
                      sb = scratch + 0x3100, arg = scratch + 0x3200;
  enum Import : std::uint32_t { Read, Close, Open, Stat, Write, SdataOpen, Fstat, ImportCount };
  const DalImportInfo imports[] = {
      {0x00800000, 0x0d5b4a14, "sys_fs", "cellFsReadWithOffset"},
      {0x00800010, 0x2cb51f0d, "sys_fs", "cellFsClose"},
      {0x00800020, 0x718bf5f8, "sys_fs", "cellFsOpen"},
      {0x00800030, 0x7de6dced, "sys_fs", "cellFsStat"},
      {0x00800040, 0x8cb722d5, "sys_fs", "cellFsWriteWithOffset"},
      {0x00800050, 0xb1840b53, "sys_fs", "cellFsSdataOpen"},
      {0x00800060, 0xef3efa34, "sys_fs", "cellFsFstat"},
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
  // Reads up to 64 bytes from the start of the open file `fd`.
  auto readAll = [&](std::uint32_t fd) {
    CHECK(call(Read, fd, 0, buf, 64, out + 0x40) == 0);  // `out` keeps the fd
    return std::string(reinterpret_cast<const char*>(base + buf), dalRead64(base, out + 0x40));
  };

  hostFile(cfg / "layout.json", R"({
    "version": 1, "game_dir": "game", "hdd_dir": "hdd", "sdata_cache": "../cache",
    "mounts": {"/dev_usb000": {"host": "usb", "block_size": 1024}},
    "files": {"/dev_bdvd/PS3_GAME/USRDIR/Exi.sdat": "plain/Exi.bin",
              "/dev_bdvd/b.txt": "plain/other.txt",
              "/dev_hdd0/tmp/w.bin": {"host": "plain/w.bin", "read_only": false}}
  })");
  const auto layout_file = dal::loadVfsConfig(cfg / "layout.json");
  CHECK(layout_file.has_value());
  const auto layout = layout_file ? dal::composeVfsLayout(&*layout_file, {})
                                  : std::expected<dal::VfsLayout, std::string>(std::unexpected("no file"));
  CHECK(layout.has_value());
  if (layout) {
    dal::configureVfs(*layout);
    CHECK(stdfs::is_directory(cfg / "hdd" / "dev_hdd0" / "game") && stdfs::is_directory(cfg / "usb"));

    // The sdata open serves the override, not the cache copy at the same path.
    dalWrite32(base, out, 0xDEADBEEF);
    put(path_a, "/dev_bdvd/PS3_GAME/USRDIR/Exi.sdat");
    CHECK(call(SdataOpen, path_a, 0, out) == 0);
    const std::uint32_t sfd = dalRead32(base, out);
    CHECK(readAll(sfd) == "plain-bytes");
    CHECK(call(Fstat, sfd, sb) == 0 && dalRead64(base, sb + 36) == 11 && dalRead32(base, sb) == 0100444);
    CHECK(call(Close, sfd) == 0);
    // So does a plain open of the same path and the sdata marker of cellFsOpen.
    CHECK(call(Open, path_a, 0, out, 0, 0) == 0 && readAll(dalRead32(base, out)) == "plain-bytes");
    CHECK(call(Close, dalRead32(base, out)) == 0);
    dalWrite32(base, arg, 0x180);
    dalWrite32(base, arg + 4, 0x10);
    CHECK(call(Open, path_a, 0, out, arg, 8) == 0 && readAll(dalRead32(base, out)) == "plain-bytes");
    CHECK(call(Close, dalRead32(base, out)) == 0);
    CHECK(call(Stat, path_a, sb) == 0 && dalRead64(base, sb + 36) == 11);
    // A spelling that normalises to the same guest path matches too.
    put(path_a, "/dev_bdvd/PS3_GAME//USRDIR/./Exi.sdat");
    CHECK(call(SdataOpen, path_a, 0, out) == 0 && readAll(dalRead32(base, out)) == "plain-bytes");
    CHECK(call(Close, dalRead32(base, out)) == 0);
    // /app_home is another guest path: no override there, so the cache copy is served.
    put(path_a, "/app_home/Exi.sdat");
    CHECK(call(SdataOpen, path_a, 0, out) == 0 && readAll(dalRead32(base, out)) == "from-cache");
    CHECK(call(Close, dalRead32(base, out)) == 0);

    // An override on a path the game directory also has wins over it, read-only.
    put(path_a, "/dev_bdvd/b.txt");
    CHECK(call(Open, path_a, 0, out, 0, 0) == 0);
    const std::uint32_t over_fd = dalRead32(base, out);
    CHECK(readAll(over_fd) == "other");
    CHECK(call(Write, over_fd, 0, buf, 1, out) == err(dal::kCellEbadf));
    CHECK(call(Close, over_fd) == 0);
    CHECK(call(Open, path_a, 1, out, 0, 0) == err(dal::kCellEperm));  // write-only on a read-only file

    // A writable override is created and written at the host path it names.
    put(path_a, "/dev_hdd0/tmp/w.bin");
    CHECK(call(Open, path_a, 0102, out, 0, 0) == 0);  // RDWR | CREAT
    const std::uint32_t w_fd = dalRead32(base, out);
    put(buf, "written");
    CHECK(call(Write, w_fd, 0, buf, 7, out) == 0 && dalRead64(base, out) == 7);
    CHECK(call(Close, w_fd) == 0);
    {
      std::ifstream in(cfg / "plain" / "w.bin", std::ios::binary);
      CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == "written");
    }
    CHECK(!stdfs::exists(cfg / "hdd" / "dev_hdd0" / "tmp" / "w.bin"));

    // Mounts from the file: an extra device with its block size; overrides report their mount's.
    hostFile(cfg / "usb" / "f.bin", "usb");
    put(path_a, "/dev_usb000/f.bin");
    CHECK(call(Stat, path_a, sb) == 0 && dalRead64(base, sb + 44) == 1024);
    put(path_a, "/dev_bdvd/b.txt");
    CHECK(call(Stat, path_a, sb) == 0 && dalRead64(base, sb + 44) == 2048);
    const dal::VfsResolution marked = dal::resolveVfs("/dev_bdvd/b.txt/");
    CHECK(marked.file_override && marked.dir_only);
    CHECK(!dal::resolveVfs("/dev_usb000/f.bin").file_override);
    CHECK(dal::resolveVfs("/dev_usb001/x").status == dal::VfsStatus::NotMounted);
    const std::vector<std::string> names = dal::vfsMountNames();
    CHECK(std::ranges::find(names, "dev_usb000") != names.end() && names.size() == 5);

    // Dropping the overrides brings back the cache copy for the same path.
    dal::VfsLayout without_files = *layout;
    without_files.files.clear();
    dal::configureVfs(std::move(without_files));
    put(path_a, "/dev_bdvd/PS3_GAME/USRDIR/Exi.sdat");
    CHECK(call(SdataOpen, path_a, 0, out) == 0 && readAll(dalRead32(base, out)) == "from-cache");
    CHECK(call(Close, dalRead32(base, out)) == 0);
  }

  p.main.free(scratch);
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
  stdfs::remove_all(root, ec);
  dal::configureVfs({}, std::nullopt);
}

struct TrophyCallbackCall {
  std::uint32_t context, status;
  std::int32_t completed, total;
  std::uint32_t arg;
};
std::vector<TrophyCallbackCall> g_trophy_callbacks;
std::uint64_t g_trophy_callback_result = 0;

DAL_PPU_FUNC(trophyStatusCallback) {
  (void)base;
  g_trophy_callbacks.push_back({static_cast<std::uint32_t>(ctx->gpr[3]),
                                static_cast<std::uint32_t>(ctx->gpr[4]),
                                static_cast<std::int32_t>(ctx->gpr[5]),
                                static_cast<std::int32_t>(ctx->gpr[6]),
                                static_cast<std::uint32_t>(ctx->gpr[7])});
  ctx->gpr[3] = g_trophy_callback_result;
}

void testSceNpAndTrophy() {
  namespace stdfs = std::filesystem;
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetHostFunctions();
  dal::resetSceNp();
  dal::resetSceNpTrophy();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t out = scratch, comm_id = scratch + 0x100, comm_sign = scratch + 0x200,
                      path_addr = scratch + 0x300, flags = scratch + 0x400;

  // A trophy pack with one entry, the trophy config: a platinum trophy (id 0) and three others
  // that link to it.
  const std::string config =
      "<trophyconf version=\"1.1\">\n"
      " <trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\"/>\n"
      " <trophy id=\"001\" hidden=\"no\" ttype=\"B\" pid=\"000\"/>\n"
      " <trophy id=\"002\" hidden=\"no\" ttype=\"S\" pid=\"000\">\n  <name>x</name>\n </trophy>\n"
      " <trophy id=\"003\" hidden=\"no\" ttype=\"G\" pid=\"000\"/>\n"
      "</trophyconf>\n";
  std::vector<std::uint8_t> trp(128, 0);
  const auto put32 = [&](std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; i++) trp[at + i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
  };
  put32(0, 0xDCA24D00);
  put32(4, 1);
  put32(12, static_cast<std::uint32_t>(128 + config.size()));  // file size (low word)
  put32(16, 1);
  put32(20, 64);
  std::memcpy(&trp[64], "TROPCONF.SFM", 12);
  put32(64 + 36, 128);                                         // entry offset (low word)
  put32(64 + 44, static_cast<std::uint32_t>(config.size()));  // entry size (low word)
  trp.insert(trp.end(), config.begin(), config.end());

  const stdfs::path root =
      stdfs::temp_directory_path() / ("dal_trophy_test_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  stdfs::remove_all(root, ec);
  const stdfs::path game = root / "game";
  stdfs::create_directories(game / "PS3_GAME" / "TROPDIR" / "NPWR00001_00");
  std::ofstream(game / "PS3_GAME" / "TROPDIR" / "NPWR00001_00" / "TROPHY.TRP", std::ios::binary)
      .write(reinterpret_cast<const char*>(trp.data()), static_cast<std::streamsize>(trp.size()));
  std::ofstream(game / "drm.bin", std::ios::binary) << "data";
  dal::configureVfs(game, root / "hdd");

  enum Import : std::uint32_t {
    Register, CreateHandle, DiskSpace, DestroyContext, Init, DestroyHandle, Unlock, Term,
    UnlockState, CreateContext, NpInit, NpTerm, NpDrm, ImportCount
  };
  const DalImportInfo imports[] = {
      {0x00800000, 0x1197b52c, "sceNpTrophy", "sceNpTrophyRegisterContext"},
      {0x00800010, 0x1c25470d, "sceNpTrophy", "sceNpTrophyCreateHandle"},
      {0x00800020, 0x370136fe, "sceNpTrophy", "sceNpTrophyGetRequiredDiskSpace"},
      {0x00800030, 0x3741ecc7, "sceNpTrophy", "sceNpTrophyDestroyContext"},
      {0x00800040, 0x39567781, "sceNpTrophy", "sceNpTrophyInit"},
      {0x00800050, 0x623cd2dc, "sceNpTrophy", "sceNpTrophyDestroyHandle"},
      {0x00800060, 0x8ceedd21, "sceNpTrophy", "sceNpTrophyUnlockTrophy"},
      {0x00800070, 0xa7fabf4d, "sceNpTrophy", "sceNpTrophyTerm"},
      {0x00800080, 0xb3ac3478, "sceNpTrophy", "sceNpTrophyGetTrophyUnlockState"},
      {0x00800090, 0xe3bf9a28, "sceNpTrophy", "sceNpTrophyCreateContext"},
      {0x008000a0, 0xbd28fdbf, "sceNp", "sceNpInit"},
      {0x008000b0, 0x4885aa18, "sceNp", "sceNpTerm"},
      {0x008000c0, 0xad218faf, "sceNp", "sceNpDrmIsAvailable"},
  };
  dal::installImports(imports, ImportCount);
  auto ctx = std::make_unique<DalPpuContext>();
  ctx->gpr[1] = 0x500000;
  CHECK(p.memory.commit(0x4f0000, 0x10000));
  auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
                  std::uint64_t d = 0, std::uint64_t e = 0) {
    ctx->gpr[3] = a;
    ctx->gpr[4] = b;
    ctx->gpr[5] = c;
    ctx->gpr[6] = d;
    ctx->gpr[7] = e;
    dalHleCall(ctx.get(), base, which);
    return static_cast<std::uint32_t>(ctx->gpr[3]);
  };
  constexpr std::uint32_t kNotInit = 0x80022902, kNotSupported = 0x80022903, kInvalidArg = 0x80022906,
                          kNotRegistered = 0x80022904, kUnknownContext = 0x8002290a,
                          kInvalidId = 0x80022920, kPlatinum = 0x80022914, kAlready = 0x80022915,
                          kAlreadyInit = 0x80022901, kBadCommId = 0x80022918;

  // sceNp.
  CHECK(call(NpTerm) == 0x8002aa01);
  CHECK(call(NpInit, 0, 0x1000) == 0x8002aa03);
  CHECK(call(NpInit, 0x1000, 0) == 0x8002aa03);
  CHECK(call(NpInit, 0x1ffff, 0x1000) == 0x8002aa0f);
  CHECK(call(NpInit, 0x20000, 0x1000) == 0);
  CHECK(call(NpInit, 0x20000, 0x1000) == 0x8002aa02);
  CHECK(call(NpTerm) == 0);
  CHECK(call(NpDrm, 0, 0) == 0x80029502);
  std::memcpy(base + path_addr, "/dev_bdvd/drm.bin", 18);
  CHECK(call(NpDrm, 0, path_addr) == 0);
  std::memcpy(base + path_addr, "/dev_bdvd/none.bin", 19);
  CHECK(call(NpDrm, 0, path_addr) == dal::kCellEnoent);

  // Init and the tables.
  CHECK(call(CreateHandle, out) == kNotInit);
  CHECK(call(Init, 0, 0, 0xffffffff, 1) == kNotSupported);
  CHECK(call(Init, 0, 0, 0xffffffff, 0) == 0);
  CHECK(call(Init, 0, 0, 0xffffffff, 0) == kAlreadyInit);
  CHECK(call(CreateHandle, 0) == kInvalidArg);
  CHECK(call(CreateHandle, out) == 0 && dalRead32(base, out) == 1);
  CHECK(call(CreateHandle, out) == 0 && dalRead32(base, out) == 2);
  CHECK(call(DestroyHandle, 2) == 0 && call(DestroyHandle, 2) == 0x80022911);
  CHECK(call(DestroyHandle, 9) == kInvalidArg);

  std::memcpy(base + comm_id, "NPWR00001\0", 10);
  base[comm_id + 10] = 0;
  dalWrite32(base, comm_sign, 0xB9DDE13B);
  dalWrite16(base, comm_sign + 4, 0x100);
  CHECK(call(CreateContext, out, comm_id, 0) == kInvalidArg);
  CHECK(call(CreateContext, out, comm_id, comm_sign, 2) == kNotSupported);
  base[comm_sign + 8] = 1;
  CHECK(call(CreateContext, out, comm_id, comm_sign) == kBadCommId);
  base[comm_sign + 8] = 0;
  CHECK(call(CreateContext, out, comm_id, comm_sign) == 0 && dalRead32(base, out) == 1);

  // Required space is the pack minus its header and entry table; bad ids are rejected.
  CHECK(call(DiskSpace, 1, 1, 0) == kInvalidArg);
  CHECK(call(DiskSpace, 0, 0, out) == kInvalidArg);
  CHECK(call(DiskSpace, 1, 3, out) == 0x80022911);
  CHECK(call(DiskSpace, 1, 1, out) == 0 && dalRead64(base, out) == config.size());

  // Unlocking needs a registered context; read-only state without an install is empty.
  CHECK(call(Unlock, 1, 1, 1, 0) == kNotRegistered);
  CHECK(call(UnlockState, 1, 1, flags, out) == kNotRegistered);

  // Registration: the install question, then the progress events in order.
  const std::uint32_t cb = dal::registerHostFunction("trophyStatusCallback", trophyStatusCallback);
  g_trophy_callbacks.clear();
  g_trophy_callback_result = static_cast<std::uint64_t>(-1);  // refuse the install
  CHECK(call(Register, 1, 1, 0, 0, 0) == kInvalidArg);
  CHECK(call(Register, 1, 1, cb, 0x1234, 2) == kNotSupported);
  CHECK(call(Register, 1, 1, cb, 0x1234, 0) == 0x8002290f);
  CHECK(g_trophy_callbacks.size() == 1);
  CHECK(g_trophy_callbacks[0].context == 1 && g_trophy_callbacks[0].status == 1 &&
        g_trophy_callbacks[0].completed == 0 && g_trophy_callbacks[0].total == 0 &&
        g_trophy_callbacks[0].arg == 0x1234);
  CHECK(!stdfs::exists(root / "hdd" / "dev_hdd0" / "home" / "00000001" / "trophy" / "NPWR00001_00"));

  g_trophy_callbacks.clear();
  g_trophy_callback_result = 0;
  CHECK(call(Register, 1, 1, cb, 0x1234, 0) == 0);
  std::vector<TrophyCallbackCall> expected = {{1, 1, 0, 0, 0x1234}};
  const auto expect_run = [&](std::uint32_t status, std::int32_t total) {
    for (std::int32_t i = 0; i <= total; i++) expected.push_back({1, status, i, total, 0x1234});
  };
  expect_run(5, 3);  // 4 trophies: progress count 3, finalize max(3, 9) - 5 = 4
  expect_run(6, 3);
  expect_run(7, 4);
  expect_run(8, 0);
  CHECK(g_trophy_callbacks.size() == expected.size());
  bool same = g_trophy_callbacks.size() == expected.size();
  for (std::size_t i = 0; same && i < expected.size(); i++) {
    const auto& a = g_trophy_callbacks[i];
    const auto& b = expected[i];
    same = a.context == b.context && a.status == b.status && a.completed == b.completed &&
           a.total == b.total && a.arg == b.arg;
  }
  CHECK(same);
  const stdfs::path trophy_dir = root / "hdd" / "dev_hdd0" / "home" / "00000001" / "trophy" / "NPWR00001_00";
  CHECK(stdfs::exists(trophy_dir / "TROPCONF.SFM"));

  // Installed: no disk space needed, and the next registration starts from the installed status.
  CHECK(call(DiskSpace, 1, 1, out) == 0 && dalRead64(base, out) == 0);
  g_trophy_callbacks.clear();
  CHECK(call(Register, 1, 1, cb, 0, 0) == 0 && g_trophy_callbacks[0].status == 3);

  // Unlock state and the platinum link.
  CHECK(call(UnlockState, 1, 1, flags, out) == 0 && dalRead32(base, out) == 4 &&
        dalRead32(base, flags) == 0);
  dalWrite32(base, out, 0x55555555);
  CHECK(call(Unlock, 1, 1, 4, out) == kInvalidId);
  CHECK(call(Unlock, 1, 1, 0xffffffff, out) == kInvalidId);
  CHECK(call(Unlock, 1, 1, 0, out) == kPlatinum);
  CHECK(call(Unlock, 1, 1, 1, out) == 0 && dalRead32(base, out) == 0xffffffff);
  CHECK(call(Unlock, 1, 1, 1, out) == kAlready);
  CHECK(call(Unlock, 1, 1, 2, out) == 0 && dalRead32(base, out) == 0xffffffff);
  CHECK(call(Unlock, 1, 1, 3, out) == 0 && dalRead32(base, out) == 0);  // completes the platinum
  CHECK(call(UnlockState, 1, 1, flags, out) == 0 && dalRead32(base, flags) == 0xf);
  CHECK(stdfs::exists(trophy_dir / "UNLOCKED.TXT"));

  // The unlock state survives a restart (Term, Init, new context, register).
  CHECK(call(Term) == 0 && call(Term) == kNotInit);
  CHECK(call(UnlockState, 1, 1, flags, out) == kNotInit);
  CHECK(call(Init, 0, 0, 0xffffffff, 0) == 0);
  CHECK(call(UnlockState, 1, 1, flags, out) == kUnknownContext);
  CHECK(call(CreateHandle, out) == 0 && call(CreateContext, out, comm_id, comm_sign) == 0);
  CHECK(call(Register, 1, 1, cb, 0, 0) == 0);
  CHECK(call(UnlockState, 1, 1, flags, out) == 0 && dalRead32(base, flags) == 0xf);
  CHECK(call(DestroyContext, 1) == 0 && call(DestroyContext, 1) == kUnknownContext);

  dal::resetSceNpTrophy();
  dal::resetSceNp();
  dal::resetHostFunctions();
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
  stdfs::remove_all(root, ec);
}

struct SysutilCallbackCall {
  std::uint64_t status, param;
  std::uint32_t userdata;
};
std::vector<SysutilCallbackCall> g_sysutil_callbacks;
std::function<void()> g_sysutil_callback_hook;

DAL_PPU_FUNC(sysutilCallback) {
  (void)base;
  g_sysutil_callbacks.push_back(
      {ctx->gpr[3], ctx->gpr[4], static_cast<std::uint32_t>(ctx->gpr[5])});
  if (g_sysutil_callback_hook) g_sysutil_callback_hook();
}

void testCellSysutil() {
  dal::setLogSink([](std::string_view) {});
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetHostFunctions();
  dal::resetCellSysutil();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);

  enum Import : std::uint32_t { Register, Unregister, Check, EnableBgm, BgmStatus, ImportCount };
  const DalImportInfo imports[] = {
      {0x00800000, 0x9d98afa0, "cellSysutil", "cellSysutilRegisterCallback"},
      {0x00800010, 0x02ff3c1b, "cellSysutil", "cellSysutilUnregisterCallback"},
      {0x00800020, 0x189a74da, "cellSysutil", "cellSysutilCheckCallback"},
      {0x00800030, 0x220894e3, "cellSysutil", "cellSysutilEnableBgmPlayback"},
      {0x00800040, 0xa11552f6, "cellSysutil", "cellSysutilGetBgmPlaybackStatus"},
  };
  dal::installImports(imports, ImportCount);
  auto ctx = std::make_unique<DalPpuContext>();
  ctx->gpr[1] = 0x500000;
  CHECK(p.memory.commit(0x4f0000, 0x10000));
  auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0) {
    ctx->gpr[3] = a;
    ctx->gpr[4] = b;
    ctx->gpr[5] = c;
    dalHleCall(ctx.get(), base, which);
    return static_cast<std::uint32_t>(ctx->gpr[3]);
  };
  constexpr std::uint32_t kErrorValue = 0x8002b102, kErrorBusy = 0x8002b105,
                          kBgmErrorParam = 0x8002b101;

  const std::uint32_t cb = dal::registerHostFunction("sysutilCallback", sysutilCallback);
  g_sysutil_callbacks.clear();
  g_sysutil_callback_hook = nullptr;

  // Only slots 0..3 exist.
  CHECK(call(Register, 4, cb, 1) == kErrorValue);
  CHECK(call(Unregister, 4) == kErrorValue);
  CHECK(call(Unregister, 0xffffffff) == kErrorValue);

  // No registered callback: nothing is queued and a check does nothing.
  CHECK(dal::queueSysutilEvent(dal::kSysutilRequestExitGame, 1) == 0);
  CHECK(call(Check) == 0 && g_sysutil_callbacks.empty());

  // An event is delivered by the next check, once, with (status, param, userdata).
  CHECK(call(Register, 0, cb, 0x1234) == 0);
  CHECK(call(Register, 2, cb, 0x5678) == 0);
  CHECK(dal::queueSysutilEvent(dal::kSysutilRequestExitGame, 7) == 2);
  CHECK(g_sysutil_callbacks.empty());
  CHECK(call(Check) == 0);
  CHECK(g_sysutil_callbacks.size() == 2);
  if (g_sysutil_callbacks.size() == 2) {
    CHECK(g_sysutil_callbacks[0].status == 0x101 && g_sysutil_callbacks[0].param == 7 &&
          g_sysutil_callbacks[0].userdata == 0x1234);
    CHECK(g_sysutil_callbacks[1].status == 0x101 && g_sysutil_callbacks[1].param == 7 &&
          g_sysutil_callbacks[1].userdata == 0x5678);
  }
  CHECK(call(Check) == 0 && g_sysutil_callbacks.size() == 2);

  // An unregistered slot gets nothing; an event keeps the registrations of its enqueue time.
  g_sysutil_callbacks.clear();
  CHECK(call(Unregister, 2) == 0);
  CHECK(dal::queueSysutilEvent(dal::kSysutilBgmPlaybackPlay, 0) == 1);
  CHECK(call(Register, 3, cb, 9) == 0);
  CHECK(call(Check) == 0 && g_sysutil_callbacks.size() == 1 &&
        g_sysutil_callbacks[0].userdata == 0x1234);
  CHECK(call(Unregister, 3) == 0);

  // A plain closure runs on the checking thread with its context; one queued from inside a
  // callback waits for the next check.
  g_sysutil_callbacks.clear();
  int ran = 0;
  g_sysutil_callback_hook = [&] {
    dal::queueSysutilCall([&](DalPpuContext*, std::uint8_t*) { ran += 10; });
  };
  dal::queueSysutilCall([&](DalPpuContext* c, std::uint8_t*) {
    CHECK(c == ctx.get());
    ran++;
  });
  CHECK(dal::queueSysutilEvent(dal::kSysutilSystemMenuOpen, 0) == 1);
  CHECK(call(Check) == 0 && ran == 1 && g_sysutil_callbacks.size() == 1);
  g_sysutil_callback_hook = nullptr;
  CHECK(call(Check) == 0 && ran == 11 && g_sysutil_callbacks.size() == 1);

  // The callback is called through callGuestFunction: r1 is restored.
  CHECK(ctx->gpr[1] == 0x500000);

  // DRAWING_BEGIN/END pair up.
  CHECK(dal::queueSysutilEvent(dal::kSysutilDrawingEnd, 0) == -1);
  CHECK(dal::queueSysutilEvent(dal::kSysutilDrawingBegin, 0) == 1);
  CHECK(dal::queueSysutilEvent(dal::kSysutilDrawingBegin, 0) == static_cast<std::int32_t>(kErrorBusy));
  CHECK(dal::queueSysutilEvent(dal::kSysutilDrawingEnd, 0) == 1);
  CHECK(dal::queueSysutilEvent(dal::kSysutilDrawingEnd, 0) == -1);
  CHECK(call(Check) == 0);

  // BGM status: stopped and disabled until enabled.
  const std::uint32_t status = scratch;
  std::memset(base + status, 0xaa, 40);
  CHECK(call(BgmStatus, 0) == kBgmErrorParam);
  CHECK(call(BgmStatus, status) == 0);
  CHECK(base[status] == 1 && base[status + 1] == 1);
  bool zero_rest = true;
  for (std::uint32_t i = 2; i < 32; i++) zero_rest &= base[status + i] == 0;
  CHECK(zero_rest && base[status + 32] == 0xaa);
  CHECK(call(EnableBgm) == 0);
  CHECK(call(BgmStatus, status) == 0 && base[status] == 1 && base[status + 1] == 0);

  dal::resetCellSysutil();
  g_sysutil_callbacks.clear();
  dal::resetHostFunctions();
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

void testVideoAndAudioOut() {
  dal::setLogSink([](std::string_view) {});
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetCellSysutil();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);

  enum Import : std::uint32_t { GetState, Availability, Configure, AudioState, ImportCount };
  const DalImportInfo imports[] = {
      {0x00800000, 0x887572d5, "cellSysutil", "cellVideoOutGetState"},
      {0x00800010, 0xa322db75, "cellSysutil", "cellVideoOutGetResolutionAvailability"},
      {0x00800020, 0x0bae8772, "cellSysutil", "cellVideoOutConfigure"},
      {0x00800030, 0xf4e3caa0, "cellSysutil", "cellAudioOutGetState"},
  };
  dal::installImports(imports, ImportCount);
  auto ctx = std::make_unique<DalPpuContext>();
  ctx->gpr[1] = 0x500000;
  auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
                  std::uint64_t d = 0) {
    ctx->gpr[3] = a;
    ctx->gpr[4] = b;
    ctx->gpr[5] = c;
    ctx->gpr[6] = d;
    dalHleCall(ctx.get(), base, which);
    return static_cast<std::uint32_t>(ctx->gpr[3]);
  };
  constexpr std::uint32_t kIllegalParameter = 0x8002b222, kOutOfRange = 0x8002b223,
                          kDeviceNotFound = 0x8002b224, kUnsupportedMode = 0x8002b226,
                          kIllegalConfiguration = 0x8002b221, kAudioIllegal = 0x8002b242,
                          kAudioRange = 0x8002b243;

  // RESOLUTION 0x1d: 480, 720, 1080 and 480 16:9.
  dal::ParamSfo sfo;
  sfo.ints["RESOLUTION"] = 0x1d;
  sfo.ints["SOUND_FORMAT"] = 0x1;
  dal::setGameParamSfo(sfo);
  CHECK(call(Availability, 0, 2, 0, 0) == 1);   // 720
  CHECK(call(Availability, 0, 1, 0, 0) == 1);   // 1080
  CHECK(call(Availability, 0, 4, 0, 0) == 1);   // 480
  CHECK(call(Availability, 0, 5, 0, 0) == 0);   // 576
  CHECK(call(Availability, 0, 2, 1, 0) == 0);   // 720 is 16:9 only
  CHECK(call(Availability, 0, 4, 2, 0) == 1);   // 480 16:9
  CHECK(call(Availability, 0, 5, 2, 0) == 0);
  CHECK(call(Availability, 1, 2, 0, 0) == 0);   // secondary output
  CHECK(call(Availability, 2, 2, 0, 0) == 0x8002b225);

  // Default state: 1280x720 progressive 16:9 RGB, 59.94 Hz.
  const std::uint32_t state = scratch;
  std::memset(base + state, 0xaa, 24);
  CHECK(call(GetState, 0, 0, 0) == kIllegalParameter);
  CHECK(call(GetState, 0, 1, state) == kDeviceNotFound);
  CHECK(call(GetState, 1, 0, state) == kDeviceNotFound);
  CHECK(call(GetState, 0, 0, state) == 0);
  const std::uint8_t expected_default[16] = {0, 1, 0, 0, 0, 0, 0, 0, 2, 1, 0, 2, 0, 0, 0, 1};
  CHECK(std::memcmp(base + state, expected_default, 16) == 0 && base[state + 16] == 0xaa);

  // Without 720p the default falls back to 480.
  dal::ParamSfo sd;
  sd.ints["RESOLUTION"] = 0x1;
  dal::setGameParamSfo(sd);
  CHECK(call(GetState, 0, 0, state) == 0 && base[state + 8] == 4 && base[state + 11] == 1);
  CHECK(call(Availability, 0, 2, 0, 0) == 0 && call(Availability, 0, 4, 0, 0) == 1);
  dal::setGameParamSfo(sfo);

  // Configure validates, then GetState reports the stored mode.
  const std::uint32_t config = scratch + 0x100;
  auto setConfig = [&](std::uint8_t res, std::uint8_t format, std::uint8_t aspect,
                       std::uint32_t pitch) {
    std::memset(base + config, 0, 16);
    base[config] = res;
    base[config + 1] = format;
    base[config + 2] = aspect;
    dalWrite32(base, config + 12, pitch);
  };
  CHECK(call(Configure, 0, 0, 0, 0) == kIllegalParameter);
  setConfig(2, 0, 0, 0);
  CHECK(call(Configure, 0, config, 0, 0) == kOutOfRange);
  setConfig(0x92, 0, 0, 0x1400);
  CHECK(call(Configure, 0, config, 0, 0) == kIllegalConfiguration);
  setConfig(0x3, 0, 0, 0x1400);
  CHECK(call(Configure, 0, config, 0, 0) == kUnsupportedMode);
  setConfig(0x81, 0, 0, 0x1400);
  CHECK(call(Configure, 0, config, 0, 0) == kUnsupportedMode);
  CHECK(call(GetState, 0, 0, state) == 0 && base[state + 8] == 2);  // nothing was stored
  setConfig(1, 0, 0, 0x1e00);
  CHECK(call(Configure, 0, config, 0, 0) == 0);
  CHECK(call(GetState, 0, 0, state) == 0 && base[state + 8] == 1 && base[state + 9] == 1 &&
        base[state + 11] == 2);
  setConfig(4, 0, 1, 0xb40);
  CHECK(call(Configure, 0, config, 0, 0) == 0);
  CHECK(call(GetState, 0, 0, state) == 0 && base[state + 8] == 4 && base[state + 11] == 1);

  // Audio out: LPCM 2 ch 48 kHz, layout 2CH.
  std::memset(base + state, 0xaa, 24);
  CHECK(call(AudioState, 0, 0, 0) == kAudioIllegal);
  CHECK(call(AudioState, 2, 0, state) == kAudioIllegal);
  CHECK(call(AudioState, 0, 1, state) == kAudioRange);
  CHECK(call(AudioState, 0, 0, state) == 0);
  const std::uint8_t expected_audio[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 4, 0, 0, 0, 0, 1};
  CHECK(std::memcmp(base + state, expected_audio, 20) == 0 && base[state + 20] == 0xaa);
  CHECK(call(AudioState, 1, 0, state) == 0 && base[state] == 0x10 &&
        dalRead32(base, state + 16) == 0xd00c1680);

  dal::setGameParamSfo({});
  dal::resetCellSysutil();
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

std::vector<std::pair<std::int64_t, std::uint32_t>> g_dialog_results;
std::vector<std::uint64_t> g_dialog_sysutil_events;

DAL_PPU_FUNC(msgDialogCallback) {
  (void)base;
  g_dialog_results.emplace_back(static_cast<std::int32_t>(ctx->gpr[3]),
                                static_cast<std::uint32_t>(ctx->gpr[4]));
}

DAL_PPU_FUNC(msgDialogSysutilCallback) {
  (void)base;
  g_dialog_sysutil_events.push_back(ctx->gpr[3]);
}

// A scripted pad for the cellPad tests: the state a host backend would report.
struct ScriptedPad final : public dal::PadProvider {
  dal::PadPortState state = dal::neutralPadState();
  std::uint32_t vibration_port = 99;
  int small_motor = -1, large_motor = -1;

  dal::PadPortState poll(std::uint32_t port) override {
    return port == 0 ? state : dal::PadPortState{};
  }
  void setVibration(std::uint32_t port, std::uint8_t small, std::uint8_t large) override {
    vibration_port = port;
    small_motor = small;
    large_motor = large;
  }
};

void testCellPad() {
  dal::setLogSink([](std::string_view) {});
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetCellPad();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t info = scratch, data = scratch + 0x200, act = scratch + 0x400;

  enum Import : std::uint32_t { Init, End, SetPortSetting, GetData, GetInfo2, SetActDirect, ImportCount };
  const DalImportInfo imports[] = {
      {0x00800000, 0x1cf98800, "sys_io", "cellPadInit"},
      {0x00800010, 0x4d9b75d5, "sys_io", "cellPadEnd"},
      {0x00800020, 0x578e3c98, "sys_io", "cellPadSetPortSetting"},
      {0x00800030, 0x8b72cda1, "sys_io", "cellPadGetData"},
      {0x00800040, 0xa703a51d, "sys_io", "cellPadGetInfo2"},
      {0x00800050, 0xf65544ee, "sys_io", "cellPadSetActDirect"},
  };
  dal::installImports(imports, ImportCount);
  auto ctx = std::make_unique<DalPpuContext>();
  auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0) {
    ctx->gpr[3] = a;
    ctx->gpr[4] = b;
    dalHleCall(ctx.get(), base, which);
    return static_cast<std::uint32_t>(ctx->gpr[3]);
  };
  constexpr std::uint32_t kInvalid = 0x80121102, kAlready = 0x80121103, kUninit = 0x80121104,
                          kNoDevice = 0x80121107, kUnsupported = 0x80121108;
  const auto len = [&] { return static_cast<std::int32_t>(dalRead32(base, data)); };
  const auto word = [&](std::uint32_t i) { return dalRead16(base, data + 4 + 2 * i); };
  const auto fill = [&](std::uint32_t addr, std::uint32_t size) { std::memset(base + addr, 0xAA, size); };

  // Everything before Init.
  CHECK(call(End) == kUninit);
  CHECK(call(GetInfo2, info) == kUninit);
  CHECK(call(GetData, 0, data) == kUninit);
  CHECK(call(SetPortSetting, 0, 2) == kUninit);
  CHECK(call(SetActDirect, 0, act) == kUninit);

  // Init validation and state.
  CHECK(call(Init, 0) == kInvalid);
  CHECK(call(Init, 128) == kInvalid);
  CHECK(call(Init, 7) == 0);
  CHECK(call(Init, 7) == kAlready);
  CHECK(call(Init, 0) == kAlready);  // the initialised check comes first
  CHECK(call(End) == 0 && call(End) == kUninit);

  // No pad connected: zeroed info, NO_DEVICE everywhere, the data block untouched.
  CHECK(dal::setPadBackend("none") && !dal::setPadBackend("keyboard"));
  CHECK(call(Init, 7) == 0);
  fill(info, 0x100);
  CHECK(call(GetInfo2, 0) == kInvalid);
  CHECK(call(GetInfo2, info) == 0);
  bool zero = true;
  for (std::uint32_t i = 0; i < 124; i++) zero = zero && base[info + i] == (i == 3 ? 7 : 0);
  CHECK(zero);  // max_connect 7 in the last byte of the first word
  CHECK(base[info + 124] == 0xAA);
  fill(data, 0x20);
  CHECK(call(GetData, 0, data) == kNoDevice && base[data] == 0xAA);
  CHECK(call(GetData, 6, data) == kNoDevice);
  CHECK(call(GetData, 127, data) == kInvalid && call(GetData, 0, 0) == kInvalid);
  std::memset(base + act, 0, 8);
  CHECK(call(SetActDirect, 0, act) == kNoDevice);
  CHECK(call(SetPortSetting, 0, 6) == 0 && call(SetPortSetting, 127, 0) == kInvalid);
  CHECK(call(SetPortSetting, 50, 6) == 0);  // beyond the 7 ports: accepted, no state
  CHECK(call(End) == 0);

  // Neutral pad: GetInfo2 layout in big-endian words.
  CHECK(dal::setPadBackend("neutral"));
  CHECK(call(Init, 4) == 0);
  fill(info, 0x100);
  CHECK(call(GetInfo2, info) == 0);
  CHECK(dalRead32(base, info + 0) == 4 && dalRead32(base, info + 4) == 1 &&
        dalRead32(base, info + 8) == 0);
  CHECK(dalRead32(base, info + 12) == 3);  // connected + assign changed, reported once
  CHECK(dalRead32(base, info + 16) == 0 && dalRead32(base, info + 36) == 0);
  CHECK(dalRead32(base, info + 40) == 0);  // port_setting
  CHECK(dalRead32(base, info + 68) == dal::kPadCapabilityStandard && dalRead32(base, info + 96) == 0);
  CHECK(dalRead32(base, info + 72) == 0);          // port 1: nothing connected
  CHECK(dalRead32(base, info + 12 + 4 * 4) == 0);  // port 4 is beyond max_connect 4
  CHECK(base[info + 123] == 0 && base[info + 124] == 0xAA);  // exactly 124 bytes written
  CHECK(call(GetInfo2, info) == 0 && dalRead32(base, info + 12) == 1 && dalRead32(base, info + 4) == 1);

  // The first GetData reports everything, later ones report no change.
  fill(data, 0x100);
  CHECK(call(GetData, 0, data) == 0 && len() == 8);
  CHECK(word(0) == 0 && word(1) == 0x74 && word(2) == 0 && word(3) == 0);
  CHECK(word(4) == 0x80 && word(5) == 0x80 && word(6) == 0x80 && word(7) == 0x80);
  CHECK(word(8) == 0 && word(19) == 0);  // pressure area cleared
  CHECK(call(GetData, 0, data) == 0 && len() == 0);
  CHECK(call(GetData, 1, data) == kNoDevice);
  CHECK(call(GetData, 4, data) == kNoDevice);

  // Port setting keeps only the two defined bits.
  CHECK(call(SetPortSetting, 0, 0xff) == 0);
  CHECK(call(GetInfo2, info) == 0 && dalRead32(base, info + 40) == 6);
  CHECK(call(SetPortSetting, 0, 2) == 0);
  CHECK(call(GetInfo2, info) == 0 && dalRead32(base, info + 40) == 2);

  // A host backend plugs in; pressure data appears only when asked for.
  auto host = std::make_unique<ScriptedPad>();
  ScriptedPad* pad = host.get();
  dal::setPadProvider(std::move(host));
  CHECK(call(End) == 0 && call(Init, 4) == 0);
  CHECK(call(GetInfo2, info) == 0 && dalRead32(base, info + 12) == 3 && dalRead32(base, info + 40) == 0);
  CHECK(call(SetPortSetting, 0, 2) == 0);
  fill(data, 0x100);
  pad->state.button[dal::kPadButtonDigital2] = 0x40;  // cross
  pad->state.button[dal::kPadButtonPressureFirst + 9] = 0xff;
  CHECK(call(GetData, 0, data) == 0 && len() == 20 && word(1) == 0x7a);
  CHECK(word(3) == 0x40 && word(17) == 0xff && word(20) == 0xAAAA);  // sensors not copied
  CHECK(call(GetData, 0, data) == 0 && len() == 0);
  pad->state.button[dal::kPadButtonDigital1] = 0x10;  // d-pad up
  CHECK(call(GetData, 0, data) == 0 && len() == 20 && word(2) == 0x10);
  pad->state.button[dal::kPadButtonSensorFirst] = 600;  // ignored while sensors are off
  CHECK(call(GetData, 0, data) == 0 && len() == 0);
  CHECK(call(SetPortSetting, 0, 6) == 0);
  CHECK(call(GetData, 0, data) == 0 && len() == 24 && word(1) == 0x7c && word(20) == 600 &&
        word(21) == 399);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  CHECK(call(GetData, 0, data) == 0 && len() == 24);  // sensors on: repeated every 10 ms

  // Unplugging and replugging is reported through the assign-changed flag.
  CHECK(call(SetPortSetting, 0, 0) == 0);
  pad->state.connected = false;
  CHECK(call(GetData, 0, data) == kNoDevice);
  CHECK(call(GetInfo2, info) == 0 && dalRead32(base, info + 12) == 2 && dalRead32(base, info + 4) == 0 &&
        dalRead32(base, info + 68) == 0);
  CHECK(call(GetInfo2, info) == 0 && dalRead32(base, info + 12) == 0);
  pad->state.connected = true;
  CHECK(call(GetInfo2, info) == 0 && dalRead32(base, info + 12) == 3);
  CHECK(call(GetData, 0, data) == 0 && len() == 8);  // a replugged pad reports everything again

  // SetActDirect: validation order, capability check, motor routing.
  std::memset(base + act, 0, 8);
  base[act] = 1;
  base[act + 1] = 200;
  CHECK(call(SetActDirect, 0, 0) == kInvalid && call(SetActDirect, 127, act) == kInvalid);
  base[act + 5] = 1;  // reserved byte
  CHECK(call(SetActDirect, 0, act) == kInvalid);
  base[act + 5] = 0;
  CHECK(call(SetActDirect, 5, act) == kNoDevice && call(SetActDirect, 1, act) == kNoDevice);
  CHECK(call(SetActDirect, 0, act) == 0 && pad->vibration_port == 0 && pad->small_motor == 1 &&
        pad->large_motor == 200);
  pad->state.device_capability = dal::kPadCapabilityStandard & ~0x10u;
  CHECK(call(SetActDirect, 0, act) == kUnsupported);

  dal::resetCellPad();
  p.main.free(scratch);
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

void testCellMsgDialog() {
  std::vector<std::string> log_lines;
  dal::setLogSink([&](std::string_view line) { log_lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetHostFunctions();
  dal::resetCellSysutil();
  dal::resetCellMsgDialog();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);

  enum Import : std::uint32_t { Open2, Close, BarReset, BarInc, Register, Check, ImportCount };
  const DalImportInfo imports[] = {
      {0x00800000, 0x7603d3db, "cellSysutil", "cellMsgDialogOpen2"},
      {0x00800010, 0x20543730, "cellSysutil", "cellMsgDialogClose"},
      {0x00800020, 0x7bc2c8a8, "cellSysutil", "cellMsgDialogProgressBarReset"},
      {0x00800030, 0x94862702, "cellSysutil", "cellMsgDialogProgressBarInc"},
      {0x00800040, 0x9d98afa0, "cellSysutil", "cellSysutilRegisterCallback"},
      {0x00800050, 0x189a74da, "cellSysutil", "cellSysutilCheckCallback"},
  };
  dal::installImports(imports, ImportCount);
  auto ctx = std::make_unique<DalPpuContext>();
  ctx->gpr[1] = 0x500000;
  CHECK(p.memory.commit(0x4f0000, 0x10000));
  auto call = [&](Import which, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0,
                  std::uint64_t d = 0) {
    ctx->gpr[3] = a;
    ctx->gpr[4] = b;
    ctx->gpr[5] = c;
    ctx->gpr[6] = d;
    dalHleCall(ctx.get(), base, which);
    return static_cast<std::uint32_t>(ctx->gpr[3]);
  };
  constexpr std::uint32_t kErrorParam = 0x8002b301, kErrorNotOpened = 0x8002b302,
                          kErrorBusy = 0x8002b105;
  constexpr std::uint32_t kOk = 2u << 4, kYesNo = 1u << 4;
  const std::uint32_t text = scratch;
  auto setText = [&](const char* s) { std::memcpy(base + text, s, std::strlen(s) + 1); };
  auto logged = [&](std::string_view needle) {
    for (const std::string& line : log_lines) {
      if (line.find(needle) != std::string::npos) return true;
    }
    return false;
  };

  const std::uint32_t dialog_cb = dal::registerHostFunction("msgDialogCallback", msgDialogCallback);
  const std::uint32_t sysutil_cb =
      dal::registerHostFunction("msgDialogSysutilCallback", msgDialogSysutilCallback);
  g_dialog_results.clear();
  g_dialog_sysutil_events.clear();
  dal::setDialogAnswerDelay(std::chrono::milliseconds(0));
  CHECK(call(Register, 0, sysutil_cb, 0) == 0);

  // Nothing is open.
  CHECK(call(Close) == kErrorNotOpened);
  CHECK(call(BarReset, 0) == kErrorNotOpened);
  CHECK(call(BarInc, 0, 1) == kErrorNotOpened);

  // Parameter checks: no text, unknown type bits, a cursor or bars where they do not belong.
  setText("hello");
  CHECK(call(Open2, kOk, 0, 0, 0) == kErrorParam);
  CHECK(call(Open2, 0x8, text, 0, 0) == kErrorParam);        // bit 3 is not a type bit
  CHECK(call(Open2, kOk | (1u << 8), text, 0, 0) == kErrorParam);
  CHECK(call(Open2, kYesNo | (3u << 8), text, 0, 0) == kErrorParam);
  CHECK(call(Open2, kYesNo | (1u << 12), text, 0, 0) == kErrorParam);
  CHECK(call(Open2, 3u << 12, text, 0, 0) == kErrorParam);   // more than two bars
  CHECK(call(Open2, 3u << 4, text, 0, 0) == kErrorParam);    // unknown button type
  std::memset(base + text, 'a', 512);
  base[text + 512] = 0;
  CHECK(call(Open2, kOk, text, 0, 0) == kErrorParam);        // text of CELL_MSGDIALOG_STRING_SIZE
  CHECK(g_dialog_sysutil_events.empty());

  // An OK dialog answers itself with OK; DRAWING_BEGIN/END surround it and the callback gets the
  // button and the user data. A second dialog is refused while the first one is open.
  setText("\xe3\x82\xbb\xe3\x83\xbc\xe3\x83\x96");  // UTF-8 text reaches the log verbatim
  CHECK(call(Open2, kOk, text, dialog_cb, 0x77) == 0);
  CHECK(logged("open type 0x20") && logged("\xe3\x82\xbb\xe3\x83\xbc\xe3\x83\x96"));
  CHECK(call(Open2, kOk, text, 0, 0) == kErrorBusy);
  CHECK(call(Check) == 0);
  CHECK(g_dialog_sysutil_events.size() == 1 && g_dialog_sysutil_events[0] == dal::kSysutilDrawingBegin);
  CHECK(g_dialog_results.empty());
  CHECK(call(Check) == 0);
  CHECK(g_dialog_sysutil_events.size() == 2 && g_dialog_sysutil_events[1] == dal::kSysutilDrawingEnd);
  CHECK(g_dialog_results.size() == 1 && g_dialog_results[0].first == 1 &&
        g_dialog_results[0].second == 0x77);
  CHECK(ctx->gpr[1] == 0x500000);
  CHECK(call(Close) == kErrorNotOpened);

  // A Yes/No dialog answers YES by default and NO when asked to.
  g_dialog_results.clear();
  CHECK(call(Open2, kYesNo | (1u << 8), text, dialog_cb, 1) == 0);
  CHECK(call(Check) == 0 && call(Check) == 0);
  CHECK(g_dialog_results.size() == 1 && g_dialog_results[0].first == 1);
  CHECK(!dal::setDialogAnswer("maybe"));
  CHECK(dal::setDialogAnswer("no"));
  CHECK(call(Open2, kYesNo, text, dialog_cb, 2) == 0);
  CHECK(call(Check) == 0 && call(Check) == 0);
  CHECK(g_dialog_results.size() == 2 && g_dialog_results[1].first == 2 &&
        g_dialog_results[1].second == 2);
  CHECK(dal::setDialogAnswer("ok"));

  // A dialog without buttons waits for cellMsgDialogClose; Close(0) ends it with button NONE (-1).
  // Type 0x85 has no progress bar; 0x1000 has one.
  g_dialog_results.clear();
  CHECK(call(Open2, 0x85, text, dialog_cb, 3) == 0);
  for (int i = 0; i < 5; i++) CHECK(call(Check) == 0);
  CHECK(g_dialog_results.empty());
  CHECK(call(BarInc, 0, 1) == kErrorParam && call(BarReset, 0) == kErrorParam);
  ctx->fpr[1].f64 = 0.0;
  CHECK(call(Close) == 0);
  CHECK(call(Check) == 0 && call(Check) == 0);
  CHECK(g_dialog_results.size() == 1 && g_dialog_results[0].first == -1 &&
        g_dialog_results[0].second == 3);

  g_dialog_results.clear();
  CHECK(call(Open2, 0x1000, text, dialog_cb, 4) == 0);
  CHECK(call(BarInc, 0, 5) == 0 && call(BarReset, 0) == 0);
  CHECK(call(BarInc, 1, 5) == kErrorParam);
  ctx->fpr[1].f64 = 1.0e9;  // a far-away deadline keeps the dialog open
  CHECK(call(Close) == 0);
  for (int i = 0; i < 3; i++) CHECK(call(Check) == 0);
  CHECK(g_dialog_results.empty() && call(Open2, 0x1000, text, 0, 0) == kErrorBusy);
  ctx->fpr[1].f64 = -5.0;  // a negative delay counts as 0; a later Close replaces the deadline
  CHECK(call(Close) == 0);
  CHECK(call(Check) == 0 && call(Check) == 0);
  CHECK(g_dialog_results.size() == 1 && g_dialog_results[0].first == -1);

  // After a close a new dialog opens again, and the events stayed paired.
  CHECK(call(Open2, 0x4, text, 0, 0) == 0);
  ctx->fpr[1].f64 = 0.0;
  CHECK(call(Close) == 0);
  CHECK(call(Check) == 0 && call(Check) == 0);
  CHECK(dal::queueSysutilEvent(dal::kSysutilDrawingEnd, 0) == -1);

  dal::resetCellMsgDialog();
  dal::resetCellSysutil();
  g_dialog_results.clear();
  g_dialog_sysutil_events.clear();
  dal::resetHostFunctions();
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

// Script and observations shared by the save data callbacks below.
struct SaveFileOp {
  std::uint32_t operation = 0, type = 1, offset = 0, size = 0, buf_size = 0, buf = 0;
  std::string name;
};

struct SaveScript {
  // funcStat
  std::int32_t stat_result = 0;
  std::uint32_t set_param = 0, recreate = 0;
  std::uint32_t stat_calls = 0, stat_is_new = 0, stat_file_num = 0, stat_file_list_num = 0;
  std::int32_t stat_size_kb = 0, stat_hdd_free = 0;
  std::string stat_title, stat_dir;
  // funcFile
  std::vector<SaveFileOp> file_ops;
  std::size_t next_op = 0;
  std::int32_t file_final = 1;
  std::vector<std::uint32_t> file_exc;
  // funcList
  std::int32_t list_result = 0;
  std::uint32_t list_calls = 0, list_dir_num = 0, list_dir_list_num = 0, focus = 1, fixed_num = 0,
                fixed_list = 0, new_data = 0, focus_dir = 0;
};
SaveScript g_save;

DAL_PPU_FUNC(saveStatCallback) {
  const auto result = static_cast<std::uint32_t>(ctx->gpr[3]);
  const auto get = static_cast<std::uint32_t>(ctx->gpr[4]);
  const auto set = static_cast<std::uint32_t>(ctx->gpr[5]);
  g_save.stat_calls++;
  g_save.stat_hdd_free = static_cast<std::int32_t>(dalRead32(base, get));
  g_save.stat_is_new = dalRead32(base, get + 4);
  g_save.stat_dir = reinterpret_cast<const char*>(base + get + 32);
  g_save.stat_title = reinterpret_cast<const char*>(base + get + 64);
  g_save.stat_size_kb = static_cast<std::int32_t>(dalRead32(base, get + 1620));
  g_save.stat_file_num = dalRead32(base, get + 1628);
  g_save.stat_file_list_num = dalRead32(base, get + 1632);
  dalWrite32(base, result, static_cast<std::uint32_t>(g_save.stat_result));
  dalWrite32(base, set, g_save.set_param);
  dalWrite32(base, set + 4, g_save.recreate);
}

DAL_PPU_FUNC(saveFileCallback) {
  const auto result = static_cast<std::uint32_t>(ctx->gpr[3]);
  const auto get = static_cast<std::uint32_t>(ctx->gpr[4]);
  const auto set = static_cast<std::uint32_t>(ctx->gpr[5]);
  g_save.file_exc.push_back(dalRead32(base, get));
  if (g_save.next_op >= g_save.file_ops.size()) {
    dalWrite32(base, result, static_cast<std::uint32_t>(g_save.file_final));
    return;
  }
  const SaveFileOp& op = g_save.file_ops[g_save.next_op++];
  dalWrite32(base, set, op.operation);
  dalWrite32(base, set + 8, op.type);
  // The file name goes into a spare area of the callback scratch block (after the structures).
  const std::uint32_t name_addr = result + 0x900;
  std::memcpy(base + name_addr, op.name.c_str(), op.name.size() + 1);
  dalWrite32(base, set + 28, op.name.empty() ? 0 : name_addr);
  dalWrite32(base, set + 32, op.offset);
  dalWrite32(base, set + 36, op.size);
  dalWrite32(base, set + 40, op.buf_size);
  dalWrite32(base, set + 44, op.buf);
  dalWrite32(base, result, 0);
}

DAL_PPU_FUNC(saveListCallback) {
  const auto result = static_cast<std::uint32_t>(ctx->gpr[3]);
  const auto get = static_cast<std::uint32_t>(ctx->gpr[4]);
  const auto set = static_cast<std::uint32_t>(ctx->gpr[5]);
  g_save.list_calls++;
  g_save.list_dir_num = dalRead32(base, get);
  g_save.list_dir_list_num = dalRead32(base, get + 4);
  dalWrite32(base, result, static_cast<std::uint32_t>(g_save.list_result));
  dalWrite32(base, set, g_save.focus);
  dalWrite32(base, set + 4, g_save.focus_dir);
  dalWrite32(base, set + 8, g_save.fixed_num);
  dalWrite32(base, set + 12, g_save.fixed_list);
  dalWrite32(base, set + 16, g_save.new_data);
}

void testCellSaveData() {
  namespace stdfs = std::filesystem;
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetHostFunctions();
  dal::resetCellSaveData();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x10000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t dir_addr = scratch, set_buf = scratch + 0x100, buf = scratch + 0x1000,
                      set_param = scratch + 0x4000, data = scratch + 0x6000,
                      set_list = scratch + 0x7100, prefix = scratch + 0x7200,
                      new_data = scratch + 0x7300, new_dir = scratch + 0x7340,
                      fixed_list = scratch + 0x7400, focus_dir = scratch + 0x7500;

  const stdfs::path root =
      stdfs::temp_directory_path() / ("dal_savedata_test_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  stdfs::remove_all(root, ec);
  stdfs::create_directories(root / "game", ec);
  dal::configureVfs(root / "game", root / "hdd");
  const stdfs::path saves = root / "hdd" / "dev_hdd0" / "home" / "00000001" / "savedata";

  enum Import : std::uint32_t { AutoLoad, AutoSave, ListLoad, ListSave, Delete, ImportCount };
  const DalImportInfo imports[] = {
      {0x00800000, 0xfbd5c856, "cellSysutil", "cellSaveDataAutoLoad2"},
      {0x00800010, 0x8b7ed64b, "cellSysutil", "cellSaveDataAutoSave2"},
      {0x00800020, 0x1dfbfdd6, "cellSysutil", "cellSaveDataListLoad2"},
      {0x00800030, 0x2de0d663, "cellSysutil", "cellSaveDataListSave2"},
      {0x00800040, 0xedadd797, "cellSysutil", "cellSaveDataDelete2"},
  };
  dal::installImports(imports, ImportCount);
  auto ctx = std::make_unique<DalPpuContext>();
  ctx->gpr[1] = 0x500000;
  CHECK(p.memory.commit(0x4f0000, 0x10000));
  auto call = [&](Import which, std::initializer_list<std::uint64_t> args) {
    int reg = 3;
    for (const std::uint64_t arg : args) ctx->gpr[reg++] = arg;
    dalHleCall(ctx.get(), base, which);
    return static_cast<std::uint32_t>(ctx->gpr[3]);
  };
  auto put = [&](std::uint32_t addr, std::string_view text) {
    std::memcpy(base + addr, text.data(), text.size());
    base[addr + text.size()] = 0;
  };
  auto logged = [&](std::string_view needle) {
    for (const std::string& line : lines) {
      if (line.find(needle) != std::string::npos) return true;
    }
    return false;
  };
  auto text = [&](const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  };

  const std::uint32_t stat_cb = dal::registerHostFunction("saveStatCallback", saveStatCallback);
  const std::uint32_t file_cb = dal::registerHostFunction("saveFileCallback", saveFileCallback);
  const std::uint32_t list_cb = dal::registerHostFunction("saveListCallback", saveListCallback);
  constexpr std::uint32_t kErrCbResult = 0x8002b401, kErrParam = 0x8002b404, kErrFailure = 0x8002b407;

  // setBuf: dirListMax 4, fileListMax 8, bufSize 0x2000, buf.
  dalWrite32(base, set_buf, 4);
  dalWrite32(base, set_buf + 4, 8);
  dalWrite32(base, set_buf + 32, 0x2000);
  dalWrite32(base, set_buf + 36, buf);
  put(dir_addr, "SAVE0001");
  auto autoLoad = [&](std::uint32_t err_dialog = 0) {
    return call(AutoLoad, {0, dir_addr, err_dialog, set_buf, stat_cb, file_cb, 0, 0x1234});
  };
  auto autoSave = [&](std::uint32_t err_dialog = 0) {
    return call(AutoSave, {0, dir_addr, err_dialog, set_buf, stat_cb, file_cb, 0, 0x1234});
  };

  // Argument checks: lowercase name, version above 1, missing funcStat, bad errDialog, no setBuf.
  put(dir_addr, "save0001");
  CHECK(autoLoad() == kErrParam);
  put(dir_addr, "SAVE0001");
  CHECK(call(AutoLoad, {2, dir_addr, 0, set_buf, stat_cb, file_cb, 0, 0}) == kErrParam);
  CHECK(call(AutoLoad, {0, dir_addr, 0, set_buf, 0, file_cb, 0, 0}) == kErrParam);
  CHECK(autoLoad(3) == kErrParam);
  CHECK(call(AutoLoad, {0, dir_addr, 0, 0, stat_cb, file_cb, 0, 0}) == kErrParam);
  CHECK(call(AutoLoad, {0, 0, 0, set_buf, stat_cb, file_cb, 0, 0}) == kErrParam);
  CHECK(g_save.stat_calls == 0);

  // First boot: no save. funcStat sees isNewData and no files, and reports NODATA.
  g_save = SaveScript{};
  g_save.stat_result = -4;
  CHECK(autoLoad(0) == kErrCbResult);
  CHECK(g_save.stat_calls == 1 && g_save.stat_is_new == 1 && g_save.stat_file_num == 0 &&
        g_save.stat_size_kb == 0 && g_save.stat_dir == "SAVE0001" &&
        g_save.stat_hdd_free == 40 * 1024 * 1024 - 256);
  CHECK(logged("there is no save data (no dialog requested)"));
  lines.clear();
  CHECK(autoLoad(1) == kErrCbResult);
  CHECK(logged("error dialog (headless, not shown): there is no save data"));
  lines.clear();
  CHECK(autoLoad(2) == kErrCbResult && logged("error dialog"));  // NOREPEAT: the first one shows
  lines.clear();
  CHECK(autoLoad(2) == kErrCbResult);  // the same error again raises no dialog
  CHECK(!logged("error dialog"));
  g_save.stat_result = 1;  // OK_LAST ends the call successfully
  CHECK(autoLoad(1) == 0);
  g_save.stat_result = 0;  // OK_NEXT for a new save without setParam is a parameter error
  CHECK(autoLoad(1) == kErrParam);
  g_save.stat_result = 9;  // out of range
  CHECK(autoLoad() == kErrParam);
  CHECK(!stdfs::exists(saves / "SAVE0001"));

  // Save: setParam, a secure file, a normal file and the icon.
  g_save = SaveScript{};
  put(set_param + 0, "My title");
  put(set_param + 128, "My subtitle");
  put(set_param + 256, "My detail");
  put(set_param + 1288, "LP-1");
  g_save.set_param = set_param;
  put(data, "hello");
  put(data + 0x100, "xyz");
  put(data + 0x200, "PNG!");
  g_save.file_ops = {{1, 0, 0, 5, 5, data, "DATA.BIN"},
                     {1, 1, 0, 3, 3, data + 0x100, "B.DAT"},
                     {1, 2, 0, 4, 4, data + 0x200, ""}};
  CHECK(autoSave() == 0);
  CHECK(g_save.stat_is_new == 1 && g_save.next_op == 3);
  CHECK(text(saves / "SAVE0001" / "DATA.BIN") == "hello" && text(saves / "SAVE0001" / "B.DAT") == "xyz" &&
        text(saves / "SAVE0001" / "ICON0.PNG") == "PNG!");
  CHECK(!stdfs::exists(saves / ".working_SAVE0001") && !stdfs::exists(saves / ".backup_SAVE0001"));
  {
    const auto sfo = dal::readParamSfoFile(saves / "SAVE0001" / "PARAM.SFO");
    CHECK(sfo.has_value());
    if (sfo) {
      CHECK(sfo->strings.at("CATEGORY") == "SD" && sfo->strings.at("TITLE") == "My title" &&
            sfo->strings.at("SUB_TITLE") == "My subtitle" && sfo->strings.at("DETAIL") == "My detail" &&
            sfo->strings.at("SAVEDATA_DIRECTORY") == "SAVE0001" &&
            sfo->strings.at("SAVEDATA_LIST_PARAM") == "LP-1");
      CHECK(sfo->ints.at("*DATA.BIN") == 1 && sfo->ints.at("*B.DAT") == 0 && sfo->ints.at("ATTRIBUTE") == 0);
    }
  }
  CHECK(g_save.file_exc.size() == 4 && g_save.file_exc[1] == 5 && g_save.file_exc[2] == 3);

  // Load: the stat step lists the files (PARAM.SFO excluded) and the READ returns the bytes.
  g_save = SaveScript{};
  std::memset(base + data + 0x400, 0, 16);
  g_save.file_ops = {{0, 0, 0, 5, 16, data + 0x400, "DATA.BIN"}};
  CHECK(autoLoad() == 0);
  CHECK(g_save.stat_is_new == 0 && g_save.stat_title == "My title" && g_save.stat_file_num == 3 &&
        g_save.stat_file_list_num == 3 && g_save.stat_size_kb == 3 + 35);
  CHECK(std::memcmp(base + data + 0x400, "hello", 6) == 0);
  CHECK(g_save.file_exc.size() == 2 && g_save.file_exc[1] == 5);
  // Sorted by name: B.DAT (normal), DATA.BIN (secure), ICON0.PNG (icon).
  CHECK(dalRead32(base, buf + 0) == 1 && dalRead32(base, buf + 56) == 0 && dalRead32(base, buf + 112) == 2);
  CHECK(dalRead64(base, buf + 8) == 3 && dalRead64(base, buf + 64) == 5);
  CHECK(std::strcmp(reinterpret_cast<const char*>(base + buf + 40), "B.DAT") == 0);
  CHECK(std::strcmp(reinterpret_cast<const char*>(base + buf + 96), "DATA.BIN") == 0);
  CHECK(text(saves / "SAVE0001" / "DATA.BIN") == "hello");  // a load commits nothing

  // Reads of a missing file and past the end fail; callback result codes map to the CBRESULT error.
  g_save = SaveScript{};
  g_save.file_ops = {{0, 1, 0, 4, 4, data + 0x400, "NONE.DAT"}};
  CHECK(autoLoad() == kErrFailure);
  g_save = SaveScript{};
  g_save.file_ops = {{0, 0, 5, 4, 4, data + 0x400, "DATA.BIN"}};
  CHECK(autoLoad() == kErrFailure);
  g_save = SaveScript{};
  g_save.file_final = -3;
  CHECK(autoLoad() == kErrCbResult);
  g_save = SaveScript{};
  g_save.file_final = 7;
  CHECK(autoLoad() == kErrParam);
  g_save = SaveScript{};
  g_save.file_ops = {{0, 0, 0, 8, 4, data + 0x400, "DATA.BIN"}};  // buffer smaller than the size
  CHECK(autoLoad() == kErrParam);
  g_save = SaveScript{};
  g_save.file_ops = {{0, 0, 0, 1, 1, data + 0x400, "PARAM.SFO"}};  // system files are refused
  CHECK(autoLoad() == kErrParam);

  // Write without truncation, a delete and a missing delete.
  g_save = SaveScript{};
  put(data + 0x300, "EY");
  g_save.file_ops = {{3, 0, 1, 2, 2, data + 0x300, "DATA.BIN"}, {2, 1, 0, 0, 0, 0, "B.DAT"}};
  CHECK(autoSave() == 0);
  CHECK(text(saves / "SAVE0001" / "DATA.BIN") == "hEYlo" && !stdfs::exists(saves / "SAVE0001" / "B.DAT"));
  {
    const auto sfo = dal::readParamSfoFile(saves / "SAVE0001" / "PARAM.SFO");
    CHECK(sfo && !sfo->ints.contains("*B.DAT") && sfo->ints.at("*DATA.BIN") == 1);
  }
  g_save = SaveScript{};
  g_save.file_ops = {{2, 1, 0, 0, 0, 0, "B.DAT"}};
  CHECK(autoSave() == kErrFailure);
  g_save = SaveScript{};  // a plain WRITE truncates
  g_save.file_ops = {{1, 0, 0, 2, 2, data + 0x300, "DATA.BIN"}};
  CHECK(autoSave() == 0 && text(saves / "SAVE0001" / "DATA.BIN") == "EY");

  // reCreateMode YES needs setParam and drops the old files.
  g_save = SaveScript{};
  g_save.recreate = 2;
  g_save.file_ops = {{1, 1, 0, 3, 3, data + 0x100, "NEW.DAT"}};
  CHECK(autoSave() == kErrParam);
  g_save.set_param = set_param;
  g_save.next_op = 0;
  CHECK(autoSave() == 0);
  CHECK(!stdfs::exists(saves / "SAVE0001" / "DATA.BIN") && text(saves / "SAVE0001" / "NEW.DAT") == "xyz");
  {
    const auto sfo = dal::readParamSfoFile(saves / "SAVE0001" / "PARAM.SFO");
    CHECK(sfo && !sfo->ints.contains("*DATA.BIN") && sfo->ints.contains("*NEW.DAT"));
  }

  // List operations: ListSave2 creates the game's new-data slot; ListLoad2 picks by focus.
  put(prefix, "SAVE");
  dalWrite32(base, set_list, 0);
  dalWrite32(base, set_list + 4, 1);
  dalWrite32(base, set_list + 8, prefix);
  put(new_dir, "SAVE0002");
  dalWrite32(base, new_data, 0);
  dalWrite32(base, new_data + 4, new_dir);
  g_save = SaveScript{};
  g_save.focus = 5;  // NEWDATA
  g_save.new_data = new_data;
  g_save.set_param = set_param;
  g_save.file_ops = {{1, 1, 0, 3, 3, data + 0x100, "B.DAT"}};
  CHECK(call(ListSave, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == 0);
  CHECK(g_save.list_calls == 1 && g_save.list_dir_num == 1 && g_save.stat_dir == "SAVE0002" &&
        g_save.stat_is_new == 1);
  CHECK(text(saves / "SAVE0002" / "B.DAT") == "xyz");

  g_save = SaveScript{};
  put(fixed_list, "SAVE0001");
  put(fixed_list + 48, "SAVE0002");
  put(focus_dir, "SAVE0001");
  g_save.focus = 0;  // DIRNAME
  g_save.focus_dir = focus_dir;
  g_save.fixed_num = 2;
  g_save.fixed_list = fixed_list;
  g_save.file_ops = {{0, 1, 0, 3, 3, data + 0x400, "NEW.DAT"}};
  CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == 0);
  CHECK(g_save.list_dir_num == 2 && g_save.list_dir_list_num == 2 && g_save.stat_dir == "SAVE0001" &&
        g_save.stat_is_new == 0);
  CHECK(std::memcmp(base + data + 0x400, "xyz", 3) == 0);

  g_save.focus = 1;  // LISTHEAD
  g_save.stat_calls = 0;
  CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == 0);
  CHECK(g_save.stat_calls == 1);

  g_save = SaveScript{};  // nothing matches the prefix and there is no new-data slot: cancel
  put(prefix, "NOPE");
  CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == 1);
  CHECK(g_save.stat_calls == 0);
  g_save.list_result = 1;  // funcList OK_LAST ends the call
  CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == 0);
  g_save.list_result = -4;
  CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == kErrCbResult);
  put(prefix, "");  // an empty prefix is a parameter error
  CHECK(call(ListLoad, {0, set_list, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == kErrParam);
  CHECK(call(ListLoad, {0, 0, set_buf, list_cb, stat_cb, file_cb, 0, 0}) == kErrParam);
  CHECK(call(ListLoad, {0, set_list, set_buf, 0, stat_cb, file_cb, 0, 0}) == kErrParam);

  // Delete2 has only a selection UI in RPCS3.
  CHECK(call(Delete, {0}) == 1);
  CHECK(ctx->gpr[1] == 0x500000);

  stdfs::remove_all(root, ec);
  dal::resetCellSaveData();
  dal::resetHostFunctions();
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
}

// Guest allocation callbacks of the cellPngDec test: a bump allocator over a guest block.
struct PngTestHeap {
  std::uint32_t next = 0;
  std::uint32_t end = 0;
  std::vector<std::uint32_t> freed;
  std::uint32_t malloc_calls = 0;
};
PngTestHeap g_png_heap;

DAL_PPU_FUNC(pngTestMalloc) {
  (void)base;
  const auto size = static_cast<std::uint32_t>(ctx->gpr[3]);
  g_png_heap.malloc_calls++;
  const std::uint32_t addr = g_png_heap.next;
  if (addr + size > g_png_heap.end) {
    ctx->gpr[3] = 0;
    return;
  }
  g_png_heap.next = (addr + size + 15) & ~15u;
  ctx->gpr[3] = addr;
}

DAL_PPU_FUNC(pngTestFree) {
  (void)base;
  g_png_heap.freed.push_back(static_cast<std::uint32_t>(ctx->gpr[3]));
  ctx->gpr[3] = 0;
}

// cbCtrlStrm: hands out the rest of a source in pieces of `piece` bytes through a staging buffer.
struct PngTestFeed {
  std::vector<std::uint8_t> rest;
  std::size_t pos = 0;
  std::size_t piece = 16;
  std::uint32_t staging = 0;
  std::uint32_t calls = 0;
  std::uint32_t last_decoded = 0;
  std::uint32_t arg_seen = 0;
};
PngTestFeed g_png_feed;

DAL_PPU_FUNC(pngTestStream) {
  const auto info = static_cast<std::uint32_t>(ctx->gpr[3]);
  const auto param = static_cast<std::uint32_t>(ctx->gpr[4]);
  g_png_feed.arg_seen = static_cast<std::uint32_t>(ctx->gpr[5]);
  g_png_feed.last_decoded = dalRead32(base, info);
  g_png_feed.calls++;
  const std::size_t count = std::min(g_png_feed.piece, g_png_feed.rest.size() - g_png_feed.pos);
  std::memcpy(base + g_png_feed.staging, g_png_feed.rest.data() + g_png_feed.pos, count);
  g_png_feed.pos += count;
  dalWrite32(base, param, g_png_feed.staging);
  dalWrite32(base, param + 4, static_cast<std::uint32_t>(count));
  ctx->gpr[3] = 0;
}

struct PngTestImage {
  unsigned width = 0;
  unsigned height = 0;
  std::vector<std::uint8_t> file;
};

struct PngPaletteEntry {
  std::uint8_t r, g, b, a;
};

// Input of the libpng writer. Like the decoder, the writer keeps its setjmp in a function with
// plain locals only; the caller owns every container.
struct PngWriteJob {
  unsigned width = 0;
  unsigned height = 0;
  int color_type = 0;
  int depth = 8;
  unsigned interlace = 0;
  png_bytep* rows = nullptr;
  const png_color* palette = nullptr;
  const std::uint8_t* trans = nullptr;
  int palette_size = 0;
  std::vector<std::uint8_t>* out = nullptr;
};

void pngWriteCallback(png_structp png, png_bytep data, size_t count) {
  auto* job = static_cast<PngWriteJob*>(png_get_io_ptr(png));
  job->out->insert(job->out->end(), data, data + count);
}

void pngFlushCallback(png_structp) {}

void pngWriteErrorCallback(png_structp png, png_const_charp) { png_longjmp(png, 1); }

bool writePng(PngWriteJob* job) {
  png_structp png =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, job, pngWriteErrorCallback, nullptr);
  if (png == nullptr) return false;
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_write_struct(&png, nullptr);
    return false;
  }
  if (setjmp(png_jmpbuf(png)) != 0) {
    png_destroy_write_struct(&png, &info);
    return false;
  }
  png_set_write_fn(png, job, pngWriteCallback, pngFlushCallback);
  png_set_IHDR(png, info, job->width, job->height, job->depth, job->color_type,
               job->interlace != 0 ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  if (job->palette != nullptr) {
    png_set_PLTE(png, info, job->palette, job->palette_size);
    if (job->trans != nullptr) png_set_tRNS(png, info, job->trans, job->palette_size, nullptr);
  }
  png_write_info(png, info);
  if (job->depth < 8) png_set_packing(png);  // the rows carry one index per byte
  png_set_interlace_handling(png);
  png_write_image(png, job->rows);
  png_write_end(png, info);
  png_destroy_write_struct(&png, &info);
  return true;
}

// Encodes `raw` into a PNG of the given type and depth. Rows are tightly packed, 16-bit samples
// are big-endian, and a depth below 8 takes one index (or gray level) per byte.
PngTestImage encodePng(unsigned width, unsigned height, const std::vector<std::uint8_t>& raw,
                       int color_type, int png_depth, unsigned interlace = 0,
                       const std::vector<PngPaletteEntry>& palette = {}) {
  const std::size_t channels = color_type == PNG_COLOR_TYPE_RGB_ALPHA ? 4
                               : color_type == PNG_COLOR_TYPE_RGB    ? 3
                               : color_type == PNG_COLOR_TYPE_GRAY_ALPHA ? 2
                                                                         : 1;
  const std::size_t row_bytes = std::size_t{width} * channels * (png_depth == 16 ? 2 : 1);
  CHECK(raw.size() == row_bytes * height);
  std::vector<png_bytep> rows(height);
  std::vector<std::uint8_t> pixels = raw;  // libpng wants non-const rows
  for (unsigned row = 0; row < height; row++) rows[row] = pixels.data() + row * row_bytes;
  std::vector<png_color> colors;
  std::vector<std::uint8_t> alphas;
  bool translucent = false;
  for (const PngPaletteEntry& entry : palette) {
    colors.push_back({entry.r, entry.g, entry.b});
    alphas.push_back(entry.a);
    translucent = translucent || entry.a != 255;
  }
  PngTestImage image{width, height, {}};
  PngWriteJob job;
  job.width = width;
  job.height = height;
  job.color_type = color_type;
  job.depth = png_depth;
  job.interlace = interlace;
  job.rows = rows.data();
  job.palette = colors.empty() ? nullptr : colors.data();
  job.trans = translucent ? alphas.data() : nullptr;
  job.palette_size = static_cast<int>(colors.size());
  job.out = &image.file;
  CHECK(writePng(&job));
  return image;
}

// Inserts a chunk so that it starts at byte `at` (33 is the first position after IHDR; the last
// 12 bytes of a file are IEND).
void insertChunk(std::vector<std::uint8_t>& png, std::size_t at, const char* type,
                 const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> chunk;
  const auto length = static_cast<std::uint32_t>(payload.size());
  for (int shift = 24; shift >= 0; shift -= 8) chunk.push_back(static_cast<std::uint8_t>(length >> shift));
  chunk.insert(chunk.end(), type, type + 4);
  chunk.insert(chunk.end(), payload.begin(), payload.end());
  const auto crc = static_cast<std::uint32_t>(crc32(0, chunk.data() + 4, static_cast<uInt>(chunk.size() - 4)));
  for (int shift = 24; shift >= 0; shift -= 8) chunk.push_back(static_cast<std::uint8_t>(crc >> shift));
  png.insert(png.begin() + static_cast<std::ptrdiff_t>(at), chunk.begin(), chunk.end());
}

void testCellPngDec() {
  namespace stdfs = std::filesystem;
  std::vector<std::string> lines;
  dal::setLogSink([&](std::string_view line) { lines.emplace_back(line); });
  dal::resetCensus();
  dal::resetLogOnce();
  dal::resetHostFunctions();
  dal::resetCellPngDec();
  dal::Process& p = dal::process();
  CHECK(p.memory.reserve());
  std::uint8_t* base = p.memory.base();
  const std::uint32_t scratch = p.main.allocate(0x100000, 0x10000);
  CHECK(scratch != 0);
  const std::uint32_t heap = scratch, params = scratch + 0x10000, src_buf = scratch + 0x20000,
                      out_buf = scratch + 0x40000, staging = scratch + 0x80000;
  g_png_heap = {heap, heap + 0x10000, {}, 0};
  g_png_feed = {};
  g_png_feed.staging = staging;

  const stdfs::path root =
      stdfs::temp_directory_path() / ("dal_pngdec_test_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  stdfs::remove_all(root, ec);
  stdfs::create_directories(root / "game", ec);
  stdfs::create_directories(root / "dump", ec);
  dal::configureVfs(root / "game", std::nullopt);
  dal::setPngDumpDirectory(root / "dump");

  enum Import : std::uint32_t {
    Create, ExtOpen, ExtReadHeader, ExtSetParameter, ExtDecodeData, Close, Destroy, ImportCount
  };
  const DalImportInfo imports[] = {
      {0x00800000, 0x157d30c5, "cellPngDec", "cellPngDecCreate"},
      {0x00800010, 0x0c515302, "cellPngDec", "cellPngDecExtOpen"},
      {0x00800020, 0x8b33f863, "cellPngDec", "cellPngDecExtReadHeader"},
      {0x00800030, 0x9e9d7d42, "cellPngDec", "cellPngDecExtSetParameter"},
      {0x00800040, 0x726fc1d0, "cellPngDec", "cellPngDecExtDecodeData"},
      {0x00800050, 0x5b3d1ff1, "cellPngDec", "cellPngDecClose"},
      {0x00800060, 0x820dae1a, "cellPngDec", "cellPngDecDestroy"},
  };
  dal::installImports(imports, ImportCount);
  auto ctx = std::make_unique<DalPpuContext>();
  ctx->gpr[1] = 0x500000;
  CHECK(p.memory.commit(0x4f0000, 0x10000));
  auto call = [&](Import which, std::initializer_list<std::uint64_t> args) {
    int reg = 3;
    for (const std::uint64_t arg : args) ctx->gpr[reg++] = arg;
    dalHleCall(ctx.get(), base, which);
    return static_cast<std::uint32_t>(ctx->gpr[3]);
  };

  constexpr std::uint32_t kOk = 0, kErrHeader = 0x80611201, kErrFormat = 0x80611202,
                          kErrArg = 0x80611203, kErrSeq = 0x80611204, kErrOpenFile = 0x80611207;
  constexpr std::uint32_t kSpaceRgb = 2, kSpacePal = 4, kSpaceGa = 9, kSpaceRgba = 10,
                          kSpaceArgb = 20;

  // Guest scratch addresses inside the params block.
  const std::uint32_t handle_pp = params, thread_in = params + 0x10, thread_out = params + 0x40,
                      stream_pp = params + 0x50, src = params + 0x60, open_info = params + 0x90,
                      cb_strm = params + 0xa0, info = params + 0xb0, ext_info = params + 0xd0,
                      in_param = params + 0xe0, out_param = params + 0x110,
                      ext_in = params + 0x140, ext_out = params + 0x150,
                      ctrl_param = params + 0x160, out_info = params + 0x170,
                      file_name = params + 0x200;
  const std::uint32_t malloc_opd = dal::registerHostFunction("pngTestMalloc", pngTestMalloc);
  const std::uint32_t free_opd = dal::registerHostFunction("pngTestFree", pngTestFree);
  const std::uint32_t stream_opd = dal::registerHostFunction("pngTestStream", pngTestStream);

  // Create: the handle block comes from the guest allocator; the codec version is reported.
  dalWrite32(base, thread_in + 12, malloc_opd);
  dalWrite32(base, thread_in + 16, 0x1111);
  dalWrite32(base, thread_in + 20, free_opd);
  dalWrite32(base, thread_in + 24, 0x2222);
  CHECK(call(Create, {0, thread_in, thread_out}) == kErrArg);
  CHECK(call(Create, {handle_pp, thread_in, thread_out}) == kOk);
  const std::uint32_t handle = dalRead32(base, handle_pp);
  CHECK(handle >= heap && handle < heap + 0x10000);
  CHECK(dalRead32(base, thread_out) == 0x00420000);
  CHECK(g_png_heap.malloc_calls == 1);
  dalWrite32(base, thread_in + 12, 0);
  CHECK(call(Create, {handle_pp, thread_in, thread_out}) == kErrArg);
  dalWrite32(base, thread_in + 12, malloc_opd);

  auto putSource = [&](const std::vector<std::uint8_t>& bytes) {
    std::memcpy(base + src_buf, bytes.data(), bytes.size());
    dalWrite32(base, src, 1);  // CELL_PNGDEC_BUFFER
    dalWrite32(base, src + 20, src_buf);
    dalWrite32(base, src + 24, static_cast<std::uint32_t>(bytes.size()));
  };
  auto setParam = [&](std::uint32_t mode, std::uint32_t space, std::uint32_t depth,
                      std::uint32_t alpha_select = 0, std::uint32_t alpha = 0) {
    dalWrite32(base, in_param, 0);
    dalWrite32(base, in_param + 4, mode);
    dalWrite32(base, in_param + 8, space);
    dalWrite32(base, in_param + 12, depth);
    dalWrite32(base, in_param + 16, 1);
    dalWrite32(base, in_param + 20, alpha_select);
    dalWrite32(base, in_param + 24, alpha);
  };
  auto bytesAt = [&](std::uint32_t addr, std::size_t count) {
    return std::vector<std::uint8_t>(base + addr, base + addr + count);
  };

  // Opens `bytes` as a buffer source and reads its header; returns the stream, 0 on failure.
  auto openAndRead = [&](const std::vector<std::uint8_t>& bytes) -> std::uint32_t {
    putSource(bytes);
    dalWrite32(base, stream_pp, 0);
    if (call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) != kOk) return 0;
    const std::uint32_t stream = dalRead32(base, stream_pp);
    if (call(ExtReadHeader, {handle, stream, info, 0}) != kOk) return 0;
    return stream;
  };
  struct Decoded {
    std::uint32_t result;
    std::vector<std::uint8_t> out;
  };
  // DecodeData into a 0xEE-filled buffer.
  auto decode = [&](std::uint32_t stream, std::uint64_t bytes_per_line, std::size_t out_size) {
    std::memset(base + out_buf, 0xEE, out_size);
    dalWrite64(base, ctrl_param, bytes_per_line);
    Decoded decoded{call(ExtDecodeData, {handle, stream, out_buf, ctrl_param, out_info, 0, 0}), {}};
    decoded.out = bytesAt(out_buf, out_size);
    return decoded;
  };
  // SetParameter (extIn null), then DecodeData.
  auto setAndDecode = [&](std::uint32_t stream, std::uint32_t mode, std::uint32_t space,
                          std::uint32_t depth, std::uint32_t alpha_select, std::uint32_t alpha,
                          std::uint64_t bytes_per_line, std::size_t out_size) {
    setParam(mode, space, depth, alpha_select, alpha);
    CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kOk);
    return decode(stream, bytes_per_line, out_size);
  };
  auto closeStream = [&](std::uint32_t stream) { CHECK(call(Close, {handle, stream}) == kOk); };

  // 3x2 RGBA8, every sample distinct.
  const std::vector<std::uint8_t> rgba = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                          13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
  const PngTestImage rgba_png =
      encodePng(3, 2, rgba, PNG_COLOR_TYPE_RGB_ALPHA, 8);

  // Header values; SetParameter and DecodeData before ReadHeader are sequence errors.
  {
    putSource(rgba_png.file);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kOk);
    const std::uint32_t stream = dalRead32(base, stream_pp);
    CHECK(dalRead32(base, open_info) != 0);
    CHECK(stream != 0 && stream != handle);
    setParam(0, kSpaceRgba, 8);
    CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kErrSeq);
    dalWrite64(base, ctrl_param, 12);
    CHECK(call(ExtDecodeData, {handle, stream, out_buf, ctrl_param, out_info, 0, 0}) == kErrSeq);
    CHECK(call(ExtReadHeader, {handle, stream, info, ext_info}) == kOk);
    CHECK(dalRead32(base, info) == 3 && dalRead32(base, info + 4) == 2);
    CHECK(dalRead32(base, info + 8) == 4 && dalRead32(base, info + 12) == kSpaceRgba);
    CHECK(dalRead32(base, info + 16) == 8 && dalRead32(base, info + 20) == 0);
    CHECK(dalRead32(base, info + 24) == 0);
    CHECK(dalRead64(base, ext_info) == 0);
    CHECK(call(ExtReadHeader, {0, stream, info, 0}) == kErrArg);
    CHECK(call(ExtReadHeader, {handle, stream + 1, info, 0}) == kErrArg);
    CHECK(call(ExtReadHeader, {handle, stream, 0, 0}) == kErrArg);
    closeStream(stream);
    CHECK(call(ExtReadHeader, {handle, stream, info, 0}) == kErrArg);  // closed
  }

  // RGBA8 -> RGBA8 with 5 padding bytes per row; the pad is not written.
  {
    const std::uint32_t stream = openAndRead(rgba_png.file);
    CHECK(stream != 0);
    setParam(0, kSpaceRgba, 8);
    CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kOk);
    CHECK(dalRead64(base, out_param) == 12);
    CHECK(dalRead32(base, out_param + 8) == 3 && dalRead32(base, out_param + 12) == 2);
    CHECK(dalRead32(base, out_param + 16) == 4 && dalRead32(base, out_param + 20) == 8);
    CHECK(dalRead32(base, out_param + 24) == 0 && dalRead32(base, out_param + 28) == kSpaceRgba);
    CHECK(dalRead32(base, out_param + 32) == 0);
    const Decoded decoded = decode(stream, 17, 29);
    CHECK(decoded.result == kOk);
    std::vector<std::uint8_t> expected(29, 0xEE);
    std::copy(rgba.begin(), rgba.begin() + 12, expected.begin());
    std::copy(rgba.begin() + 12, rgba.end(), expected.begin() + 17);
    CHECK(decoded.out == expected);
    CHECK(dalRead32(base, out_info) == (1u << 15) && dalRead32(base, out_info + 4) == 0);
    CHECK(dalRead32(base, out_info + 8) == 0 && dalRead32(base, out_info + 12) == 0);
    // A row pitch below outputWidthByte is refused and leaves the status at STOP.
    const Decoded refused = decode(stream, 11, 29);
    CHECK(refused.result == kErrArg);
    CHECK(dalRead32(base, out_info + 12) == 1);
    closeStream(stream);
  }

  // RGBA8 -> ARGB8, and the same bottom to top.
  {
    const std::uint32_t stream = openAndRead(rgba_png.file);
    const Decoded top = setAndDecode(stream, 0, kSpaceArgb, 8, 0, 0, 12, 24);
    CHECK(top.result == kOk);
    const std::vector<std::uint8_t> argb = {4,  1,  2,  3,  8,  5,  6,  7,  12, 9,  10, 11,
                                            16, 13, 14, 15, 20, 17, 18, 19, 24, 21, 22, 23};
    CHECK(top.out == argb);
    CHECK(dalRead32(base, out_param + 28) == kSpaceArgb);
    const Decoded flipped = setAndDecode(stream, 1, kSpaceArgb, 8, 0, 0, 12, 24);
    CHECK(flipped.result == kOk);
    std::vector<std::uint8_t> argb_flipped(argb.begin() + 12, argb.end());
    argb_flipped.insert(argb_flipped.end(), argb.begin(), argb.begin() + 12);
    CHECK(flipped.out == argb_flipped);
    CHECK(dalRead32(base, out_param + 24) == 1);
    closeStream(stream);
  }

  // RGBA8 -> RGB8 drops the alpha; colour to gray-alpha is refused.
  {
    const std::uint32_t stream = openAndRead(rgba_png.file);
    const Decoded rgb = setAndDecode(stream, 0, kSpaceRgb, 8, 0, 0, 9, 18);
    CHECK(rgb.result == kOk);
    const std::vector<std::uint8_t> expected = {1,  2,  3,  5,  6,  7,  9,  10, 11,
                                                13, 14, 15, 17, 18, 19, 21, 22, 23};
    CHECK(rgb.out == expected);
    CHECK(dalRead32(base, out_param + 16) == 3);
    setParam(0, kSpaceGa, 8);
    CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kErrArg);
    closeStream(stream);
  }

  // Palette input: 4x2, 8-bit indices, a palette with one translucent entry.
  {
    const std::vector<PngPaletteEntry> pal = {{255, 0, 0, 255}, {0, 255, 0, 128}, {0, 0, 255, 255}};
    const std::vector<std::uint8_t> indices = {0, 1, 2, 1, 2, 2, 0, 0};
    const PngTestImage image = encodePng(4, 2, indices, PNG_COLOR_TYPE_PALETTE, 8, 0, pal);
    const std::uint32_t stream = openAndRead(image.file);
    CHECK(stream != 0);
    CHECK(dalRead32(base, info + 8) == 1 && dalRead32(base, info + 12) == kSpacePal);
    CHECK(dalRead32(base, info + 16) == 8);
    CHECK((dalRead32(base, info + 24) & (1u << 3)) != 0);  // PLTE
    const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 16, 32);
    CHECK(decoded.result == kOk);
    const std::uint8_t red[] = {255, 0, 0, 255}, green[] = {0, 255, 0, 128}, blue[] = {0, 0, 255, 255};
    std::vector<std::uint8_t> expected;
    for (const std::uint8_t index : indices) {
      const std::uint8_t* color = index == 0 ? red : index == 1 ? green : blue;
      expected.insert(expected.end(), color, color + 4);
    }
    CHECK(decoded.out == expected);
    // Fixed alpha applies to a palette input, whatever its palette alpha.
    const Decoded fixed = setAndDecode(stream, 0, kSpaceArgb, 8, 1, 0x40, 16, 32);
    CHECK(fixed.result == kOk);
    for (std::size_t pixel = 0; pixel < 8; pixel++) {
      const std::uint8_t* color = indices[pixel] == 0 ? red : indices[pixel] == 1 ? green : blue;
      CHECK(fixed.out[pixel * 4] == 0x40 && fixed.out[pixel * 4 + 1] == color[0] &&
            fixed.out[pixel * 4 + 3] == color[2]);
    }
    // Palette output at the input's own depth hands the indices through.
    const Decoded raw = setAndDecode(stream, 0, kSpacePal, 8, 0, 0, 4, 8);
    CHECK(raw.result == kOk);
    CHECK(raw.out == indices);
    CHECK(dalRead64(base, out_param) == 4);
    closeStream(stream);
  }

  // 2-bit palette, width 5 (the file pads its rows to bytes), expanded to RGBA.
  {
    std::vector<PngPaletteEntry> pal;
    for (const std::uint8_t gray : {10, 90, 170, 250}) pal.push_back({gray, gray, gray, 255});
    const std::vector<std::uint8_t> indices = {0, 1, 2, 3, 0, 3, 2, 1, 0, 3};
    const PngTestImage image = encodePng(5, 2, indices, PNG_COLOR_TYPE_PALETTE, 2, 0, pal);
    const std::uint32_t stream = openAndRead(image.file);
    CHECK(stream != 0);
    CHECK(dalRead32(base, info + 16) == 2);
    const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 20, 40);
    CHECK(decoded.result == kOk);
    const std::uint8_t levels[] = {10, 90, 170, 250};
    bool match = decoded.result == kOk;
    for (std::size_t pixel = 0; pixel < indices.size() && match; pixel++) {
      match = decoded.out[pixel * 4] == levels[indices[pixel]] && decoded.out[pixel * 4 + 3] == 255;
    }
    CHECK(match);
    closeStream(stream);
  }

  // RGB8: stream alpha is opaque, fixed alpha is the requested value, RGB output is the data.
  {
    const std::vector<std::uint8_t> rgb = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    const PngTestImage image = encodePng(2, 2, rgb, PNG_COLOR_TYPE_RGB, 8);
    const std::uint32_t stream = openAndRead(image.file);
    CHECK(dalRead32(base, info + 8) == 3 && dalRead32(base, info + 12) == kSpaceRgb);
    const Decoded opaque = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 8, 16);
    CHECK(opaque.result == kOk);
    CHECK(opaque.out == (std::vector<std::uint8_t>{10, 20, 30, 255, 40, 50, 60, 255,
                                                   70, 80, 90, 255, 100, 110, 120, 255}));
    const Decoded fixed = setAndDecode(stream, 0, kSpaceRgba, 8, 1, 0x7f, 8, 16);
    CHECK(fixed.out[3] == 0x7f && fixed.out[7] == 0x7f && fixed.out[15] == 0x7f &&
          fixed.out[0] == 10);
    const Decoded same = setAndDecode(stream, 0, kSpaceRgb, 8, 0, 0, 6, 12);
    CHECK(same.out == rgb);
    closeStream(stream);
  }

  // 16-bit RGBA: stripped to the high byte for 8-bit output, kept big-endian for 16-bit output.
  {
    const std::vector<std::uint8_t> rgba16 = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
                                              0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    const PngTestImage image =
        encodePng(2, 1, rgba16, PNG_COLOR_TYPE_RGB_ALPHA, 16);
    const std::uint32_t stream = openAndRead(image.file);
    CHECK(dalRead32(base, info + 16) == 16);
    const Decoded eight = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 8, 8);
    CHECK(eight.result == kOk);
    CHECK(eight.out == (std::vector<std::uint8_t>{0x12, 0x56, 0x9a, 0xde, 0x01, 0x03, 0x05, 0x07}));
    const Decoded sixteen = setAndDecode(stream, 0, kSpaceRgba, 16, 0, 0, 16, 16);
    CHECK(sixteen.result == kOk && sixteen.out == rgba16);
    CHECK(dalRead64(base, out_param) == 16 && dalRead32(base, out_param + 20) == 16);
    const Decoded argb16 = setAndDecode(stream, 0, kSpaceArgb, 16, 0, 0, 16, 16);
    CHECK(argb16.out[0] == 0xde && argb16.out[1] == 0xf0 && argb16.out[2] == 0x12 &&
          argb16.out[3] == 0x34 && argb16.out[6] == 0x9a && argb16.out[7] == 0xbc);
    closeStream(stream);
  }

  // Adam7: the guest gets the same image as from a non-interlaced file.
  {
    std::vector<std::uint8_t> grid;
    for (unsigned i = 0; i < 5 * 5 * 4; i++) grid.push_back(static_cast<std::uint8_t>(i * 7 + 3));
    const PngTestImage image =
        encodePng(5, 5, grid, PNG_COLOR_TYPE_RGB_ALPHA, 8, 1);
    const std::uint32_t stream = openAndRead(image.file);
    CHECK(stream != 0 && dalRead32(base, info + 20) == 1);
    const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 20, 100);
    CHECK(decoded.result == kOk && decoded.out == grid);
    closeStream(stream);
  }

  // Chunk bits: ReadHeader sees what precedes IDAT, DecodeData everything plus IDAT.
  {
    PngTestImage image = rgba_png;
    insertChunk(image.file, image.file.size() - 12, "tIME", {0x07, 0xe8, 1, 1, 0, 0, 0});
    insertChunk(image.file, 33, "prVt", {1, 2, 3});
    insertChunk(image.file, 33, "tEXt", {'k', 0, 'v'});
    insertChunk(image.file, 33, "gAMA", {0, 1, 0x86, 0xa0});
    const std::uint32_t stream = openAndRead(image.file);
    CHECK(stream != 0);
    CHECK(dalRead32(base, info + 24) == 1u);  // gAMA only
    const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 12, 24);
    CHECK(decoded.result == kOk);
    CHECK(dalRead32(base, out_info) == ((1u << 0) | (1u << 9) | (1u << 15)));
    CHECK(dalRead32(base, out_info + 4) == 1 && dalRead32(base, out_info + 8) == 1);
    closeStream(stream);
  }

  // Header errors and argument checks of Open.
  {
    putSource(std::vector<std::uint8_t>(64, 0x42));
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrHeader);
    putSource(std::vector<std::uint8_t>(4, 0x89));
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrHeader);
    putSource(rgba_png.file);
    CHECK(call(ExtOpen, {0, stream_pp, src, open_info, 0, 0}) == kErrArg);
    CHECK(call(ExtOpen, {handle, 0, src, open_info, 0, 0}) == kErrArg);
    dalWrite32(base, src, 7);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrArg);
    // A valid signature followed by junk opens, but there is no IHDR to report.
    std::vector<std::uint8_t> junk = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    junk.insert(junk.end(), 56, 0x42);
    putSource(junk);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kOk);
    const std::uint32_t junk_stream = dalRead32(base, stream_pp);
    CHECK(call(ExtReadHeader, {handle, junk_stream, info, 0}) == kErrHeader);
    closeStream(junk_stream);
    // A truncated image opens (the signature is fine) and fails when decoded.
    std::vector<std::uint8_t> broken = rgba_png.file;
    broken.resize(40);
    putSource(broken);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kOk);
    const std::uint32_t stream = dalRead32(base, stream_pp);
    CHECK(call(ExtReadHeader, {handle, stream, info, 0}) == kOk);
    const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 12, 24);
    CHECK(decoded.result == kErrFormat);
    CHECK(dalRead32(base, out_info + 12) == 1);
    closeStream(stream);
  }

  // ExtSetParameter's extra parameters: LINE_MODE only, and the extra output.
  {
    const std::uint32_t stream = openAndRead(rgba_png.file);
    setParam(0, kSpaceRgba, 8);
    dalWrite32(base, ext_in, 0);
    dalWrite32(base, ext_in + 4, 1);
    dalWrite32(base, ext_in + 8, 0);
    CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, ext_in, ext_out}) == kErrArg);
    dalWrite32(base, ext_in, 1);
    CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, ext_in, ext_out}) == kOk);
    CHECK(dalRead64(base, ext_out) == 12 && dalRead32(base, ext_out + 8) == 1);
    dalWrite32(base, ext_in + 4, 0);
    CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, ext_in, ext_out}) == kOk);
    CHECK(dalRead32(base, ext_out + 8) == 2);
    dalWrite32(base, ext_in + 4, 9);
    CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, ext_in, ext_out}) == kOk);
    CHECK(dalRead32(base, ext_out + 8) == 2);
    // Partial output through cbCtrlDisp is not implemented: refused, not silently ignored.
    dalWrite32(base, ext_in + 4, 1);
    CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, ext_in, ext_out}) == kOk);
    dalWrite64(base, ctrl_param, 12);
    CHECK(call(ExtDecodeData, {handle, stream, out_buf, ctrl_param, out_info, cb_strm, 0}) == kErrArg);
    // Invalid output parameters.
    setParam(2, kSpaceRgba, 8);
    CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kErrArg);
    setParam(0, 3, 8);
    CHECK(call(ExtSetParameter, {handle, stream, in_param, out_param, 0, 0}) == kErrArg);
    closeStream(stream);
  }

  // File source through the VFS, with an offset into the file.
  {
    const std::string name = "/dev_bdvd/test.png";
    std::vector<std::uint8_t> padded = {1, 2, 3, 4, 5};
    padded.insert(padded.end(), rgba_png.file.begin(), rgba_png.file.end());
    {
      std::ofstream out(root / "game" / "test.png", std::ios::binary);
      out.write(reinterpret_cast<const char*>(padded.data()),
                static_cast<std::streamsize>(padded.size()));
    }
    std::memcpy(base + file_name, name.c_str(), name.size() + 1);
    dalWrite32(base, src, 0);  // CELL_PNGDEC_FILE
    dalWrite32(base, src + 4, file_name);
    dalWrite64(base, src + 8, 5);
    dalWrite32(base, src + 16, 0);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kOk);
    const std::uint32_t stream = dalRead32(base, stream_pp);
    CHECK(call(ExtReadHeader, {handle, stream, info, 0}) == kOk);
    const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 12, 24);
    CHECK(decoded.result == kOk && decoded.out == rgba);
    closeStream(stream);

    // The size limits what is read: a file size short of the image cannot decode.
    dalWrite32(base, src + 16, 40);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kOk);
    const std::uint32_t cut = dalRead32(base, stream_pp);
    CHECK(call(ExtReadHeader, {handle, cut, info, 0}) == kOk);
    CHECK(setAndDecode(cut, 0, kSpaceRgba, 8, 0, 0, 12, 24).result == kErrFormat);
    closeStream(cut);

    dalWrite64(base, src + 8, 0);
    dalWrite32(base, src + 16, 0);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrHeader);  // junk first
    const std::string missing = "/dev_bdvd/missing.png";
    std::memcpy(base + file_name, missing.c_str(), missing.size() + 1);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrOpenFile);
    // Partial decoding is for buffers only.
    dalWrite32(base, cb_strm, stream_opd);
    dalWrite32(base, cb_strm + 4, 0x3333);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, cb_strm, 0}) == kErrFormat);
  }

  // Streamed source: the first buffer holds only the start, the callback supplies the rest.
  {
    const std::size_t head = 40;
    const std::vector<std::uint8_t> first(rgba_png.file.begin(), rgba_png.file.begin() + head);
    g_png_feed = {};
    g_png_feed.staging = staging;
    g_png_feed.rest.assign(rgba_png.file.begin() + head, rgba_png.file.end());
    putSource(first);
    dalWrite32(base, cb_strm, stream_opd);
    dalWrite32(base, cb_strm + 4, 0x3333);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, cb_strm, 0}) == kOk);
    const std::uint32_t stream = dalRead32(base, stream_pp);
    CHECK(call(ExtReadHeader, {handle, stream, info, 0}) == kOk);
    CHECK(g_png_feed.calls == 0);  // the header was already there
    const std::size_t frees_before = g_png_heap.freed.size();
    const Decoded decoded = setAndDecode(stream, 0, kSpaceRgba, 8, 0, 0, 12, 24);
    CHECK(decoded.result == kOk && decoded.out == rgba);
    CHECK(g_png_feed.calls >= 2 && g_png_feed.arg_seen == 0x3333);
    CHECK(g_png_feed.pos == g_png_feed.rest.size());
    CHECK(g_png_feed.last_decoded > head);
    CHECK(g_png_heap.freed.size() == frees_before + 1);  // the callback's scratch block
    closeStream(stream);

    // A callback that runs dry before the image ends fails the decode.
    g_png_feed = {};
    g_png_feed.staging = staging;
    g_png_feed.rest.assign(rgba_png.file.begin() + head, rgba_png.file.begin() + head + 20);
    putSource(first);
    CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, cb_strm, 0}) == kOk);
    const std::uint32_t dry = dalRead32(base, stream_pp);
    CHECK(call(ExtReadHeader, {handle, dry, info, 0}) == kOk);
    CHECK(setAndDecode(dry, 0, kSpaceRgba, 8, 0, 0, 12, 24).result == kErrFormat);
    closeStream(dry);
  }

  // The dump holds the first decoded image.
  {
    const stdfs::path dumped = root / "dump" / "png_0.png";
    CHECK(stdfs::exists(dumped));
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    CHECK(png_image_begin_read_from_file(&image, dumped.string().c_str()) != 0);
    image.format = PNG_FORMAT_RGBA;
    std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(image));
    CHECK(png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) != 0);
    CHECK(image.width == 3 && image.height == 2 && pixels.size() == 24 &&
          std::memcmp(pixels.data(), rgba.data(), 24) == 0);
    png_image_free(&image);
  }

  // Destroy frees the handle block; the handle is then unknown.
  const std::size_t frees_before_destroy = g_png_heap.freed.size();
  CHECK(call(Destroy, {handle}) == kOk);
  CHECK(g_png_heap.freed.size() == frees_before_destroy + 1 && g_png_heap.freed.back() == handle);
  CHECK(call(Destroy, {handle}) == kErrArg);
  CHECK(call(ExtOpen, {handle, stream_pp, src, open_info, 0, 0}) == kErrArg);
  CHECK(ctx->gpr[1] == 0x500000);

  stdfs::remove_all(root, ec);
  p.main.free(scratch);
  dal::resetCellPngDec();
  dal::resetHostFunctions();
  dal::setLogSink({});
  dal::resetCensus();
  dal::resetLogOnce();
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

// Words of a fragment program as stored in guest memory: the 16-bit halves of each word swapped.
std::uint32_t fpStoredWord(std::uint32_t decoded) { return std::rotl(decoded, 16); }

void testRsxArgCensus() {
  CHECK(dal::rsxMethodName(0x1A04 + 2 * 0x20) == "NV4097_SET_TEXTURE_FORMAT[2]");
  CHECK(dal::rsxMethodName(0x0208) == "NV4097_SET_SURFACE_FORMAT");
  CHECK(dal::rsxMethodName(0x0004) == "0x0004");
  CHECK(dal::rsxDecodeArgument(0x0208, 8 | (2u << 5) | (2u << 8) | (10u << 16) | (9u << 24)) ==
        "color=A8R8G8B8 depth=Z24S8 type=swizzle aa=center_1 size=1024x512");
  CHECK(dal::rsxDecodeArgument(0x1A04 + 0x20, 2 | (2u << 4) | (0xA5u << 8) | (1u << 16)) ==
        "location=main cubemap=0 border=0 dim=2D format=A8R8G8B8_LN_NR mipmaps=1");
  CHECK(dal::rsxTextureFormatName(0x86 | 0x40) == "DXT1_SZ_UN");
  CHECK(dal::rsxDecodeArgument(0x1814, 5 | (3u << 24)) == "first=5 count=4");
  CHECK(dal::rsxDecodeArgument(0x1808, 5) == "TRIANGLES");
  CHECK(dal::rsxDecodeArgument(0x08E4, 0x1000 | 2) == "location=main offset=0x1000");
  CHECK(dal::rsxDecodeArgument(0x1740, 2 | (3u << 4) | (12u << 8)) == "type=f size=3 stride=12 frequency=0");
  CHECK(dal::rsxDecodeArgument(0x1680, 0x80000040) == "location=main offset=0x40");
  CHECK(dal::rsxDecodeArgument(0x0324, (1u << 16) | 1u) == "R-B-");
  CHECK(dal::rsxFpOpcodeName(0x42) == "IFE" && dal::rsxFpOpcodeName(0x30) == "OP_0x30");
  CHECK(dal::rsxVpVecOpcodeName(7) == "DP4" && dal::rsxVpScaOpcodeName(2) == "RCP");

  // Fragment program: MOV, then MUL with an inline constant and the end flag.
  const std::vector<std::uint32_t> fp = {
      fpStoredWord(1u << 24), 0, 0, 0,                                      // MOV
      fpStoredWord((2u << 24) | 1u), 0, fpStoredWord(2u), 0,                // MUL, end, src1 is a constant
      0x3F800000, 0x3F800000, 0, 0,                                         // the constant
  };
  const auto fpReader = [&](std::uint32_t index, std::uint32_t& word) {
    if (index >= fp.size()) return false;
    word = fp[index];
    return true;
  };
  const dal::RsxFpScan scan = dal::scanFragmentProgram(fpReader);
  CHECK(scan.terminated && scan.instructions == 2 && scan.constant_words == 1 && scan.words.size() == 12);
  CHECK((scan.opcodes == std::map<std::uint32_t, std::uint32_t>{{1, 1}, {2, 1}}));
  // The opcode's high bit lives in the second source word.
  const std::vector<std::uint32_t> hi = {fpStoredWord((2u << 24) | 1u), 0, fpStoredWord(0x80000000u), 0};
  const dal::RsxFpScan hi_scan = dal::scanFragmentProgram([&](std::uint32_t index, std::uint32_t& word) {
    if (index >= hi.size()) return false;
    word = hi[index];
    return true;
  });
  CHECK(hi_scan.terminated && hi_scan.opcodes.count(0x42) == 1);
  // A program that runs off readable memory is not reported as terminated.
  const dal::RsxFpScan cut = dal::scanFragmentProgram([&](std::uint32_t index, std::uint32_t& word) {
    if (index >= 8) return false;
    word = fp[index];
    return true;
  });
  CHECK(!cut.terminated && cut.instructions == 2);

  std::vector<std::uint32_t> ucode(dal::kRsxVpInstructionCount * 4, 0);
  ucode[3 * 4 + 1] = (1u << 22) | (2u << 27);  // MOV / RCP
  ucode[4 * 4 + 3] = 1;                        // end
  const dal::RsxVpScan vp = dal::scanVertexProgram(ucode.data(), 3);
  CHECK(vp.terminated && vp.instructions == 2);
  CHECK((vp.vec_opcodes == std::map<std::uint32_t, std::uint32_t>{{0, 1}, {1, 1}}));
  CHECK((vp.sca_opcodes == std::map<std::uint32_t, std::uint32_t>{{0, 1}, {2, 1}}));
  CHECK(!dal::scanVertexProgram(std::vector<std::uint32_t>(dal::kRsxVpInstructionCount * 4, 0).data(), 540).terminated);

  // Collector: feed the register writes of one draw.
  namespace stdfs = std::filesystem;
  const stdfs::path root = stdfs::temp_directory_path() / ("dal_rsx_census_test_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  stdfs::remove_all(root, ec);
  dal::RsxArgCensus census(
      root / "census.json", root / "dump",
      [&](std::uint32_t location, std::uint32_t offset, std::uint32_t& word) {
        return location == 1 && offset >= 0x2000 && offset % 4 == 0 && fpReader((offset - 0x2000) / 4, word);
      },
      1000000);
  census.onWrite(0x0208, 8 | (2u << 5) | (2u << 8) | (10u << 16) | (9u << 24));
  census.onWrite(0x1740, 2 | (3u << 4) | (12u << 8));
  census.onWrite(0x1A0C, 1u << 31);
  census.onWrite(0x1A04, 2 | (2u << 4) | (0xA5u << 8));
  census.onWrite(0x08E4, 0x2000 | 2);
  census.onWrite(0x1E9C, 0);
  census.onWrite(0x1EA0, 0);
  for (std::uint32_t i = 0; i < 4; i++) census.onWrite(0x0B80 + i * 4, i == 3 ? 1u : (i == 1 ? (1u << 22) : 0u));
  census.onWrite(0x1808, 5);
  census.onWrite(0x1814, 0 | (5u << 24));
  census.onWrite(0x1808, 0);
  census.onFlip(1500000);
  census.onFlip(2500000);
  CHECK(census.writesOf(0x0208) == 1 && census.distinctValuesOf(0x0208) == 1);
  CHECK(census.writesOf(0x0B80) == 1 && census.draws() == 1);
  CHECK(census.fragmentProgramCount() == 1 && census.vertexProgramCount() == 1);
  const std::string json = census.formatJson();
  CHECK(json.find("\"name\": \"NV4097_SET_SURFACE_FORMAT\"") != std::string::npos);
  CHECK(json.find("A8R8G8B8_LN_NR") != std::string::npos);
  CHECK(json.find("\"flips_per_second\"") != std::string::npos);
  CHECK(json.find("\"instructions\": 2") != std::string::npos);
  CHECK(census.writeFiles());
  std::size_t dumps = 0;
  for (const auto& entry : stdfs::directory_iterator(root / "dump")) {
    const std::string name = entry.path().filename().string();
    dumps += (name.starts_with("fp_") || name.starts_with("vp_")) && name.ends_with(".bin") ? 1 : 0;
    if (name.starts_with("fp_")) CHECK(stdfs::file_size(entry.path()) == 48);
    if (name.starts_with("vp_")) CHECK(stdfs::file_size(entry.path()) == 16);
  }
  CHECK(dumps == 2 && stdfs::exists(root / "census.json"));
  stdfs::remove_all(root, ec);
}

void testRsxRegisters() {
  auto regs = std::make_unique<dal::RsxRegisters>();
  regs->reset();

  // Plain writes land in the register file; the offset is masked to the register window.
  regs->apply(0x0A6C, 0x207);
  CHECK(regs->reg(0x0A6C) == 0x207 && regs->regs[0x0A6C >> 2] == 0x207);
  regs->apply(0x10000 + 0x0A6C, 0x201);
  CHECK(regs->reg(0x0A6C) == 0x201);

  // Eight program words at load slot 3 fill instructions 3 and 4 and move the load pointer by two.
  regs->apply(dal::kMethodNv4097SetTransformProgramLoad, 3);
  CHECK(regs->vp_load == 3);
  for (std::uint32_t i = 0; i < 8; i++) regs->apply(dal::kMethodNv4097SetTransformProgram + i * 4, 0x100 + i);
  for (std::uint32_t i = 0; i < 8; i++) CHECK(regs->vp_ucode[12 + i] == 0x100 + i);
  CHECK(regs->vp_ucode[11] == 0 && regs->vp_ucode[20] == 0);
  CHECK(regs->vp_load == 5 && regs->reg(dal::kMethodNv4097SetTransformProgramLoad) == 5);
  // A block that starts mid-instruction (word 2 onwards) completes it, then starts the next one.
  regs->apply(dal::kMethodNv4097SetTransformProgramLoad, 0);
  regs->apply(dal::kMethodNv4097SetTransformProgram + 2 * 4, 0x20);
  regs->apply(dal::kMethodNv4097SetTransformProgram + 3 * 4, 0x30);
  CHECK(regs->vp_ucode[2] == 0x20 && regs->vp_ucode[3] == 0x30 && regs->vp_load == 1);
  // Writes past the last instruction are dropped.
  regs->apply(dal::kMethodNv4097SetTransformProgramLoad, dal::kRsxVpInstructionCount);
  regs->apply(dal::kMethodNv4097SetTransformProgram, 0xDEAD);
  CHECK(regs->vp_load == dal::kRsxVpInstructionCount);
  regs->apply(dal::kMethodNv4097SetTransformProgramLoad, dal::kRsxVpInstructionCount - 1);
  for (std::uint32_t i = 0; i < 8; i++) regs->apply(dal::kMethodNv4097SetTransformProgram + i * 4, 0x500 + i);
  CHECK(regs->vp_ucode[(dal::kRsxVpInstructionCount - 1) * 4 + 3] == 0x503 && regs->vp_ucode[0] == 0);
  CHECK(regs->vp_load == dal::kRsxVpInstructionCount);
  // Further writes at the end of the store neither land nor move the pointer.
  for (std::uint32_t i = 0; i < 4; i++) regs->apply(dal::kMethodNv4097SetTransformProgram + i * 4, 0x600 + i);
  CHECK(regs->vp_load == dal::kRsxVpInstructionCount && regs->vp_ucode[0] == 0);
  CHECK(regs->reg(dal::kMethodNv4097SetTransformProgramLoad) == dal::kRsxVpInstructionCount);
  regs->apply(dal::kMethodNv4097SetTransformProgramStart, 7);
  CHECK(regs->vp_start == 7);

  // Constants are addressed from the load slot and the load slot itself stays put.
  regs->apply(dal::kMethodNv4097SetTransformConstantLoad, 3);
  CHECK(regs->constant_load == 3);
  for (std::uint32_t i = 0; i < 8; i++) regs->apply(dal::kMethodNv4097SetTransformConstant + i * 4, 0x200 + i);
  for (std::uint32_t i = 0; i < 8; i++) CHECK(regs->vp_constants[12 + i] == 0x200 + i);
  CHECK(regs->vp_constants[11] == 0 && regs->vp_constants[20] == 0);
  CHECK(regs->constant_load == 3 && regs->reg(dal::kMethodNv4097SetTransformConstantLoad) == 3);
  // The last slot is 467; slot 468 and beyond are dropped.
  regs->apply(dal::kMethodNv4097SetTransformConstantLoad, dal::kRsxVpConstantCount - 1);
  for (std::uint32_t i = 0; i < 8; i++) regs->apply(dal::kMethodNv4097SetTransformConstant + i * 4, 0x300 + i);
  CHECK(regs->vp_constants[(dal::kRsxVpConstantCount - 1) * 4 + 3] == 0x303 && regs->vp_constants[0] == 0);

  // Reset restores the defaults and clears both stores.
  regs->reset();
  CHECK(regs->vp_load == 0 && regs->vp_start == 0 && regs->constant_load == 0);
  CHECK(regs->vp_ucode[12] == 0 && regs->vp_constants[12] == 0 && regs->reg(0x0A6C) == 0x201);
  CHECK(regs->reg(0x0208) == 0x121 && regs->reg(0x020C) == 0x40);              // surface format, pitch A
  CHECK(regs->reg(0x0A30) == 0x45000000 && regs->reg(0x0A38) == 0x3F000000);  // viewport scale x, z
  CHECK(regs->reg(0x0A20) == 0x45000000 && regs->reg(0x0A3C) == 0);           // viewport offset x, scale w
  CHECK(regs->reg(0x0A00) == 0x10000000 && regs->reg(0x08C0) == 0x10000000);  // viewport, scissor
  CHECK(regs->reg(0x0314) == 0x10001 && regs->reg(0x0318) == 0);              // blend sfactor, dfactor
  CHECK(regs->reg(0x0320) == 0x80068006);                                     // blend equation
  CHECK(regs->reg(0x0324) == 0x1010101);                                      // color mask
  CHECK(regs->reg(0x0308) == 0x207 && regs->reg(0x030C) == 0);                // alpha func, ref
  CHECK(regs->reg(0x0A6C) == 0x201 && regs->reg(0x0A70) == 1);                // depth func, mask
  CHECK(regs->reg(0x1830) == 0x405 && regs->reg(0x1834) == 0x901);            // cull face, front face
  CHECK(regs->reg(0x0368) == 0x1D01);                                         // shade mode
  CHECK(regs->reg(0x08E4) == 1 && regs->reg(0x1D8C) == 0xFFFFFF00);           // shader program, zs clear
  CHECK(regs->reg(0x1A04) == 0x18429 && regs->reg(0x1A04 + 15 * 0x20) == 0x18429);
  CHECK(regs->reg(0x1A14) == 0x2052000 && regs->reg(0x1A08 + 3 * 0x20) == 0x30101);
  CHECK(regs->reg(0x1740) == 2 && regs->reg(0x177C) == 2 && regs->reg(0x1680) == 0);
  CHECK(regs->reg(0x0904 + 3 * 0x20) == 0x1BC21 && regs->reg(0x0904 + 4 * 0x20) == 0);
  CHECK(regs->reg(0x1DB0) == 0xFFFFFFFF && regs->reg(0x1D7C) == 0xFFFF0000);

  // The command types compose into the variant and start out zeroed.
  dal::RsxCommand command = dal::RsxDraw{};
  CHECK(std::holds_alternative<dal::RsxDraw>(command));
  command = dal::RsxFlip{};
  CHECK(std::get<dal::RsxFlip>(command).flip_index == 0);
  command = dal::RsxClear{};
  CHECK(std::get<dal::RsxClear>(command).surfaces.color.pitch == 0);
  CHECK(static_cast<int>(dal::RsxPrimitive::Points) == 1 && static_cast<int>(dal::RsxPrimitive::Polygon) == 10);
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
  testVfsConfig();
  testSceNpAndTrophy();
  testCellSysutil();
  testVideoAndAudioOut();
  testCellPad();
  testCellMsgDialog();
  testCellSaveData();
  testCellPngDec();
  testFifoParser();
  testRsxArgCensus();
  testRsxRegisters();
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
