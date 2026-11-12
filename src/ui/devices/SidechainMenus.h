// SPDX-License-Identifier: AGPL-3.0-or-later
// Choosing what a device's sidechain listens to, its key (docs/design.md, section 8), in one place, inside the device (the
// owner, 2026-11-10: a button in the Devices view's list "takes up space and looks cluttered"): a drop-down in the device's
// editor, as Ableton Live and Bitwig keep theirs (the compressor's Sidechain part, the vocoder's carrier, the Multiband's), or
// in a plug-in's window, by its Plug-in menu (app/PluginWindow.h). It shows the track it listens to, a dot in that track's
// colour and its name, or None while it listens to the sound passing through it; a click lists None and every other track
// in the song's order, each with its colour, those that would make a loop greyed with why. Choosing a track is one undo
// step, which turns the sidechain on: no switch of its own, no routing to set up. Phazite's compressor, still as it was made,
// starts ducking then. And the track menu's Duck Under, which adds a compressor that does it in one step.
#pragma once

#include "model/base/IDs.h"
#include "model/devices/DeviceHandle.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
// The menus, built when they open: a device's sidechain (None, then the tracks), and the tracks a track can duck under.
// Choosing an item makes the change; `changed` is called after it. None's item says what the device hears then, as given
// ("None (its own sound)"); without a meaning, "None" alone.
juce::PopupMenu makeSidechainMenu (ProjectContext&, DeviceID, const std::function<void()>& changed = {},
                                   const juce::String& noneMeaning = "its own sound");
juce::PopupMenu makeDuckUnderMenu (ProjectContext&, TrackID ducked);

// The word for what a sidechain listens to, on the label beside the drop-down and as its title for screen readers: the word
// every DAW prints on its source picker (Ableton Live, FL Studio, Logic, Cubase, Studio One). "Key", Pro Tools' word, was
// Phazite's until 2026-11-12, and read as the song's musical key, which the piano roll, the inspector and the browser name so.
inline constexpr const char* sidechainLabel = "Sidechain";

// How wide the label is, in small text, with a little room after it.
int getSidechainLabelWidth();

// The drop-down, titled sidechainLabel. Off, saying why, while the device has no sidechain of its own to choose (one in a
// container hears what passes through it).
class SidechainButton : public juce::Button
{
public:
    SidechainButton (ProjectContext&, DeviceID);

    // What it shows: the track the sidechain listens to, or none (None).
    void show (const SidechainChoice& source);

    // Shows what the song says it listens to now: after a change made elsewhere (an undo, Duck Under).
    void refresh();

    // What None means for its device, in its menu ("its own sound" at first, as a compressor hears it then); empty for None
    // alone.
    void setNoneMeaning (const juce::String&);

    // What a click shows (tests run its items).
    juce::PopupMenu getMenu();

    void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

    using juce::Button::clicked; // the overload with the keys held, which this leaves as it is
    void clicked() override;

    // Called after the menu changed the sidechain, once it shows the change.
    std::function<void()> onChange;

    static constexpr int minimumWidth = 96; // room for a dot, a short name and the arrow

private:
    ProjectContext& context;
    const DeviceID device;
    SidechainChoice source;
    juce::String noneMeaning { "its own sound" };
};
} // namespace phazite::ui
