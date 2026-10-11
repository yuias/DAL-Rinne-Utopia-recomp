#include <cstdint>
#include <format>
#include <string>

#include "core/log.h"
#include "dal/ppu_override.h"

namespace {

// Enough to see the arguments of the first calls without flooding the log from a hot function.
constexpr std::uint64_t kLoggedCalls = 8;

}  // namespace

extern "C" {

DAL_OVERRIDE_HANDLER(dalHookNop) {}

DAL_OVERRIDE_HANDLER(dalHookLogCall) {
    const std::uint64_t n = __atomic_load_n(site->calls, __ATOMIC_RELAXED);
    if (n < 1 || n > kLoggedCalls) {
        return;
    }
    dal::logInfo("dal: hook {:#010x}{} call {}: r3={:#x} r4={:#x} r5={:#x} r6={:#x} lr={:#x}", site->guest,
                 site->label != nullptr ? std::format(" \"{}\"", site->label) : std::string(), n, ctx->gpr[3],
                 ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->lr);
}

DAL_OVERRIDE_HANDLER(dalOverrideReturn0) { ctx->gpr[3] = 0; }

DAL_OVERRIDE_HANDLER(dalOverrideOriginal) { site->original(ctx, base); }

}  // extern "C"
