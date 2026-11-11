// SPDX-License-Identifier: AGPL-3.0-or-later
// The sampler's editor (devices/sampler/Sampler.h; roadmap step 4.6; docs/design.md, section 9). First the sound: the three
// modes, Classic, One-Shot and Slice, as buttons; a button with its name, which opens a menu of the sounds that come with
// Phazite, Load Sound File... and Find Root Key; Reverse; and the sound drawn, which takes a sound dropped on it (from the
// browser or Explorer). On the drawing, what the sound plays is between its start and end markers, Classic's loop a band between
// its own two, One-Shot's fades as ramps, and Slice's slices as lines, those put in by hand in the accent colour, each
// voice's place in the sound as it plays. Every marker drags, snapping to the zero crossing nearest (Alt places it
// anywhere, Shift in fine steps), each drag one undo step; the loop's band drags whole; a click under the drawing adds a
// slice there and a double-click on a slice takes it away; a press on the sound plays it (in Slice mode, the slice under
// the pointer) while it is held. Under the drawing, in one row, the controls of the mode it plays in (Classic's and
// One-Shot's with Start Random beside Start; in Slice mode, the knob of the way it slices). Then Pitch over Voice, the Amp
// envelope with its Key Time (Classic's; faint in the other modes, where the fades shape a note), the Filter with how far
// velocity and the keys move it, its Envelope and the LFO, each switched on at its title. A dock narrower than them shows
// them in pages.
#pragma once

#include "DeviceEditorBase.h"
#include "SynthParts.h"

#include <array>
#include <memory>

namespace phazite::ui
{
class SamplerEditor : public DeviceEditorBase,
                      private DisplayTicker::Display
{
public:
    explicit SamplerEditor (const DeviceHandle&);
    ~SamplerEditor() override;

    // The sound's drawing, the button that names it and the menu it opens: the factory sounds, the one playing ticked, Load
    // Sound File... and Find Root Key.
    juce::Component& getSampleView() const;
    juce::Button& getSoundButton() const;
    juce::PopupMenu getSoundMenu();

    // The controls under the drawing for a mode (a Sampler::Mode); the one shown is the mode's.
    juce::Component& getModeControls (int mode) const;

    // Puts a sound file on the sampler; returns whether it did.
    bool dropFile (const juce::File&);

    // Where the drawing has a marker for a parameter (start, end, loopStart, loopEnd), and each slice after the first, in the
    // drawing's own coordinates: for tests and screen readers.
    float getMarkerX (int parameter) const;
    std::vector<float> getSliceXs() const;

    // Shows what changed elsewhere (undo, the sound, the mode); the ticker does it.
    void refresh();

private:
    class SampleView;
    class SamplePart;

    SampleView* sampleView = nullptr;
    SamplePart* samplePart = nullptr;
    juce::Component* ampContent = nullptr;
    std::unique_ptr<juce::FileChooser> chooser;

    void chooseFile();
    void tick() override { refresh(); }
};
} // namespace phazite::ui
