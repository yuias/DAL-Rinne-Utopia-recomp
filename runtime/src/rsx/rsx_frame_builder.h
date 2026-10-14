#pragma once

// Turns the stream of completed RSX register writes into plain commands (clear, draw, flip) that
// carry every guest byte they need, so a renderer never touches guest memory; a copy the guest CPU made of a
// render target's bytes becomes a fourth kind, a copy of the target image. Draw boundaries,
// vertex and index addressing follow RPCS3 `Emu/RSX/NV47/HW/nv4097.cpp` (`set_begin_end`, `draw_arrays`,
// `draw_index_array`), `NV47/FW/draw_call.hpp` (`draw_clause::append`), `Core/RSXDrawCommands.cpp`,
// `Utils/rsx_utils.h` and `VK/VKVertexBuffers.cpp`; clears follow `VK/VKGSRender.cpp` `clear_surface`.
// Texture bytes come from a cache shared by all draws (rsx_texture_cache.h); the builder starts a new cache
// epoch at the flips and synchronization points where the guest can learn how far the RSX has got.

#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rsx/rsx_frame.h"
#include "rsx/rsx_registers.h"
#include "rsx/rsx_state.h"
#include "rsx/rsx_texture_cache.h"

namespace dal {

// RSX-visible memory; location 0 = local (offset from kRsxLocalBase), 1 = main memory through the IO map.
class RsxMemoryView {
public:
    virtual ~RsxMemoryView() = default;
    // Copies raw (big-endian) guest bytes; false when any part of the range is not readable.
    virtual bool read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) = 0;
};

// Live view over guest memory. The RSX thread reads through it without the guest lock: like the
// hardware, it relies on the game not freeing or unmapping memory a submitted draw still reads.
class GuestRsxMemoryView final : public RsxMemoryView {
public:
    explicit GuestRsxMemoryView(const std::uint8_t* base) : base_(base) {}
    bool read(std::uint32_t location, std::uint32_t offset, std::span<std::uint8_t> out) override;

private:
    const std::uint8_t* base_;
};

// Upper bounds for what one command may copy; a draw or texture beyond them is dropped and counted.
inline constexpr std::uint32_t kRsxMaxVertexBytes = 64u << 20;
inline constexpr std::uint32_t kRsxMaxIndexCount = 1u << 20;
inline constexpr std::uint32_t kRsxMaxTextureBytes = 64u << 20;
inline constexpr std::size_t kRsxMaxKnownSurfaces = 64;

// A copy the guest CPU made of `size` bytes from src to dst (location 0 = local, 1 = main through the IO
// map), at the point of the command stream the RSX consumer had reached when the copy ended.
struct RsxGuestCopy {
    std::uint32_t src_location = 0, src_offset = 0, dst_location = 0, dst_offset = 0, size = 0;
};

class RsxFrameBuilder {
public:
    explicit RsxFrameBuilder(RsxMemoryView& memory) : memory_(memory) {}

    // Called after RsxRegisters::apply for every completed write.
    void onWrite(const RsxRegisters& regs, std::uint32_t method, std::uint32_t value);
    void onFlip(const RsxRegisters& regs, std::uint32_t buffer, const RsxDisplayBuffer& display);

    // Moves out what was built since the last call.
    std::vector<RsxCommand> takeCommands();

    // Deferred mode: SET_BEGIN_END(0) only marks the draw as pending, and finishDraw() copies its guest
    // bytes and builds the command. The caller must call finishDraw() before the next write, with the
    // registers as that write left them. Off by default: the draw is built inside onWrite().
    void setDeferDrawEnd(bool defer) { defer_draw_end_ = defer; }
    bool drawPending() const { return draw_pending_; }
    void finishDraw(const RsxRegisters& regs);

    // A guest copy at this point of the stream (the caller has built any pending draw). When `src` is the first
    // byte of a known color surface that is not stale and `size` covers it, emits RsxCopy and registers the
    // destination as a copied known surface; returns whether it did.
    bool onGuestCopy(const RsxGuestCopy& copy);

    // Color surfaces cleared or drawn to so far, or copied to by the guest, indexed by RsxTextureDesc::rt_alias.
    // Entries are only appended or replaced in place, so an index stays valid for the life of the builder. With
    // the table full, a new surface takes the slot of a copy destination (never of a surface the RSX drew).
    std::span<const RsxSurface> knownSurfaces() const { return known_surfaces_; }
    void setKnownSurfaces(std::vector<RsxSurface> surfaces) {
        known_surfaces_ = std::move(surfaces);
        copy_checked_.clear();
        copy_order_.clear();
    }

    // Index the next flip command gets; a replay continues the numbering of the run it was captured from.
    void setFlipIndex(std::uint64_t index) { flips_ = index; }

    const RsxTextureCache& textureCache() const { return textures_; }

private:
    struct Range {
        std::uint32_t first = 0, count = 0;
    };

    void beginDraw(std::uint32_t value);
    void addRange(bool indexed, std::uint32_t value);
    void endDraw(const RsxRegisters& regs);
    void buildDraw(const RsxRegisters& regs);
    void clear(const RsxRegisters& regs, std::uint32_t mask);
    bool fillVertices(const RsxRegisters& regs, RsxDraw& draw, std::uint32_t vertex_base, std::uint32_t vertex_count);
    void fillTextures(const RsxRegisters& regs, RsxDraw& draw);
    void copyTexture(RsxTextureDesc& t, std::uint32_t register_pitch, std::uint64_t size);
    void fillFragmentProgram(const RsxRegisters& regs, RsxDraw& draw);
    void fillVertexProgram(const RsxRegisters& regs, RsxDraw& draw);
    std::int32_t noteSurface(const RsxSurface& surface);
    std::int32_t findSurface(std::uint32_t location, std::uint32_t offset) const;
    // Index of the known surface a texture at (location, offset) aliases, -1 when it is read from memory. A
    // surface made by a guest copy is an alias only while its guest bytes are unchanged (checked once per epoch).
    std::int32_t aliasIndex(std::uint32_t location, std::uint32_t offset);
    // Reads the guest bytes under `surface` (the rows a copy of it covers) and hashes them.
    bool hashSurfaceBytes(const RsxSurface& surface, std::uint64_t& hash);

    RsxMemoryView& memory_;
    std::vector<RsxCommand> commands_;
    std::vector<RsxSurface> known_surfaces_;
    // Texture cache epoch in which a copied surface's guest bytes were last compared with its copy_hash.
    std::unordered_map<std::int32_t, std::uint64_t> copy_checked_;
    // Sequence number of the last guest copy into a slot, to evict the least recently copied destination.
    std::unordered_map<std::int32_t, std::uint64_t> copy_order_;
    std::uint64_t copy_serial_ = 0;
    std::uint64_t flips_ = 0;
    RsxTextureCache textures_;
    std::vector<std::uint8_t> texture_scratch_;

    // The open draw clause between SET_BEGIN_END(prim) and SET_BEGIN_END(0).
    bool in_draw_ = false;
    bool skip_draw_ = false;  // BEGIN with a primitive RPCS3 rejects: the clause is ignored up to END
    bool have_range_ = false, indexed_ = false;
    RsxPrimitive primitive_ = RsxPrimitive::Triangles;
    Range range_;
    bool defer_draw_end_ = false;
    bool draw_pending_ = false;  // the clause ended and buildDraw() has not run yet
};

}  // namespace dal
