// SPDX-License-Identifier: AGPL-3.0-or-later
#include "VocoderEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "devices/vocoder/Vocoder.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Vocoder;

    constexpr float quietest = -60.0f;  // the bottom of a column, in decibels
    constexpr float lightHeight = 4.0f; // each light in a column, and the gap above it
    constexpr float lightGap = 2.0f;
    constexpr float markHeight = 8.0f; // where a band listens, along the bottom
    constexpr float lightFall = 0.8f;  // of a column's height, kept per refresh, so that it falls smoothly
    constexpr int buzzNote = 48;       // C3 (middle C is C4, as the piano roll names it): the buzz at no pitch
    constexpr float consonantRadius = 5.0f;
    constexpr float carrierBarWidth = 64.0f; // how loud the carrier plays, under its name
    constexpr float carrierBarHeight = 3.0f;
} // namespace

class VocoderEditor::Bands : public juce::Component,
                             private DeviceTicker::Display
{
public:
    explicit Bands (const DeviceHandle& d)
        : DeviceTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          formant (device.getParameter ("formant"))
    {
        setTitle ("Vocoder bands");
        setDescription ("The voice's level in each band, a column of lights where the carrier plays the band, over a mark where it "
                        "listens to the voice. Drag sideways to move the carrier's bands up or down (Formant).");

        for (const auto& parameter : Vocoder::info.parameters)
        {
            parameters.push_back (device.getParameter (parameter.id));
            values.push_back (parameter.defaultValue);
        }

        readValues();
    }

    float getShownLevel (int band) const { return shown[(size_t) band]; }
    float getShownConsonant() const { return consonant; }

    juce::String getCarrierText() const
    {
        switch ((Vocoder::Carrier) juce::jlimit (0, (int) Vocoder::Carrier::track, juce::roundToInt (values[Vocoder::carrier])))
        {
            case Vocoder::Carrier::buzz:
                return "Buzz " + juce::MidiMessage::getMidiNoteName (buzzNote + juce::roundToInt (values[Vocoder::pitch]), true, true, 4);
            case Vocoder::Carrier::noise:
                return "Noise";
            case Vocoder::Carrier::voice:
                return "Voice";
            case Vocoder::Carrier::track:
                break;
        }

        const auto source = device.getSidechainSource();
        return source.track.isValid() ? "Track: " + source.name : juce::String ("Track: none feeds its sidechain");
    }

    bool needsTrack() const
    {
        return juce::roundToInt (values[Vocoder::carrier]) == (int) Vocoder::Carrier::track
               && ! device.getSidechainSource().track.isValid();
    }

    graph::FrequencyAxis getFrequencies() const { return { getLocalBounds().toFloat().reduced (1.0f) }; }

    juce::Point<float> getColumnPosition (int band) const
    {
        const auto area = getColumnArea();
        return { getFrequencies().xFor (Vocoder::getCarrierBandFrequency (values, band, getSampleRate())), area.getBottom() };
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        const auto area = getLocalBounds().toFloat().reduced (1.0f);
        const auto frequencies = getFrequencies();
        graph::paintBackground (g, p, area);
        graph::paintFrequencyGrid (g, p, frequencies, true);

        const auto count = Vocoder::getNumBands (values);
        const auto columns = getColumnArea();
        const auto spacing = columns.getWidth() / (float) std::max (1, count);
        const auto columnWidth = juce::jlimit (3.0f, 12.0f, spacing * 0.6f);
        const auto numLights = std::max (1, (int) (columns.getHeight() / (lightHeight + lightGap)));
        for (int band = 0; band < count; ++band)
        {
            const auto x = getColumnPosition (band).x;
            const auto listensAt = frequencies.xFor (Vocoder::getBandFrequency (values, band));

            // Where it listens, and the lean from there to where it plays.
            g.setColour (p.textMuted.withAlpha (GraphTint::marker));
            g.drawLine (listensAt, area.getBottom() - 1.0f, x, columns.getBottom() + 1.0f, 1.0f);

            // Its column: every light, faint; those the voice reaches, lit.
            const auto decibels = juce::Decibels::gainToDecibels (shown[(size_t) band], quietest);
            const auto lit = juce::roundToInt ((decibels - quietest) / -quietest * (float) numLights);

            for (int light = 0; light < numLights; ++light)
            {
                const auto y = columns.getBottom() - (float) (light + 1) * (lightHeight + lightGap) + lightGap;
                g.setColour (light < lit ? tint : tint.withAlpha (GraphTint::grid * 1.2f));
                g.fillRect (juce::Rectangle<float> (x - columnWidth * 0.5f, y, columnWidth, lightHeight));
            }
        }

        // What it says, and the carrier, in the warning colour when it needs a track; the consonant light.
        const auto formantText = std::abs (values[Vocoder::formant]) > 0.05f
                                     ? "  Formant " + juce::String (values[Vocoder::formant] > 0.0f ? "+" : "")
                                           + juce::String (values[Vocoder::formant], 1) + " st"
                                     : juce::String();
        graph::paintReadout (g, p, area,
                             juce::String (count) + " bands  " + graph::formatFrequency (values[Vocoder::lowFrequency]) + " to "
                                 + graph::formatFrequency (values[Vocoder::highFrequency]) + formantText,
                             juce::Justification::topLeft);

        if (needsTrack())
        {
            g.setColour (p.warning);
            g.setFont (juce::FontOptions (TextSize::small));
            g.drawText (getCarrierText(), area.reduced ((float) Space::xs), juce::Justification::topRight, true);
        }
        else
        {
            graph::paintReadout (g, p, area, getCarrierText(), juce::Justification::topRight);
        }

        // How loud the carrier plays, under its name: nothing while it is silent.
        const auto bar =
            juce::Rectangle<float> (area.getRight() - (float) Space::xs - carrierBarWidth,
                                    area.getY() + (float) Space::xs + TextSize::small + 1.0f, carrierBarWidth, carrierBarHeight);
        const auto carrierDecibels = juce::Decibels::gainToDecibels (carrierLevel, quietest);
        g.setColour (tint.withAlpha (GraphTint::grid * 1.2f));
        g.fillRect (bar);
        g.setColour (tint);
        g.fillRect (bar.withWidth (bar.getWidth() * (carrierDecibels - quietest) / -quietest));

        const auto light = juce::Rectangle<float> (2.0f * consonantRadius, 2.0f * consonantRadius)
                               .withCentre ({ area.getRight() - (float) Space::s - consonantRadius,
                                              area.getY() + (float) Space::xs + TextSize::small + (float) Space::s + 4.0f });
        g.setColour (tint.withAlpha (GraphTint::grid + (1.0f - GraphTint::grid) * consonant));
        g.fillEllipse (light);
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText ("Consonant", light.withRightX (light.getX() - (float) Space::xs).withLeft (area.getX()).expanded (0.0f, 4.0f),
                    juce::Justification::centredRight, true);
    }

    // A drag sideways moves the carrier's bands by as far as the pointer goes, in semitones, as one undo step.
    void mouseDown (const juce::MouseEvent& e) override
    {
        formant.beginGesture();
        startFormant = formant.getValue();
        startHertz = getFrequencies().frequencyAt (e.position.x);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto semitones = 12.0 * std::log2 (getFrequencies().frequencyAt (e.position.x) / startHertz);
        const auto range = formant.getRange();
        formant.setValue (juce::jlimit (range.start, range.end, startFormant + (float) semitones));
        readValues();
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { formant.endGesture(); }

private:
    DeviceHandle device;
    ParameterHandle formant;
    std::vector<ParameterHandle> parameters; // every parameter, in the vocoder's order
    std::vector<float> values;
    std::array<float, Vocoder::maxBands> shown {};
    float consonant = 0.0f, carrierLevel = 0.0f;
    float startFormant = 0.0f;
    double startHertz = 1000.0;

    juce::Rectangle<float> getColumnArea() const
    {
        auto area = getLocalBounds().toFloat().reduced (1.0f);
        area.removeFromTop (TextSize::small + 2.0f * (float) Space::xs + 2.0f * consonantRadius + (float) Space::s + 4.0f);
        area.removeFromBottom (markHeight);
        return area;
    }

    double getSampleRate() const
    {
        auto* scope = device.getScope();
        return scope != nullptr ? scope->getSampleRate() : 48000.0;
    }

    void readValues()
    {
        for (size_t i = 0; i < parameters.size(); ++i)
            if (parameters[i].isValid())
                values[i] = parameters[i].getValue();
    }

    void tick() override
    {
        readValues();

        if (auto* vocoder = device.getProcessor<Vocoder>())
        {
            // Each column rises at once and falls smoothly.
            for (int band = 0; band < Vocoder::maxBands; ++band)
                shown[(size_t) band] = std::max (vocoder->getBandLevel (band).get(), shown[(size_t) band] * lightFall);

            consonant = std::max (vocoder->getConsonant().get(), consonant * lightFall);
            carrierLevel = std::max (vocoder->getCarrierLevel().get(), carrierLevel * lightFall);
        }

        repaint();
    }
};

//==============================================================================
VocoderEditor::VocoderEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Bands> (device));
    bands = &shown;

    const auto carrier = makeControlsPart ("Carrier", { "carrier", "pitch" }, 2, ParameterControls::Size::compact, { "Carrier", "Pitch" });
    const auto voice = makeControlsPart ("Voice", { "formant", "unvoiced" }, 2, ParameterControls::Size::large);
    const auto range = makeControlsPart ("Bands", { "bands", "lowFrequency", "highFrequency", "bandWidth" }, 4,
                                         ParameterControls::Size::compact, { "Bands", "Low", "High", "Width" });
    const auto response = makeControlsPart ("Response", { "attack", "release", "brightness", "stereo", "output" }, 5,
                                            ParameterControls::Size::compact, { "Attack", "Release", "Bright", "Stereo", "Output" });
    setParts ({ { { "Bands", &shown, 340, 226, 140, true } }, { carrier, voice }, { range, response } });
}

VocoderEditor::~VocoderEditor() = default;

juce::Component& VocoderEditor::getBands() const noexcept
{
    return *bands;
}

float VocoderEditor::getShownLevel (int band) const
{
    return bands->getShownLevel (band);
}

float VocoderEditor::getShownConsonant() const
{
    return bands->getShownConsonant();
}

juce::String VocoderEditor::getCarrierText() const
{
    return bands->getCarrierText();
}

juce::Point<float> VocoderEditor::getColumnPosition (int band) const
{
    return bands->getColumnPosition (band);
}
} // namespace phazite::ui
