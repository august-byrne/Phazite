// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MaximizerEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "devices/dynamics/Maximizer.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    using devices::Maximizer;

    constexpr float lowestLevel = -36.0f; // each curve's levels, in and out, from here to 0 dB
    constexpr float levelFall = 0.75f;    // dB per refresh, as a level falls back
    constexpr float levelRange = 24.0f;   // decibels a drag the panel's height moves its band's level
    constexpr float wheelStep = 0.5f;

    const char* const bandNames[] = { "Low", "Mid", "High" };
    constexpr std::string_view squeezeIDs[] = { "lowSqueeze", "midSqueeze", "highSqueeze" };
    constexpr std::string_view levelIDs[] = { "lowLevel", "midLevel", "highLevel" };
    constexpr std::string_view crossoverIDs[] = { "lowFrequency", "highFrequency" };

    juce::Colour getBandColour (int band)
    {
        return juce::Colour (partColours[band]);
    }

    std::unique_ptr<Knob> makeKnob (std::string_view id)
    {
        const auto& parameter = Maximizer::info.parameters[(size_t) devices::findParameter (Maximizer::info, id)];
        auto knob = std::make_unique<Knob> (juce::String (parameter.name.data(), parameter.name.size()), parameter.defaultValue);
        showAsParameter (*knob, parameter);
        return knob;
    }
} // namespace

//==============================================================================
class MaximizerEditor::Bands : public juce::Component
{
public:
    explicit Bands (const DeviceHandle& d)
        : device (d)
    {
        setTitle ("Bands");
        setDescription ("The three bands, each with its squeeze's curve and a dot at its level. Drag a band up or down to set its "
                        "level, and drag across the strip above to move where the bands split.");

        for (size_t band = 0; band < Maximizer::numBands; ++band)
        {
            squeezes[band] = device.getParameter (squeezeIDs[band]);
            levels[band] = device.getParameter (levelIDs[band]);

            for (auto [knob, attachment, id] : { std::tuple { &squeezeKnobs[band], &squeezeAttachments[band], squeezeIDs[band] },
                                                 std::tuple { &levelKnobs[band], &levelAttachments[band], levelIDs[band] } })
            {
                *knob = makeKnob (id);
                (*knob)->setColour (juce::Slider::rotarySliderFillColourId, getBandColour ((int) band));
                *attachment = std::make_unique<ParameterAttachment> (**knob);
                (*attachment)->setParameter (device.getParameter (id));
                addAndMakeVisible (**knob);
            }
        }

        for (size_t crossover = 0; crossover < 2; ++crossover)
            crossovers[crossover] = device.getParameter (crossoverIDs[crossover]);
    }

    ~Bands() override
    {
        if (auto* scope = device.getScope())
            scope->setListening (false);
    }

    Knob& getSqueezeKnob (int band) const { return *squeezeKnobs[(size_t) band]; }
    Knob& getLevelKnob (int band) const { return *levelKnobs[(size_t) band]; }
    float getShownLevel (int band) const { return shownLevels[(size_t) band]; }
    float getShownReduction (int band) const { return shownReductions[(size_t) band]; }

    juce::Rectangle<float> getStripArea() const
    {
        auto area = getLocalBounds().toFloat();
        return area.removeFromTop (std::clamp (area.getHeight() * 0.16f, 18.0f, 32.0f));
    }

    juce::Rectangle<int> getColumnArea (int band) const
    {
        auto area = getLocalBounds().withTrimmedTop (juce::roundToInt (getStripArea().getHeight()) + Space::xs);
        const auto width = (area.getWidth() - 2 * Space::xs) / Maximizer::numBands;
        return area.withX (area.getX() + band * (width + Space::xs)).withWidth (width);
    }

    juce::Rectangle<int> getBandArea (int band) const { return getColumnArea (band).withTrimmedBottom (compactKnobSize + Space::xs); }

    void resized() override
    {
        for (int band = 0; band < Maximizer::numBands; ++band)
        {
            auto knobs = getColumnArea (band).removeFromBottom (compactKnobSize);
            const auto half = knobs.getWidth() / 2;
            squeezeKnobs[(size_t) band]->setBounds (knobs.removeFromLeft (half).withSizeKeepingCentre (compactKnobSize, compactKnobSize));
            levelKnobs[(size_t) band]->setBounds (knobs.withSizeKeepingCentre (compactKnobSize, compactKnobSize));
        }
    }

    void tick()
    {
        for (auto* attachments : { &squeezeAttachments, &levelAttachments })
            for (auto& attachment : *attachments)
                attachment->update();

        if (auto* scope = device.getScope())
        {
            scope->setListening (true);

            if (auto* output = scope->getOutput())
                spectrum.pull (*output);

            spectrum.update();
        }

        if (auto* maximizer = device.getProcessor<Maximizer>())
        {
            for (size_t band = 0; band < Maximizer::numBands; ++band)
            {
                shownLevels[band] = std::max (maximizer->getBandLevel ((int) band).take(), shownLevels[band] - levelFall);
                shownReductions[band] = std::max (maximizer->getBandReduction ((int) band).take(), shownReductions[band] - levelFall);
            }
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        paintStrip (g, p);

        for (int band = 0; band < Maximizer::numBands; ++band)
            paintBand (g, p, band);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragged = nullptr;

        if (getStripArea().contains (e.position))
        {
            const auto axis = getFrequencyAxis();
            const auto lowX = axis.xFor (crossovers[0].getValue()), highX = axis.xFor (crossovers[1].getValue());
            dragged = &crossovers[std::abs (e.position.x - lowX) <= std::abs (e.position.x - highX) ? 0 : 1];
        }
        else if (const auto band = findBand (e.position); band >= 0)
        {
            dragged = &levels[(size_t) band];
        }

        if (dragged != nullptr)
        {
            dragged->beginGesture();
            startValue = dragged->getValue();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragged == nullptr)
            return;

        if (dragged == &crossovers[0] || dragged == &crossovers[1])
            dragged->setValue (constrained (*dragged, (float) getFrequencyAxis().frequencyAt (e.position.x)));
        else
            dragged->setValue (constrained (
                *dragged,
                startValue - (float) e.getDistanceFromDragStartY() / (float) std::max (1, getBandArea (0).getHeight()) * levelRange));

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragged != nullptr)
            dragged->endGesture();

        dragged = nullptr;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const auto band = findBand (e.position); band >= 0)
            levels[(size_t) band].setValue (0.0f);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (const auto band = findBand (e.position); band >= 0)
        {
            auto& level = levels[(size_t) band];
            wheelGesture.turn (level, constrained (level, level.getValue() + (wheel.deltaY > 0.0f ? wheelStep : -wheelStep)));
            repaint();
        }
    }

private:
    DeviceHandle device;
    std::array<ParameterHandle, Maximizer::numBands> squeezes, levels;
    std::array<ParameterHandle, 2> crossovers;
    std::array<std::unique_ptr<Knob>, Maximizer::numBands> squeezeKnobs, levelKnobs;
    std::array<std::unique_ptr<ParameterAttachment>, Maximizer::numBands> squeezeAttachments, levelAttachments;
    graph::SpectrumTrace spectrum;
    std::array<float, Maximizer::numBands> shownLevels { -120.0f, -120.0f, -120.0f }, shownReductions {};
    ParameterHandle* dragged = nullptr;
    float startValue = 0.0f;
    graph::WheelGesture wheelGesture; // after the handles it turns

    graph::FrequencyAxis getFrequencyAxis() const { return { getStripArea() }; }

    static float constrained (const ParameterHandle& handle, float value)
    {
        const auto range = handle.getRange();
        return range.snapToLegalValue (juce::jlimit (range.start, range.end, value));
    }

    int findBand (juce::Point<float> position) const
    {
        for (int band = 0; band < Maximizer::numBands; ++band)
            if (getBandArea (band).toFloat().contains (position))
                return band;

        return -1;
    }

    void paintStrip (juce::Graphics& g, const Palette& p)
    {
        const auto area = getStripArea();
        const auto axis = getFrequencyAxis();
        graph::paintBackground (g, p, area);

        // Each band's stretch of the spectrum in its colour.
        const double edges[] = { axis.low, crossovers[0].getValue(), crossovers[1].getValue(), axis.high };

        for (int band = 0; band < Maximizer::numBands; ++band)
        {
            const auto stretch = area.withX (axis.xFor (edges[band])).withRight (axis.xFor (edges[band + 1]));
            g.saveState();
            g.reduceClipRegion (stretch.toNearestInt());
            spectrum.paint (g, axis, { area, -90.0f, 6.0f }, getBandColour (band).withAlpha (GraphTint::spectrumFill * 2.0f),
                            getBandColour (band).withAlpha (GraphTint::spectrumLine));
            g.restoreState();
        }

        for (int crossover = 0; crossover < 2; ++crossover)
        {
            const auto x = axis.xFor (crossovers[(size_t) crossover].getValue());
            g.setColour (p.text.withAlpha (GraphTint::marker));
            g.drawLine (x, area.getY(), x, area.getBottom(), 2.0f);
        }
    }

    void paintBand (juce::Graphics& g, const Palette& p, int band)
    {
        const auto colour = getBandColour (band);
        const auto area = getBandArea (band).toFloat();
        graph::paintBackground (g, p, area);
        const auto curveArea = area.reduced ((float) Space::xs).withTrimmedTop ((float) TextSize::small + 2.0f);
        const auto squeeze = squeezes[(size_t) band].getValue();
        const auto xFor = [&curveArea] (float level)
        { return curveArea.getX() + (level - lowestLevel) / -lowestLevel * curveArea.getWidth(); };
        const auto yFor = [&curveArea] (float level)
        { return curveArea.getBottom() - (level - lowestLevel) / -lowestLevel * curveArea.getHeight(); };

        // In equals out, faint, and where the squeeze begins.
        g.setColour (p.text.withAlpha (GraphTint::grid * 2.0f));
        g.drawLine (xFor (lowestLevel), yFor (lowestLevel), xFor (0.0f), yFor (0.0f));
        g.setColour (p.text.withAlpha (GraphTint::grid));
        g.drawVerticalLine (juce::roundToInt (xFor (Maximizer::squeezeThreshold)), curveArea.getY(), curveArea.getBottom());

        // The curve, in its colour: how loud each level comes out.
        const auto curve = graph::makeCurve (curveArea,
                                             [&] (float x)
                                             {
                                                 const auto in = lowestLevel + (x - curveArea.getX()) / curveArea.getWidth() * -lowestLevel;
                                                 return yFor (in - Maximizer::getReduction (squeeze, in));
                                             });
        g.setColour (colour);
        g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth));

        // The band's level, riding it.
        if (const auto level = shownLevels[(size_t) band]; level > lowestLevel)
        {
            const auto in = std::min (level, 0.0f);
            const juce::Point<float> dot { xFor (in), yFor (in - Maximizer::getReduction (squeeze, in)) };
            g.fillEllipse (juce::Rectangle<float> (2.0f * GraphTint::levelDotRadius, 2.0f * GraphTint::levelDotRadius).withCentre (dot));
        }

        g.setColour (p.text);
        g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
        g.drawText (bandNames[band], area.reduced ((float) Space::xs, 1.0f).removeFromTop ((float) TextSize::small),
                    juce::Justification::centredLeft, false);

        if (const auto reduction = shownReductions[(size_t) band]; reduction >= 0.1f)
            graph::paintReadout (g, p, area.withTrimmedTop (1.0f), "-" + juce::String (reduction, 1) + " dB",
                                 juce::Justification::topRight);

        graph::paintReadout (g, p, area, graph::formatDecibels (levels[(size_t) band].getValue()), juce::Justification::bottomRight);
    }
};

//==============================================================================
MaximizerEditor::MaximizerEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle),
      DeviceTicker::Display (static_cast<juce::Component&> (*this))
{
    auto& shownBands = keep (std::make_unique<Bands> (device));
    bands = &shownBands;
    auto& shownLimiter = keep (std::make_unique<LimiterDisplay> (device.getParameter ("ceiling")));
    limiterDisplay = &shownLimiter;

    for (int band = 0; band < Maximizer::numBands; ++band)
    {
        addControl (squeezeIDs[(size_t) band], shownBands.getSqueezeKnob (band));
        addControl (levelIDs[(size_t) band], shownBands.getLevelKnob (band));
    }

    // The meters' first readings would show what played before the editor opened.
    if (auto* maximizer = device.getProcessor<Maximizer>())
    {
        for (int band = 0; band < Maximizer::numBands; ++band)
        {
            maximizer->getBandLevel (band).take();
            maximizer->getBandReduction (band).take();
        }

        auto& limiter = maximizer->getLimiter();
        limiter.getInputMeter().take();
        limiter.getReductionMeter().take();
        limiter.getOutputMeter().take();
    }

    const auto bandsWidth = 3 * 2 * (compactKnobSize + Space::xs) + 2 * Space::xs;
    const auto gain = makeControlsPart ("Gain", { "gain" }, 1, ParameterControls::Size::large);
    const auto limit = makeControlsPart ("Limit", { "ceiling", "release" }, 2);
    const auto split =
        makeControlsPart ("Split", { "lowFrequency", "highFrequency" }, 2, ParameterControls::Size::compact, { "Low", "High" });

    setParts ({ { { "Bands", &shownBands, bandsWidth, 196, 150, true }, { "Limiter", &shownLimiter, bandsWidth, 150, 96, true } },
                { gain, limit },
                { split } });
}

MaximizerEditor::~MaximizerEditor() = default;

juce::Component& MaximizerEditor::getBands() const noexcept
{
    return *bands;
}

float MaximizerEditor::getShownLevel (int band) const
{
    return bands->getShownLevel (band);
}

float MaximizerEditor::getShownReduction (int band) const
{
    return bands->getShownReduction (band);
}

juce::Rectangle<int> MaximizerEditor::getBandArea (int band) const
{
    return bands->getBandArea (band);
}

void MaximizerEditor::refresh()
{
    bands->tick();

    if (auto* maximizer = device.getProcessor<Maximizer>())
    {
        auto& limiter = maximizer->getLimiter();
        limiterDisplay->show (limiter.getInputMeter().take(), limiter.getReductionMeter().take(), limiter.getOutputMeter().take());
    }
}
} // namespace phazite::ui
