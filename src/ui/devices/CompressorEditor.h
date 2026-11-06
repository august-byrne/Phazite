// SPDX-License-Identifier: AGPL-3.0-or-later
// The compressor's editor (roadmap step 1.6, devices/dynamics/Compressor.h), kept plain for a quick compressor (the owner,
// 2026-11-01: "much simpler and cleaner graphically ... when you just want a quick compressor and you know what settings you
// want"): its characters as a row of buttons; its curve, drawn and edited as the multiband's are, as Maximus's (the owner,
// 2026-11-01: "i want this graph to also be used the same in the compressor plugin"; DynamicsCurve.h), lit up to the level it
// hears, with its menu's shapes to start from (4:1 over -18 dB, a limiter, a gate, upward compression...), and a meter beside
// it of how far it turns the sound down (from its middle down) or up (from its middle up); its attack and release; its
// input gain, drive and makeup; and its sidechain's key cuts with Listen, which plays the key instead of the sound while the
// editor shows (the track its sidechain listens to is chosen in its row in the Devices view: SidechainMenus.h; its mix, for
// parallel compression, is its row's too).
#pragma once

#include "DeviceEditorBase.h"
#include "DynamicsCurve.h"

#include <memory>
#include <vector>

namespace phazite::ui
{
class CompressorEditor : public DeviceEditorBase,
                         private DisplayTicker::Display
{
public:
    explicit CompressorEditor (const DeviceHandle&);
    ~CompressorEditor() override;

    DynamicsCurve& getCurve();
    juce::Button& getListenButton(); // plays the key instead of the sound, while on and showing
    juce::Button& getCharacterButton (int character);

    // What the meters show: the level the detector hears, and how far the sound goes down and up (0 or more), in decibels.
    float getShownLevel() const noexcept { return shownLevel; }
    float getShownReduction() const noexcept { return shownReduction; }
    float getShownBoost() const noexcept { return shownBoost; }

    // Brings the curve, the meter and the character's buttons up to date, and keeps the key playing while Listen is on; the
    // ticker does it.
    void refresh();

private:
    class CurvePart;
    class KeyPart;

    std::vector<ParameterHandle> parameters; // every parameter, in the compressor's order, for the curve
    std::vector<float> values;
    float shownLevel, shownReduction = 0.0f, shownBoost = 0.0f;
    DrawnNumbers drawnCurve; // what its curve and meter show

    CurvePart* curvePart = nullptr;
    ChoiceButtons* characters = nullptr;
    KeyPart* keyPart = nullptr;

    void tick() override { refresh(); }
};
} // namespace phazite::ui
