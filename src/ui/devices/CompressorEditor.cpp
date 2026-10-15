// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CompressorEditor.h"
#include "devices/dynamics/Compressor.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    using devices::Compressor;

    constexpr float lowestLevel = -60.0f; // the graph's levels, in and out, go from here to 0 dB
    constexpr float meterRange = 24.0f;   // the meter shows the sound turned down by up to this many dB
    constexpr float meterFall = 0.5f;     // dB per refresh: 15 dB a second
    constexpr int refreshRateHz = 30;
    constexpr int columns = 4; // of knobs

    const auto labelHeight = juce::roundToInt (TextSize::small) + Space::xs;

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    const devices::ParameterInfo& getInfo (int index)
    {
        return Compressor::info.parameters[(size_t) index];
    }

    // The name under a knob: the low cut's is short, as the sidechain's row below names it.
    juce::String getShortName (const devices::ParameterInfo& parameter)
    {
        return &parameter == &getInfo (Compressor::sidechainLowCut) ? juce::String ("Low Cut") : toString (parameter.name);
    }
} // namespace

//==============================================================================
class CompressorEditor::Graph : public juce::Component
{
public:
    explicit Graph (CompressorEditor& e)
        : editor (e)
    {
        setTitle ("Compressor curve");
        setDescription ("How loud each level comes out, with a dot at the level the compressor hears, and a meter of how far it "
                        "turns the sound down. Drag across the curve to move the threshold.");
    }

    juce::Rectangle<float> getCurveArea() const
    {
        return getLocalBounds().toFloat().withTrimmedRight ((float) (GraphTint::meterWidth + Space::s));
    }

    juce::Point<float> getPosition (float in, float out) const
    {
        const auto area = getCurveArea();
        return { area.getX() + (in - lowestLevel) / -lowestLevel * area.getWidth(),
                 area.getBottom() - (out - lowestLevel) / -lowestLevel * area.getHeight() };
    }

    float levelFor (float x) const
    {
        const auto area = getCurveArea();
        return lowestLevel + (x - area.getX()) / area.getWidth() * -lowestLevel;
    }

    // How loud a level comes out once the compressor has settled: turned down, then made up.
    float outputFor (float in) const { return in - Compressor::getReduction (editor.values, in) + editor.values[Compressor::makeup]; }

    void paint (juce::Graphics& g) override
    {
        const auto& palette = getPalette (*this);
        const auto area = getCurveArea();
        g.setColour (palette.background);
        g.fillRoundedRectangle (area, cornerRadius);
        paintGrid (g, palette, area);

        // The curve, within the graph.
        juce::Path curve;

        for (auto x = area.getX(); x <= area.getRight(); x += 1.5f)
        {
            const auto point = getPosition (levelFor (x), outputFor (levelFor (x)));
            const auto clipped = point.withY (juce::jlimit (area.getY(), area.getBottom(), point.y));

            if (x <= area.getX())
                curve.startNewSubPath (clipped);
            else
                curve.lineTo (clipped);
        }

        g.setColour (palette.text);
        g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth));

        // The level the compressor hears, riding the curve.
        if (editor.shownLevel > lowestLevel)
        {
            const auto in = std::min (editor.shownLevel, 0.0f);
            const auto dot = getPosition (in, juce::jlimit (lowestLevel, 0.0f, outputFor (in)));
            g.setColour (palette.accent);
            g.fillEllipse (juce::Rectangle<float> (2.0f * GraphTint::levelDotRadius, 2.0f * GraphTint::levelDotRadius).withCentre (dot));
        }

        paintMeter (g, palette);
    }

    // Dragging sideways moves the threshold by as much as the graph's levels move, as one undo step.
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! threshold.isInGesture())
        {
            threshold = editor.device.getParameter (getInfo (Compressor::threshold).id);
            threshold.beginGesture();
            startThreshold = threshold.getValue();
        }

        const auto moved = (float) e.getDistanceFromDragStartX() / getCurveArea().getWidth() * -lowestLevel;
        threshold.setValue (devices::constrain (getInfo (Compressor::threshold), startThreshold + moved));
        editor.refresh();
    }

    void mouseUp (const juce::MouseEvent&) override { threshold.endGesture(); }

private:
    CompressorEditor& editor;
    ParameterHandle threshold;
    float startThreshold = 0.0f;

    void paintGrid (juce::Graphics& g, const Palette& palette, juce::Rectangle<float> area)
    {
        g.setFont (juce::FontOptions (TextSize::small));

        for (auto level = -48.0f; level < 0.0f; level += 12.0f)
        {
            const auto at = getPosition (level, level);
            g.setColour (palette.text.withAlpha (GraphTint::grid));
            g.drawVerticalLine (juce::roundToInt (at.x), area.getY(), area.getBottom());
            g.drawHorizontalLine (juce::roundToInt (at.y), area.getX(), area.getRight());
            g.setColour (palette.textMuted);
            g.drawText (
                juce::String ((int) level),
                juce::Rectangle<float> (at.x + Space::xs, area.getBottom() - (float) labelHeight, (float) knobSize, (float) labelHeight),
                juce::Justification::centredLeft);
        }

        // Level in equals level out, and the threshold.
        g.setColour (palette.text.withAlpha (GraphTint::zeroLine));
        g.drawLine ({ getPosition (lowestLevel, lowestLevel), getPosition (0.0f, 0.0f) });
        g.setColour (palette.accent.withAlpha (GraphTint::marker));
        g.drawVerticalLine (juce::roundToInt (getPosition (editor.values[Compressor::threshold], 0.0f).x), area.getY(), area.getBottom());
    }

    // How far the sound goes down, from the top, with the number of decibels beside it.
    void paintMeter (juce::Graphics& g, const Palette& palette)
    {
        const auto strip = getLocalBounds().toFloat().removeFromRight ((float) GraphTint::meterWidth);
        g.setColour (palette.background);
        g.fillRoundedRectangle (strip, cornerRadius);
        g.setColour (palette.warning);
        g.fillRect (strip.withHeight (strip.getHeight() * juce::jlimit (0.0f, 1.0f, editor.shownReduction / meterRange)));

        auto area = getCurveArea();
        g.setColour (palette.textMuted);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (editor.shownReduction >= 0.05f ? "-" + juce::String (editor.shownReduction, 1) + " dB" : juce::String ("0 dB"),
                    area.removeFromTop ((float) labelHeight).reduced ((float) Space::xs, 0.0f), juce::Justification::centredRight);
    }
};

//==============================================================================
// Lists the tracks the sidechain can listen to as it opens, since tracks come and go, and picks one as an undo step.
class CompressorEditor::SourceBox : public juce::ComboBox
{
public:
    explicit SourceBox (CompressorEditor& e)
        : editor (e)
    {
        setTitle ("Sidechain");
        setTooltip ("The track the compressor listens to, such as a kick that turns a bass down; Off listens to the sound passing "
                    "through it");
        onChange = [this] { pick(); };
        list();
    }

    void showPopup() override
    {
        list();
        juce::ComboBox::showPopup();
    }

    // Shows the source the sidechain has now, once it has changed (with Undo, say).
    void update()
    {
        const auto source = editor.device.getSidechainSource();

        if (source.trackID != shown.trackID || source.name != shown.name)
            list();
    }

private:
    CompressorEditor& editor;
    std::vector<DeviceHandle::SidechainChoice> choices; // the tracks listed, from the box's second item on
    DeviceHandle::SidechainChoice shown;

    void list()
    {
        choices = editor.device.getSidechainChoices();
        shown = editor.device.getSidechainSource();

        // A source that would now make a loop still shows, though it isn't offered.
        if (shown.trackID != 0
            && std::none_of (choices.begin(), choices.end(), [this] (const auto& choice) { return choice.trackID == shown.trackID; }))
            choices.insert (choices.begin(), shown);

        clear (juce::dontSendNotification);
        addItem ("Off", 1);
        auto selected = 1;

        for (size_t i = 0; i < choices.size(); ++i)
        {
            addItem (choices[i].name, (int) i + 2);

            if (choices[i].trackID == shown.trackID)
                selected = (int) i + 2;
        }

        setSelectedId (selected, juce::dontSendNotification);
    }

    void pick()
    {
        const auto index = getSelectedId() - 2;
        editor.device.setSidechainSource (index >= 0 && index < (int) choices.size() ? choices[(size_t) index].trackID : 0);
        list();
    }
};

//==============================================================================
CompressorEditor::CompressorEditor (const DeviceHandle& handle)
    : device (handle),
      shownLevel (lowestLevel)
{
    graph = std::make_unique<Graph> (*this);
    addAndMakeVisible (*graph);

    for (const auto& parameter : Compressor::info.parameters)
    {
        parameters.push_back (device.getParameter (parameter.id));
        values.push_back (parameter.defaultValue);

        const auto name = toString (parameter.name);
        auto* knob = knobs.add (new Knob (name, parameter.defaultValue));
        showAsParameter (*knob, parameter);
        attachments.add (new ParameterAttachment (*knob, {}))->setParameter (device.getParameter (parameter.id));
        addAndMakeVisible (knob);

        for (auto* label : { names.add (new juce::Label ({}, getShortName (parameter))), texts.add (new juce::Label()) })
        {
            label->setJustificationType (juce::Justification::centred);
            label->setFont (juce::FontOptions (TextSize::small));
            addAndMakeVisible (label);
        }

        texts.getLast()->setColour (juce::Label::textColourId, getPalette (*this).textMuted);
    }

    sourceBox = std::make_unique<SourceBox> (*this);
    sourceLabel.setText ("Sidechain", juce::dontSendNotification);
    sourceLabel.setFont (juce::FontOptions (TextSize::small));
    sourceLabel.attachToComponent (sourceBox.get(), true);
    addAndMakeVisible (*sourceBox);

    // The meters' first readings would show what played before the editor opened.
    if (auto* compressor = device.getProcessor<Compressor>())
    {
        compressor->getLevelMeter().take();
        compressor->getReductionMeter().take();
    }

    const auto knobCell = knobSize + 2 * labelHeight;
    const auto controls = 2 * knobCell + Space::m + controlHeight;
    setSize (2 * Space::l + controls + Space::s + GraphTint::meterWidth + Space::xl + columns * (knobSize + Space::l),
             2 * Space::l + controls);
    refresh();
    startTimerHz (refreshRateHz);
}

CompressorEditor::~CompressorEditor()
{
    stopTimer();
}

juce::Component& CompressorEditor::getGraph()
{
    return *graph;
}

juce::ComboBox& CompressorEditor::getSidechainBox()
{
    return *sourceBox;
}

juce::Point<float> CompressorEditor::getGraphPosition (float inputDecibels, float outputDecibels) const
{
    return graph->getPosition (inputDecibels, outputDecibels);
}

void CompressorEditor::refresh()
{
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    for (int i = 0; i < knobs.size(); ++i)
    {
        attachments[i]->update();
        texts[i]->setText (knobs[i]->getTextFromValue (knobs[i]->getValue()), juce::dontSendNotification);
    }

    // The meters jump up and fall back smoothly.
    if (auto* compressor = device.getProcessor<Compressor>())
    {
        shownLevel = std::max (compressor->getLevelMeter().take(), shownLevel - meterFall);
        shownReduction = std::max (compressor->getReductionMeter().take(), shownReduction - meterFall);
    }

    sourceBox->update();
    graph->repaint();
}

void CompressorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void CompressorEditor::resized()
{
    auto area = getLocalBounds().reduced (Space::l);
    graph->setBounds (area.removeFromLeft (area.getHeight() + Space::s + GraphTint::meterWidth));
    area.removeFromLeft (Space::xl);

    // The knobs in two rows, each with its name and value, then the sidechain's source beside its name.
    const auto knobCell = knobSize + 2 * labelHeight;

    for (int i = 0; i < knobs.size(); ++i)
    {
        auto cell = juce::Rectangle<int> (area.getX() + (i % columns) * (knobSize + Space::l), area.getY() + (i / columns) * knobCell,
                                          knobSize + Space::l, knobCell);
        knobs[i]->setBounds (cell.removeFromTop (knobSize).withSizeKeepingCentre (knobSize, knobSize));
        names[i]->setBounds (cell.removeFromTop (labelHeight));
        texts[i]->setBounds (cell.removeFromTop (labelHeight));
    }

    auto row = area.withTrimmedTop (2 * knobCell + Space::m).removeFromTop (controlHeight);
    sourceBox->setBounds (row.withTrimmedLeft (2 * knobSize));
}
} // namespace phazite::ui
