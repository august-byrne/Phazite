// SPDX-License-Identifier: AGPL-3.0-or-later
// The synth's editor (devices/synth/Synth.h): one cycle of what its oscillators play together (their waves, their mix, the
// pulse width, the sub and the noise); its filter's response, which follows the newest note as its envelope and LFO open
// it, with the cutoff a dot to drag; its envelopes' shapes, with a line at the newest note's level; and its LFO's wave, with
// where it is. Each with its controls, in three columns as the sound goes through them: Oscillators, Unison and Voice; Filter
// and Filter Envelope; Amp and LFO. In a short dock, a column shows one part at a time under tabs; in a tall one, all.
#pragma once

#include "DeviceEditorBase.h"
#include "SynthParts.h"

namespace phazite::ui
{
class SynthEditor : public DeviceEditorBase
{
public:
    explicit SynthEditor (const DeviceHandle&);
    ~SynthEditor() override;

    FilterView& getFilter() const noexcept { return *filter; }
    EnvelopeView& getAmpEnvelope() const noexcept { return *ampEnvelope; }
    juce::Component& getOscillators() const noexcept;

private:
    class OscillatorView;
    OscillatorView* oscillators = nullptr;
    FilterView* filter = nullptr;
    EnvelopeView *filterEnvelope = nullptr, *ampEnvelope = nullptr;
    LfoView* lfo = nullptr;
};
} // namespace phazite::ui
