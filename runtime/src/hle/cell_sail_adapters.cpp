// HLE handlers of the cellSail graphics and sound adapters: Initialize, Finalize, SetPreferredFormat
// and GetFormat. Behaviour follows RPCS3 `Emu/Cell/Modules/cellSail.cpp` (functions of the same
// names): Initialize on an initialised adapter and Finalize on an uninitialised or still registered
// one return CELL_SAIL_ERROR_INVALID_STATE, GetFormat returns what SetPreferredFormat stored. The
// adapter state lives host-side, keyed by the address of the guest object (which the game allocates
// and the HLE never writes), so a freed and reused address starts clean once Finalize ran.
//
// Deviation from RPCS3: GraphicsAdapterInitialize there marks the adapter registered, which would
// make Finalize fail for every caller; here only the player's SetXAdapter registers an adapter.
// The adapters' makeup, cleanup and format-changed callbacks are stored but not called yet.
//
// Everything here runs under the guest lock.

#include <array>
#include <cstdint>
#include <cstring>
#include <map>

#include "core/guest_memory.h"
#include "core/log.h"
#include "dal/ppu_context.h"
#include "hle/cell_sail_internal.h"
#include "hle/cell_sail_types.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "lv2/sys_sync.h"

namespace dal {
namespace {

constexpr char kModule[] = "cellSail";

struct GraphicsAdapter {
  std::uint32_t arg = 0;
  std::uint32_t makeup = 0, cleanup = 0, format_changed = 0, alloc = 0, free = 0;  // OPD addresses
  // Guest-layout CellSailVideoFormat bytes. `preferred` is what the game asked for; `format` is what
  // GetFormat reports and starts out as the same bytes.
  std::array<std::uint8_t, sail::kVideoFormatSize> preferred;
  std::array<std::uint8_t, sail::kVideoFormatSize> format;
  std::uint32_t player = 0;  // address of the player the adapter is registered with, 0 if none
};

struct SoundAdapter {
  std::uint32_t arg = 0;
  std::uint32_t makeup = 0, cleanup = 0, format_changed = 0;
  std::array<std::uint8_t, sail::kAudioFormatSize> preferred;
  std::array<std::uint8_t, sail::kAudioFormatSize> format;
  std::uint32_t player = 0;
};

std::map<std::uint32_t, GraphicsAdapter> g_graphics;
std::map<std::uint32_t, SoundAdapter> g_sound;

using sail_hle::guestMapped;

// The game fills formats it does not care about with 0xff bytes ("unspecified"); an adapter that has
// not been given a preferred format reports the same.
template <std::size_t N>
std::array<std::uint8_t, N> unspecifiedFormat() {
  std::array<std::uint8_t, N> bytes;
  bytes.fill(0xff);
  return bytes;
}

// cellSailGraphicsAdapterInitialize(adapter, CellSailGraphicsAdapterFuncs* callbacks, void* arg)
void graphicsInitialize(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t adapter = argU32(ctx, 0);
  const std::uint32_t callbacks = argU32(ctx, 1);
  if (adapter == 0 || !guestMapped(callbacks, sail::kGraphicsAdapterFuncsSize)) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  if (g_graphics.contains(adapter)) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  GraphicsAdapter state;
  state.arg = argU32(ctx, 2);
  state.makeup = dalRead32(base, callbacks + sail::kGraphicsAdapterFuncsMakeup);
  state.cleanup = dalRead32(base, callbacks + sail::kGraphicsAdapterFuncsCleanup);
  state.format_changed = dalRead32(base, callbacks + sail::kGraphicsAdapterFuncsFormatChanged);
  state.alloc = dalRead32(base, callbacks + sail::kGraphicsAdapterFuncsAlloc);
  state.free = dalRead32(base, callbacks + sail::kGraphicsAdapterFuncsFree);
  state.preferred = unspecifiedFormat<sail::kVideoFormatSize>();
  state.format = state.preferred;
  g_graphics.emplace(adapter, state);
  logDebug("dal: sail: graphics adapter {:#010x} initialised (alloc {:#010x}, free {:#010x})", adapter,
           state.alloc, state.free);
  setResultCode(ctx, kCellOk);
}

// cellSailGraphicsAdapterFinalize(adapter)
void graphicsFinalize(DalPpuContext* ctx, std::uint8_t*) {
  const auto it = g_graphics.find(argU32(ctx, 0));
  if (it == g_graphics.end() || it->second.player != 0) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  g_graphics.erase(it);
  setResultCode(ctx, kCellOk);
}

// cellSailGraphicsAdapterSetPreferredFormat(adapter, const CellSailVideoFormat* format)
void graphicsSetPreferredFormat(DalPpuContext* ctx, std::uint8_t* base) {
  const auto it = g_graphics.find(argU32(ctx, 0));
  if (it == g_graphics.end()) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  const std::uint32_t format = argU32(ctx, 1);
  if (!guestMapped(format, sail::kVideoFormatSize)) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  std::memcpy(it->second.preferred.data(), base + format, sail::kVideoFormatSize);
  it->second.format = it->second.preferred;
  setResultCode(ctx, kCellOk);
}

// cellSailGraphicsAdapterGetFormat(adapter, CellSailVideoFormat* format)
void graphicsGetFormat(DalPpuContext* ctx, std::uint8_t* base) {
  const auto it = g_graphics.find(argU32(ctx, 0));
  if (it == g_graphics.end()) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  const std::uint32_t out = argU32(ctx, 1);
  if (!guestMapped(out, sail::kVideoFormatSize)) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  std::memcpy(base + out, it->second.format.data(), sail::kVideoFormatSize);
  setResultCode(ctx, kCellOk);
}

// cellSailSoundAdapterInitialize(adapter, CellSailSoundAdapterFuncs* callbacks, void* arg)
void soundInitialize(DalPpuContext* ctx, std::uint8_t* base) {
  const std::uint32_t adapter = argU32(ctx, 0);
  const std::uint32_t callbacks = argU32(ctx, 1);
  if (adapter == 0 || !guestMapped(callbacks, sail::kSoundAdapterFuncsSize)) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  if (g_sound.contains(adapter)) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  SoundAdapter state;
  state.arg = argU32(ctx, 2);
  state.makeup = dalRead32(base, callbacks + sail::kSoundAdapterFuncsMakeup);
  state.cleanup = dalRead32(base, callbacks + sail::kSoundAdapterFuncsCleanup);
  state.format_changed = dalRead32(base, callbacks + sail::kSoundAdapterFuncsFormatChanged);
  state.preferred = unspecifiedFormat<sail::kAudioFormatSize>();
  state.format = state.preferred;
  g_sound.emplace(adapter, state);
  logDebug("dal: sail: sound adapter {:#010x} initialised", adapter);
  setResultCode(ctx, kCellOk);
}

// cellSailSoundAdapterFinalize(adapter)
void soundFinalize(DalPpuContext* ctx, std::uint8_t*) {
  const auto it = g_sound.find(argU32(ctx, 0));
  if (it == g_sound.end() || it->second.player != 0) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  g_sound.erase(it);
  setResultCode(ctx, kCellOk);
}

// cellSailSoundAdapterSetPreferredFormat(adapter, const CellSailAudioFormat* format)
void soundSetPreferredFormat(DalPpuContext* ctx, std::uint8_t* base) {
  const auto it = g_sound.find(argU32(ctx, 0));
  if (it == g_sound.end()) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  const std::uint32_t format = argU32(ctx, 1);
  if (!guestMapped(format, sail::kAudioFormatSize)) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  std::memcpy(it->second.preferred.data(), base + format, sail::kAudioFormatSize);
  it->second.format = it->second.preferred;
  setResultCode(ctx, kCellOk);
}

// cellSailSoundAdapterGetFormat(adapter, CellSailAudioFormat* format)
void soundGetFormat(DalPpuContext* ctx, std::uint8_t* base) {
  const auto it = g_sound.find(argU32(ctx, 0));
  if (it == g_sound.end()) {
    setResultCode(ctx, sail::kErrorInvalidState);
    return;
  }
  const std::uint32_t out = argU32(ctx, 1);
  if (!guestMapped(out, sail::kAudioFormatSize)) {
    setResultCode(ctx, sail::kErrorInvalidArg);
    return;
  }
  std::memcpy(base + out, it->second.format.data(), sail::kAudioFormatSize);
  setResultCode(ctx, kCellOk);
}

constexpr HleEntry kHandlers[] = {
    {kModule, 0x1c983864, "cellSailGraphicsAdapterInitialize", graphicsInitialize},
    {kModule, 0x76488bb1, "cellSailGraphicsAdapterFinalize", graphicsFinalize},
    {kModule, 0x2e3ccb5e, "cellSailGraphicsAdapterSetPreferredFormat", graphicsSetPreferredFormat},
    {kModule, 0xffd58aa4, "cellSailGraphicsAdapterGetFormat", graphicsGetFormat},
    {kModule, 0x3d0d3b72, "cellSailSoundAdapterInitialize", soundInitialize},
    {kModule, 0xd1462438, "cellSailSoundAdapterFinalize", soundFinalize},
    {kModule, 0x1c9d5e5a, "cellSailSoundAdapterSetPreferredFormat", soundSetPreferredFormat},
    {kModule, 0xf25f197d, "cellSailSoundAdapterGetFormat", soundGetFormat},
};

}  // namespace

std::span<const HleEntry> cellSailAdapterHandlers() { return kHandlers; }

namespace sail_hle {

std::uint32_t bindAdapter(AdapterKind kind, std::uint32_t adapter, std::uint32_t player) {
  std::uint32_t* owner = nullptr;
  if (kind == AdapterKind::Graphics) {
    const auto it = g_graphics.find(adapter);
    if (it != g_graphics.end()) owner = &it->second.player;
  } else {
    const auto it = g_sound.find(adapter);
    if (it != g_sound.end()) owner = &it->second.player;
  }
  if (owner == nullptr || (*owner != 0 && *owner != player)) return sail::kErrorInvalidState;
  *owner = player;
  return kCellOk;
}

void unbindAdapters(std::uint32_t player) {
  for (auto& entry : g_graphics) {
    if (entry.second.player == player) entry.second.player = 0;
  }
  for (auto& entry : g_sound) {
    if (entry.second.player == player) entry.second.player = 0;
  }
}

void resetAdapters() {
  g_graphics.clear();
  g_sound.clear();
}

}  // namespace sail_hle
}  // namespace dal
