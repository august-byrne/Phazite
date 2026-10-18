// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MixerView.h"
#include "model/mixer/MixerModel.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int toolbarHeight = 36;

    constexpr int returnsGap = Space::m; // between the tracks' strips and the returns'
} // namespace

MixerView::MixerView (CommandBridge& bridge, ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext),
      master (bridge, projectContext),
      routing (projectContext)
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

    // The routing view, in place of the strips while the button is on.
    routingButton.setClickingTogglesState (true);
    routingButton.setTooltip ("Shows every channel's wires: where its sound goes, its sends and the sidechains that listen to it. "
                              "Drag from a channel's Out to route it");
    routingButton.onClick = [this] { showRouting (routingButton.getToggleState()); };
    routingViewport.setViewedComponent (&routing, false);
    routingViewport.setScrollBarsShown (false, true);
    addChildComponent (routingViewport);

    addAndMakeVisible (verticalView);
    addAndMakeVisible (addReturnButton);
    addAndMakeVisible (routingButton);

    context.getSongSelection().addChangeListener (this);
    context.addListener (this);
    showProject();
}

MixerView::~MixerView()
{
    context.removeListener (this);
    context.getSongSelection().removeChangeListener (this);
    strips.clear(); // before stripHolder, which holds them
}

void MixerView::showProject()
{
    // The strips of another project's channels go, even those with the same IDs.
    strips.clear();
    addReturnButton.setEnabled (context.hasProject());
    addReturnButton.setTooltip (
        withShortcut (commands, "Adds a return track, for an effect such as a reverb that tracks send to", "track.addReturn"));
    rebuild();
}

void MixerView::songChanged (const SongChanges& changes)
{
    if (changes.tracks)
        rebuild();
    else if (changes.devices && ! isTimerRunning())
        startTimer (250); // sends added or removed change what is related; a fader's drag changes devices many times a second
}

void MixerView::rebuild()
{
    // The tracks in the playlist's order, then the returns, together next to the master.
    const auto channels = getMixerChannels (context);
    auto ids = channels.tracks;
    ids.insert (ids.end(), channels.returns.begin(), channels.returns.end());
    firstReturn = (int) channels.tracks.size();

    // The strips of tracks that are still there are kept, in the tracks' new order.
    juce::OwnedArray<ChannelStrip> kept;

    for (const auto id : ids)
    {
        ChannelStrip* strip = nullptr;

        for (auto* existing : strips)
            if (existing->getTrack() == id)
                strip = existing;

        if (strip != nullptr)
        {
            strips.removeObject (strip, false);
        }
        else
        {
            strip = new ChannelStrip (commands, context);
            strip->setTrack (id);
            stripHolder.addAndMakeVisible (strip);
        }

        kept.add (strip);
    }

    strips.swapWith (kept); // kept now holds the strips of deleted tracks, and deletes them
    updateSelection();
    resized();
    repaint(); // the hint while there are no tracks
}

void MixerView::updateSelection()
{
    const auto& selection = context.getSongSelection();
    const auto selected = selection.getTracks();

    for (auto* strip : strips)
    {
        const auto id = strip->getTrack();
        const auto isSelected = selection.isSelected (id);
        strip->setSelected (isSelected);
        strip->setRelated (! isSelected
                           && std::any_of (selected.begin(), selected.end(),
                                           [this, id] (TrackID s) { return feeds (context, s, id) || feeds (context, id, s); }));
    }
}

void MixerView::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.background);

    if (strips.isEmpty() && context.hasProject())
    {
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::body));
        g.drawText ("No tracks yet: add one with Add > Track.", verticalView.getBounds().withTrimmedRight (MasterStrip::width),
                    juce::Justification::centred);
    }
}

void MixerView::showRouting (bool show)
{
    routingButton.setToggleState (show, juce::dontSendNotification);
    routingViewport.setVisible (show);
    verticalView.setVisible (! show);
    resized();
}

void MixerView::resized()
{
    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop (toolbarHeight).reduced (Space::s, Space::xs);
    addReturnButton.setBounds (toolbar.removeFromLeft (96));
    toolbar.removeFromLeft (Space::s);
    routingButton.setBounds (toolbar.removeFromLeft (96));

    routingViewport.setBounds (area);
    routing.setSize (std::max (area.getWidth(), routing.getPreferredWidth()),
                     std::max (1, area.getHeight() - routingViewport.getScrollBarThickness()));

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
} // namespace phazite::ui
