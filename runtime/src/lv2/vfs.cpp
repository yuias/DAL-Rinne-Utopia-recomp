// Guest path resolution. Device layout follows RPCS3 `Emu/System.cpp` (`Emulator::Load` mounts
// /dev_hdd0 and /app_home; /app_home is the directory of the boot ELF) and the mount point table
// in `Emu/Cell/lv2/sys_fs.cpp` (read-only flags, block sizes).

#include "lv2/vfs.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <mutex>
#include <system_error>

#include "core/log.h"

namespace dal {
namespace {

struct VfsState {
  std::filesystem::path game_dir;
  std::optional<std::filesystem::path> hdd0;
  std::optional<std::filesystem::path> hdd1;
};

std::mutex g_mutex;
VfsState g_state;

// Components the host would interpret instead of treating them as plain names.
bool isHostSafeName(std::string_view name) {
  if (name.empty() || name.size() > 255) {
    return false;
  }
  for (const char c : name) {
    if (static_cast<unsigned char>(c) < 0x20 || std::string_view("\\:*?\"<>|").find(c) != std::string_view::npos) {
      return false;
    }
  }
  // Win32 device names open the device whatever the extension is.
  const std::string_view stem = name.substr(0, name.find('.'));
  std::string upper;
  for (const char c : stem) {
    upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
    return false;
  }
  if (upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) &&
      upper[3] >= '0' && upper[3] <= '9') {
    return false;
  }
  return true;
}

std::filesystem::path appendUtf8(std::filesystem::path path, std::string_view name) {
  const std::u8string utf8(reinterpret_cast<const char8_t*>(name.data()), name.size());
  path /= std::filesystem::path(utf8);
  return path;
}

// Directories the console's own software keeps on hdd0; a game may not create entries directly
// under /dev_hdd0 or /dev_hdd0/game, so they must exist up front. The set follows RPCS3
// `Emu/System.cpp` `Emulator::Init`, with its default user id 00000001.
constexpr std::string_view kHdd0Skeleton[] = {
    "game",   "home/00000001/exdata", "home/00000001/savedata", "home/00000001/trophy",
    "savedata", "photo", "music", "theme", "video", "drm", "vsh", "crash_report", "tmp",
    "mms",    "data", "vm"};

bool endsWithDirMarker(std::string_view guest) {
  return guest.ends_with('/') || guest.ends_with("/.") || guest.ends_with("/..");
}

}  // namespace

void configureVfs(std::filesystem::path game_dir, std::optional<std::filesystem::path> hdd_dir) {
  VfsState state;
  state.game_dir = std::move(game_dir);
  if (hdd_dir) {
    state.hdd0 = *hdd_dir / "dev_hdd0";
    state.hdd1 = *hdd_dir / "dev_hdd1";
    std::error_code ec;
    std::filesystem::create_directories(*state.hdd1, ec);
    for (const std::string_view sub : kHdd0Skeleton) {
      if (!ec) {
        std::filesystem::create_directories(*state.hdd0 / sub, ec);
      }
    }
    if (ec) {
      logWarn("dal: fs: cannot create the hdd directories: {}", ec.message());
    }
  }
  const std::lock_guard lock(g_mutex);
  g_state = std::move(state);
}

VfsResolution resolveVfs(std::string_view guest) {
  VfsResolution result;
  if (!guest.starts_with('/')) {
    return result;
  }
  result.dir_only = endsWithDirMarker(guest);

  // Lexical normalisation; the first component is the device.
  std::vector<std::string_view> parts;
  bool escaped = false;
  for (size_t pos = 0; pos < guest.size();) {
    const size_t end = std::min(guest.find('/', pos), guest.size());
    const std::string_view name = guest.substr(pos, end - pos);
    pos = end + 1;
    if (name.empty() || name == ".") {
      continue;
    }
    if (name == "..") {
      // The parent of "/" is "/"; leaving a device is refused.
      if (parts.size() == 1) {
        escaped = true;
      }
      if (!parts.empty()) {
        parts.pop_back();
      }
      continue;
    }
    parts.push_back(name);
  }

  if (escaped) {
    result.status = VfsStatus::NotFound;
    return result;
  }
  if (parts.empty()) {
    result.status = VfsStatus::Ok;
    result.device = VfsDevice::Root;
    result.read_only = true;
    return result;
  }
  const std::string_view device_name = parts.front();

  VfsState state;
  {
    const std::lock_guard lock(g_mutex);
    state = g_state;
  }

  std::filesystem::path root;
  if (device_name == "dev_bdvd") {
    result.device = VfsDevice::Bdvd;
    result.read_only = true;
    root = state.game_dir;
  } else if (device_name == "app_home") {
    result.device = VfsDevice::AppHome;
    result.read_only = true;
    root = state.game_dir / "PS3_GAME" / "USRDIR";
  } else if (device_name == "dev_hdd0" || device_name == "dev_hdd1") {
    result.device = device_name == "dev_hdd0" ? VfsDevice::Hdd0 : VfsDevice::Hdd1;
    const auto& configured = result.device == VfsDevice::Hdd0 ? state.hdd0 : state.hdd1;
    if (!configured) {
      logOnce("vfs-no-hdd-dir", "dal: fs: {} used without --hdd-dir", std::string(device_name));
      result.status = VfsStatus::NotFound;
      return result;
    }
    root = *configured;
  } else {
    return result;
  }

  result.host = std::move(root);
  try {
    for (size_t i = 1; i < parts.size(); i++) {
      if (!isHostSafeName(parts[i])) {
        result.status = VfsStatus::NotFound;
        result.host.clear();
        return result;
      }
      result.host = appendUtf8(std::move(result.host), parts[i]);
    }
  } catch (const std::exception&) {
    // A name that is not valid UTF-8 cannot exist on the host.
    result.status = VfsStatus::NotFound;
    result.host.clear();
    return result;
  }
  result.status = VfsStatus::Ok;
  result.protected_parent =
      result.device == VfsDevice::Hdd0 &&
      (parts.size() == 2 || (parts.size() == 3 && parts[1] == "game"));
  return result;
}

std::optional<std::filesystem::path> resolveGuestPath(std::string_view guest, bool& read_only) {
  VfsResolution resolution = resolveVfs(guest);
  if (resolution.status != VfsStatus::Ok || resolution.device == VfsDevice::Root) {
    return std::nullopt;
  }
  read_only = resolution.read_only;
  return std::move(resolution.host);
}

std::vector<std::string> vfsMountNames() {
  const std::lock_guard lock(g_mutex);
  std::vector<std::string> names{"dev_bdvd", "app_home"};
  if (g_state.hdd0) {
    names.emplace_back("dev_hdd0");
    names.emplace_back("dev_hdd1");
  }
  return names;
}

VfsDeviceInfo vfsDeviceInfo(VfsDevice device) {
  switch (device) {
    case VfsDevice::Hdd0:
      return {4096, false};
    case VfsDevice::Hdd1:
      return {32768, true};
    case VfsDevice::Bdvd:
    case VfsDevice::AppHome:  // reflects the mount point of the boot ELF's directory
      return {2048, true};
    case VfsDevice::Root:
    case VfsDevice::None:
      break;
  }
  return {512, true};
}

}  // namespace dal
