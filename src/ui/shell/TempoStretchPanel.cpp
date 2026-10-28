// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TempoStretchPanel.h"
#include "model/transport/TransportModel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int switchWidth = 96;
    constexpr int headingHeight = 16, footerHeight = 32;
    const juce::String headingText ("Changing the tempo stretches");
    const juce::String footerText ("What isn't stretched keeps its speed. Every clip still starts on the same beat.");

    juce::String describeSamples (const TempoStretch& stretch)
    {
        const auto all = stretch.samples, following = stretch.samplesFollowing;

        if (all == 0)
            return "None in the song";

        if (following == all)
            return all == 1 ? "It follows the tempo" : "All " + juce::String (all) + " follow the tempo";

        if (following == 0)
            return all == 1 ? "It keeps its speed" : "All " + juce::String (all) + " keep their speed";

        return juce::String (following) + " of " + juce::String (all) + " follow the tempo";
    }

    juce::String describePatterns (const TempoStretch& stretch)
    {
        const auto all = stretch.patterns;

        if (all == 0)
            return "None in the song";

        if (stretch.patternsFollow)
            return all == 1 ? "It follows the tempo" : "All " + juce::String (all) + " follow the tempo";

        return all == 1 ? "It keeps its speed" : "All " + juce::String (all) + " keep their speed";
    }
} // namespace

TempoStretchPanel::TempoStretchPanel (CommandBridge& bridge, ProjectContext& c)
    : commands (bridge),
      context (c)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle (headingText);
    setWantsKeyboardFocus (false);

    samples.setTitle ("Samples follow the tempo");
    patterns.setTitle ("Patterns follow the tempo");
    samples.onClick = [this]
    {
        commands.invoke ("transport.samplesFollowTempo");
        update();
    };
    patterns.onClick = [this]
    {
        commands.invoke ("transport.patternsFollowTempo");
        update();
    };

    for (auto* button : { &samples, &patterns })
    {
        button->setMouseClickGrabsKeyboardFocus (false); // a tempo being typed goes on
        addAndMakeVisible (button);
    }

    update();
}

void TempoStretchPanel::update()
{
    const auto stretch = getTempoStretch (context);

    if (shown.has_value() && *shown == stretch)
        return; // it repaints only when what it says changes

    shown = stretch;
    samples.setToggleState (stretch.samples > 0 && stretch.samplesFollowing == stretch.samples, juce::dontSendNotification);
    patterns.setToggleState (stretch.patterns > 0 && stretch.patternsFollow, juce::dontSendNotification);
    samples.setEnabled (stretch.samples > 0);
    patterns.setEnabled (stretch.patterns > 0);

    // What each switch's note says, which screen readers read with it.
    samples.setDescription (describeSamples (stretch));
    patterns.setDescription (describePatterns (stretch));
    repaint();
}

int TempoStretchPanel::getIdealHeight() const
{
    return 2 * Space::m + headingHeight + 2 * Space::xs + 2 * controlHeight + footerHeight;
}

void TempoStretchPanel::paint (juce::Graphics& g)
{
    const auto& palette = getPalette (*this);
    g.fillAll (palette.surfaceRaised);
    g.setColour (palette.border);
    g.drawRect (getLocalBounds(), 1);

    auto area = getLocalBounds().reduced (Space::m);
    g.setFont (juce::FontOptions (TextSize::small));
    g.setColour (palette.textMuted);
    g.drawText (headingText, area.removeFromTop (headingHeight), juce::Justification::centredLeft);
    area.removeFromTop (Space::xs);

    g.setColour (palette.text);

    for (auto* button : { &samples, &patterns })
        g.drawText (button->getDescription(), area.removeFromTop (controlHeight).withTrimmedLeft (switchWidth + Space::s),
                    juce::Justification::centredLeft);

    area.removeFromTop (Space::xs);
    g.setColour (palette.textMuted);
    g.drawFittedText (footerText, area.removeFromTop (footerHeight), juce::Justification::topLeft, 2);
}

void TempoStretchPanel::resized()
{
    auto area = getLocalBounds().reduced (Space::m);
    area.removeFromTop (headingHeight + Space::xs);
    samples.setBounds (area.removeFromTop (controlHeight).withWidth (switchWidth));
    patterns.setBounds (area.removeFromTop (controlHeight).withWidth (switchWidth));
}
} // namespace phazite::ui
