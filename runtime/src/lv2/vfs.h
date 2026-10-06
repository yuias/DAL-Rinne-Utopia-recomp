#pragma once

// Guest path -> host path mapping and the host-side file system helpers shared by the sys_fs
// syscalls and the cellFs imports.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dal {

enum class VfsDevice { None, Root, Bdvd, AppHome, Hdd0, Hdd1 };

// NotMounted: the device is unknown. NotFound: the device exists but the path cannot be mapped
// (it leaves the device root, names a host-reserved file, or the hdd directory is not configured).
enum class VfsStatus { Ok, NotMounted, NotFound };

struct VfsResolution {
  VfsStatus status = VfsStatus::NotMounted;
  VfsDevice device = VfsDevice::None;
  std::filesystem::path host;  // empty for the virtual root
  bool read_only = false;
  // The guest path ended in '/', '/.' or '/..', so it must name a directory.
  bool dir_only = false;
  // Direct children of /dev_hdd0 and of /dev_hdd0/game: a game may not create entries there.
  bool protected_parent = false;
};

// Maps /dev_bdvd and /app_home onto `game_dir` (read-only) and /dev_hdd0, /dev_hdd1 onto
// `<hdd_dir>/dev_hdd0` and `<hdd_dir>/dev_hdd1`. Without an hdd directory the hdd devices fail
// to resolve. Intended to be called before guest code runs; tests may call it again.
void configureVfs(std::filesystem::path game_dir, std::optional<std::filesystem::path> hdd_dir);

// Directory holding decrypted copies of the game's sdata files, each at its path relative to the
// game directory. nullopt (the default) means sdata files cannot be opened.
void configureSdataCache(std::optional<std::filesystem::path> cache_dir);

// The cache file standing in for the sdata file `res` names; nullopt when no cache is configured
// or `res` is not under the game directory (/dev_bdvd, /app_home).
std::optional<std::filesystem::path> sdataCacheHostPath(const VfsResolution& res);

// `guest` must be an absolute guest path. "." components and empty components are dropped and
// ".." pops one component; a ".." that would pop the device itself fails with NotFound.
// Host names are matched by the host file system, so on Windows they are case-insensitive: a
// name that differs from the one on disk only in case resolves where a console would fail. The
// directories a game reads are disc dumps whose names already match the game's requests, so no
// case-sensitive layer is added.
VfsResolution resolveVfs(std::string_view guest);

// Convenience form: nullopt when the device is unknown/unavailable or the path escapes the root.
std::optional<std::filesystem::path> resolveGuestPath(std::string_view guest, bool& read_only);

// Names of the mounted devices without the leading '/', for listing the virtual root.
std::vector<std::string> vfsMountNames();

// Block size and uid/gid policy of a device (RPCS3 lv2_fs_mount_point).
struct VfsDeviceInfo {
  std::uint32_t block_size;
  bool no_uid_gid;
};
VfsDeviceInfo vfsDeviceInfo(VfsDevice device);

// Host-side result of stat/fstat; times are seconds since the Unix epoch.
struct FsStat {
  std::int32_t mode = 0;
  std::int32_t uid = 0;
  std::int32_t gid = 0;
  std::int64_t atime = 0;
  std::int64_t mtime = 0;
  std::int64_t ctime = 0;
  std::uint64_t size = 0;
  std::uint64_t blksize = 0;
};

inline constexpr std::uint32_t kCellFsStatSize = 52;

// Defined in sys_fs.cpp. Both return a cell error code (kCellOk on success).
std::uint32_t fsStatPath(std::string_view guest_path, FsStat& out);
std::uint32_t fsStatFd(std::uint32_t fd, FsStat& out);

// Writes the big-endian CellFsStat (kCellFsStatSize bytes) at guest address `addr`.
void writeCellFsStat(std::uint8_t* base, std::uint32_t addr, const FsStat& stat);

// Guest-facing operations behind the sys_fs syscalls, also used by the cellFs imports (same object
// table). Arguments are guest addresses read like the console does; each returns a cell error code
// and writes its outputs only on success, except the read/write counts, which are zeroed on failure.
// Defined in sys_fs.cpp.
std::uint32_t fsOpenGuest(std::uint8_t* base, std::uint32_t path_addr, std::int32_t flags,
                          std::uint32_t fd_addr, std::uint32_t arg_addr, std::uint64_t arg_size);
// cellFsSdataOpen: read-only open of an sdata container file.
std::uint32_t fsSdataOpenGuest(std::uint8_t* base, std::uint32_t path_addr, std::int32_t flags,
                               std::uint32_t fd_addr);
std::uint32_t fsCloseFd(std::uint32_t fd);
std::uint32_t fsStatGuest(std::uint8_t* base, std::uint32_t path_addr, std::uint32_t sb);
std::uint32_t fsFstatGuest(std::uint8_t* base, std::uint32_t fd, std::uint32_t sb);
std::uint32_t fsMkdirGuest(std::uint8_t* base, std::uint32_t path_addr);
std::uint32_t fsUnlinkGuest(std::uint8_t* base, std::uint32_t path_addr);
// Positional read/write: the file position is left unchanged. `count_addr` may be 0.
std::uint32_t fsReadWithOffset(std::uint8_t* base, std::uint32_t fd, std::uint64_t offset,
                               std::uint32_t buf, std::uint64_t size, std::uint32_t count_addr);
std::uint32_t fsWriteWithOffset(std::uint8_t* base, std::uint32_t fd, std::uint64_t offset,
                                std::uint32_t buf, std::uint64_t size, std::uint32_t count_addr);

}  // namespace dal
