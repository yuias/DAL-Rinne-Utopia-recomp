#pragma once

// Host-side state of the fake RSX: the fixed guest memory layout of the RSX context area, the IO
// address mapping tables and the command buffer bookkeeping of the default context. Layout and
// mapping rules follow RPCS3 `Emu/Cell/Modules/cellGcmSys.cpp` (`_cellGcmInitBody`,
// `gcmMapEaIoAddress`, `cellGcmMapMainMemory`, `cellGcmUnmapEaIoAddress`) and
// `Emu/Cell/lv2/sys_rsx.cpp` (`sys_rsx_device_map`, `sys_rsx_context_allocate`,
// `sys_rsx_context_iomap`). Everything is accessed under the guest lock.

#include <array>
#include <condition_variable>
#include <cstdint>

namespace dal {

inline constexpr std::uint32_t kRsxDeviceAddr = 0x10000000;      // default CellGcmContextData
inline constexpr std::uint32_t kRsxDmaAddr = 0x10100000;         // RsxDmaControl
inline constexpr std::uint32_t kRsxControlAddr = 0x10100040;     // CellGcmControl {put, get, ref}
inline constexpr std::uint32_t kRsxDriverInfoAddr = 0x10200000;  // RsxDriverInfo
inline constexpr std::uint32_t kRsxLabelAddr = 0x10300000;       // RsxReports
inline constexpr std::uint32_t kRsxContextCommitSize = 0x400000;
inline constexpr std::uint32_t kRsxLocalBase = 0xC0000000;
inline constexpr std::uint32_t kRsxLocalSize = 0x0F900000;
inline constexpr std::uint32_t kRsxIoSpaceSize = 0x10000000;

// RsxReports: semaphore[1024] (4 B), notify[64] (16 B), report[2048] (16 B).
inline constexpr std::uint32_t kRsxReportsNotifyOffset = 0x1000;
inline constexpr std::uint32_t kRsxReportsReportOffset = 0x1400;
inline constexpr std::uint32_t kRsxReportsSize = 0x1400 + 2048 * 16;

// Pages of 1 MiB: effective addresses below the local memory, and the IO space (up to 512 MiB).
inline constexpr std::uint32_t kRsxEaPages = 3072;
inline constexpr std::uint32_t kRsxIoPages = 512;
inline constexpr std::uint16_t kRsxUnmapped = 0xFFFF;

// CellGcmError and the lv2 error the IO map reports for a range the console cannot map.
inline constexpr std::uint32_t kGcmErrorFailure = 0x802100ff;
inline constexpr std::uint32_t kGcmErrorNoIoPageTable = 0x80210001;
inline constexpr std::uint32_t kGcmErrorIoMapInvalid = 0x80010002;

struct RsxState {
  RsxState() {
    io_of_ea.fill(kRsxUnmapped);
    ea_of_io.fill(kRsxUnmapped);
    io_map_size.fill(0);
  }

  bool initialized = false;
  std::uint32_t local_size = 0;
  std::uint32_t io_address = 0, io_size = 0, context_pp = 0;
  std::array<std::uint16_t, kRsxEaPages> io_of_ea;     // IO MiB of an EA MiB, 0xFFFF = unmapped
  std::array<std::uint16_t, kRsxIoPages> ea_of_io;     // EA MiB of an IO MiB
  std::array<std::uint16_t, kRsxEaPages> io_map_size;  // size in MiB of the mapping starting at an EA MiB
  std::uint32_t command_begin = 0, fragment_count = 0;
  std::uint32_t command_size = 0x400, segment_size = 0x100;  // gcmInfo defaults, in words
  std::uint32_t wrap_callback = 0;                           // guest OPD of the command buffer wrap callback
};

RsxState& rsxState();

// Back to the uninitialised state and decommits what initialisation committed. For tests.
void resetRsxState();

// Notified by the consumer whenever the control register's get moves.
std::condition_variable_any& rsxGetAdvanced();

// IO offset -> effective address; false when the IO page is unmapped.
bool rsxIoToEa(std::uint32_t io, std::uint32_t& ea);

// cellGcmAddressToOffset: local memory first, then the IO mapping; false when neither applies.
bool rsxEaToOffset(std::uint32_t ea, std::uint32_t& offset);

// gcmMapEaIoAddress including the sys_rsx_context_iomap checks. Returns 0 or an error code: a
// zero or unaligned size/ea/io is kGcmErrorFailure; a range that is not committed guest memory,
// leaves the EA or IO space, is kGcmErrorIoMapInvalid.
std::uint32_t rsxMapEaIo(std::uint32_t ea, std::uint32_t io, std::uint32_t size);

// cellGcmMapMainMemory: maps at the first run of free IO pages. Writes the IO offset on success.
std::uint32_t rsxMapMainMemory(std::uint32_t ea, std::uint32_t size, std::uint32_t& io_offset);

// cellGcmUnmapEaIoAddress: the low 20 bits of `ea` are ignored.
std::uint32_t rsxUnmapEa(std::uint32_t ea);

}  // namespace dal
