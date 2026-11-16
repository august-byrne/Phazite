// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MixerView.h"
#include "model/mixer/MixerModel.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <utility>

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
    setWantsKeyboardFocus (true); // F9 gives it the keys (and a screen reader its name), and Tab goes on into its strips
    setComponentID ("mixer");     // what lessons point at (Lessons::Step::target)
    setDescription ("A channel for every track, in the playlist's order, and the return tracks next to the master. Click a channel to "
                    "select its track: the channels it feeds, those that feed it and those its sidechains link it to get an outline.");

    viewport.setTitle ("Channels"); // what screen readers say as the keys go to them, to scroll them
    viewport.onScroll = [this] { layOutStrips(); };
    verticalView.setTitle ("Channels and master");
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
    const auto mixerChannels = getMixerChannels (context);
    channels = mixerChannels.tracks;
    channels.insert (channels.end(), mixerChannels.returns.begin(), mixerChannels.returns.end());
    firstReturn = (int) mixerChannels.tracks.size();
    channelIndex.clear();

    for (int i = 0; i < (int) channels.size(); ++i)
        channelIndex[channels[(size_t) i]] = i;

    // The strips of channels that are still there are kept, where their channels are now (layOutStrips()); those of deleted
    // ones go.
    std::erase_if (strips, [this] (const std::unique_ptr<ChannelStrip>& strip) { return ! channelIndex.contains (strip->getTrack()); });
    updateSelection();
    resized();
    repaint(); // the hint while there are no tracks
}

void MixerView::updateSelection()
{
    const auto& selection = context.getSongSelection();
    const auto selected = selection.getTracks();
    selectedChannels.clear();
    relatedChannels.clear();

    // What links two channels: the sound of one reaching the other (a send, a bus), or a sidechain of one listening to the
    // other, either way round, so that a kick shows the bass that ducks under it. Every channel's, for the strips made as they
    // come into view.
    if (! selected.empty())
    {
        const auto connections = getConnections (context);
        const auto linked = [this, &connections] (TrackID a, TrackID b)
        {
            const auto listens = [a, b] (const Connection& c)
            { return c.kind == Connection::Kind::sidechain && ((c.from == a && c.to == b) || (c.from == b && c.to == a)); };

            return feeds (context, a, b) || feeds (context, b, a) || std::any_of (connections.begin(), connections.end(), listens);
        };

        for (const auto id : channels)
        {
            if (selection.isSelected (id))
                selectedChannels.insert (id);
            else if (std::any_of (selected.begin(), selected.end(), [&linked, id] (TrackID s) { return linked (s, id); }))
                relatedChannels.insert (id);
        }
    }

    for (const auto& strip : strips)
        showSelectionOn (*strip);
}

void MixerView::showSelectionOn (ChannelStrip& strip) const
{
    strip.setSelected (selectedChannels.contains (strip.getTrack()));
    strip.setRelated (relatedChannels.contains (strip.getTrack()));
}

int MixerView::getChannelX (int index) const
{
    // A gap sets the returns apart.
    const auto gap = firstReturn > 0 && firstReturn < (int) channels.size() ? returnsGap : 0;
    return index * ChannelStrip::width + (index >= firstReturn ? gap : 0);
}

void MixerView::layOutStrips()
{
    // The channels in view, and one more at either end, which the Tab key goes on to.
    const auto view = viewport.getViewArea();
    auto first = -1, last = -1;

    for (int i = 0; i < (int) channels.size(); ++i)
    {
        if (const auto x = getChannelX (i); x + ChannelStrip::width > view.getX() && x < view.getRight())
        {
            first = first < 0 ? i : first;
            last = i;
        }
    }

    if (first >= 0)
    {
        first = std::max (0, first - 1);
        last = std::min ((int) channels.size() - 1, last + 1);
    }

    // A jump (a scroll bar's thumb dragged far) makes and deletes at most what part of a frame can bear, the rest a turn of the
    // message loop later, and so on, so that it never stalls a frame; two strips at least each time, as a step of a scroll
    // brings into view and takes out of it.
    const auto until = juce::Time::getMillisecondCounterHiRes() + makingBudgetMs;
    auto done = 0;
    auto left = false;
    const auto mayDoMore = [&]
    {
        if (done++ < 2 || juce::Time::getMillisecondCounterHiRes() < until)
            return true;

        left = true;
        return false;
    };

    // The strips of channels that left go, but not while they are in use: one with the keyboard focus (a value being typed),
    // or the mouse pressed on it (a fader dragged while the mixer scrolls). Out of view, one left for later shows nothing.
    const auto inRange = [first, last] (int index) { return index >= first && index <= last; };
    std::erase_if (strips,
                   [&] (const std::unique_ptr<ChannelStrip>& strip)
                   {
                       return ! inRange (channelIndex.at (strip->getTrack())) && ! strip->hasKeyboardFocus (true)
                              && ! strip->isMouseButtonDown (true) && mayDoMore();
                   });

    // A strip made as its channel comes into view shows it as it is, and nothing of another.

    for (auto i = first; i >= 0 && i <= last; ++i)
    {
        const auto id = channels[(size_t) i];

        if (std::none_of (strips.begin(), strips.end(), [id] (const std::unique_ptr<ChannelStrip>& s) { return s->getTrack() == id; }))
        {
            if (! mayDoMore())
                continue;

            auto& strip = *strips.emplace_back (std::make_unique<ChannelStrip> (commands, context, id));
            strip.onFocusedByKeys = [this, id] { showWhole (id); };
            showSelectionOn (strip);
            stripHolder.addAndMakeVisible (strip);
        }
    }

    if (left && ! std::exchange (restPosted, true))
    {
        juce::MessageManager::callAsync (
            [safe = juce::Component::SafePointer<MixerView> (this)]
            {
                if (safe != nullptr)
                {
                    safe->restPosted = false;
                    safe->layOutStrips();
                }
            });
    }

    const auto height = stripHolder.getHeight();

    for (const auto& strip : strips)
        strip->setBounds (getChannelX (channelIndex.at (strip->getTrack())), 0, ChannelStrip::width, height);

    // What a screen reader hears of the channels' view: how many there are, and which have strips.
    const auto shown = channels.empty()
                           ? juce::String ("No channels")
                           : "Channels " + juce::String (first + 1) + " to " + juce::String (last + 1) + " of "
                                 + juce::String (channels.size()) + " show; scroll sideways, or press Tab from the last, for the others";

    if (viewport.getDescription() != shown)
        viewport.setDescription (shown);
}

void MixerView::showWhole (TrackID id)
{
    // After the focus has moved (this is called as it moves), so that no strip goes in the middle of that.
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<MixerView> (this), id]
        {
            if (safe == nullptr || ! safe->channelIndex.contains (id))
                return;

            const auto x = safe->getChannelX (safe->channelIndex.at (id));
            const auto view = safe->viewport.getViewArea();

            if (x < view.getX())
                safe->viewport.setViewPosition (x, view.getY());
            else if (x + ChannelStrip::width > view.getRight())
                safe->viewport.setViewPosition (x + ChannelStrip::width - view.getWidth(), view.getY());
        });
}

void MixerView::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.background);

    if (channels.empty() && context.hasProject())
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
    const auto stripsWidth = getChannelX ((int) channels.size());
    stripHolder.setSize (stripsWidth + addReturnColumnWidth, height);
    layOutStrips();
    addReturnButton.setBounds (stripsWidth + Space::s / 2, Space::xs, controlHeight, controlHeight);
}
} // namespace phazite::ui
