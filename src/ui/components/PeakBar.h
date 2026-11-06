// SPDX-License-Identifier: AGPL-3.0-or-later
// A narrow bar along a track's header's right edge, as tall as its row, that shows how loud the track plays now, as FL
// Studio's playlist shows that a track sounds (the owner, 2026-11-06: "the full lanes height, right on the edge (small
// width), and is colored"): the peak of its louder channel after its fader and mute (what is heard), from -48 to 0 dB on a
// decibel scale, as a length up from the bottom in the track's colour, over a trough of the colour, faint: the length tells
// the level, not the colour alone. It rises at once and falls 20 dB in half a second, holding no peak, so that each beat
// shows apart. A peak over 0 dB, where the sound clips, lights its top in the danger colour for two seconds. It takes no
// clicks: the header has them. It reads its source (model/mixer/LevelSource.h) at each tick of the displays' ticker
// (DisplayTicker.h), on the message thread, and repaints only when what it draws moved: in silence, nothing. Screen
// readers read the level.
#pragma once

#include "DisplayTicker.h"
#include "model/mixer/LevelSource.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class PeakBar : public juce::Component,
                private DisplayTicker::Display
{
public:
    PeakBar();
    ~PeakBar() override;

    // What to show, as the model hands it out (MixerModel.h: getLevel()), or an empty source for nothing. With nothing to
    // measure (a plain folder's), it draws nothing, not even its trough.
    void setSource (LevelSource);

    // The track's colour, which it fills with.
    void setFillColour (juce::Colour);

    // The level it shows now, in decibels, as it falls; whether its clip light is lit. For tests.
    float getLevel() const noexcept { return level; }
    bool isClipLit() const noexcept { return clipTicks > 0; }

    void paint (juce::Graphics&) override;
    bool hitTest (int, int) override { return false; }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    // Ticks at once (DisplayTicker::tickNow() for every display); for tests.
    void tickForTest() { tick(); }

private:
    static constexpr float minDb = -48.0f, maxDb = 0.0f;

    LevelSource source;
    juce::Colour fill { juce::Colours::white };
    float level = LevelSource::silenceDb, shown = LevelSource::silenceDb;
    int clipTicks = 0; // how much longer its clip light stays lit
    bool measuring = false;

    juce::String getLevelText() const;
    void tick() override;
};
} // namespace phazite::ui
