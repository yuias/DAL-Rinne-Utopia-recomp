// lv2 syscall dispatch. Handlers live in their own files and are referenced from the table.

#include "lv2/syscalls.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>

#include "core/census.h"
#include "core/log.h"
#include "dal/ppu_context.h"
#include "lv2/cell_error.h"

namespace dal {

// Handlers defined in sys_misc.cpp.
void sysProcessExit(DalPpuContext* ctx, std::uint8_t* base);
void sysTtyRead(DalPpuContext* ctx, std::uint8_t* base);
void sysTtyWrite(DalPpuContext* ctx, std::uint8_t* base);
void sysTimerUsleep(DalPpuContext* ctx, std::uint8_t* base);
void sysTimerSleep(DalPpuContext* ctx, std::uint8_t* base);
void sysTimeGetTimebaseFrequency(DalPpuContext* ctx, std::uint8_t* base);
void sysMemoryAllocate(DalPpuContext* ctx, std::uint8_t* base);
void sysMemoryFree(DalPpuContext* ctx, std::uint8_t* base);
void sysMemoryGetUserMemorySize(DalPpuContext* ctx, std::uint8_t* base);

namespace {

// The numbers the game's code issues (found by scanning the generated code for the r11 value in
// front of each syscall site) plus the implemented ones. Names come from config/lv2_syscalls.json,
// which mirrors RPCS3 `Emu/Cell/lv2/lv2.cpp`; 988 has none. Keep sorted by number.
constexpr SyscallEntry kSyscalls[] = {
    {22, "_sys_process_exit", sysProcessExit},
    {44, "sys_ppu_thread_join", nullptr},
    {47, "sys_ppu_thread_set_priority", nullptr},
    {48, "sys_ppu_thread_get_priority", nullptr},
    {82, "sys_event_flag_create", nullptr},
    {83, "sys_event_flag_destroy", nullptr},
    {85, "sys_event_flag_wait", nullptr},
    {87, "sys_event_flag_set", nullptr},
    {100, "sys_mutex_create", nullptr},
    {101, "sys_mutex_destroy", nullptr},
    {102, "sys_mutex_lock", nullptr},
    {104, "sys_mutex_unlock", nullptr},
    {105, "sys_cond_create", nullptr},
    {106, "sys_cond_destroy", nullptr},
    {107, "sys_cond_wait", nullptr},
    {108, "sys_cond_signal", nullptr},
    {109, "sys_cond_signal_all", nullptr},
    {118, "sys_event_flag_clear", nullptr},
    {128, "sys_event_queue_create", nullptr},
    {129, "sys_event_queue_destroy", nullptr},
    {130, "sys_event_queue_receive", nullptr},
    {132, "sys_event_flag_cancel", nullptr},
    {133, "sys_event_queue_drain", nullptr},
    {134, "sys_event_port_create", nullptr},
    {135, "sys_event_port_destroy", nullptr},
    {136, "sys_event_port_connect_local", nullptr},
    {137, "sys_event_port_disconnect", nullptr},
    {138, "sys_event_port_send", nullptr},
    {139, "sys_event_flag_get", nullptr},
    {141, "sys_timer_usleep", sysTimerUsleep},
    {142, "sys_timer_sleep", sysTimerSleep},
    {147, "sys_time_get_timebase_frequency", sysTimeGetTimebaseFrequency},
    {169, "sys_spu_initialize", nullptr},
    {181, "sys_spu_thread_write_ls", nullptr},
    {182, "sys_spu_thread_read_ls", nullptr},
    {190, "sys_spu_thread_write_spu_mb", nullptr},
    {191, "sys_spu_thread_connect_event", nullptr},
    {192, "sys_spu_thread_disconnect_event", nullptr},
    {251, "sys_spu_thread_group_connect_event_all_threads", nullptr},
    {252, "sys_spu_thread_group_disconnect_event_all_threads", nullptr},
    {348, "sys_memory_allocate", sysMemoryAllocate},
    {349, "sys_memory_free", sysMemoryFree},
    {352, "sys_memory_get_user_memory_size", sysMemoryGetUserMemorySize},
    {402, "sys_tty_read", sysTtyRead},
    {403, "sys_tty_write", sysTtyWrite},
    {801, "sys_fs_open", nullptr},
    {802, "sys_fs_read", nullptr},
    {803, "sys_fs_write", nullptr},
    {804, "sys_fs_close", nullptr},
    {805, "sys_fs_opendir", nullptr},
    {806, "sys_fs_readdir", nullptr},
    {807, "sys_fs_closedir", nullptr},
    {818, "sys_fs_lseek", nullptr},
    {988, nullptr, nullptr},
};

static_assert(std::ranges::is_sorted(kSyscalls, {}, &SyscallEntry::number),
              "syscall table must be sorted by number");

}  // namespace

std::span<const SyscallEntry> syscallTable() { return kSyscalls; }

const SyscallEntry* findSyscall(uint32_t number) {
  const auto it = std::ranges::lower_bound(kSyscalls, number, {}, &SyscallEntry::number);
  return it != std::end(kSyscalls) && it->number == number ? &*it : nullptr;
}

}  // namespace dal

extern "C" {

void dalSyscall(DalPpuContext* ctx, uint8_t* base, uint32_t site) {
  (void)site;
  const uint32_t number = static_cast<uint32_t>(ctx->gpr[11]);
  const dal::SyscallEntry* entry = dal::findSyscall(number);
  const char* name = entry != nullptr ? entry->name : nullptr;
  const bool implemented = entry != nullptr && entry->handler != nullptr;
  dal::censusSyscall(number, name, implemented);

  if (implemented) {
    entry->handler(ctx, base);
    return;
  }

  // The key is built on the stack: unimplemented syscalls can be hot.
  std::array<char, 24> key;
  const auto end = std::format_to_n(key.data(), key.size(), "default-sc-{}", number);
  dal::logOnce(std::string_view(key.data(), static_cast<size_t>(end.out - key.data())),
               "dal: default sc {} {}({:#x}, {:#x}, {:#x}, {:#x})", number,
               name != nullptr ? name : "?", ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]);
  ctx->gpr[3] = dal::kCellOk;
}

}  // extern "C"
