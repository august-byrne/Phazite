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
                                    private DisplayTicker::Display
{
public:
    explicit OscillatorView (const DeviceHandle& device)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this))
    {
        setTitle ("Oscillators' wave");
        setDescription ("One cycle of what the oscillators play together: their waves and mix, the pulse width, osc 2's sync, the sub "
                        "and the noise.");

        for (const auto index : { Synth::osc1Wave, Synth::osc2Wave, Synth::oscMix, Synth::pulseWidth, Synth::subLevel, Synth::noiseLevel,
                                  Synth::osc2Pitch, Synth::subShape, Synth::subOctave, Synth::osc2Sync })
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
        const auto subWave = juce::roundToInt (value (7));
        const auto subRatio = juce::roundToInt (value (8)) == 1 ? 0.25f : 0.5f; // an octave down, or two
        const auto synced = value (9) > 0.5f;

        g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
        g.drawHorizontalLine (juce::roundToInt (inner.getCentreY()), inner.getX(), inner.getRight());

        // Two cycles of oscillator 1, so that the sub, an octave down, shows one (two octaves down, half of one); oscillator 2
        // synced starts its cycle again with each of oscillator 1's.
        juce::Random jitter (7);
        const auto curve = graph::makeCurve (
            inner,
            [&] (float x)
            {
                const auto t = (x - inner.getX()) / inner.getWidth() * 2.0f;
                const auto phase1 = t - std::floor (t);
                const auto turns2 = synced ? phase1 * ratio2 : t * ratio2;
                const auto phase2 = turns2 - std::floor (turns2);
                auto out = (1.0f - mix) * waveAt (wave1, phase1, width) + mix * waveAt (wave2, phase2, width);
                const auto subTurns = t * subRatio;
                out += sub * waveAt (subWave, subTurns - std::floor (subTurns), 0.5f);
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
    DrawnNumbers drawn;

    void tick() override
    {
        std::vector<float> values;

        for (const auto& handle : handles)
            values.push_back (handle.getValue());

        if (drawn.changed (std::move (values)))
            repaint();
    }
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
    filterEnvelopeView.setCurves (parameter (Synth::filterAttackCurve), parameter (Synth::filterDecayCurve),
                                  parameter (Synth::filterReleaseCurve));
    filterEnvelope = &filterEnvelopeView;

    auto& ampView = keep (std::make_unique<EnvelopeView> ("Amp envelope", parameter (Synth::attack), parameter (Synth::decay),
                                                          parameter (Synth::sustain), parameter (Synth::release),
                                                          [shown] { return shown() != nullptr ? shown()->amp.get() : 0.0f; }));
    ampView.setCurves (parameter (Synth::attackCurve), parameter (Synth::decayCurve), parameter (Synth::releaseCurve));
    ampEnvelope = &ampView;

    auto lfoShape = std::make_shared<ParameterHandle> (parameter (Synth::lfoShape));
    auto& lfoView = keep (std::make_unique<LfoView> (
        "LFO wave", [lfoShape] (float phase) { return getDrawnLfoValue (juce::roundToInt (lfoShape->getValue()), phase); },
        [shown] { return shown() != nullptr ? shown()->lfoPhase.get() : -1.0f; }));
    lfoView.setControls (parameter (Synth::lfoDepth), parameter (Synth::lfoRate)); // a drag for the depth, the wheel for the rate
    lfo = &lfoView;

    // A part with a picture takes two rows of controls in the dock, and two parts stack in a column only a row each; each part's
    // lists share its first column, as wide as a list is, so that two parts fit side by side on a page.
    const auto oscillatorsPart =
        makePicturePart ("Oscillators", oscillatorView, 56, 32,
                         { "osc1Wave", "osc2Pitch", "oscMix", "subLevel", "osc2Wave", "osc2Detune", "pulseWidth", "noiseLevel" }, 4,
                         ParameterControls::Size::compact, { "Osc 1", "Pitch 2", "Mix", "Sub", "Osc 2", "Detune 2", "Width", "Noise" });
    const auto syncAndSub = makeControlsPart ("Sync and Sub", { "osc2Sync", "osc2Envelope", "subOctave", "subShape" }, 4,
                                              ParameterControls::Size::compact, { "Sync 2", "Env 2", "Sub Octave", "Sub Shape" });
    const auto pitch = makeControlsPart ("Pitch", { "pitchAmount", "pitchDecay", "drift" }, 3, ParameterControls::Size::compact,
                                         { "Envelope", "Decay", "Drift" });
    auto unison = makeControlsPart ("Unison", { "unisonVoices", "unisonDetune", "unisonSpread" }, 3);
    const auto voice = makeControlsPart ("Voice", { "voiceMode", "glide", "glideMode", "bendRange", "velocity", "level" }, 6,
                                         ParameterControls::Size::compact, { "Mode", "Glide", "Glide Mode", "Bend", "Velocity", "Level" });
    auto filterPart = makePicturePart (
        "Filter", filterView, 70, 40,
        { "filterType", "cutoff", "resonance", "keyTracking", "filterSlope", "drive", "filterEnvelope", "filterVelocity" }, 4,
        ParameterControls::Size::compact, { "Type", "Cutoff", "Resonance", "Key", "Slope", "Drive", "Envelope", "Velocity" });
    auto filterEnvelopePart = makePicturePart (
        "Filter Envelope", filterEnvelopeView, 56, 32,
        { "filterAttack", "filterDecay", "filterSustain", "filterRelease", "filterAttackCurve", "filterDecayCurve", "filterReleaseCurve" },
        4, ParameterControls::Size::compact, { "Attack", "Decay", "Sustain", "Release", "A Curve", "D Curve", "R Curve" });
    const auto amp =
        makePicturePart ("Amp", ampView, 56, 32, { "attack", "decay", "sustain", "release", "attackCurve", "decayCurve", "releaseCurve" },
                         4, ParameterControls::Size::compact, { "Attack", "Decay", "Sustain", "Release", "A Curve", "D Curve", "R Curve" });
    auto lfoPart = makePicturePart ("LFO", lfoView, 56, 32, { "lfoShape", "lfoRate", "lfoDepth", "lfoTarget", "lfoFade" }, 3,
                                    ParameterControls::Size::compact, { "Shape", "Rate", "Depth", "Target", "Fade In" });

    // The parts that switch on and off, each by the switch at its title's right end: the filter and its envelope, on at first;
    // the LFO and the unison, off (the owner, 2026-11-06: not every part need play from the start; they can be switched on).
    filterPart.titleSwitch = &makePartSwitch ("filterOn", *filterPart.content);
    filterEnvelopePart.titleSwitch = &makePartSwitch ("filterEnvelopeOn", *filterEnvelopePart.content);
    lfoPart.titleSwitch = &makePartSwitch ("lfoOn", *lfoPart.content);
    unison.titleSwitch = &makePartSwitch ("unisonOn", *unison.content);

    // A part to a column, as tall as the dock is short, so that a column never needs tabs: in the dock, two side by side on
    // each page, the sound (the oscillators and filter), then its envelopes, then its LFO, its pitch and osc 2's sync and the
    // sub's shape, then how it plays.
    setParts (
        { { oscillatorsPart }, { filterPart }, { amp }, { filterEnvelopePart }, { lfoPart }, { pitch, syncAndSub }, { unison, voice } });
}

SynthEditor::~SynthEditor() = default;

juce::Component& SynthEditor::getOscillators() const noexcept
{
    return *oscillators;
}
} // namespace phazite::ui
