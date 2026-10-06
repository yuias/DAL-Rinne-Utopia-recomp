// Self-checking unit tests for the runtime core library. No generated code is involved.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
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
#include "core/threads.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/sys_ppu_thread.h"
#include "lv2/sys_spu.h"
#include "lv2/syscalls.h"
#include "lv2/sys_sync.h"

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
  CHECK(table.size() == 57);  // the 53 census numbers plus the implemented 22, 41, 43 and 103
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
  testSysPrxForUser();
  testMutexAndCond();
  testEventObjects();
  testSpuAndSysmodule();
  testSpurs();
  testLwmutex();
  testPpuThreads();
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
