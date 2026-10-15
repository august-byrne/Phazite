// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ReverbEditor.h"
#include "devices/reverb/Reverb.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
namespace
{
    using devices::Reverb;

    constexpr double graphSampleRate = 48000.0;               // the fades are drawn as they play at this rate
    constexpr double bassHertz = 200.0, trebleHertz = 8000.0; // the frequencies whose fades are drawn
    constexpr float floorDecibels = -60.0f;                   // the graph's bottom: a fade's end
    constexpr int refreshRateHz = 10;

    const auto labelHeight = juce::roundToInt (TextSize::small) + Space::xs;

    juce::String toString (std::string_view text)
    {
        return juce::String (text.data(), text.size());
    }

    juce::Label& setUp (juce::Label& label)
    {
        label.setJustificationType (juce::Justification::centred);
        label.setFont (juce::FontOptions (TextSize::small));
        return label;
    }
} // namespace

//==============================================================================
class ReverbEditor::Graph : public juce::Component
{
public:
    explicit Graph (ReverbEditor& e)
        : editor (e)
    {
        setTitle ("Reverb decay");
        setDescription ("How the tail fades after the pre-delay: the bass's fade, and the treble's, which the damping shortens");
    }

    float xFor (double seconds) const { return (float) (seconds / editor.getShownSeconds() * getWidth()); }
    float yFor (float decibels) const { return decibels / floorDecibels * (float) getHeight(); }

    void paint (juce::Graphics& g) override
    {
        const auto& palette = getPalette (*this);
        const auto width = (float) getWidth(), height = (float) getHeight();
        g.setColour (palette.background);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);

        // A line every 20 dB, and every second (or tenth of one, for short tails), labelled.
        g.setFont (juce::FontOptions (TextSize::small));
        g.setColour (palette.text.withAlpha (GraphTint::grid));

        for (const auto level : { -20.0f, -40.0f })
            g.drawHorizontalLine (juce::roundToInt (yFor (level)), 0.0f, width);

        const auto shown = editor.getShownSeconds();
        const auto step = shown > 3.0 ? 1.0 : (shown > 1.0 ? 0.5 : 0.1);

        for (auto time = step; time < shown; time += step)
        {
            const auto x = xFor (time);
            g.setColour (palette.text.withAlpha (GraphTint::grid));
            g.drawVerticalLine (juce::roundToInt (x), 0.0f, height);
            g.setColour (palette.textMuted);
            g.drawText (juce::String (time, step < 1.0 ? 1 : 0) + " s",
                        juce::Rectangle<float> (x + Space::xs, height - (float) labelHeight, (float) knobSize, (float) labelHeight),
                        juce::Justification::centredLeft);
        }

        // The fades: from full level at the end of the pre-delay down to -60 dB, the bass's filled.
        const auto& settings = editor.values;
        const auto start = (double) settings[Reverb::predelay] * 0.001;
        const auto bassEnd = start + Reverb::getDecayAt (settings, bassHertz, graphSampleRate);
        const auto trebleEnd = start + Reverb::getDecayAt (settings, trebleHertz, graphSampleRate);

        juce::Path bass;
        bass.startNewSubPath (xFor (start), height);
        bass.lineTo (xFor (start), 0.0f);
        bass.lineTo (xFor (bassEnd), height);
        bass.closeSubPath();
        g.setColour (palette.accent.withAlpha (GraphTint::spectrumFill));
        g.fillPath (bass);
        g.setColour (palette.text);
        g.drawLine ({ xFor (start), 0.0f, xFor (bassEnd), height }, GraphTint::curveWidth);
        g.setColour (palette.accent);
        g.drawLine ({ xFor (start), 0.0f, xFor (trebleEnd), height }, GraphTint::curveWidth);
        // Which line is which, in their colours.
        auto legend = getLocalBounds().toFloat().reduced ((float) Space::xs).removeFromTop ((float) labelHeight);
        g.setColour (palette.accent);
        g.drawText ("Treble", legend.removeFromRight ((float) knobSize), juce::Justification::centredRight);
        g.setColour (palette.text);
        g.drawText ("Bass", legend.removeFromRight ((float) knobSize), juce::Justification::centredRight);
    }

private:
    ReverbEditor& editor;
};

//==============================================================================
ReverbEditor::ReverbEditor (const DeviceHandle& handle)
    : device (handle)
{
    graph = std::make_unique<Graph> (*this);
    addAndMakeVisible (*graph);

    for (const auto& parameter : Reverb::info.parameters)
    {
        parameters.push_back (device.getParameter (parameter.id));
        values.push_back (parameter.defaultValue);

        const auto name = toString (parameter.name);
        auto* knob = knobs.add (new Knob (name, parameter.defaultValue));
        showAsParameter (*knob, parameter);
        attachments.add (new ParameterAttachment (*knob, {}))->setParameter (device.getParameter (parameter.id));
        addAndMakeVisible (knob);
        addAndMakeVisible (setUp (*names.add (new juce::Label ({}, name))));
        addAndMakeVisible (setUp (*texts.add (new juce::Label())));
        texts.getLast()->setColour (juce::Label::textColourId, getPalette (*this).textMuted);
    }

    setSize (2 * Space::l + knobs.size() * (knobSize + Space::l),
             2 * Space::l + GraphTint::height / 2 + Space::m + knobSize + 2 * labelHeight);
    refresh();
    startTimerHz (refreshRateHz);
}

ReverbEditor::~ReverbEditor()
{
    stopTimer();
}

juce::Component& ReverbEditor::getGraph()
{
    return *graph;
}

double ReverbEditor::getShownSeconds() const
{
    const auto end = (double) values[Reverb::predelay] * 0.001 + Reverb::getDecayAt (values, bassHertz, graphSampleRate);
    return std::max (1.0, 1.1 * end);
}

void ReverbEditor::refresh()
{
    for (size_t i = 0; i < parameters.size(); ++i)
        if (parameters[i].isValid())
            values[i] = parameters[i].getValue();

    for (int i = 0; i < knobs.size(); ++i)
    {
        attachments[i]->update();
        texts[i]->setText (knobs[i]->getTextFromValue (knobs[i]->getValue()), juce::dontSendNotification);
    }

    graph->repaint();
}

void ReverbEditor::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).surface);
}

void ReverbEditor::resized()
{
    auto area = getLocalBounds().reduced (Space::l);
    graph->setBounds (area.removeFromTop (GraphTint::height / 2));
    area.removeFromTop (Space::m);

    for (int i = 0; i < knobs.size(); ++i)
    {
        auto cell = area.removeFromLeft (knobSize + Space::l);
        knobs[i]->setBounds (cell.removeFromTop (knobSize).withSizeKeepingCentre (knobSize, knobSize));
        names[i]->setBounds (cell.removeFromTop (labelHeight));
        texts[i]->setBounds (cell.removeFromTop (labelHeight));
    }
}
} // namespace phazite::ui
