// SPDX-License-Identifier: AGPL-3.0-or-later
// What a sound selected in the browser holds, under its tree, as FL Studio's, Bitwig's and Ableton Live's browsers show it
// before it goes into the song (docs/design.md, section 11; the owner, 2026-11-02): its waveform, then whether it is a loop
// (at what tempo, and how many bars it lasts) or a one-shot, how long it lasts, and its format. Not in the inspector,
// which shows what is in the song, as no DAW's inspector shows a file. Its levels load in the background, as a clip's do.
#pragma once

#include "services/Library.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <optional>

namespace phazite::ui
{
class SoundView : public juce::Component,
                  private juce::ChangeListener
{
public:
    SoundView();
    ~SoundView() override;

    // A sound to show, or anything else, for which it shows nothing.
    void show (const std::optional<LibraryItem>&);
    bool showsSound() const noexcept { return shown != juce::File(); }

    // Its line under the waveform ("Loop · 120 BPM · 2 bars · 4.00 s · 44.1 kHz, 24-bit stereo"), for tests.
    const juce::String& getDetails() const noexcept { return details; }

    void paint (juce::Graphics&) override;

private:
    juce::AudioFormatManager formats;
    juce::AudioThumbnailCache cache { 8 };
    juce::AudioThumbnail thumbnail { 512, formats, cache };
    juce::File shown;
    juce::String details;

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); } // more of its levels
};
} // namespace phazite::ui
