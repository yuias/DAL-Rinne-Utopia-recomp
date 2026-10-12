#pragma once

#include "overlay/overlay_model.h"

namespace dal {

// Wires every OverlaySources member to the runtime's any-thread providers: audio engine and cellAudio
// counters, the RSX unimplemented counts, the fast-forward state and switches, the RSX capture request
// and the log level. The providers are stateless, so the returned object may outlive the caller's scope.
OverlaySources makeRuntimeOverlaySources();

}  // namespace dal
