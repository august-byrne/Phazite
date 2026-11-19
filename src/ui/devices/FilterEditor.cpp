// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FilterEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "GraphDot.h"
#include "devices/filter/FilterEffect.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <vector>

namespace phazite::ui
{
namespace
{
    using devices::FilterEffect;

    constexpr float lowestGain = -48.0f, highestGain = 24.0f; // the curve's levels, in decibels
    constexpr float driveStep = 1.0f;                         // decibels a notch of the wheel turns the drive

    const devices::ParameterInfo& getInfo (int index)
    {
        return FilterEffect::info.parameters[(size_t) index];
    }
} // namespace

class FilterEditor::Graph : public juce::Component,
                            public juce::TooltipClient,
                            private DisplayTicker::Display
{
public:
    explicit Graph (const DeviceHandle& d)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          cutoff (device.getParameter (getInfo (FilterEffect::cutoff).id)),
          resonance (device.getParameter (getInfo (FilterEffect::resonance).id)),
          drive (device.getParameter (getInfo (FilterEffect::drive).id))
    {
        setTitle ("Filter response");
        setDescription ("How the filter shapes the sound, over the spectrum of what it plays. Drag anywhere to move the dot: "
                        "sideways for the cutoff, up or down for the resonance. The wheel turns the drive.");

        for (const auto& parameter : FilterEffect::info.parameters)
        {
            parameters.push_back (device.getParameter (parameter.id));
            values.push_back (parameter.defaultValue);
        }

        readValues();

        // A drag moves the dot, the cutoff with it sideways and the resonance up and down, as one undo step.
        dot.setDrag ({ &cutoff, [this] (float hertz) { return getFrequencies().xFor (hertz); },
                       [this] (float x) { return (float) getFrequencies().frequencyAt (x); } },
                     { &resonance, [this] (float amount) { return getCurveArea().getBottom() - amount * getCurveArea().getHeight(); },
                       [this] (float y) { return (getCurveArea().getBottom() - y) / std::max (1.0f, getCurveArea().getHeight()); } },
                     [this]
                     {
                         readValues();
                         repaint();
                     });
        dot.setWheel ([this] { return &drive; });
    }

    ~Graph() override
    {
        if (auto* scope = device.getScope())
            scope->setListening (false);
    }

    juce::Rectangle<float> getCurveArea() const { return getLocalBounds().toFloat().reduced (1.0f); }
    graph::FrequencyAxis getFrequencies() const { return { getCurveArea() }; }

    juce::Point<float> getDotPosition() const
    {
        const auto area = getCurveArea();
        return { getFrequencies().xFor (values[FilterEffect::cutoff]),
                 area.getBottom() - values[FilterEffect::resonance] * area.getHeight() };
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        const auto area = getCurveArea();
        const auto frequencies = getFrequencies();
        const graph::LevelAxis levels { area, lowestGain, highestGain };
        graph::paintBackground (g, p, area);
        graph::paintFrequencyGrid (g, p, frequencies, true);

        // What comes in, faint; what goes out, lit; then the response over them.
        const auto spectrumLevels = graph::SpectrumTrace::getLevels (area);
        input.paint (g, frequencies, spectrumLevels, p.textMuted.withAlpha (GraphTint::spectrumFill),
                     p.textMuted.withAlpha (GraphTint::spectrumLine));
        output.paint (g, frequencies, spectrumLevels, tint.withAlpha (GraphTint::spectrumFill), tint.withAlpha (GraphTint::spectrumLine));

        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (levels.yFor (0.0f)), area.getX(), area.getRight());

        // What is heard: the response with what came in mixed back in at the Mix (FilterEffect::getHeardMagnitude()).
        const auto curve = graph::makeCurve (area, [&] (float x) { return levels.yFor (getCurveDecibels (frequencies.frequencyAt (x))); });
        g.setColour (tint);
        g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth));

        // The dot: the cutoff, as high as the resonance.
        dot.paint (g, p, tint);

        graph::paintReadouts (g, p, area, getReadout(),
                              values[FilterEffect::drive] > 0.05f ? "Drive " + graph::formatDecibels (values[FilterEffect::drive])
                                                                  : juce::String());
    }

    // The curve's level at a frequency, in decibels, as drawn (down to just under its bottom).
    float getCurveDecibels (double frequency) const
    {
        const auto magnitude = FilterEffect::getHeardMagnitude (values, frequency, getSampleRate());
        return (float) juce::Decibels::gainToDecibels (magnitude, (double) lowestGain - 1.0);
    }

    float getSpectrumLevel (double frequency) const { return output.getLevelAt (frequency); }

    // The type, Driven, the cutoff and the resonance; and for a type whose curve can't show the Mix (Sample & Hold, which has
    // no single response), the Mix under 100 %.
    juce::String getReadout() const
    {
        const auto type =
            juce::String (getInfo (FilterEffect::filterType).choices[(size_t) juce::roundToInt (values[FilterEffect::filterType])].data());
        const auto mix = values[FilterEffect::mix];
        const auto showsMix = mix < 0.995f && ! FilterEffect::getResponse (values, 1000.0, getSampleRate()).has_value();
        return type + (FilterEffect::isDriven (values) ? "  Driven  " : "  ") + graph::formatFrequency (values[FilterEffect::cutoff])
               + "  Res " + juce::String (juce::roundToInt (values[FilterEffect::resonance] * 100.0f)) + "%"
               + (showsMix ? "  Mix " + juce::String (juce::roundToInt (mix * 100.0f)) + "%" : juce::String());
    }

    juce::String getTooltip() override { return dot.getDrag().getTooltip(); }

    // Anywhere on it, the wheel turns the drive, in finer steps with Shift.
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const auto range = drive.getRange();
        const auto step = (wheel.deltaY > 0.0f ? driveStep : -driveStep) * graph::getFineFactor (e.mods);
        wheelGesture.turn (drive, juce::jlimit (range.start, range.end, drive.getBaseValue() + step));
        readValues();
        repaint();
    }

private:
    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the filter's order, for the curve
    std::vector<float> values;
    ParameterHandle cutoff, resonance, drive;
    graph::SpectrumTrace input, output;
    DrawnNumbers drawn;
    GraphDot dot { *this, [this] { return getDotPosition(); } }; // after the handles it drags
    graph::WheelGesture wheelGesture { dot.getDrag() };          // and the dot, whose press ends it

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

    // Shown again: the spectra of the sound now, not of what played while nobody looked.
    void shownAgain() override
    {
        if (auto* scope = device.getScope())
            scope->drain();

        input.clear();
        output.clear();
    }

    void tick() override
    {
        readValues();
        auto spectraMoved = false;

        if (auto* scope = device.getScope())
        {
            scope->setListening (true);
            input.setSampleRate (scope->getSampleRate());
            output.setSampleRate (scope->getSampleRate());

            if (auto* tap = scope->getInput())
                input.pull (*tap);

            if (auto* tap = scope->getOutput())
                output.pull (*tap);

            spectraMoved = input.update();
            spectraMoved = output.update() || spectraMoved;
        }

        auto numbers = values;
        numbers.push_back ((float) getSampleRate());

        if (drawn.changed (std::move (numbers)) || spectraMoved)
            repaint();
    }
};

//==============================================================================
// The Character part: Clean and Driven as two buttons along its top, over the type, the slope and the drive.
class FilterEditor::CharacterPart : public juce::Component
{
public:
    CharacterPart (ChoiceButtons& b, ParameterControls& c)
        : buttons (b),
          controls (c)
    {
        addAndMakeVisible (buttons);
        addAndMakeVisible (controls);
    }

    int getPreferredWidth() const { return std::max (buttons.getPreferredWidth(), controls.getPreferredWidth()); }
    int getPreferredHeight() const { return buttons.getPreferredHeight() + Space::xs + controls.getPreferredHeight(); }

    void resized() override
    {
        auto area = getLocalBounds();
        buttons.setBounds (area.removeFromTop (buttons.getPreferredHeight()));
        area.removeFromTop (Space::xs);
        controls.setBounds (area);
    }

private:
    ChoiceButtons& buttons;
    ParameterControls& controls;
};

FilterEditor::FilterEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Graph> (device));
    graph = &shown;

    const auto sweep = makeControlsPart ("Sweep", { "cutoff", "resonance" }, 2, ParameterControls::Size::large);

    auto& characters = keep (std::make_unique<ChoiceButtons> (
        device, getInfo (FilterEffect::character).id, ChoiceButtons::Direction::across,
        std::vector<juce::String> {
            "Clean: the filter is linear, and the drive saturates what goes into it, giving back the gain it adds",
            "Driven: the drive pushes the sound into the filter's loop, where its resonance saturates: louder and thicker as it is "
            "driven, and singing on its own at the top of the Resonance (low-pass, high-pass, band-pass and notch)" }));
    addControl (getInfo (FilterEffect::character).id, characters);
    auto& knobs = makeControls ({ "filterType", "filterSlope", "drive" }, 3);
    auto& shownCharacter = keep (std::make_unique<CharacterPart> (characters, knobs));
    const DeviceLayout::Part character { "Character", &shownCharacter, shownCharacter.getPreferredWidth(),
                                         shownCharacter.getPreferredHeight(), shownCharacter.getPreferredHeight() };
    const auto quality = makeControlsPart ("Quality", { "quality" }, 1);
    setParts ({ { { "Response", &shown, 320, 196, 100, true, GraphTint::minimumWidth } }, { sweep, character, quality } });
}

FilterEditor::~FilterEditor() = default;

juce::Component& FilterEditor::getGraph() const noexcept
{
    return *graph;
}

juce::Point<float> FilterEditor::getDotPosition() const
{
    return graph->getDotPosition();
}

float FilterEditor::getCurveDecibels (double frequency) const
{
    return graph->getCurveDecibels (frequency);
}

juce::String FilterEditor::getReadout() const
{
    return graph->getReadout();
}

float FilterEditor::getSpectrumLevel (double frequency) const
{
    return graph->getSpectrumLevel (frequency);
}
} // namespace phazite::ui
