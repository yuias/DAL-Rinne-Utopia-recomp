// HLE handlers of the cellSail player: MemAllocatorInitialize, PlayerInitialize2, Finalize, the
// adapter and repeat-mode setters, Boot, the descriptor calls (Create, Add, Destroy) and the
// descriptor's database and capability calls. Behaviour follows RPCS3 `Emu/Cell/Modules/cellSail.cpp`
// (the function of the same name) for argument handling, error codes and the event pairing: Initialize2
// reports PLAYER_STATE_CHANGED(INITIALIZED), Boot reports PLAYER_STATE_CHANGED(BOOT_TRANSITION) and
// then PLAYER_CALL_COMPLETED(BOOT), and CreateDescriptor accepts `x-cell-fs://` URIs and reads the PAMF
// header. The rest of the event script comes from the game's own notify callback, which stores the
// state and clears its busy byte and ignores the other majors.
//
// Deviations from RPCS3:
// - Events are queued to the `_libsail-control` service thread instead of calling the callback inside
//   the HLE call, as the library's own thread would do. PlayerFinalize waits for the queue to drain
//   (bounded), so the game can free its callback argument once Finalize returns.
// - Initialize2 on an initialised player returns INVALID_STATE (RPCS3 overwrites the player).
// - Boot reports CLOSED after BOOT_TRANSITION: that is the state a booted player is in.
// - A file that cannot be read still gets a descriptor (RPCS3 does the same); CreateDatabase reports
//   the failure. The game ignores CreateDescriptor's result and needs the pointer for every later
//   call, and its Open/Start waits are only released by events.
// - SetRepeatMode returns CELL_OK on success (RPCS3 returns the mode).
//
// Object state lives host-side, keyed by the address of the guest object; the game allocates the
// player and the adapters and the HLE never writes them. Only the descriptor block is allocated here,
// through the game's own allocator callbacks, because the game holds its address. Everything runs
// under the guest lock. Allocator callbacks run on the calling guest thread, which is how the game
// expects them to be called; notify callbacks run on the service thread.

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/guest_memory.h"
#include "core/log.h"
#include "core/threads.h"
#include "dal/ppu_context.h"
#include "hle/cell_sail_internal.h"
#include "hle/cell_sail_types.h"
#include "hle/hle.h"
#include "hle/sail_service.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"
#include "lv2/vfs.h"
#include "media/pamf.h"

namespace dal {
namespace {

constexpr char kModule[] = "cellSail";
constexpr char kUriScheme[] = "x-cell-fs://";
constexpr std::uint32_t kUriMaxLength = 1024;
// Same read the host movie player does: the sequence information and stream table are in the first
// 2048 bytes, and the EP table behind them is not needed for linear playback.
constexpr std::size_t kPamfHeaderReadSize = 2048;

// What the game's allocator callback receives for a descriptor. Its heap (the callback forwards to
// it with the boundary as the alignment) clamps the alignment to at least 16 bytes and rounds it up
// to a power of two, and takes no other boundary rule, so 16 is enough for a block nothing reads.
constexpr std::uint32_t kDescriptorBoundary = 0x10;

// Bound on PlayerFinalize's wait for the service thread, which may be inside an allocator callback.
constexpr auto kFinalizeTimeout = std::chrono::seconds(2);

struct Player {
  std::uint64_t serial = 0;  // tells a queued event which incarnation of the address it was for
  std::uint32_t alloc_opd = 0, free_opd = 0, alloc_arg = 0;
  std::uint32_t notify_opd = 0, notify_arg = 0;
  std::uint32_t max_audio_streams = 0, max_video_streams = 0;
  sail::PlayerState state = sail::PlayerState::Initialized;
  std::int32_t repeat_mode = 0;
  std::array<std::uint8_t, sail::kStartCommandSize> repeat_command{};
};

struct Descriptor {
  std::uint32_t player = 0;  // 0 once the player is finalized
  bool registered = false;   // added to a live player
  std::uint32_t free_opd = 0, alloc_arg = 0;
  std::string guest_path;
  std::optional<PamfInfo> info;
  std::uint32_t error_code = kCellOk;  // why `info` is empty
};

std::map<std::uint32_t, Player> g_players;
std::map<std::uint32_t, Descriptor> g_descriptors;
std::uint64_t g_next_serial = 1;

using sail_hle::guestMapped;

struct Event {
  std::uint32_t major;
  std::uint32_t minor;
  std::uint64_t arg0;
  std::uint64_t arg1;
};

bool isCurrent(std::uint32_t address, std::uint64_t serial) {
  const auto it = g_players.find(address);
  return it != g_players.end() && it->second.serial == serial;
}

// Delivers `events` to the player's notify callback on the service thread, in order. Events for a
// player that was finalized meanwhile are dropped, so nothing reaches the game's callback argument
// after Finalize.
void queueEvents(std::uint32_t address, const Player& player, std::vector<Event> events) {
  queueSailCall([address, serial = player.serial, opd = player.notify_opd, arg = player.notify_arg,
                 events = std::move(events)](DalPpuContext* ctx, std::uint8_t* base) {
    for (const Event& event : events) {
      if (!isCurrent(address, serial)) return;
      callGuestFunction(ctx, base, opd,
                        {arg, sail::makeEvent(event.major, event.minor), event.arg0, event.arg1});
    }
  });
}

constexpr Event stateChanged(sail::PlayerState state) {
  return {sail::kEventPlayerStateChanged, 0, static_cast<std::uint64_t>(state), 0};
}

constexpr Event callCompleted(std::uint32_t call) {
  return {sail::kEventPlayerCallCompleted, call, 0, 0};
}

// Waits until every call queued before this one has run. False on timeout. Returns at once when the
// service thread cannot run at all, since nothing is pending then.
bool drainServiceQueue() {
  struct Marker {
    std::condition_variable_any done_cv;
    bool done = false;
  };
  auto marker = std::make_shared<Marker>();
  const bool queued = queueSailCall([marker](DalPpuContext*, std::uint8_t*) {
    marker->done = true;
    marker->done_cv.notify_all();
  });
  if (!queued) return true;
  return blockingWait(marker->done_cv, [&] { return marker->done; }, "sail finalize", kFinalizeTimeout);
}

Player* findPlayer(std::uint32_t address) {
  const auto it = g_players.find(address);
  return it == g_players.end() ? nullptr : &it->second;
}

Descriptor* findDescriptor(std::uint32_t address) {
  const auto it = g_descriptors.find(address);
  return it == g_descriptors.end() ? nullptr : &it->second;
}

// cellSailMemAllocatorInitialize(CellSailMemAllocator* self, CellSailMemAllocatorFuncs* callbacks,
//                                void* arg)
// The third argument is not in RPCS3's two-argument signature; the game passes one and the player
// passes it back to every allocator callback.
void sailMemAllocatorInitialize(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t self = argU32(ctx, 0);
  const std::uint32_t callbacks = argU32(ctx, 1);
  if (!guestMapped(self, sail::kAllocatorSize) || callbacks == 0) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  dalWrite32(base, self + sail::kAllocatorCallbacks, callbacks);
  dalWrite32(base, self + sail::kAllocatorArg, argU32(ctx, 2));
  setResultCode(ctx, kCellOk);
}

// cellSailPlayerInitialize2(CellSailPlayer* self, CellSailMemAllocator* allocator,
//                           CellSailPlayerFuncNotified callback, void* callback_arg,
//                           CellSailPlayerAttribute* attribute, CellSailPlayerResource* resource)
void playerInitialize2(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t self = argU32(ctx, 0);
  const std::uint32_t allocator = argU32(ctx, 1);
  const std::uint32_t callback = argU32(ctx, 2);
  const std::uint32_t attribute = argU32(ctx, 4);
  if (self == 0 || callback == 0 || !guestMapped(allocator, sail::kAllocatorSize) ||
      !guestMapped(attribute, sail::kPlayerAttributeSize)) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  const std::uint32_t funcs = dalRead32(base, allocator + sail::kAllocatorCallbacks);
  if (!guestMapped(funcs, sail::kAllocatorFuncsSize)) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  if (g_players.contains(self)) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }

  Player player;
  player.serial = g_next_serial++;
  player.alloc_opd = dalRead32(base, funcs + sail::kAllocatorFuncsAlloc);
  player.free_opd = dalRead32(base, funcs + sail::kAllocatorFuncsFree);
  player.alloc_arg = dalRead32(base, allocator + sail::kAllocatorArg);
  player.notify_opd = callback;
  player.notify_arg = argU32(ctx, 3);
  player.max_audio_streams = dalRead32(base, attribute + sail::kPlayerAttributeMaxAudioStreams);
  player.max_video_streams = dalRead32(base, attribute + sail::kPlayerAttributeMaxVideoStreams);
  const auto it = g_players.emplace(self, player).first;
  logDebug("dal: sail: player {:#010x} initialised (notify {:#010x}, arg {:#010x})", self, callback,
           player.notify_arg);

  queueEvents(self, it->second, {stateChanged(sail::PlayerState::Initialized)});
  setResultCode(ctx, kCellOk);
}

// cellSailPlayerFinalize(CellSailPlayer* self)
// The game calls it from whatever state the movie ended in and frees its callback argument right
// after, so queued events are delivered first and none follows.
void playerFinalize(DalPpuContext* ctx, std::uint8_t*) {
  const std::uint32_t self = argU32(ctx, 0);
  if (findPlayer(self) == nullptr) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  if (!drainServiceQueue()) {
    logWarn("dal: sail: the service thread did not drain within {} s while finalizing player {:#010x}",
            kFinalizeTimeout.count(), self);
  }
  sail_hle::unbindAdapters(self);
  for (auto& entry : g_descriptors) {
    if (entry.second.player == self) {
      entry.second.player = 0;
      entry.second.registered = false;
    }
  }
  g_players.erase(self);
  logDebug("dal: sail: player {:#010x} finalized", self);
  setResultCode(ctx, kCellOk);
}

// cellSailPlayerSetSoundAdapter(CellSailPlayer* self, u32 index, CellSailSoundAdapter* adapter)
void playerSetSoundAdapter(DalPpuContext* ctx, std::uint8_t*) {
  const std::uint32_t self = argU32(ctx, 0);
  const Player* player = findPlayer(self);
  if (player == nullptr) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  // RPCS3 compares with `>`, so index == max is accepted.
  if (argU32(ctx, 1) > player->max_audio_streams) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  setResultCode(ctx, sail_hle::bindAdapter(sail_hle::AdapterKind::Sound, argU32(ctx, 2), self));
}

// cellSailPlayerSetGraphicsAdapter(CellSailPlayer* self, u32 index, CellSailGraphicsAdapter* adapter)
void playerSetGraphicsAdapter(DalPpuContext* ctx, std::uint8_t*) {
  const std::uint32_t self = argU32(ctx, 0);
  const Player* player = findPlayer(self);
  if (player == nullptr) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  if (argU32(ctx, 1) > player->max_video_streams) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  setResultCode(ctx, sail_hle::bindAdapter(sail_hle::AdapterKind::Graphics, argU32(ctx, 2), self));
}

// cellSailPlayerSetRepeatMode(CellSailPlayer* self, s32 repeat_mode, CellSailStartCommand* command)
void playerSetRepeatMode(DalPpuContext* ctx, std::uint8_t* base) {
  Player* player = findPlayer(argU32(ctx, 0));
  if (player == nullptr) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  player->repeat_mode = static_cast<std::int32_t>(argU32(ctx, 1));
  const std::uint32_t command = argU32(ctx, 2);
  if (guestMapped(command, sail::kStartCommandSize)) {
    std::memcpy(player->repeat_command.data(), base + command, sail::kStartCommandSize);
  }
  setResultCode(ctx, kCellOk);
}

// cellSailPlayerBoot(CellSailPlayer* self, u64 user_param)
void playerBoot(DalPpuContext* ctx, std::uint8_t*) {
  const std::uint32_t self = argU32(ctx, 0);
  Player* player = findPlayer(self);
  if (player == nullptr || player->state != sail::PlayerState::Initialized) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  player->state = sail::PlayerState::Closed;
  queueEvents(self, *player,
              {stateChanged(sail::PlayerState::BootTransition), stateChanged(sail::PlayerState::Closed),
               callCompleted(sail::kCallBoot)});
  setResultCode(ctx, kCellOk);
}

struct HeaderRead {
  std::optional<PamfInfo> info;
  std::uint32_t error_code = kCellOk;
  std::string error;
};

// Reads and parses the PAMF header of the guest file `guest_path`. Nothing here formats a host path:
// the file name can contain characters the narrow conversion rejects.
HeaderRead readPamfHeader(const std::string& guest_path) {
  HeaderRead result;
  try {
    bool read_only = false;
    const auto host = resolveGuestPath(guest_path, read_only);
    if (!host) {
      result.error_code = sail::kErrorInvalidFd;
      result.error = "the path is not mapped";
      return result;
    }
    std::ifstream in(*host, std::ios::binary);
    if (!in) {
      result.error_code = sail::kErrorInvalidFd;
      result.error = "the file cannot be opened";
      return result;
    }
    std::vector<std::uint8_t> header(kPamfHeaderReadSize);
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const auto got = static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0));
    auto parsed = parsePamfHeader(std::span<const std::uint8_t>(header).first(got));
    if (!parsed) {
      result.error_code = sail::kErrorUnsupportedStream;
      result.error = parsed.error();
      return result;
    }
    result.info = std::move(*parsed);
  } catch (const std::exception&) {
    result.info.reset();
    result.error_code = sail::kErrorInvalidFd;
    result.error = "file system error";
  }
  return result;
}

// cellSailPlayerCreateDescriptor(CellSailPlayer* self, s32 stream_type, void* media_info,
//                                const char* uri, CellSailDescriptor** desc)
void playerCreateDescriptor(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t self = argU32(ctx, 0);
  const std::uint32_t uri_addr = argU32(ctx, 3);
  const std::uint32_t out = argU32(ctx, 4);
  const Player* player = findPlayer(self);
  if (player == nullptr) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  if (!guestMapped(uri_addr, 1) || !guestMapped(out, 4)) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  dalWrite32(base, out, 0);
  if (static_cast<std::int32_t>(argU32(ctx, 1)) != sail::kStreamPamf) {
    logWarn("dal: sail: descriptor stream type {} is not supported", static_cast<std::int32_t>(argU32(ctx, 1)));
    setResultCode(ctx, sail::kErrorUnsupportedStream);
    return;
  }
  const std::string uri = readGuestString(base, uri_addr, kUriMaxLength);
  if (!uri.starts_with(kUriScheme)) {
    logWarn("dal: sail: descriptor URI {} is not an {} URI", uri, kUriScheme);
    setResultCode(ctx, sail::kErrorUnsupportedStream);
    return;
  }

  if (player->alloc_opd == 0) {
    setResultCode(ctx, sail::kErrorMemory);
    return;
  }
  // The callback may wait inside the game's heap, so nothing is read through `player` afterwards.
  const std::uint32_t alloc_arg = player->alloc_arg;
  const std::uint32_t free_opd = player->free_opd;
  const std::uint64_t serial = player->serial;
  const auto block = static_cast<std::uint32_t>(callGuestFunction(
      ctx, base, player->alloc_opd, {alloc_arg, kDescriptorBoundary, sail::kDescriptorMaxSize}));
  if (!guestMapped(block, sail::kDescriptorMaxSize)) {
    logError("dal: sail: the allocator callback returned {:#010x} for a descriptor", block);
    setResultCode(ctx, sail::kErrorMemory);
    return;
  }
  std::memset(base + block, 0, sail::kDescriptorMaxSize);

  Descriptor descriptor;
  // The callback can release the guest lock, so the player may have been finalized (or its address
  // reused) meanwhile. The descriptor is then left unlinked: the game still gets the block and can
  // destroy it, but no finalized player's state is attached to it.
  descriptor.player = isCurrent(self, serial) ? self : 0;
  descriptor.free_opd = free_opd;
  descriptor.alloc_arg = alloc_arg;
  descriptor.guest_path = uri.substr(sizeof(kUriScheme) - 1);
  HeaderRead header = readPamfHeader(descriptor.guest_path);
  descriptor.info = std::move(header.info);
  descriptor.error_code = header.error_code;
  if (descriptor.info) {
    const PamfInfo& info = *descriptor.info;
    logInfo("dal: sail: descriptor {:#010x} for {}: {} streams, {} .. {} (90 kHz)", block,
            descriptor.guest_path, info.streams.size(), info.start_pts, info.end_pts);
  } else {
    logError("dal: sail: descriptor {:#010x} for {}: {}", block, descriptor.guest_path, header.error);
  }
  if (descriptor.player == 0) {
    logWarn("dal: sail: player {:#010x} was finalized while its descriptor was allocated", self);
  }
  // A block that is already tracked means the game freed it without DestroyDescriptor and got the
  // address back; the new descriptor replaces the stale one.
  if (g_descriptors.contains(block)) {
    logWarn("dal: sail: descriptor block {:#010x} was still tracked and is replaced", block);
  }
  g_descriptors.insert_or_assign(block, std::move(descriptor));
  dalWrite32(base, out, block);
  setResultCode(ctx, kCellOk);
}

// cellSailPlayerAddDescriptor(CellSailPlayer* self, CellSailDescriptor* desc)
void playerAddDescriptor(DalPpuContext* ctx, std::uint8_t*) {
  const std::uint32_t self = argU32(ctx, 0);
  if (findPlayer(self) == nullptr) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  Descriptor* descriptor = findDescriptor(argU32(ctx, 1));
  if (descriptor == nullptr || descriptor->player != self) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  if (descriptor->registered) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  descriptor->registered = true;
  setResultCode(ctx, kCellOk);
}

// cellSailPlayerDestroyDescriptor(CellSailPlayer* self, CellSailDescriptor* desc)
// The game calls it after PlayerFinalize, which releases the descriptor; a descriptor still added to
// a live player is refused (INVALID_STATE, as in RPCS3).
void playerDestroyDescriptor(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t self = argU32(ctx, 0);
  const std::uint32_t address = argU32(ctx, 1);
  const Descriptor* descriptor = findDescriptor(address);
  if (descriptor == nullptr || (descriptor->player != 0 && descriptor->player != self)) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  if (descriptor->registered) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  const std::uint32_t free_opd = descriptor->free_opd;
  const std::uint32_t alloc_arg = descriptor->alloc_arg;
  g_descriptors.erase(address);
  if (free_opd != 0) {
    callGuestFunction(ctx, base, free_opd, {alloc_arg, kDescriptorBoundary, address});
  }
  setResultCode(ctx, kCellOk);
}

// cellSailDescriptorCreateDatabase(CellSailDescriptor* self, void* database, u32 size, u64 arg)
// The block is a CellPamfReader-sized object only cellPamfReaderGetNumberOfSpecificStreams reads.
void descriptorCreateDatabase(DalPpuContext* ctx, std::uint8_t* base) {
  const Descriptor* descriptor = findDescriptor(argU32(ctx, 0));
  const std::uint32_t database = argU32(ctx, 1);
  if (descriptor == nullptr || argU32(ctx, 2) < sail::kPamfReaderSize ||
      !guestMapped(database, sail::kPamfReaderSize)) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  if (!descriptor->info) {
    setResultCode(ctx, descriptor->error_code);
    return;
  }
  sail_hle::writePamfDatabase(base, database, *descriptor->info);
  setResultCode(ctx, kCellOk);
}

// cellSailDescriptorDestroyDatabase(CellSailDescriptor* self, void* database)
void descriptorDestroyDatabase(DalPpuContext* ctx, std::uint8_t* base) {
  if (findDescriptor(argU32(ctx, 0)) == nullptr || !sail_hle::clearPamfDatabase(base, argU32(ctx, 1))) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  setResultCode(ctx, kCellOk);
}

// cellSailDescriptorGetCapabilities(CellSailDescriptor* self, u32* out0, u32* out1, u32* out2,
//                                   u32* out3)
// RPCS3 has no signature for this call. The game passes four word pointers and keeps bit 1 of the
// last one without ever reading it, so the capabilities are all reported as clear.
void descriptorGetCapabilities(DalPpuContext* ctx, std::uint8_t* base) {
  if (findDescriptor(argU32(ctx, 0)) == nullptr) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  for (int i = 1; i <= 4; i++) {
    const std::uint32_t out = argU32(ctx, i);
    if (guestMapped(out, 4)) dalWrite32(base, out, 0);
  }
  setResultCode(ctx, kCellOk);
}

constexpr HleEntry kHandlers[] = {
    {kModule, 0x346ebba3, "cellSailMemAllocatorInitialize", sailMemAllocatorInitialize},
    {kModule, 0x23654375, "cellSailPlayerInitialize2", playerInitialize2},
    {kModule, 0x18b4629d, "cellSailPlayerFinalize", playerFinalize},
    {kModule, 0x1139a206, "cellSailPlayerSetSoundAdapter", playerSetSoundAdapter},
    {kModule, 0x18bcd21b, "cellSailPlayerSetGraphicsAdapter", playerSetGraphicsAdapter},
    {kModule, 0xfc5baf8a, "cellSailPlayerSetRepeatMode", playerSetRepeatMode},
    {kModule, 0xbdf21b0f, "cellSailPlayerBoot", playerBoot},
    {kModule, 0xd7938b8d, "cellSailPlayerCreateDescriptor", playerCreateDescriptor},
    {kModule, 0x7c8dff3b, "cellSailPlayerAddDescriptor", playerAddDescriptor},
    {kModule, 0xfc839bd4, "cellSailPlayerDestroyDescriptor", playerDestroyDescriptor},
    {kModule, 0x0abb318b, "cellSailDescriptorCreateDatabase", descriptorCreateDatabase},
    {kModule, 0x28336e89, "cellSailDescriptorDestroyDatabase", descriptorDestroyDatabase},
    {kModule, 0xac9c3b1f, "cellSailDescriptorGetCapabilities", descriptorGetCapabilities},
};

}  // namespace

std::span<const HleEntry> cellSailPlayerHandlers() { return kHandlers; }

void resetCellSail() {
  g_players.clear();
  g_descriptors.clear();
  sail_hle::resetAdapters();
}

}  // namespace dal
