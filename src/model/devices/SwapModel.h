// SPDX-License-Identifier: AGPL-3.0-or-later
// Swap (docs/design.md, section 11): trying sounds on a drum sampler's pad, or presets on one of Phazite's own devices, in
// their place in the song, each heard at once: in the beat while the song plays, through the track's devices; while it is
// stopped, the pad plays, and an instrument a short note. One is kept as one undo step, or the old one put back. The browser
// tries what it moves to (ui/browser/Browser.h). Anything else that changes the song meanwhile (a step, a drag, an undo,
// a save) keeps what is tried first, as its own step (model/commands/internal/Undo.h: Trial), and ends the swap. By the
// open project's context and IDs; message thread only.
#pragma once

#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <optional>

namespace phazite
{
class ProjectContext;

class Swap
{
public:
    // A drum sampler's pad (from 0), or with pad -1 one of Phazite's own devices, of the open project. Nothing changes until
    // something is tried. Closed from the start for a device that isn't there, or isn't one of those.
    Swap (ProjectContext&, DeviceID, int pad = -1);
    ~Swap(); // puts the old one back, unless it was kept

    // Until it is kept or put back, something else kept what it tried, its device went, or another project opened.
    bool isOpen() const;

    DeviceID getDevice() const;
    int getPad() const; // -1 for a device's presets

    // What it swaps ("Pad 1", or the device's name), and what played there before: a pad's sound, or nothing for a device.
    juce::String getTargetName() const;
    juce::String getOldName() const;

    // A sound file for a pad; a preset of the device's type for a device.
    bool canTry (const juce::File&) const;

    // Puts it in place, heard at once. Returns false, changing nothing, for what canTry() refuses, or once closed.
    bool tryFile (const juce::File&);
    juce::File getTried() const; // what is in place now, or none while the old one is

    // Keeps what is tried as one undo step ("Put Kick 03 on Pad 1", "Load Preset Warm Pad"), a sound copied into the project
    // as one put on a pad is; or puts the old one back, leaving nothing to redo. Either closes it.
    void keep();
    void putBack();

    struct Impl;

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (Swap)
};

// The swap open on a device of the open project, if one is: the pad it swaps, or -1 for the device's presets.
std::optional<int> findOpenSwap (const ProjectContext&, DeviceID);
} // namespace phazite
