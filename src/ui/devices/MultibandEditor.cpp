// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MultibandEditor.h"
#include "DeviceGraph.h"
#include "DeviceTint.h"
#include "devices/dynamics/Multiband.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    using devices::Multiband;

    constexpr float lowestLevel = -60.0f; // the columns' levels, from here to 0 dB
    constexpr float levelFall = 0.75f;    // dB per refresh, as a level falls back: about 22 dB a second
    constexpr float gainRange = 48.0f;    // decibels a drag the column's height moves its band's gain
    constexpr float wheelStep = 0.5f;     // decibels a notch of the wheel moves it

    const char* const bandNames[] = { "Low", "Mid", "High" };
    constexpr std::string_view gainIDs[] = { "lowGain", "midGain", "highGain" };
    constexpr std::string_view crossoverIDs[] = { "lowFrequency", "highFrequency" };
} // namespace

//==============================================================================
class MultibandEditor::Display : public juce::Component,
                                 private DisplayTicker::Display
{
public:
    explicit Display (const DeviceHandle& d)
        : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
          device (d)
    {
        setTitle ("Bands");
        setDescription ("The three bands, each turned down above -30 dB and brought up below -40 dB, with how loud it comes in, how "
                        "far it is pushed and how loud it goes out. Drag a band up or down to set its gain, and drag across the "
                        "spectrum to move where the bands split.");

        // Each band's gain: a knob at the foot of its column, which the column's drag also turns.
        for (size_t band = 0; band < Multiband::numBands; ++band)
        {
            gains[band] = device.getParameter (gainIDs[band]);
            const auto index = devices::findParameter (Multiband::info, gainIDs[band]);
            const auto& parameter = Multiband::info.parameters[(size_t) index];
            gainKnobs[band] = std::make_unique<Knob> (juce::String (parameter.name.data(), parameter.name.size()), parameter.defaultValue);
            showAsParameter (*gainKnobs[band], parameter);
            gainAttachments[band] = std::make_unique<ParameterAttachment> (*gainKnobs[band]);
            gainAttachments[band]->setParameter (device.getParameter (gainIDs[band]));
            addAndMakeVisible (*gainKnobs[band]);
        }

        for (size_t crossover = 0; crossover < 2; ++crossover)
            crossovers[crossover] = device.getParameter (crossoverIDs[crossover]);

        // The meters' first readings would show what played before the editor opened.
        if (auto* multiband = device.getProcessor<Multiband>())
            for (int band = 0; band < Multiband::numBands; ++band)
                takeMeters (*multiband, band);
    }

    ~Display() override
    {
        if (auto* scope = device.getScope())
            scope->setListening (false);
    }

    float getShownLevel (int band) const { return shownLevels[(size_t) band]; }
    float getShownChange (int band) const { return shownChanges[(size_t) band]; }
    Knob& getGainKnob (int band) const { return *gainKnobs[(size_t) band]; }

    void resized() override
    {
        for (int band = 0; band < Multiband::numBands; ++band)
            gainKnobs[(size_t) band]->setBounds (getKnobArea (band));
    }

    // Under each column, its gain's knob.
    juce::Rectangle<int> getKnobArea (int band) const
    {
        const auto column = getColumnArea (band);
        return column.withTop (column.getBottom() - compactKnobSize).withSizeKeepingCentre (compactKnobSize, compactKnobSize);
    }

    // The spectrum's strip at the top, then the bands' columns.
    juce::Rectangle<float> getSpectrumArea() const
    {
        auto area = getLocalBounds().toFloat();
        return area.removeFromTop (std::clamp (area.getHeight() * 0.22f, 22.0f, 48.0f));
    }

    // A band's column: its meter, then its knob.
    juce::Rectangle<int> getColumnArea (int band) const
    {
        auto area = getLocalBounds().withTrimmedTop (juce::roundToInt (getSpectrumArea().getHeight()) + Space::xs);
        const auto width = (area.getWidth() - 2 * Space::xs) / Multiband::numBands;
        return area.withX (area.getX() + band * (width + Space::xs)).withWidth (width);
    }

    juce::Rectangle<int> getBandArea (int band) const { return getColumnArea (band).withTrimmedBottom (compactKnobSize + Space::xs); }

    graph::FrequencyAxis getFrequencyAxis() const { return { getSpectrumArea() }; }

    int getCrossoverX (int crossover) const
    {
        return juce::roundToInt (getFrequencyAxis().xFor (crossovers[(size_t) crossover].getValue()));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = getPalette (*this);
        const auto tint = getDeviceTint (*this);
        paintSpectrum (g, p, tint);

        for (int band = 0; band < Multiband::numBands; ++band)
            paintBand (g, p, tint, band);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragged = nullptr;

        // Across the spectrum, the crossover nearest; on a column, its band's gain.
        if (getSpectrumArea().contains (e.position))
        {
            const auto lowX = (float) getCrossoverX (0), highX = (float) getCrossoverX (1);
            dragged = &crossovers[std::abs (e.position.x - lowX) <= std::abs (e.position.x - highX) ? 0 : 1];
        }
        else if (const auto band = findBand (e.position); band >= 0)
        {
            dragged = &gains[(size_t) band];
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

        if (isCrossover (*dragged))
        {
            dragged->setValue (constrained (*dragged, (float) getFrequencyAxis().frequencyAt (e.position.x)));
        }
        else
        {
            const auto height = (float) std::max (1, getBandArea (0).getHeight());
            dragged->setValue (constrained (*dragged, startValue - (float) e.getDistanceFromDragStartY() / height * gainRange));
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragged != nullptr)
            dragged->endGesture();

        dragged = nullptr;
    }

    // A double-click on a column puts its band back at 0 dB.
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const auto band = findBand (e.position); band >= 0)
            gains[(size_t) band].setValue (0.0f);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (const auto band = findBand (e.position); band >= 0)
        {
            auto& gain = gains[(size_t) band];
            wheelGesture.turn (gain, constrained (gain, gain.getValue() + (wheel.deltaY > 0.0f ? wheelStep : -wheelStep)));
            repaint();
        }
    }

private:
    DeviceHandle device;
    std::array<ParameterHandle, Multiband::numBands> gains;
    std::array<std::unique_ptr<Knob>, Multiband::numBands> gainKnobs;
    std::array<std::unique_ptr<ParameterAttachment>, Multiband::numBands> gainAttachments;
    std::array<ParameterHandle, 2> crossovers;
    static constexpr float silentLevel = -120.0f;

    graph::SpectrumTrace spectrum;
    std::array<float, Multiband::numBands> shownLevels { silentLevel, silentLevel, silentLevel }, shownChanges {};
    DrawnNumbers drawn;
    ParameterHandle* dragged = nullptr;
    float startValue = 0.0f;
    graph::WheelGesture wheelGesture; // after the handles it turns

    bool isCrossover (const ParameterHandle& handle) const { return &handle == &crossovers[0] || &handle == &crossovers[1]; }

    static float constrained (const ParameterHandle& handle, float value)
    {
        const auto range = handle.getRange();
        return range.snapToLegalValue (juce::jlimit (range.start, range.end, value));
    }

    int findBand (juce::Point<float> position) const
    {
        for (int band = 0; band < Multiband::numBands; ++band)
            if (getBandArea (band).toFloat().contains (position))
                return band;

        return -1;
    }

    void takeMeters (Multiband& multiband, int band)
    {
        multiband.getBandLevel (band).take();
        multiband.getBandCut (band).take();
        multiband.getBandLift (band).take();
    }

    void tick() override
    {
        for (auto& attachment : gainAttachments)
            attachment->update();

        auto spectrumMoved = false;

        if (auto* scope = device.getScope())
        {
            scope->setListening (true);

            if (auto* output = scope->getOutput())
                spectrum.pull (*output);

            spectrumMoved = spectrum.update();
        }

        // Each band's level and how far it is pushed: up at once, falling back smoothly, down to silence.
        if (auto* multiband = device.getProcessor<Multiband>())
        {
            for (size_t band = 0; band < Multiband::numBands; ++band)
            {
                const auto level = multiband->getBandLevel ((int) band).take();
                const auto cut = multiband->getBandCut ((int) band).take(), lift = multiband->getBandLift ((int) band).take();
                shownLevels[band] = std::max ({ level, shownLevels[band] - levelFall, silentLevel });

                const auto change = lift > cut ? lift : -cut;
                shownChanges[band] = std::abs (change) >= std::abs (shownChanges[band]) - levelFall
                                         ? change
                                         : shownChanges[band] - std::copysign (levelFall, shownChanges[band]);
            }
        }

        std::vector<float> numbers (shownLevels.begin(), shownLevels.end());
        numbers.insert (numbers.end(), shownChanges.begin(), shownChanges.end());

        for (const auto& gain : gains)
            numbers.push_back (gain.getValue());

        for (const auto& crossover : crossovers)
            numbers.push_back (crossover.getValue());

        if (drawn.changed (std::move (numbers)) || spectrumMoved)
            repaint();
    }

    void paintSpectrum (juce::Graphics& g, const Palette& p, juce::Colour tint)
    {
        const auto area = getSpectrumArea();
        graph::paintBackground (g, p, area);
        const auto axis = getFrequencyAxis();
        spectrum.paint (g, axis, { area, -90.0f, 6.0f }, tint.withAlpha (GraphTint::spectrumFill),
                        tint.withAlpha (GraphTint::spectrumLine));

        // Where the bands split.
        for (int crossover = 0; crossover < 2; ++crossover)
        {
            const auto x = (float) getCrossoverX (crossover);
            g.setColour (tint);
            g.drawLine (x, area.getY(), x, area.getBottom(), 2.0f);
            g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ x, area.getCentreY() }));
            graph::paintReadout (g, p, area.withX (x).withWidth (64.0f), graph::formatFrequency (crossovers[(size_t) crossover].getValue()),
                                 juce::Justification::topLeft);
        }
    }

    void paintBand (juce::Graphics& g, const Palette& p, juce::Colour tint, int band)
    {
        const auto column = getBandArea (band).toFloat();
        graph::paintBackground (g, p, column);
        auto meter =
            column.reduced ((float) Space::xs).withTrimmedTop ((float) TextSize::small).withTrimmedBottom ((float) TextSize::small);
        const graph::LevelAxis levels { meter, lowestLevel, 0.0f };

        // The zones: turned down above, brought up below.
        const auto downY = levels.yFor (Multiband::downwardThreshold), upY = levels.yFor (Multiband::upwardThreshold);
        g.setColour (p.danger.withAlpha (GraphTint::partCurve * 0.5f));
        g.fillRect (meter.withBottom (downY));
        g.setColour (p.boost.withAlpha (GraphTint::partCurve * 0.5f));
        g.fillRect (meter.withTop (upY));
        g.setColour (p.danger.withAlpha (GraphTint::marker));
        g.drawHorizontalLine (juce::roundToInt (downY), meter.getX(), meter.getRight());
        g.setColour (p.boost.withAlpha (GraphTint::marker));
        g.drawHorizontalLine (juce::roundToInt (upY), meter.getX(), meter.getRight());

        // Coming in (thin), then going out (wide), after the push and the band's gain.
        const auto level = shownLevels[(size_t) band];
        const auto change = level > lowestLevel ? shownChanges[(size_t) band] : 0.0f;
        const auto out = level + change + gains[(size_t) band].getValue();
        const auto inBar = meter.withWidth (6.0f).withX (meter.getCentreX() - meter.getWidth() * 0.25f);
        const auto outBar = meter.withWidth (meter.getWidth() * 0.3f).withX (meter.getCentreX() + 2.0f);

        if (level > lowestLevel)
        {
            g.setColour (p.textMuted);
            g.fillRect (inBar.withTop (levels.yFor (std::min (level, 0.0f))));
        }

        if (out > lowestLevel)
        {
            g.setColour (tint);
            g.fillRect (outBar.withTop (levels.yFor (std::min (out, 0.0f))));
        }

        // How far it is pushed: an arrow from where it comes in, down in red or up in green.
        if (std::abs (change) >= 0.1f)
        {
            const auto fromY = levels.yFor (std::min (level, 0.0f));
            const auto toY = levels.yFor (juce::jlimit (lowestLevel, 0.0f, level + change));
            const auto x = inBar.getRight() + (outBar.getX() - inBar.getRight()) * 0.5f;
            juce::Path arrow;
            arrow.addArrow ({ x, fromY, x, toY }, 2.0f, 8.0f, std::min (8.0f, std::abs (toY - fromY)));
            g.setColour (change < 0.0f ? p.danger : p.boost);
            g.fillPath (arrow);
            graph::paintReadout (g, p, column.withTrimmedTop ((float) TextSize::small + 2.0f), graph::formatDecibels (change),
                                 juce::Justification::topRight);
        }

        // Its name, and its gain over its knob.
        g.setColour (p.text);
        g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold));
        g.drawText (bandNames[band], column.reduced ((float) Space::xs, 1.0f).removeFromTop ((float) TextSize::small),
                    juce::Justification::centredLeft, false);
        graph::paintReadout (g, p, column.withTop (column.getBottom() - (float) TextSize::small - 2.0f * Space::xs),
                             graph::formatDecibels (gains[(size_t) band].getValue()), juce::Justification::centredBottom);
    }
};

//==============================================================================
MultibandEditor::MultibandEditor (const DeviceHandle& handle)
    : DeviceEditorBase (handle)
{
    auto& shown = keep (std::make_unique<Display> (device));
    display = &shown;

    for (int band = 0; band < Multiband::numBands; ++band)
        addControl (gainIDs[(size_t) band], shown.getGainKnob (band));

    // The bands' display, then Depth and Time with how much of each squeeze, then where the bands split and the levels.
    const auto amount = makeControlsPart ("Amount", { "depth", "time" }, 2, ParameterControls::Size::large);
    const auto squeeze = makeControlsPart ("Squeeze", { "upward", "downward" }, 2);
    const auto split = makeControlsPart ("Split and Level", { "lowFrequency", "highFrequency", "inputGain", "outputGain" }, 2,
                                         ParameterControls::Size::compact, { "Low Split", "High Split", "In", "Out" });
    const auto bandsWidth = 3 * (ParameterControls::getCellWidth (ParameterControls::Size::compact) + Space::l);

    setParts ({ { { "Bands", &shown, bandsWidth, 196, 120, true } }, { amount, squeeze }, { split } });
}

MultibandEditor::~MultibandEditor() = default;

juce::Component& MultibandEditor::getDisplay() const noexcept
{
    return *display;
}

float MultibandEditor::getShownLevel (int band) const
{
    return display->getShownLevel (band);
}

float MultibandEditor::getShownChange (int band) const
{
    return display->getShownChange (band);
}

juce::Rectangle<int> MultibandEditor::getBandArea (int band) const
{
    return display->getBandArea (band);
}

int MultibandEditor::getCrossoverX (int crossover) const
{
    return display->getCrossoverX (crossover);
}
} // namespace phazite::ui
