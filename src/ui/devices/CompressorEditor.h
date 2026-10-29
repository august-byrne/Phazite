// SPDX-License-Identifier: AGPL-3.0-or-later
// The compressor's editor (roadmap step 1.6, devices/dynamics/Compressor.h): its curve, with a dot riding it at the level the
// detector hears and a meter of how far it turns the sound down; beside it the last seconds, as FabFilter's compressor shows
// them, what it hears (pale, or the track its sidechain listens to, in that track's colour), what goes out and how far it
// turned the sound down (red, from the top), with the threshold a line across; its Threshold and Ratio, large, then the
// rest, and its key's cuts with Listen, which plays the key instead of the sound while the editor shows (the track its
// sidechain listens to is chosen in its header: SidechainMenus.h). Dragging across the curve, or the threshold's line up or
// down, moves the threshold, as one undo step. On a folded card it shows its history alone, which shows the ducking.
#pragma once

#include "DeviceEditorBase.h"
#include "LevelHistoryView.h"

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
    LevelHistoryView& getHistory();
    juce::Button& getListenButton(); // plays the key instead of the sound, while on and showing

    // Where the graph draws a level in (x) and out (y), in decibels, in its own coordinates.
    juce::Point<float> getGraphPosition (float inputDecibels, float outputDecibels) const;

    // What the meters show: the level the detector hears, and how far the sound goes down (0 or more), in decibels.
    float getShownLevel() const noexcept { return shownLevel; }
    float getShownReduction() const noexcept { return shownReduction; }

    // Brings the curve, the meters and the history up to date, and keeps the key playing while Listen is on; the ticker does
    // it.
    void refresh();

private:
    class Graph;
    class KeyPart;
    class Activity;

    std::vector<ParameterHandle> parameters; // every parameter, in the compressor's order, for the curve
    std::vector<float> values;
    float shownLevel, shownReduction = 0.0f;
    DrawnNumbers drawnGraph; // what its curve and meter show

    Activity* activity = nullptr;
    KeyPart* keyPart = nullptr;

    void tick() override { refresh(); }
    juce::Component* getFoldedPicture() const override; // its history, which shows the ducking
};
} // namespace phazite::ui
