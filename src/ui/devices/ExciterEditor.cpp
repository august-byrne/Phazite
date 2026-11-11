// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExciterEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "GraphDot.h"
#include "devices/saturation/Exciter.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Exciter;

    constexpr float lowestLift = -3.0f, highestLift = 15.0f;    // the lift's levels, in decibels
    constexpr float driveStep = 1.0f;                           // decibels a notch of the wheel turns the drive
    constexpr float lowestReach = -30.0f, highestReach = 12.0f; // the meter's, in decibels against the curve's bend
    constexpr float reachFall = 0.5f;                           // decibels a refresh: 15 dB a second
    constexpr float addedFill = 0.45f;                          // how strongly what is added is filled, over the spectra

    const devices::ParameterInfo& getInfo (int index)
    {
        return Exciter::info.parameters[(size_t) index];
    }

    // An amount as the top's lift, in decibels, and back.
    float liftOf (float amount)
    {
        return juce::Decibels::gainToDecibels (1.0f + amount);
    }

    float amountFor (float lift)
    {
        return juce::Decibels::decibelsToGain (lift) - 1.0f;
    }
} // namespace

//==============================================================================
class ExciterEditor::Graph : public juce::Component,
                             private DisplayTicker::Display
{
public:
    explicit Graph (const DeviceHandle& d)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          device (d),
          frequency (device.getParameter (getInfo (Exciter::frequency).id)),
          amount (device.getParameter (getInfo (Exciter::amount).id)),
          drive (device.getParameter (getInfo (Exciter::drive).id))
    {
        setTitle ("Excited band");
        setDescription ("What the exciter adds, over the spectrum of the sound, and how far it lifts each frequency. Drag the dot, "
                        "or press anywhere to put it there: sideways for where the band starts, up or down for the amount. The wheel "
                        "turns the drive. The meter shows how far the band reaches into the curve: over its line, it turns into "
                        "harmonics.");

        for (const auto& parameter : Exciter::info.parameters)
        {
            parameters.push_back (device.getParameter (parameter.id));
            values.push_back (parameter.defaultValue);
        }

        readValues();

        // The meter's first reading would show what played before the editor opened.
        if (auto* exciter = device.getProcessor<Exciter>())
            exciter->getReach().take();
    }

    ~Graph() override
    {
        if (auto* exciter = device.getProcessor<Exciter>())
            exciter->getAddedTap().setListening (false);

        if (auto* scope = device.getScope())
            scope->setListening (false);
    }

    juce::Rectangle<float> getMeterArea() const
    {
        return getLocalBounds().toFloat().reduced (1.0f).removeFromRight ((float) GraphTint::meterWidth);
    }

    juce::Rectangle<float> getCurveArea() const
    {
        return getLocalBounds().toFloat().reduced (1.0f).withTrimmedRight ((float) (GraphTint::meterWidth + Space::xs));
    }

    graph::FrequencyAxis getFrequencies() const { return { getCurveArea() }; }
    graph::LevelAxis getLifts() const { return { getCurveArea(), lowestLift, highestLift }; }

    juce::Point<float> getDotPosition() const
    {
        return { getFrequencies().xFor (values[Exciter::frequency]), getLifts().yFor (liftOf (values[Exciter::amount])) };
    }

    float getReach() const noexcept { return shownReach; }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        const auto area = getCurveArea();
        const auto frequencies = getFrequencies();
        const auto lifts = getLifts();
        graph::paintBackground (g, p, area);
        graph::paintFrequencyGrid (g, p, frequencies, true);

        // What comes in, faint; what goes out, in its colour; and what is added, filled, over them.
        const graph::LevelAxis spectrumLevels { area, -90.0f, 6.0f };
        input.paint (g, frequencies, spectrumLevels, p.textMuted.withAlpha (GraphTint::spectrumFill),
                     p.textMuted.withAlpha (GraphTint::spectrumLine));
        output.paint (g, frequencies, spectrumLevels, tint.withAlpha (GraphTint::spectrumFill), tint.withAlpha (GraphTint::spectrumLine));
        added.paint (g, frequencies, spectrumLevels, tint.withAlpha (addedFill), tint.withAlpha (GraphTint::spectrumLine));

        // Where the band starts, and where High Cut ends it.
        g.setColour (p.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (lifts.yFor (0.0f)), area.getX(), area.getRight());
        g.setColour (tint.withAlpha (GraphTint::marker));
        g.drawVerticalLine (juce::roundToInt (frequencies.xFor (values[Exciter::frequency])), area.getY(), area.getBottom());

        if (values[Exciter::highCut] < getInfo (Exciter::highCut).maximum - 0.5f)
            g.drawVerticalLine (juce::roundToInt (frequencies.xFor (values[Exciter::highCut])), area.getY(), area.getBottom());

        // The lift.
        const auto rate = getSampleRate();
        const auto curve = graph::makeCurve (area, [&] (float x)
                                             { return lifts.yFor ((float) Exciter::getLift (values, frequencies.frequencyAt (x), rate)); });
        g.setColour (tint);
        g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth));

        dot.paint (g, p, tint);

        graph::paintReadouts (g, p, area,
                              graph::formatFrequency (values[Exciter::frequency]) + "  "
                                  + graph::formatDecibels (liftOf (values[Exciter::amount])),
                              "Drive " + graph::formatDecibels (values[Exciter::drive]));
        paintMeter (g, p, tint);
    }

    // A drag moves the dot, the frequency with it sideways and the amount up and down, as one undo step.
    void mouseDown (const juce::MouseEvent& e) override
    {
        frequency.beginGesture();
        amount.beginGesture();
        dragTo (e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override { dragTo (e.position); }

    void mouseUp (const juce::MouseEvent&) override
    {
        amount.endGesture();
        frequency.endGesture();
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
    std::vector<ParameterHandle> parameters; // every parameter, in the exciter's order, for the lift
    std::vector<float> values;
    ParameterHandle frequency, amount, drive;
    graph::SpectrumTrace input, output, added;
    graph::WheelGesture wheelGesture; // after the handles it turns
    float shownReach = lowestReach;
    DrawnNumbers drawn;
    GraphDot dot { *this, [this] { return getDotPosition(); } };

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
        const auto frequencyRange = frequency.getRange(), amountRange = amount.getRange();
        frequency.setValue (juce::jlimit (frequencyRange.start, frequencyRange.end, (float) getFrequencies().frequencyAt (position.x)));
        amount.setValue (juce::jlimit (amountRange.start, amountRange.end, amountFor (getLifts().levelAt (position.y))));
        readValues();
        repaint();
    }

    // How far the band reaches into the curve, from the bottom up, and a line where the curve bends.
    void paintMeter (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto meter = getMeterArea();
        const graph::LevelAxis reaches { meter, lowestReach, highestReach };
        g.setColour (p.background);
        g.fillRoundedRectangle (meter, cornerRadius);
        const auto top = reaches.yFor (juce::jlimit (lowestReach, highestReach, shownReach));
        g.setColour (tint);
        g.fillRect (meter.withTop (top));
        g.setColour (p.text.withAlpha (GraphTint::marker));
        g.drawHorizontalLine (juce::roundToInt (reaches.yFor (0.0f)), meter.getX(), meter.getRight());
    }

    void tick() override
    {
        readValues();
        auto spectraMoved = false;

        if (auto* scope = device.getScope())
        {
            scope->setListening (true);

            for (auto* trace : { &input, &output, &added })
                trace->setSampleRate (scope->getSampleRate());

            if (auto* tap = scope->getInput())
                input.pull (*tap);

            if (auto* tap = scope->getOutput())
                output.pull (*tap);

            spectraMoved = input.update();
            spectraMoved = output.update() || spectraMoved;
        }

        // What is added, and how far the band reached into the curve: up at once, falling back slowly.
        if (auto* exciter = device.getProcessor<Exciter>())
        {
            added.pull (exciter->getAddedTap());
            spectraMoved = added.update() || spectraMoved;
            const auto reached = juce::Decibels::gainToDecibels (exciter->getReach().take(), lowestReach);
            shownReach = std::max (reached, std::max (lowestReach, shownReach - reachFall));
        }

        auto numbers = values;
        numbers.insert (numbers.end(), { (float) getSampleRate(), shownReach });

        if (drawn.changed (std::move (numbers)) || spectraMoved)
            repaint();
    }
};

//==============================================================================
// The curve's controls, and Listen beside them: while it is on and the editor shows, the exciter plays what it adds alone
// (Exciter::listenToAdded(), which the refresh asks for again and again). Hidden, or with the editor gone, the sound comes
// back within half a second.
class ExciterEditor::HarmonicsPart : public juce::Component
{
public:
    explicit HarmonicsPart (ParameterControls& c)
        : controls (c)
    {
        listen.setTitle ("Listen");
        listen.setTooltip ("Plays what the exciter adds alone, without the sound it is added to, to set it by ear");
        listen.setClickingTogglesState (true);
        addAndMakeVisible (controls);
        addAndMakeVisible (listen);
    }

    int getPreferredWidth() const { return controls.getPreferredWidth() + Space::xs + listenWidth; }

    void resized() override
    {
        auto area = getLocalBounds();
        controls.setBounds (area.removeFromLeft (controls.getPreferredWidth()));
        area.removeFromLeft (Space::xs);
        listen.setBounds (area.withSizeKeepingCentre (area.getWidth(), controlHeight));
    }

    ParameterControls& controls;
    juce::TextButton listen { "Listen" };

private:
    static constexpr int listenWidth = 64;
};

//==============================================================================
ExciterEditor::ExciterEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DisplayTicker::Display (static_cast<juce::Component&> (*this))
{
    auto& shown = keep (std::make_unique<Graph> (device));
    graph = &shown;

    const auto band = makeControlsPart ("Band", { "frequency", "amount" }, 2, ParameterControls::Size::large);
    auto& curve = makeControls ({ "drive", "colour", "highCut" }, 3);
    auto& shownHarmonics = keep (std::make_unique<HarmonicsPart> (curve));
    harmonics = &shownHarmonics;

    setParts ({ { { "Excited band", &shown, 320, 196, 100, true, GraphTint::minimumWidth } },
                { band,
                  { "Harmonics", &shownHarmonics, shownHarmonics.getPreferredWidth(), curve.getPreferredHeight(),
                    curve.getPreferredHeight() } } });
}

ExciterEditor::~ExciterEditor() = default;

juce::Component& ExciterEditor::getGraph() const noexcept
{
    return *graph;
}

juce::Button& ExciterEditor::getListenButton() const noexcept
{
    return harmonics->listen;
}

juce::Point<float> ExciterEditor::getDotPosition() const
{
    return graph->getDotPosition();
}

float ExciterEditor::getShownReach() const
{
    return graph->getReach();
}

// Listen: what is added alone, for as long as the editor shows and asks.
void ExciterEditor::tick()
{
    if (harmonics->listen.getToggleState())
        if (auto* exciter = device.getProcessor<Exciter>())
            exciter->listenToAdded();
}
} // namespace phazite::ui
