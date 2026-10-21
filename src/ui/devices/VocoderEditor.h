// SPDX-License-Identifier: AGPL-3.0-or-later
// The vocoder's editor (devices/vocoder/Vocoder.h), as a hardware vocoder's bank of level meters: a column of lights for
// each band, as high as the voice is in it, standing where the carrier plays that band, over a mark where the band listens
// to the voice, so that Formant shows as the columns leaning away from their marks; what the carrier is (the buzz's note,
// or the track that feeds it, or a warning that none does), how loud it plays, and a light for the consonants it hears. A
// drag sideways moves the carrier's bands (Formant), as one undo step. Then its carrier's controls, its Formant and
// Unvoiced, large, and its bands' and their response's controls.
#pragma once

#include "DeviceEditorBase.h"

namespace phazite::ui
{
class VocoderEditor : public DeviceEditorBase
{
public:
    explicit VocoderEditor (const DeviceHandle&);
    ~VocoderEditor() override;

    juce::Component& getBands() const noexcept;

    // What the bands show now: a band's level (a gain), the consonant light (0 to 1), and the carrier's description.
    float getShownLevel (int band) const;
    float getShownConsonant() const;
    juce::String getCarrierText() const;

    // Where the picture draws a band's column (its centre at the bottom), in its own coordinates.
    juce::Point<float> getColumnPosition (int band) const;

private:
    class Bands;
    Bands* bands = nullptr;
};
} // namespace phazite::ui
