// Self-checking unit tests for the runtime core library. No generated code is involved.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/census.h"
#include "core/elf_loader.h"
#include "core/fatal.h"
#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "dal/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/syscalls.h"

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
  CHECK(table.size() == 53);
  bool sorted = true;
  for (std::size_t i = 1; i < table.size(); i++) sorted = sorted && table[i - 1].number < table[i].number;
  CHECK(sorted);
  const dal::SyscallEntry* usleep_entry = dal::findSyscall(141);
  CHECK(usleep_entry != nullptr && std::string_view(usleep_entry->name) == "sys_timer_usleep");
  const dal::SyscallEntry* unnamed = dal::findSyscall(988);
  CHECK(unnamed != nullptr && unnamed->name == nullptr);
  CHECK(dal::findSyscall(999) == nullptr);

  auto ctx = std::make_unique<DalPpuContext>();
  ctx->gpr[11] = 141;
  ctx->gpr[3] = 0x1234;
  dalSyscall(ctx.get(), nullptr, 0x10000);
  CHECK(ctx->gpr[3] == dal::kCellOk);
  dalSyscall(ctx.get(), nullptr, 0x10000);
  ctx->gpr[11] = 999;
  ctx->gpr[3] = 0x1234;
  dalSyscall(ctx.get(), nullptr, 0x10004);
  CHECK(ctx->gpr[3] == dal::kCellOk);

  CHECK(countContaining(lines, "dal: first sc 141 sys_timer_usleep") == 1);
  CHECK(countContaining(lines, "dal: first sc 999 ?") == 1);
  CHECK(countContaining(lines, "dal: default sc 141 sys_timer_usleep(0x1234") == 1);
  CHECK(countContaining(lines, "dal: default sc 999 ?(0x1234") == 1);

  const std::string census = dal::formatCensus();
  CHECK(census.find("census: syscalls 3 calls, 2 distinct, 2 default-handled") != std::string::npos);
  CHECK(census.find("  sc 141 sys_timer_usleep: 2 [default]") != std::string::npos);
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
  testTimebase();
  testGuestLockFifo();
  testReservationAndDcbz();
  testFaultChild();
  testOverflowChild();
  testStopChild();
  testStrictDispatchChild();
  testUnsortedTableChild();
  std::printf("runtime core tests: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed != 0 ? 1 : 0;
}
