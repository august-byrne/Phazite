// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FilterEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "devices/filter/FilterEffect.h"
#include "ui/theme/LookAndFeel.h"

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
        setDescription ("How the filter shapes the sound, over the spectrum of what it plays. Drag the dot sideways to move the "
                        "cutoff and up or down for the resonance; the wheel turns the drive.");

        for (const auto& parameter : FilterEffect::info.parameters)
        {
            parameters.push_back (device.getParameter (parameter.id));
            values.push_back (parameter.defaultValue);
        }

        readValues();
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
        const graph::LevelAxis spectrumLevels { area, -90.0f, 6.0f };
        input.paint (g, frequencies, spectrumLevels, p.textMuted.withAlpha (GraphTint::spectrumFill),
                     p.textMuted.withAlpha (GraphTint::spectrumLine));
        output.paint (g, frequencies, spectrumLevels, tint.withAlpha (GraphTint::spectrumFill), tint.withAlpha (GraphTint::spectrumLine));

        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (levels.yFor (0.0f)), area.getX(), area.getRight());

        const auto rate = getSampleRate();
        const auto curve =
            graph::makeCurve (area,
                              [&] (float x)
                              {
                                  const auto magnitude = FilterEffect::getMagnitude (values, frequencies.frequencyAt (x), rate);
                                  return levels.yFor ((float) juce::Decibels::gainToDecibels (magnitude, (double) lowestGain - 1.0));
                              });
        g.setColour (tint);
        g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth));

        // The dot: the cutoff, as high as the resonance.
        const auto dot = getDotPosition();
        g.setColour (tint);
        g.fillEllipse (juce::Rectangle<float> (2.0f * GraphTint::dotRadius, 2.0f * GraphTint::dotRadius).withCentre (dot));
        g.setColour (p.text);
        g.drawEllipse (juce::Rectangle<float> (2.0f * GraphTint::dotRadius, 2.0f * GraphTint::dotRadius).withCentre (dot), 1.5f);

        const auto type =
            juce::String (getInfo (FilterEffect::filterType).choices[(size_t) juce::roundToInt (values[FilterEffect::filterType])].data());
        graph::paintReadout (g, p, area,
                             type + "  " + graph::formatFrequency (values[FilterEffect::cutoff]) + "  Res "
                                 + juce::String (juce::roundToInt (values[FilterEffect::resonance] * 100.0f)) + "%",
                             juce::Justification::topLeft);

        if (values[FilterEffect::drive] > 0.05f)
            graph::paintReadout (g, p, area, "Drive " + graph::formatDecibels (values[FilterEffect::drive]), juce::Justification::topRight);
    }

    // A drag moves the dot, the cutoff with it sideways and the resonance up and down, as one undo step.
    void mouseDown (const juce::MouseEvent& e) override
    {
        cutoff.beginGesture();
        resonance.beginGesture();
        dragTo (e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override { dragTo (e.position); }

    void mouseUp (const juce::MouseEvent&) override
    {
        resonance.endGesture();
        cutoff.endGesture();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const auto range = drive.getRange();
        wheelGesture.turn (drive, juce::jlimit (range.start, range.end, drive.getValue() + (wheel.deltaY > 0.0f ? driveStep : -driveStep)));
        readValues();
        repaint();
    }

private:
    DeviceHandle device;
    std::vector<ParameterHandle> parameters; // every parameter, in the filter's order, for the curve
    std::vector<float> values;
    ParameterHandle cutoff, resonance, drive;
    graph::SpectrumTrace input, output;
    graph::WheelGesture wheelGesture; // after the handles it turns
    DrawnNumbers drawn;

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

    void dragTo (juce::Point<float> position)
    {
        const auto area = getCurveArea();
        const auto cutoffRange = cutoff.getRange();
        cutoff.setValue (juce::jlimit (cutoffRange.start, cutoffRange.end, (float) getFrequencies().frequencyAt (position.x)));
        resonance.setValue (juce::jlimit (0.0f, 1.0f, (area.getBottom() - position.y) / std::max (1.0f, area.getHeight())));
        readValues();
        repaint();
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
FilterEditor::FilterEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Graph> (device));
    graph = &shown;

    const auto sweep = makeControlsPart ("Sweep", { "cutoff", "resonance" }, 2, ParameterControls::Size::large);
    const auto character = makeControlsPart ("Character", { "filterType", "filterSlope", "drive" }, 3);
    setParts ({ { { "Response", &shown, 320, 196, 100, true } }, { sweep, character } });
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
} // namespace phazite::ui
