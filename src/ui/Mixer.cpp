// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Mixer.h"
#include "CommandUI.h"
#include "LookAndFeel.h"
#include "model/Tracks.h"

namespace phazite::ui
{
namespace
{
    constexpr int toolbarHeight = 36;

    bool isTrackState (const juce::ValueTree& state)
    {
        return state.hasType (te::IDs::TRACK) || state.hasType (te::IDs::FOLDERTRACK);
    }
} // namespace

Mixer::Mixer (te::Engine& e, CommandRegistry& registry, te::SelectionManager& selectionManager)
    : engine (e),
      commands (registry),
      selection (selectionManager),
      master (e, registry)
{
    setTitle ("Mixer");
    setDescription ("A channel for every track, in the playlist's order. Click a channel's name to select its track.");

    viewport.setViewedComponent (&stripHolder, false);
    viewport.setScrollBarsShown (false, true);
    addReturnButton.onClick = [this] { commands.invoke ("track.addReturn"); };
    addReturnButton.setEnabled (false);

    for (auto* c : std::initializer_list<juce::Component*> { &viewport, &master, &addReturnButton })
        addAndMakeVisible (c);

    selection.addChangeListener (this);
}

Mixer::~Mixer()
{
    selection.removeChangeListener (this);
    editState.removeListener (this);
    strips.clear(); // before stripHolder, which holds them
}

void Mixer::setEdit (te::Edit* newEdit)
{
    if (newEdit == edit.get() && editState.isValid() == (newEdit != nullptr))
        return;

    editState.removeListener (this);
    edit = newEdit;
    editState = newEdit != nullptr ? newEdit->state : juce::ValueTree();
    editState.addListener (this);
    cancelPendingUpdate();

    strips.clear();
    master.setEdit (newEdit);
    addReturnButton.setEnabled (newEdit != nullptr);
    addReturnButton.setTooltip (
        withShortcut (commands, "Adds a return track, for an effect such as a reverb that tracks send to", "track.addReturn"));
    rebuild();
}

void Mixer::rebuild()
{
    auto* e = edit.get();
    const auto tracks = e != nullptr ? getPlaylistTracks (*e) : juce::Array<te::Track*>();

    // The strips of tracks that are still there are kept, in the tracks' new order.
    juce::OwnedArray<ChannelStrip> kept;

    for (auto* track : tracks)
    {
        ChannelStrip* strip = nullptr;

        for (auto* existing : strips)
            if (existing->getTrack() == track)
                strip = existing;

        if (strip != nullptr)
        {
            strips.removeObject (strip, false);
        }
        else
        {
            strip = new ChannelStrip (engine, commands, selection);
            strip->setTrack (track);
            stripHolder.addAndMakeVisible (strip);
        }

        kept.add (strip);
    }

    strips.swapWith (kept); // kept now holds the strips of deleted tracks, and deletes them
    updateSelection();
    resized();
}

void Mixer::updateSelection()
{
    for (auto* strip : strips)
        strip->setSelected (strip->getTrack() != nullptr && selection.isSelected (strip->getTrack()));
}

void Mixer::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.background);
    g.setColour (p.border);
    g.fillRect (master.getX() - 1, 0, 1, getHeight());

    if (strips.isEmpty() && edit.get() != nullptr)
    {
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::body));
        g.drawText ("No tracks yet: add one with Track > Add Track.", viewport.getBounds(), juce::Justification::centred);
    }
}

void Mixer::resized()
{
    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop (toolbarHeight).reduced (Space::s, Space::xs);
    addReturnButton.setBounds (toolbar.removeFromLeft (96));

    master.setBounds (area.removeFromRight (MasterStrip::width));
    viewport.setBounds (area);

    // The strips fill the height, leaving room for the scroll bar along the bottom.
    const auto height = std::max (200, area.getHeight() - viewport.getScrollBarThickness());
    stripHolder.setSize (std::max (1, strips.size() * ChannelStrip::width), height);

    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (i * ChannelStrip::width, 0, ChannelStrip::width, height);
}

void Mixer::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (isTrackState (child))
        triggerAsyncUpdate();
}

void Mixer::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (isTrackState (child))
        triggerAsyncUpdate();
}

void Mixer::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == editState || isTrackState (parent))
        triggerAsyncUpdate();
}
} // namespace phazite::ui
