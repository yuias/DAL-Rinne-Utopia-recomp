#include <cstdint>

#include "ppu_override_handlers.h"
#include "rsx/rsx_thread.h"

// The game's memmove. A render target lives only on the GPU here, so when the game copies its guest
// bytes elsewhere (to reuse the picture as a texture), the RSX thread must copy the image too.
// The original always runs first, so guest memory is exactly as before; the notification comes after
// it because the original can yield the guest lock, and the RSX thread must not hash a half-copied
// destination. Most calls are plain memory moves, which rsxNoteGuestCopy ignores cheaply.
extern "C" DAL_OVERRIDE_HANDLER(renderTargetCopy) {
    // Read before the original runs; it clobbers the argument registers.
    const auto dst = static_cast<std::uint32_t>(ctx->gpr[3]);
    const auto src = static_cast<std::uint32_t>(ctx->gpr[4]);
    const auto size = static_cast<std::uint32_t>(ctx->gpr[5]);
    site->original(ctx, base);
    dal::rsxNoteGuestCopy(dst, src, size);
}
