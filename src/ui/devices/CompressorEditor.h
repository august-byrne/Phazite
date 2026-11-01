// SPDX-License-Identifier: AGPL-3.0-or-later
// The compressor's editor (roadmap step 1.6, devices/dynamics/Compressor.h), kept plain for a quick compressor (the owner,
// 2026-11-01: "much simpler and cleaner graphically ... when you just want a quick compressor and you know what settings you
// want"): its characters as a row of buttons; one picture, its curve (level in against level out, with the makeup) with a
// dot riding it at the level the compressor hears and a meter of how far it turns the sound down, as compact compressors keep
// the gain reduction in view and little else; its Threshold and Ratio, large, then the rest; and its sidechain's key
// cuts with Listen, which plays the key instead of the sound while the editor shows (the track its sidechain listens to is
// chosen in its header: SidechainMenus.h; its mix, for parallel compression, is its header's too). On the curve, as
// Kilohearts' Dynamics, Ozone and Vital set theirs: a drag across moves the threshold, the line above it dragged up or down
// sets the ratio, and the line below it the makeup, each one undo step. On a folded card it shows nothing of its own.
#pragma once

#include "DeviceEditorBase.h"

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

    juce::Component& getGraph();
    juce::Button& getListenButton(); // plays the key instead of the sound, while on and showing
    juce::Button& getCharacterButton (int character);

    // Where the graph draws a level in (x) and out (y), in decibels, in its own coordinates.
    juce::Point<float> getGraphPosition (float inputDecibels, float outputDecibels) const;

    // What the meters show: the level the detector hears, and how far the sound goes down (0 or more), in decibels.
    float getShownLevel() const noexcept { return shownLevel; }
    float getShownReduction() const noexcept { return shownReduction; }

    // Brings the curve, the meter and the character's buttons up to date, and keeps the key playing while Listen is on; the
    // ticker does it.
    void refresh();

private:
    class Graph;
    class KeyPart;

    std::vector<ParameterHandle> parameters; // every parameter, in the compressor's order, for the curve
    std::vector<float> values;
    float shownLevel, shownReduction = 0.0f;
    DrawnNumbers drawnGraph; // what its curve and meter show

    Graph* graph = nullptr;
    ChoiceButtons* characters = nullptr;
    KeyPart* keyPart = nullptr;

    void tick() override { refresh(); }
};
} // namespace phazite::ui
