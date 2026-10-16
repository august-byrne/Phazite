// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Inspector.h"
#include "model/arrangement/Tracks.h"
#include "model/engine/LiveInput.h"
#include "model/project/ProjectContext.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
Inspector::Inspector (te::Engine& engine, CommandBridge& bridge, ProjectContext& context)
    : projectContext (context),
      selection (context.getSelection()),
      strip (engine, bridge, context)
{
    setTitle ("Inspector");
    setDescription ("The channel of the selected track.");
    stripView.setViewedComponent (&strip, false);
    stripView.setScrollBarsShown (true, false);
    addAndMakeVisible (stripView);
    selection.addChangeListener (this);
    startTimerHz (2);
}

Inspector::~Inspector()
{
    selection.removeChangeListener (this);
}

void Inspector::showTrack()
{
    te::Track* track = nullptr;

    if (auto* edit = projectContext.getEdit())
    {
        track = getSelectedTracks (selection).getFirst();

        if (track == nullptr)
            track = getSelectedAudioTrack (selection);

        if (track == nullptr)
            track = getLiveMidiTrack (*edit);

        if (track == nullptr)
            track = getPlaylistTracks (*edit).getFirst();
    }

    if (strip.getTrack() != track)
        strip.setTrack (track);
}

void Inspector::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);

    g.setColour (p.text);
    g.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));
    g.drawText (getTitle(), getLocalBounds().reduced (Space::m).removeFromTop (24), juce::Justification::centredLeft);
}

void Inspector::resized()
{
    // The strip fills the panel, but is no shorter than its parts need (then it scrolls) and no narrower than the
    // mixer's strips.
    auto area = getLocalBounds().reduced (Space::m);
    area.removeFromTop (24 + Space::s);
    const auto scrolls = area.getHeight() < ChannelStrip::getMinimumHeight();
    stripView.setBounds (area);
    strip.setSize (std::max (ChannelStrip::width, area.getWidth() - (scrolls ? stripView.getScrollBarThickness() : 0)),
                   std::max (area.getHeight(), ChannelStrip::getMinimumHeight()));
}
} // namespace phazite::ui
