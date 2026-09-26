#pragma once

#include "crosspad/pad/IPadLogicHandler.hpp"

#include <atomic>

/**
 * Pad handler for Pad Mixer.
 *
 * The mixer owns the pads only to learn which one the player hit; the sound
 * is the base instrument's. Every call is handed on to the base logic (the
 * Sampler), so pads and a running sequence sound exactly as they do anywhere
 * else -- and a hit the sequencer plays back is never taken as a selection.
 *
 * Pad callbacks arrive on the pad/event thread, NOT on the LVGL task, so this
 * class touches no LVGL object; the app's LVGL timer reads the hit pad.
 */
class PadMixerPadLogic : public crosspad::IPadLogicHandler {
public:
    void onActivate(crosspad::PadManager& padManager) override;
    void onDeactivate(crosspad::PadManager& padManager) override;
    void onPadPress(crosspad::PadManager& padManager, uint8_t padIdx,
                    uint8_t velocity) override;
    void onPadRelease(crosspad::PadManager& padManager, uint8_t padIdx) override;
    void onPadPressure(crosspad::PadManager& padManager, uint8_t padIdx,
                       uint8_t pressure) override;

    /// The pad a player hit since the last call, or -1. Drained by the LVGL
    /// timer.
    int takeHitPad() { return hitPad_.exchange(-1); }

private:
    /// The base instrument, looked up on activation: it is whatever the
    /// platform installed, and never this handler. A plain pointer, atomic
    /// because the pad thread reads it: a base logic stays registered for
    /// good (PadManager::setBasePadLogic()), so it cannot dangle.
    std::atomic<crosspad::IPadLogicHandler*> base_{nullptr};
    std::atomic<int> hitPad_{-1};
};
