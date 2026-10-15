// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MixerView.h"
#include "model/arrangement/Tracks.h"
#include "model/mixer/Mixer.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int toolbarHeight = 36;

    constexpr int returnsGap = Space::m; // between the tracks' strips and the returns'

    bool isTrackState (const juce::ValueTree& state)
    {
        return state.hasType (te::IDs::TRACK) || state.hasType (te::IDs::FOLDERTRACK);
    }

    // Whether a channel's sound reaches another's: through a send to a return, or by being in a bus folder.
    bool feeds (const te::Track& from, const te::Track& to)
    {
        if (const auto bus = getReturnBus (to); bus >= 0)
            for (auto* send : getSends (from))
                if (send->getBusNumber() == bus)
                    return true;

        for (auto* parent = from.getParentFolderTrack(); parent != nullptr; parent = parent->getParentFolderTrack())
            if (parent == &to && isBus (*parent))
                return true;

        return false;
    }
} // namespace

MixerView::MixerView (te::Engine& e, CommandBridge& bridge, te::SelectionManager& selectionManager)
    : engine (e),
      commands (bridge),
      selection (selectionManager)
{
    setTitle ("Mixer");
    setDescription ("A channel for every track, in the playlist's order, and the return tracks next to the master. Click a channel to "
                    "select its track: the channels it feeds, and those that feed it, get an outline.");

    viewport.setViewedComponent (&stripHolder, false);
    viewport.setScrollBarsShown (false, true);
    verticalView.setViewedComponent (&content, false);
    verticalView.setScrollBarsShown (true, false);
    content.addAndMakeVisible (viewport);
    content.addAndMakeVisible (master);
    addReturnButton.onClick = [this] { commands.invoke ("track.addReturn"); };
    addReturnButton.setEnabled (false);

    addAndMakeVisible (verticalView);
    addAndMakeVisible (addReturnButton);

    selection.addChangeListener (this);
    startTimerHz (4);
}

MixerView::~MixerView()
{
    stopTimer();
    selection.removeChangeListener (this);
    editState.removeListener (this);
    strips.clear(); // before stripHolder, which holds them
}

void MixerView::setEdit (te::Edit* newEdit)
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

void MixerView::rebuild()
{
    // The tracks in the playlist's order, then the returns, together next to the master.
    auto* e = edit.get();
    auto tracks = e != nullptr ? getPlaylistTracks (*e) : juce::Array<te::Track*>();
    juce::Array<te::Track*> returns;

    for (auto* track : tracks)
        if (getReturnBus (*track) >= 0)
            returns.add (track);

    tracks.removeValuesIn (returns);
    firstReturn = tracks.size();
    tracks.addArray (returns);

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

void MixerView::updateSelection()
{
    const auto selected = getSelectedTracks (selection);

    for (auto* strip : strips)
    {
        auto* t = strip->getTrack();
        const auto isSelected = t != nullptr && selection.isSelected (t);
        strip->setSelected (isSelected);
        strip->setRelated (
            t != nullptr && ! isSelected
            && std::any_of (selected.begin(), selected.end(), [t] (te::Track* s) { return feeds (*s, *t) || feeds (*t, *s); }));
    }
}

void MixerView::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.background);

    if (strips.isEmpty() && edit.get() != nullptr)
    {
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::body));
        g.drawText ("No tracks yet: add one with Add > Track.", verticalView.getBounds().withTrimmedRight (MasterStrip::width),
                    juce::Justification::centred);
    }
}

void MixerView::resized()
{
    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop (toolbarHeight).reduced (Space::s, Space::xs);
    addReturnButton.setBounds (toolbar.removeFromLeft (96));

    // At least as tall as the strips need to be; a shorter mixer scrolls up and down.
    verticalView.setBounds (area);
    const auto minimum = std::max (ChannelStrip::getMinimumHeight() + viewport.getScrollBarThickness(), MasterStrip::getMinimumHeight());
    const auto scrolls = area.getHeight() < minimum;
    content.setSize (area.getWidth() - (scrolls ? verticalView.getScrollBarThickness() : 0), std::max (area.getHeight(), minimum));

    auto inside = content.getLocalBounds();
    master.setBounds (inside.removeFromRight (MasterStrip::width));
    viewport.setBounds (inside);

    // The strips fill the height, leaving room for the scroll bar along the bottom. A gap sets the returns apart.
    const auto height = inside.getHeight() - viewport.getScrollBarThickness();
    const auto gap = firstReturn > 0 && firstReturn < strips.size() ? returnsGap : 0;
    stripHolder.setSize (std::max (1, strips.size() * ChannelStrip::width + gap), height);

    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (i * ChannelStrip::width + (i >= firstReturn ? gap : 0), 0, ChannelStrip::width, height);
}

void MixerView::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (isTrackState (child))
        triggerAsyncUpdate();
}

void MixerView::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (isTrackState (child))
        triggerAsyncUpdate();
}

void MixerView::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == editState || isTrackState (parent))
        triggerAsyncUpdate();
}
} // namespace phazite::ui
