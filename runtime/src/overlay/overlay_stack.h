#pragma once

// Several overlays in the renderer's one PresentOverlay slot, drawn in insertion order (later = on top).

#include <vector>

#include "render/present_overlay.h"

namespace dal {

class OverlayStack final : public PresentOverlay {
public:
    // Not owned; add every overlay before the renderer uses the stack.
    void add(PresentOverlay* overlay) {
        if (overlay != nullptr) overlays_.push_back(overlay);
    }

    // Every child sees every flip. A request set by any child is kept.
    // A field added to PresentOverlayRequests must be added to the merge below, or it is silently dropped.
    PresentOverlayRequests onFlip(const PresentFrameInfo& info) override {
        PresentOverlayRequests merged;
        for (PresentOverlay* overlay : overlays_) {
            const PresentOverlayRequests requests = overlay->onFlip(info);
            merged.dump_display = merged.dump_display || requests.dump_display;
        }
        return merged;
    }

    bool wantsDraw() const override {
        for (const PresentOverlay* overlay : overlays_) {
            if (overlay->wantsDraw()) return true;
        }
        return false;
    }

    bool wantsIdlePresent() const override {
        for (const PresentOverlay* overlay : overlays_) {
            if (overlay->wantsIdlePresent()) return true;
        }
        return false;
    }

    void record(VkCommandBuffer cmd, const Presenter::Target& target) override {
        for (PresentOverlay* overlay : overlays_) {
            if (overlay->wantsDraw()) overlay->record(cmd, target);
        }
    }

private:
    std::vector<PresentOverlay*> overlays_;
};

}  // namespace dal
