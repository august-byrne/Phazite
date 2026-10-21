// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SynthEditor.h"
#include "DeviceTint.h"
#include "devices/synth/Synth.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Synth;

    const devices::ParameterInfo& getInfo (int index)
    {
        return Synth::info.parameters[(size_t) index];
    }

    // A wave at a phase from 0 to 1, from -1 to 1, as the synth's oscillators play it: sine, triangle, saw or square (a pulse
    // as wide as the width).
    float waveAt (int wave, float phase, float width)
    {
        switch (wave)
        {
            case 0:
                return std::sin (juce::MathConstants<float>::twoPi * phase);
            case 1:
                return 1.0f - 4.0f * std::abs (phase - 0.5f);
            case 3:
                return phase < width ? 1.0f : -1.0f;
            case 2:
            default:
                return 2.0f * phase - 1.0f;
        }
    }
} // namespace

//==============================================================================
// What the oscillators play together, one cycle (two of the sub).
class SynthEditor::OscillatorView : public juce::Component,
                                    private DeviceTicker::Display
{
public:
    explicit OscillatorView (const DeviceHandle& device)
        : DeviceTicker::Display (static_cast<juce::Component&> (*this))
    {
        setTitle ("Oscillators' wave");
        setDescription ("One cycle of what the oscillators play together: their waves and mix, the pulse width, the sub and the noise.");

        for (const auto index :
             { Synth::osc1Wave, Synth::osc2Wave, Synth::oscMix, Synth::pulseWidth, Synth::subLevel, Synth::noiseLevel, Synth::osc2Pitch })
            handles.push_back (device.getParameter (getInfo (index).id));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        const auto area = getLocalBounds().toFloat();
        graph::paintBackground (g, p, area);
        const auto inner = area.reduced (4.0f);

        const auto value = [this] (size_t i) { return handles[i].getValue(); };
        const auto wave1 = juce::roundToInt (value (0)), wave2 = juce::roundToInt (value (1));
        const auto mix = value (2), width = value (3), sub = value (4), noise = value (5);
        const auto ratio2 = std::exp2 (value (6) / 12.0f); // oscillator 2's pitch, against oscillator 1's

        g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
        g.drawHorizontalLine (juce::roundToInt (inner.getCentreY()), inner.getX(), inner.getRight());

        // Two cycles of oscillator 1, so that the sub, an octave down, shows one.
        juce::Random jitter (7);
        const auto curve = graph::makeCurve (
            inner,
            [&] (float x)
            {
                const auto t = (x - inner.getX()) / inner.getWidth() * 2.0f;
                const auto phase1 = t - std::floor (t), phase2 = t * ratio2 - std::floor (t * ratio2);
                auto out = (1.0f - mix) * waveAt (wave1, phase1, width) + mix * waveAt (wave2, phase2, width);
                out += sub * std::sin (juce::MathConstants<float>::pi * t);
                out += noise * (jitter.nextFloat() * 2.0f - 1.0f) * 0.6f;
                out /= 1.0f + sub + 0.6f * noise;
                return inner.getCentreY() - juce::jlimit (-1.0f, 1.0f, out) * inner.getHeight() * 0.45f;
            },
            1.0f);
        g.setColour (tint);
        g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth));
    }

private:
    std::vector<ParameterHandle> handles;

    void tick() override { repaint(); }
};

//==============================================================================
SynthEditor::SynthEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    const auto parameter = [this] (int index) { return device.getParameter (getInfo (index).id); };
    const auto shown = [this]() -> const Synth::Shown*
    {
        auto* synth = device.getProcessor<Synth>();
        return synth != nullptr ? &synth->getShown() : nullptr;
    };
    const auto rate = [this]
    {
        auto* scope = device.getScope();
        return scope != nullptr ? scope->getSampleRate() : 48000.0;
    };

    auto& oscillatorView = keep (std::make_unique<OscillatorView> (device));
    oscillators = &oscillatorView;

    // Every parameter, read once for each drawing of the filter's response.
    auto all = std::make_shared<std::vector<ParameterHandle>>();

    for (const auto& each : Synth::info.parameters)
        all->push_back (device.getParameter (each.id));

    auto& filterView = keep (std::make_unique<FilterView> (
        "Filter response", parameter (Synth::cutoff), parameter (Synth::resonance),
        [all]
        {
            std::vector<float> values;

            for (const auto& each : *all)
                values.push_back (each.getValue());

            return values;
        },
        [rate] (const std::vector<float>& values, double cutoffHertz, double frequency)
        { return Synth::getFilterMagnitude (values, cutoffHertz, frequency, rate()); },
        [shown] { return shown() != nullptr ? shown()->cutoff.get() : 0.0f; }));
    filter = &filterView;

    auto& filterEnvelopeView = keep (std::make_unique<EnvelopeView> (
        "Filter envelope", parameter (Synth::filterAttack), parameter (Synth::filterDecay), parameter (Synth::filterSustain),
        parameter (Synth::filterRelease), [shown] { return shown() != nullptr ? shown()->filterEnvelope.get() : 0.0f; }));
    filterEnvelope = &filterEnvelopeView;

    auto& ampView = keep (std::make_unique<EnvelopeView> ("Amp envelope", parameter (Synth::attack), parameter (Synth::decay),
                                                          parameter (Synth::sustain), parameter (Synth::release),
                                                          [shown] { return shown() != nullptr ? shown()->amp.get() : 0.0f; }));
    ampEnvelope = &ampView;

    auto lfoShape = std::make_shared<ParameterHandle> (parameter (Synth::lfoShape));
    auto& lfoView = keep (std::make_unique<LfoView> (
        "LFO wave", [lfoShape] (float phase) { return getDrawnLfoValue (juce::roundToInt (lfoShape->getValue()), phase); },
        [shown] { return shown() != nullptr ? shown()->lfoPhase.get() : -1.0f; }));
    lfo = &lfoView;

    const auto oscillatorsPart =
        makePicturePart ("Oscillators", oscillatorView, 56, 32,
                         { "osc1Wave", "osc2Wave", "osc2Pitch", "osc2Detune", "oscMix", "pulseWidth", "subLevel", "noiseLevel" }, 4,
                         ParameterControls::Size::compact, { "Osc 1", "Osc 2", "Pitch 2", "Detune 2", "Mix", "Width", "Sub", "Noise" });
    const auto unison = makeControlsPart ("Unison", { "unisonVoices", "unisonDetune", "unisonSpread" }, 3);
    const auto voice = makeControlsPart ("Voice", { "voiceMode", "glide", "bendRange", "velocity", "level" }, 3,
                                         ParameterControls::Size::compact, { "Mode", "Glide", "Bend", "Velocity", "Level" });
    const auto filterPart = makePicturePart (
        "Filter", filterView, 70, 40, { "filterType", "cutoff", "resonance", "filterSlope", "filterEnvelope", "keyTracking" }, 3,
        ParameterControls::Size::compact, { "Type", "Cutoff", "Resonance", "Slope", "Envelope", "Key" });
    const auto filterEnvelopePart = makePicturePart ("Filter Envelope", filterEnvelopeView, 56, 32,
                                                     { "filterAttack", "filterDecay", "filterSustain", "filterRelease" }, 4);
    const auto amp = makePicturePart ("Amp", ampView, 56, 32, { "attack", "decay", "sustain", "release" }, 4);
    const auto lfoPart = makePicturePart ("LFO", lfoView, 56, 32, { "lfoShape", "lfoRate", "lfoTarget", "lfoDepth" }, 2,
                                          ParameterControls::Size::compact, { "Shape", "Rate", "Target", "Depth" });

    setParts ({ { oscillatorsPart, unison, voice }, { filterPart, filterEnvelopePart }, { amp, lfoPart } });
}

SynthEditor::~SynthEditor() = default;

juce::Component& SynthEditor::getOscillators() const noexcept
{
    return *oscillators;
}
} // namespace phazite::ui
