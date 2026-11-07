// SPDX-License-Identifier: AGPL-3.0-or-later
// A clip in the playlist: in its track's color, a preview of what it holds (an audio clip's waveform and fades, a MIDI
// clip's notes), which fills it, and over that its name where it says more than its track's header does, and marks for an
// alias and for a sample that follows the tempo. It only draws: the playlist handles the mouse, with the Smart Tool. The
// playlist makes one only for a clip in view.
#pragma once

#include "ClipWaveform.h"
#include "model/arrangement/PlaylistModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <optional>
#include <vector>

namespace phazite::ui
{
class ClipComponent : public juce::Component
{
public:
    ClipComponent (ProjectContext&, ClipID);
    ~ClipComponent() override;

    ClipID getClip() const noexcept { return id; }
    const ClipInfo& getInfo() const noexcept { return info; } // as it was shown last

    // Shows the clip as it is now (getClip() in model/arrangement/PlaylistModel.h), with what it holds; call it when
    // the song changes.
    void showClip (const ClipInfo&);

    // An audio clip's waveform, which the playlist keeps while the clip is in the song (this only while it is in view),
    // or nullptr.
    void showWaveform (ClipWaveform*);

    // Shows it in its track's color, whether it is selected, frosted if its track is frozen, and faint, as a muted clip is, while
    // its track isn't heard (muted, or kept quiet by other tracks' solos).
    void showLook (juce::Colour, bool isSelected, bool isFrozen = false, bool trackSilent = false);

    // While its render is being made (model/arrangement/AudioClipModel.h), how far it has come, from 0 to 1: its waveform
    // faint, as it is silent until then, and a bar along its bottom edge; nothing once it plays.
    void showRendering (std::optional<double> progress);
    std::optional<double> getRendering() const noexcept { return rendering; }
    juce::Colour getColour() const noexcept { return colour; }
    bool isFrozen() const noexcept { return frozen; }
    bool isFaint() const noexcept { return info.muted || trackSilent; } // muted, or on a track that isn't heard
    juce::Colour getInk() const;                                        // what is drawn on it: its notes, its waveform

    // What its tag at its top left says: its name where that isn't its track's, which the track's header shows (a sample's,
    // or one given it), and the take a clip recorded round a loop plays, of how many; nothing for a clip called after its
    // track, which shows no tag.
    juce::String getLabel() const;

    void paint (juce::Graphics&) override;

private:
    ProjectContext& context;
    const ClipID id;
    ClipInfo info;
    std::vector<NoteShape> notes; // a MIDI clip's
    juce::Colour colour;
    bool selected = false, frozen = false, trackSilent = false;
    std::optional<double> rendering;  // to a hundredth
    ClipWaveform* waveform = nullptr; // an audio clip's, the playlist's

    // The marks at its top right, once drawn, and the color each drawing is in now: an alias's link, and the metronome of a
    // sample that follows the tempo.
    std::unique_ptr<juce::Drawable> aliasMark, tempoMark;
    juce::Colour aliasMarkColour { juce::Colours::black }, tempoMarkColour { juce::Colours::black };

    juce::Colour getFill() const;
    void describe();                                              // what screen readers say of it besides its name
    juce::Rectangle<int> getShape() const;                        // what it draws in: all of it but its last pixel's column
    void paintTags (juce::Graphics&, juce::Rectangle<int> shape); // its name and its marks, over what it holds
    void paintFades (juce::Graphics&, juce::Rectangle<int>);
    void paintNotes (juce::Graphics&, juce::Rectangle<int>, juce::Colour);
};

// Shades what a fade takes away from an audio clip, above its curve from silence (at x0) to full volume (at x1) between
// a top and a bottom, as the playlist and the audio clip editor draw fades.
void paintFade (juce::Graphics&, float x0, float x1, float top, float bottom, FadeCurve);
} // namespace phazite::ui
