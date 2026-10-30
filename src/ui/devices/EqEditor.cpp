// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EqEditor.h"
#include "DeviceTint.h"
#include "devices/eq/Equalizer.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>
#include <iterator>

namespace phazite::ui
{
namespace
{
    using Eq = devices::Equalizer;

    constexpr double lowestFrequency = 20.0, highestFrequency = 20000.0;
    constexpr double gainRange = 24.0;      // the graph shows the bands' whole range, -24 to +24 dB
    constexpr double analyzerFloor = -90.0; // the analyzer's bottom, in dB; its top is 0 dB
    constexpr float analyzerFall = 3.0f;    // dB per refresh: 90 dB a second
    constexpr double defaultSampleRate = 48000.0;
    constexpr int bandsGroup = 1;   // the band buttons' radio group
    constexpr int graphWidth = 400; // as wide at any height
    constexpr int bandRows = 2;     // of band buttons

    const auto labelHeight = juce::roundToInt (TextSize::small) + Space::xs;
    const auto knobCell = compactKnobSize + Space::l;

    // Beside the graph: the band buttons in two rows, the band's switch and shape, its slope, then its knobs and the output's.
    int getControlsWidth()
    {
        return EqEditor::numKnobs * knobCell;
    }

    int getControlsHeight()
    {
        return bandRows * controlHeight + Space::xs + Space::s + 2 * controlHeight + Space::xs + Space::s + compactKnobSize
               + 2 * labelHeight;
    }

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    const devices::ParameterInfo& getInfo (int index)
    {
        return Eq::info.parameters[(size_t) index];
    }

    int indexOf (int band, Eq::Field field)
    {
        return Eq::getParameterIndex (band, field);
    }

    bool hasGain (int type)
    {
        return type == (int) Eq::BandType::lowShelf || type == (int) Eq::BandType::bell || type == (int) Eq::BandType::highShelf;
    }

    bool isCut (int type)
    {
        return type == (int) Eq::BandType::lowCut || type == (int) Eq::BandType::highCut;
    }

    juce::Colour getBandColour (int band)
    {
        return juce::Colour (partColours[(size_t) band % std::size (partColours)]);
    }

    double toDecibels (double magnitude)
    {
        return 20.0 * std::log10 (std::max (magnitude, 1.0e-6));
    }
} // namespace

//==============================================================================
class EqEditor::Graph : public juce::Component
{
public:
    explicit Graph (EqEditor& e)
        : editor (e)
    {
        setTitle ("EQ curve");
        setDescription ("Drag a band's dot to move its frequency and gain; the wheel changes its Q, a double-click turns it on or "
                        "off, and a right-click picks its shape");
    }

    float xFor (double frequency) const
    {
        return (float) (std::log (frequency / lowestFrequency) / std::log (highestFrequency / lowestFrequency) * getWidth());
    }

    double frequencyFor (float x) const
    {
        return lowestFrequency * std::pow (highestFrequency / lowestFrequency, juce::jlimit (0.0, 1.0, (double) x / getWidth()));
    }

    float yForGain (double decibels) const { return (float) ((0.5 - decibels / (2.0 * gainRange)) * getHeight()); }
    double gainFor (float y) const { return (0.5 - (double) y / getHeight()) * 2.0 * gainRange; }
    float yForLevel (double decibels) const { return (float) (decibels / analyzerFloor * getHeight()); }

    // A shelf's or bell's dot is at its gain; a cut's or notch's on the line at 0 dB.
    juce::Point<float> getDot (int band) const
    {
        const auto& shown = editor.values;
        const auto type = juce::roundToInt (shown[(size_t) indexOf (band, Eq::type)]);
        const auto gain = hasGain (type) ? (double) shown[(size_t) indexOf (band, Eq::gain)] : 0.0;
        return { xFor (shown[(size_t) indexOf (band, Eq::frequency)]), yForGain (juce::jlimit (-gainRange, gainRange, gain)) };
    }

    // The nearest dot within reach, or -1.
    int findBandAt (juce::Point<float> position) const
    {
        auto found = -1;
        auto nearest = GraphTint::dotRadius * 1.5f;

        for (int band = 0; band < Eq::numBands; ++band)
        {
            if (const auto distance = getDot (band).getDistanceFrom (position); distance <= nearest)
            {
                nearest = distance;
                found = band;
            }
        }

        return found;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& palette = getPalette (*this);
        const auto width = (float) getWidth(), height = (float) getHeight();
        g.setColour (palette.background);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);

        paintGrid (g, palette);

        // What the EQ plays, behind its curve.
        juce::Path played;
        played.startNewSubPath (0.0f, height);

        for (auto x = 0.0f; x <= width; x += 2.0f)
            played.lineTo (x, yForLevel (juce::jlimit (analyzerFloor, 0.0, (double) editor.spectrum.getLevelAt (frequencyFor (x)))));

        played.lineTo (width, height);
        played.closeSubPath();

        // What comes in, faint, then what it plays, in the EQ's colour.
        const graph::FrequencyAxis frequencies { getLocalBounds().toFloat(), lowestFrequency, highestFrequency };
        editor.input.paint (g, frequencies, { getLocalBounds().toFloat(), (float) analyzerFloor, 0.0f },
                            palette.textMuted.withAlpha (GraphTint::spectrumFill),
                            palette.textMuted.withAlpha (GraphTint::spectrumLine * 0.5f));
        g.setColour (getDeviceTint (*this).withAlpha (GraphTint::spectrumFill * 1.5f));
        g.fillPath (played);

        // The selected band's own curve, filled to 0 dB, and the curve of every band together.
        const auto sampleRate = editor.getSampleRate();
        const auto zero = yForGain (0.0);
        juce::Path band, curve;
        band.startNewSubPath (0.0f, zero);

        for (auto x = 0.0f; x <= width; x += 1.5f)
        {
            const auto frequency = frequencyFor (x);
            const auto own = toDecibels (Eq::getBandMagnitude (editor.values, editor.selectedBand, frequency, sampleRate));
            const auto total = toDecibels (Eq::getMagnitude (editor.values, frequency, sampleRate));
            band.lineTo (x, yForGain (juce::jlimit (-gainRange, gainRange, own)));

            if (x <= 0.0f)
                curve.startNewSubPath (x, yForGain (juce::jlimit (-gainRange, gainRange, total)));
            else
                curve.lineTo (x, yForGain (juce::jlimit (-gainRange, gainRange, total)));
        }

        band.lineTo (width, zero);
        band.closeSubPath();
        g.setColour (getBandColour (editor.selectedBand).withAlpha (GraphTint::partCurve));
        g.fillPath (band);
        g.setColour (palette.text);
        g.strokePath (curve, juce::PathStrokeType (GraphTint::curveWidth));

        paintDots (g, palette);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto band = findBandAt (e.position);

        if (band < 0)
            return;

        editor.selectBand (band);

        if (e.mods.isPopupMenu())
        {
            showMenu (band);
            return;
        }

        // Its frequency and, for a shelf or a bell, its gain: one gesture, one undo step.
        dragFrequency = editor.device.getParameter (getInfo (indexOf (band, Eq::frequency)).id, "Move Band " + juce::String (band + 1));
        dragGain = editor.device.getParameter (getInfo (indexOf (band, Eq::gain)).id);
        dragFrequency.beginGesture();

        if (hasGain (juce::roundToInt (editor.values[(size_t) indexOf (band, Eq::type)])))
            dragGain.beginGesture();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragFrequency.isInGesture())
            return;

        dragFrequency.setValue ((float) juce::jlimit (lowestFrequency, highestFrequency, frequencyFor (e.position.x)));

        if (dragGain.isInGesture())
            dragGain.setValue ((float) juce::jlimit (-gainRange, gainRange, gainFor (e.position.y)));

        editor.refresh();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragGain.endGesture();
        dragFrequency.endGesture();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (const auto band = findBandAt (e.position); band >= 0)
        {
            auto on = editor.device.getParameter (getInfo (indexOf (band, Eq::on)).id);
            on.setValue (on.getValue() > 0.5f ? 0.0f : 1.0f);
            editor.refresh();
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        auto band = findBandAt (e.position);

        if (band < 0)
            band = editor.selectedBand;

        const auto& info = getInfo (indexOf (band, Eq::q));
        auto q = editor.device.getParameter (info.id);
        q.setValue (devices::constrain (info, q.getValue() * std::pow (2.0f, 2.0f * wheel.deltaY)));
        editor.refresh();
    }

private:
    EqEditor& editor;
    ParameterHandle dragFrequency, dragGain;

    void paintGrid (juce::Graphics& g, const Palette& palette)
    {
        const auto width = (float) getWidth(), height = (float) getHeight();
        g.setFont (juce::FontOptions (TextSize::small));

        for (const auto frequency : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const auto x = xFor (frequency);
            g.setColour (palette.text.withAlpha (GraphTint::grid));
            g.drawVerticalLine (juce::roundToInt (x), 0.0f, height);
            g.setColour (palette.textMuted);
            g.drawText (frequency >= 1000.0 ? juce::String ((int) (frequency / 1000.0)) + "k" : juce::String ((int) frequency),
                        juce::Rectangle<float> (x + Space::xs, height - (float) labelHeight, (float) knobSize, (float) labelHeight),
                        juce::Justification::centredLeft);
        }

        for (const auto gain : { -18.0, -12.0, -6.0, 6.0, 12.0, 18.0 })
        {
            const auto y = yForGain (gain);
            g.setColour (palette.text.withAlpha (GraphTint::grid));
            g.drawHorizontalLine (juce::roundToInt (y), 0.0f, width);
            g.setColour (palette.textMuted);
            g.drawText ((gain > 0.0 ? "+" : "") + juce::String ((int) gain),
                        juce::Rectangle<float> ((float) Space::xs, y - (float) labelHeight, (float) knobSize, (float) labelHeight),
                        juce::Justification::centredLeft);
        }

        g.setColour (palette.text.withAlpha (GraphTint::zeroLine));
        g.drawHorizontalLine (juce::roundToInt (yForGain (0.0)), 0.0f, width);
    }

    // Each band's dot, filled while it is on, with its number, and the selected one outlined in the accent colour.
    void paintDots (juce::Graphics& g, const Palette& palette)
    {
        g.setFont (juce::FontOptions (TextSize::small));

        for (int band = 0; band < Eq::numBands; ++band)
        {
            const auto on = editor.values[(size_t) indexOf (band, Eq::on)] > 0.5f;
            const auto colour = getBandColour (band);
            const auto dot = juce::Rectangle<float> (2.0f * GraphTint::dotRadius, 2.0f * GraphTint::dotRadius).withCentre (getDot (band));

            if (on)
            {
                g.setColour (colour);
                g.fillEllipse (dot);
            }

            const auto selected = band == editor.selectedBand;
            g.setColour (selected ? palette.accent : (on ? palette.text : palette.textMuted));
            g.drawEllipse (dot, selected ? 2.0f : 1.0f);
            g.setColour (on ? getInkFor (colour) : palette.textMuted);
            g.drawText (juce::String (band + 1), dot, juce::Justification::centred);
        }
    }

    void showMenu (int band)
    {
        const auto type = juce::roundToInt (editor.values[(size_t) indexOf (band, Eq::type)]);
        const auto slope = juce::roundToInt (editor.values[(size_t) indexOf (band, Eq::slope)]);
        const auto on = editor.values[(size_t) indexOf (band, Eq::on)] > 0.5f;
        const auto& types = getInfo (indexOf (band, Eq::type)).choices;
        const auto& slopes = getInfo (indexOf (band, Eq::slope)).choices;
        constexpr int slopeItems = 100, onItem = 200;

        juce::PopupMenu menu;
        menu.addSectionHeader ("Band " + juce::String (band + 1));

        for (size_t i = 0; i < types.size(); ++i)
            menu.addItem ((int) i + 1, toString (types[i]), true, (int) i == type);

        if (isCut (type))
        {
            menu.addSeparator();

            for (size_t i = 0; i < slopes.size(); ++i)
                menu.addItem (slopeItems + (int) i, toString (slopes[i]), true, (int) i == slope);
        }

        menu.addSeparator();
        menu.addItem (onItem, on ? "Turn Off" : "Turn On");

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safe = juce::Component::SafePointer<Graph> (this), band, on] (int result)
                            {
                                if (safe == nullptr || result == 0)
                                    return;

                                auto& owner = safe->editor;
                                const auto set = [&owner, band] (Eq::Field field, float value)
                                {
                                    auto parameter = owner.device.getParameter (getInfo (indexOf (band, field)).id);
                                    parameter.setValue (value);
                                };

                                if (result == onItem)
                                    set (Eq::on, on ? 0.0f : 1.0f);
                                else if (result >= slopeItems)
                                    set (Eq::slope, (float) (result - slopeItems));
                                else
                                    set (Eq::type, (float) (result - 1));

                                owner.refresh();
                            });
    }
};

//==============================================================================
EqEditor::EqEditor (const DeviceHandle& handle)
    : DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      device (handle),
      pulled (8192)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    for (const auto& parameter : Eq::info.parameters)
    {
        parameters.push_back (device.getParameter (parameter.id));
        values.push_back (parameter.defaultValue);
    }

    graph = std::make_unique<Graph> (*this);
    addAndMakeVisible (*graph);

    for (int band = 0; band < Eq::numBands; ++band)
    {
        auto* button = bandButtons.add (new juce::TextButton (juce::String (band + 1)));
        button->setTitle ("Band " + juce::String (band + 1));
        button->setTooltip ("Band " + juce::String (band + 1) + ": shows its controls below");
        button->setRadioGroupId (bandsGroup);
        button->setClickingTogglesState (true);
        button->setColour (juce::TextButton::buttonOnColourId, getBandColour (band));
        button->onClick = [this, band] { selectBand (band); };
        addAndMakeVisible (button);
    }

    onButton.setTitle ("Band On");
    typeBox.setTitle ("Band Shape");
    slopeBox.setTitle ("Slope");

    for (auto* control :
         std::initializer_list<juce::Component*> { &onButton, &typeBox, &slopeBox, &frequencyKnob, &gainKnob, &qKnob, &outputKnob })
        addAndMakeVisible (control);

    // Each knob's name, then what it is set to.
    for (const auto* name : { "Frequency", "Gain", "Q", "Output" })
        addLabel (name);

    for (int i = 0; i < numKnobs; ++i)
        addLabel ({}).setColour (juce::Label::textColourId, getPalette (*this).textMuted);

    showAsParameter (outputKnob, getInfo (Eq::outputParameter));
    outputAttachment.setParameter (device.getParameter (getInfo (Eq::outputParameter).id));
    applyDeviceTint (*this, device);

    selectBand (selectedBand);
    setSize (2 * Space::s + graphWidth + Space::m + getControlsWidth(), 2 * Space::s + std::max (GraphTint::height, getControlsHeight()));
    refresh();
}

int EqEditor::getMinimumHeight() const
{
    return 2 * Space::s + std::max (GraphTint::minimumHeight, getControlsHeight());
}

EqEditor::~EqEditor()
{
    if (auto* eq = device.getProcessor<Eq>())
        eq->getOutputTap().setListening (false);

    if (auto* scope = device.getScope())
        scope->setListening (false);
}

juce::Component& EqEditor::getGraph()
{
    return *graph;
}

juce::Point<float> EqEditor::getDotPosition (int band) const
{
    return graph->getDot (band);
}

double EqEditor::getSampleRate() const
{
    if (auto* eq = device.getProcessor<Eq>(); eq != nullptr && eq->getSampleRate() > 0.0)
        return eq->getSampleRate();

    return defaultSampleRate;
}

juce::Label& EqEditor::addLabel (const juce::String& text)
{
    auto* label = labels.add (new juce::Label ({}, text));
    label->setJustificationType (juce::Justification::centred);
    label->setFont (juce::FontOptions (TextSize::small));
    addAndMakeVisible (label);
    return *label;
}

void EqEditor::selectBand (int band)
{
    selectedBand = juce::jlimit (0, Eq::numBands - 1, band);
    bandButtons[selectedBand]->setToggleState (true, juce::dontSendNotification);

    const auto handleOf = [this] (Eq::Field field) { return device.getParameter (getInfo (indexOf (selectedBand, field)).id); };
    onAttachment.setParameter (handleOf (Eq::on));
    typeAttachment.setParameter (handleOf (Eq::type));
    slopeAttachment.setParameter (handleOf (Eq::slope));

    showAsParameter (frequencyKnob, getInfo (indexOf (selectedBand, Eq::frequency)));
    showAsParameter (gainKnob, getInfo (indexOf (selectedBand, Eq::gain)));
    showAsParameter (qKnob, getInfo (indexOf (selectedBand, Eq::q)));
    frequencyAttachment.setParameter (handleOf (Eq::frequency));
    gainAttachment.setParameter (handleOf (Eq::gain));
    qAttachment.setParameter (handleOf (Eq::q));
    refresh();
}

void EqEditor::refresh()
{
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    onAttachment.update();
    typeAttachment.update();
    frequencyAttachment.update();
    qAttachment.update();
    outputAttachment.update();

    // A cut's slope matters, and a shelf's or bell's gain; the other's control is off, switched off once rather than on by its
    // attachment and off again at each tick, which repainted it each time.
    const auto type = juce::roundToInt (values[(size_t) indexOf (selectedBand, Eq::type)]);

    if (isCut (type))
        slopeAttachment.update();
    else
        slopeBox.setEnabled (false);

    if (hasGain (type))
        gainAttachment.update();
    else
        gainKnob.setEnabled (false);

    for (int band = 0; band < Eq::numBands; ++band)
        bandButtons[band]->setAlpha (values[(size_t) indexOf (band, Eq::on)] > 0.5f ? 1.0f : GraphTint::partOff);

    // What each knob is set to, under its name (none for a knob that doesn't apply).
    Knob* const knobs[] = { &frequencyKnob, &gainKnob, &qKnob, &outputKnob };

    for (int i = 0; i < numKnobs; ++i)
        labels[i + numKnobs]->setText (knobs[i]->isEnabled() ? knobs[i]->getTextFromValue (knobs[i]->getValue()) : juce::String(),
                                       juce::dontSendNotification);

    auto spectraMoved = false;

    if (auto* eq = device.getProcessor<Eq>())
    {
        auto& tap = eq->getOutputTap();
        tap.setListening (true);
        spectrum.setSampleRate (getSampleRate());

        for (int n; (n = tap.pull (pulled.data(), (int) pulled.size())) > 0;)
            spectrum.add (pulled.data(), n);

        spectraMoved = spectrum.update (analyzerFall);
    }

    // And, fainter behind it, what comes in.
    if (auto* scope = device.getScope())
    {
        scope->setListening (true);
        input.setSampleRate (getSampleRate());

        if (auto* tap = scope->getInput())
            input.pull (*tap);

        spectraMoved = input.update() || spectraMoved;
    }

    // The curve and its dots over the spectra, repainted when any of them moves.
    auto numbers = values;
    numbers.insert (numbers.end(), { (float) selectedBand, (float) getSampleRate() });

    if (drawnGraph.changed (std::move (numbers)) || spectraMoved)
        graph->repaint();
}

void EqEditor::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void EqEditor::resized()
{
    // The graph on the left, as tall as the editor; the band's controls beside it.
    auto area = getLocalBounds().reduced (Space::s);
    graph->setBounds (area.removeFromLeft (area.getWidth() - getControlsWidth() - Space::m));
    area.removeFromLeft (Space::m);

    // The band buttons, in two rows.
    const auto perRow = Eq::numBands / bandRows;

    for (int row = 0; row < bandRows; ++row)
    {
        auto buttons = area.removeFromTop (controlHeight);
        const auto buttonWidth = (buttons.getWidth() - (perRow - 1) * Space::xs) / perRow;

        for (int i = 0; i < perRow; ++i)
        {
            bandButtons[row * perRow + i]->setBounds (buttons.removeFromLeft (buttonWidth));
            buttons.removeFromLeft (Space::xs);
        }

        if (row + 1 < bandRows)
            area.removeFromTop (Space::xs);
    }

    area.removeFromTop (Space::s);

    // The band's switch and shape, then its slope.
    auto shapes = area.removeFromTop (controlHeight);
    onButton.setBounds (shapes.removeFromLeft (knobCell));
    typeBox.setBounds (shapes);
    area.removeFromTop (Space::xs);
    slopeBox.setBounds (area.removeFromTop (controlHeight).withTrimmedLeft (knobCell));
    area.removeFromTop (Space::s);

    // Its knobs, then the output's, each with its name and value.
    Knob* const knobs[] = { &frequencyKnob, &gainKnob, &qKnob, &outputKnob };

    for (int i = 0; i < numKnobs; ++i)
    {
        auto cell = area.removeFromLeft (knobCell).withHeight (compactKnobSize + 2 * labelHeight);
        knobs[i]->setBounds (cell.removeFromTop (compactKnobSize).withSizeKeepingCentre (compactKnobSize, compactKnobSize));
        labels[i]->setBounds (cell.removeFromTop (labelHeight));
        labels[i + numKnobs]->setBounds (cell.removeFromTop (labelHeight));
    }
}
} // namespace phazite::ui
