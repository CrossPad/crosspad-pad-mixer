#include "crosspad-pad-mixer/PadMixerPadLogic.hpp"

#include "crosspad/pad/PadManager.hpp"

void PadMixerPadLogic::onActivate(crosspad::PadManager& padManager)
{
    auto base = padManager.getPadLogic(padManager.getBasePadLogic());
    crosspad::IPadLogicHandler* b = (base.get() == this) ? nullptr : base.get();
    if (b) b->onActivate(padManager);
    base_.store(b);
    hitPad_.store(-1);   // a hit from before the app opened is not a selection
}

void PadMixerPadLogic::onDeactivate(crosspad::PadManager& padManager)
{
    crosspad::IPadLogicHandler* b = base_.exchange(nullptr);
    if (b) b->onDeactivate(padManager);
}

void PadMixerPadLogic::onPadPress(crosspad::PadManager& padManager,
                                  uint8_t padIdx, uint8_t velocity)
{
    if (auto* b = base_.load()) b->onPadPress(padManager, padIdx, velocity);
    // Only a finger picks the channel; the loop playing back must not drag
    // the selection around under the fader being moved.
    if (padIdx < 16 && !crosspad::PadManager::isSequencedPress()) hitPad_.store(padIdx);
}

void PadMixerPadLogic::onPadRelease(crosspad::PadManager& padManager,
                                    uint8_t padIdx)
{
    if (auto* b = base_.load()) b->onPadRelease(padManager, padIdx);
}

void PadMixerPadLogic::onPadPressure(crosspad::PadManager& padManager,
                                     uint8_t padIdx, uint8_t pressure)
{
    if (auto* b = base_.load()) b->onPadPressure(padManager, padIdx, pressure);
}
