// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MixerView.h"
#include "model/mixer/MixerModel.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int returnsGap = Space::m; // between the tracks' strips and the returns'

    // The slim column after the returns, whose "+" adds one: as wide as the button, with a little room around it, as FL Studio
    // and Ardour put theirs after their channels.
    constexpr int addReturnColumnWidth = controlHeight + Space::s;
} // namespace

MixerView::MixerView (CommandBridge& bridge, ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext),
      master (bridge, projectContext)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Mixer");
    setComponentID ("mixer"); // what lessons point at (Lessons::Step::target)
    setDescription ("A channel for every track, in the playlist's order, and the return tracks next to the master. Click a channel to "
                    "select its track: the channels it feeds, those that feed it and those its sidechains link it to get an outline.");

    viewport.setViewedComponent (&stripHolder, false);
    viewport.setScrollBarsShown (false, true);
    verticalView.setViewedComponent (&content, false);
    verticalView.setScrollBarsShown (true, false);
    content.addAndMakeVisible (viewport);
    content.addAndMakeVisible (master);
    addReturnButton.onClick = [this] { commands.invoke ("track.addReturn"); };
    addReturnButton.setEnabled (false);
    stripHolder.addAndMakeVisible (addReturnButton);
    addAndMakeVisible (verticalView);

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
        withShortcut (commands,
                      "Adds a return track, for an effect that several tracks share, such as a reverb: each sends it a copy of "
                      "its sound, and it plays its effect alongside them",
                      "track.addReturn"));
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

    // What links two channels: the sound of one reaching the other (a send, a bus), or a sidechain of one listening to the
    // other, either way round, so that a kick shows the bass that ducks under it.
    const auto connections = getConnections (context);
    const auto linked = [this, &connections] (TrackID a, TrackID b)
    {
        const auto listens = [a, b] (const Connection& c)
        { return c.kind == Connection::Kind::sidechain && ((c.from == a && c.to == b) || (c.from == b && c.to == a)); };

        return feeds (context, a, b) || feeds (context, b, a) || std::any_of (connections.begin(), connections.end(), listens);
    };

    for (auto* strip : strips)
    {
        const auto id = strip->getTrack();
        const auto isSelected = selection.isSelected (id);
        strip->setSelected (isSelected);
        strip->setRelated (! isSelected
                           && std::any_of (selected.begin(), selected.end(), [&linked, id] (TrackID s) { return linked (s, id); }));
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

void MixerView::resized()
{
    const auto area = getLocalBounds();

    // At least as tall as the strips need to be; a shorter mixer scrolls up and down.
    verticalView.setBounds (area);
    const auto minimum = std::max (ChannelStrip::getMinimumHeight() + viewport.getScrollBarThickness(), MasterStrip::getMinimumHeight());
    const auto scrolls = area.getHeight() < minimum;
    content.setSize (area.getWidth() - (scrolls ? verticalView.getScrollBarThickness() : 0), std::max (area.getHeight(), minimum));

    auto inside = content.getLocalBounds();
    master.setBounds (inside.removeFromRight (MasterStrip::width));
    viewport.setBounds (inside);

    // The strips fill the height, leaving room for the scroll bar along the bottom. A gap sets the returns apart, and the
    // column after them holds the "+" that adds one, level with the strips' names.
    const auto height = inside.getHeight() - viewport.getScrollBarThickness();
    const auto gap = firstReturn > 0 && firstReturn < strips.size() ? returnsGap : 0;
    const auto stripsWidth = strips.size() * ChannelStrip::width + gap;
    stripHolder.setSize (stripsWidth + addReturnColumnWidth, height);

    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (i * ChannelStrip::width + (i >= firstReturn ? gap : 0), 0, ChannelStrip::width, height);

    addReturnButton.setBounds (stripsWidth + Space::s / 2, Space::xs, controlHeight, controlHeight);
}
} // namespace phazite::ui
