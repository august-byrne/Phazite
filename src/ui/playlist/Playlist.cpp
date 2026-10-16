// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PlaylistParts.h"
#include "TrackHeights.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Sections.h"
#include "model/arrangement/Tracks.h"
#include "model/transport/Playback.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    bool isTrackState (const juce::ValueTree& state)
    {
        return state.hasType (te::IDs::TRACK) || state.hasType (te::IDs::FOLDERTRACK);
    }

    // The clip a state belongs to (a note belongs to its clip, say), or an invalid tree.
    juce::ValueTree findClipState (juce::ValueTree state)
    {
        while (state.isValid() && ! te::Clip::isClipState (state) && ! isTrackState (state))
            state = state.getParent();

        return te::Clip::isClipState (state) ? state : juce::ValueTree();
    }

    bool isTempoState (const juce::ValueTree& state)
    {
        return state.hasType (te::IDs::TEMPO) || state.hasType (te::IDs::TIMESIG);
    }

    // A clip on a track; sections are arranger clips, which the sections strip shows.
    bool isContentClipState (const juce::ValueTree& state)
    {
        return te::Clip::isClipState (state) && ! state.hasType (te::IDs::ARRANGERCLIP);
    }
} // namespace

//==============================================================================
Playlist::Lanes::Lanes (Playlist& p)
    : playlist (p)
{
    setTitle ("Tracks");
    setWantsKeyboardFocus (true); // a click here takes the keyboard back from text fields and sliders
}

void Playlist::Lanes::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto clip = g.getClipBounds();
    g.fillAll (p.background);

    auto* ed = playlist.getEdit();

    if (ed == nullptr)
        return;

    // The song's sections, as ghosts behind the tracks: a faint wash of each one's color.
    const auto sections = getSections (*ed);
    const auto& timeline = playlist.getView();
    const auto xOf = [&timeline] (te::BeatPosition beat) { return juce::roundToInt (timeline.beatToX (beat)); };

    for (auto* section : sections)
    {
        const auto left = xOf (section->getStartBeat());
        const auto wash =
            juce::Rectangle<int> (left, clip.getY(), xOf (section->getEndBeat()) - left, clip.getHeight()).getIntersection (clip);

        if (! wash.isEmpty())
        {
            g.setColour (getSectionColour (*section).withAlpha (playlist.selection.isSelected (section) ? SectionTint::selectedWash
                                                                                                        : SectionTint::wash));
            g.fillRect (wash);
        }
    }

    // The selected tracks' rows a shade lighter. The rows aren't in their tracks' colors: the headers and the clips
    // carry them.
    for (const auto& row : playlist.rows)
    {
        auto* track = row.track.get();
        const auto area = getRowArea (row).getIntersection (clip);

        if (track == nullptr || area.isEmpty() || ! playlist.selection.isSelected (track))
            continue;

        g.setColour (p.text.withAlpha (TrackTint::selectedRow));
        g.fillRect (area);
    }

    paintGridLines (g, playlist, clip);

    // A line under each track, and a stronger one under the pinned band.
    g.setColour (p.border);

    for (const auto& row : playlist.rows)
        g.fillRect (getRowArea (row).removeFromBottom (1).getIntersection (clip));

    if (playlist.pinnedHeight > 0)
    {
        g.setColour (p.textMuted);
        g.fillRect (clip.getX(), playlist.pinnedHeight - 2, clip.getWidth(), 2);
    }

    // Where each section starts, and where the last one ends: lines in their colors, over the grid.
    for (auto* section : sections)
    {
        g.setColour (getSectionColour (*section).withAlpha (SectionTint::edge));
        g.fillRect (xOf (section->getStartBeat()), clip.getY(), 1, clip.getHeight());
    }

    if (auto* last = sections.getLast())
    {
        g.setColour (getSectionColour (*last).withAlpha (SectionTint::edge));
        g.fillRect (xOf (last->getEndBeat()), clip.getY(), 1, clip.getHeight());
    }
}

juce::Rectangle<int> Playlist::Lanes::getRowArea (const Row& row) const
{
    const juce::Rectangle<int> rowArea (0, playlist.getRowTop (row), getWidth(), row.height);
    const auto band = getLocalBounds().withHeight (playlist.pinnedHeight);
    return rowArea.getIntersection (row.pinned ? band : getLocalBounds().withTrimmedTop (playlist.pinnedHeight));
}

void Playlist::Lanes::paintOverChildren (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto& timeline = playlist.getView();

    // The range, on each of its tracks' rows.
    if (! playlist.range.isEmpty())
    {
        const auto left = juce::roundToInt (timeline.beatToX (playlist.range.getRange().getStart()));
        const auto right = juce::roundToInt (timeline.beatToX (playlist.range.getRange().getEnd()));

        for (auto* track : playlist.range.getTracks())
        {
            if (const auto row = playlist.getRowOf (track); row >= 0)
            {
                const auto area = getRowArea (playlist.rows[(size_t) row]).withX (left).withWidth (std::max (1, right - left));
                g.setColour (p.accent.withAlpha (0.25f));
                g.fillRect (area);
                g.setColour (p.accent);
                g.fillRect (area.withWidth (1));
                g.fillRect (area.withTrimmedLeft (area.getWidth() - 1));
            }
        }
    }

    // Where dragged files would go: a line at that time, on that track or below the tracks.
    if (playlist.fileDropBeat.has_value())
    {
        const auto x = juce::roundToInt (timeline.beatToX (*playlist.fileDropBeat));
        const auto area = juce::isPositiveAndBelow (playlist.fileDropRow, (int) playlist.rows.size())
                              ? getRowArea (playlist.rows[(size_t) playlist.fileDropRow])
                              : getLocalBounds();
        g.setColour (p.accent);
        g.fillRect (x - 1, area.getY(), 3, area.getHeight());
    }

    // Where a device dragged from the browser would go: onto a track, or onto a new track under the last.
    if (playlist.deviceDropping)
    {
        auto lastRowBottom = 0;

        for (const auto& row : playlist.rows)
            lastRowBottom = std::max (lastRowBottom, getRowArea (row).getBottom());

        const auto area = juce::isPositiveAndBelow (playlist.fileDropRow, (int) playlist.rows.size())
                              ? getRowArea (playlist.rows[(size_t) playlist.fileDropRow])
                              : getLocalBounds().withTop (lastRowBottom);
        g.setColour (p.accent);
        g.drawRect (area, 2);
    }

    if (! playlist.rubberBand.isEmpty())
    {
        g.setColour (p.accent.withAlpha (0.15f));
        g.fillRect (playlist.rubberBand);
        g.setColour (p.accent);
        g.drawRect (playlist.rubberBand, 1);
    }

    // Where a section dragged in the time bar would go, or where its end would be.
    if (const auto target = playlist.sectionsStrip.getDragTarget())
    {
        g.setColour (p.accent);
        g.fillRect (juce::roundToInt (timeline.beatToX (*target)) - 1, 0, 3, getHeight());
    }
}

//==============================================================================
Playlist::AddTrackRow::AddTrackRow (Playlist& p)
    : juce::Button ("Add track at the end"),
      playlist (p)
{
    setTitle ("Add track at the end");
    setDescription ("Click to add a track at the end, or right-click for a folder or a return track. Double-clicking under "
                    "the last track adds one too, and so does dropping an audio or MIDI file there.");
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    onClick = [this] { playlist.addTrackAtEnd (false); };
}

void Playlist::AddTrackRow::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    // A dashed outline, as a place for a track to be.
    const auto& p = getPalette (*this);
    const auto area = getLocalBounds().reduced (Space::s, Space::s).toFloat();
    g.setColour (highlighted || down ? p.text : p.textMuted);

    juce::Path outline, dashed;
    outline.addRoundedRectangle (area, cornerRadius);
    const float dashes[] = { 4.0f, 3.0f };
    juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
    g.fillPath (dashed);

    g.setFont (juce::FontOptions (TextSize::body));
    g.drawText ("+ Add track", area, juce::Justification::centred);
}

void Playlist::AddTrackRow::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        playlist.showAddTrackMenu();
    else
        juce::Button::mouseDown (e);
}

//==============================================================================
Playlist::Playlist (CommandBridge& bridge, ProjectContext& projectContext)
    : TimelineEditor (bridge, GridOwner::playlist),
      context (projectContext),
      selection (projectContext.getSelection()),
      range (projectContext.getRangeSelection()),
      addTrackRow (std::make_unique<AddTrackRow> (*this)),
      lanes (std::make_unique<Lanes> (*this))
{
    setTitle ("Playlist");

    addTrackButton.setButtonText ("Track");
    addTrackButton.onClick = [this] { getCommands().invoke ("track.add"); };
    addTrackButton.setEnabled (false); // until a project is open

    // While a section is dragged, where it would go shows across the tracks too.
    sectionsStrip.onDragChanged = [this] { lanes->repaint(); };

    headerColumn.setTitle ("Track headers");
    headerColumn.addAndMakeVisible (scrollingHeaders);
    headerColumn.addAndMakeVisible (pinnedHeaders);
    scrollingHeaders.addAndMakeVisible (headerContent);
    headerContent.addChildComponent (*addTrackRow); // shown while a project is open
    pinnedHeaders.setTitle ("Pinned tracks");

    // The clips' layers let the mouse through to the lanes, which handle it.
    for (auto* layer : { &scrollingClips, &pinnedClips })
    {
        layer->setInterceptsMouseClicks (false, false);
        lanes->addAndMakeVisible (layer);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &addTrackButton, &sectionsStrip, &headerColumn, lanes.get() })
        addAndMakeVisible (c);

    selection.addChangeListener (this);
    range.addChangeListener (this);
    context.addListener (this);
    setEdit (context.getEdit());
    toolChanged(); // the Smart Tool's pointer and hint
}

Playlist::~Playlist()
{
    context.removeListener (this);
    range.removeChangeListener (this);
    selection.removeChangeListener (this);
    editState.removeListener (this);
    headers.clear();        // before headerContent, which holds them
    clipComponents.clear(); // and the lanes
}

void Playlist::setEdit (te::Edit* newEdit)
{
    // The same project again (after a save, say) keeps the view as it is.
    if (newEdit == getEdit() && editState.isValid() == (newEdit != nullptr))
        return;

    editState.removeListener (this);
    editState = newEdit != nullptr ? newEdit->state : juce::ValueTree();
    editState.addListener (this);

    scrollY = 0;
    dropRow = -1;
    selectionAnchor = nullptr;
    lastClip = nullptr;
    gesture = nullptr;
    rubberBand = {};
    fileDropBeat.reset();
    headers.clear();
    clipComponents.clear();
    rows.clear();
    cancelPendingUpdate();

    setTimelineEdit (newEdit);
    sectionsStrip.setEdit (newEdit);
    addTrackButton.setEnabled (newEdit != nullptr);
    addTrackRow->setVisible (newEdit != nullptr);
    rebuild();
}

//==============================================================================
juce::Rectangle<int> Playlist::getTimelineArea() const
{
    return lanes->getBounds();
}

te::BeatRange Playlist::getContentRange() const
{
    auto end = 16.0; // an empty song still shows some bars

    // Everything on the time line, sections included (Tracktion's Edit length counts them), so that an outline longer
    // than the song so far can be scrolled to and fits Zoom to Fit. Not the song's end (getSongEnd()), which sections
    // don't make longer.
    if (auto* e = getEdit())
        end = std::max (end, e->tempoSequence.toBeats (te::toPosition (e->getLength())).inBeats());

    return { te::BeatPosition(), te::BeatPosition::fromBeats (end) };
}

void Playlist::timelineChanged()
{
    layOutClips();
    lanes->repaint();
    sectionsStrip.repaint();
}

void Playlist::gridChanged()
{
    lanes->repaint();
}

void Playlist::verticalScrollBarMoved (double newRangeStart)
{
    setScrollY (juce::roundToInt (newRangeStart));
}

bool Playlist::scrollVerticallyBy (int pixels)
{
    const auto before = scrollY;
    setScrollY (scrollY + pixels);
    return scrollY != before;
}

//==============================================================================
void Playlist::paint (juce::Graphics& g)
{
    paintEditorBackground (g);
}

void Playlist::paintOverChildren (juce::Graphics& g)
{
    if (dropRow < 0)
        return;

    // A line where the dragged track would go.
    const auto y = dropRow < (int) rows.size() ? getRowTop (rows[(size_t) dropRow]) : pinnedHeight + contentHeight - scrollY;
    g.setColour (getPalette (*this).accent);
    g.fillRect (headerColumn.getX(), lanes->getY() + y - 1, lanes->getRight() - headerColumn.getX(), 3);
}

void Playlist::resized()
{
    auto area = getLocalBounds();
    layOutToolbarControls (area.removeFromTop (toolbarHeight).reduced (Space::s, Space::xs));

    const auto verticalBarWidth = getVerticalScrollBarWidth();
    horizontalScrollBar.setBounds (area.removeFromBottom (scrollBarSize).withTrimmedLeft (headerWidth).withTrimmedRight (verticalBarWidth));

    // The time bar: the song's sections over the bar numbers. Left of it, over the track headers, the button
    // that adds a track.
    auto timeBar = area.removeFromTop (SectionsStrip::preferredHeight + rulerHeight).withTrimmedRight (verticalBarWidth);
    addTrackButton.setBounds (timeBar.removeFromLeft (headerWidth).reduced (Space::s, Space::s).withWidth (96));
    sectionsStrip.setBounds (timeBar.removeFromTop (SectionsStrip::preferredHeight));
    ruler.setBounds (timeBar);

    verticalScrollBar.setBounds (area.removeFromRight (verticalBarWidth));
    headerColumn.setBounds (area.removeFromLeft (headerWidth));
    lanes->setBounds (area);

    layOutRows();
    setViewStart (getView().start);
}

void Playlist::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Ctrl+wheel zooms around the pointer, as in FL Studio.
    if (e.mods.isCommandDown())
    {
        zoomByWheel (e.getEventRelativeTo (lanes.get()).position.x, wheel.deltaY);
        return;
    }

    // Alt+wheel makes every track taller or shorter.
    if (e.mods.isAltDown())
    {
        if (auto* ed = getEdit())
        {
            const auto change = juce::roundToInt (getWheelNotches (wheel.deltaY) * TrackHeight::step / 2);

            for (auto* track : getPlaylistTracks (*ed))
                resizeTrack (*track, change);

            layOutRows();
        }

        return;
    }

    const auto sideways = e.mods.isShiftDown() ? wheel.deltaY : wheel.deltaX;
    const auto upDown = e.mods.isShiftDown() ? 0.0f : wheel.deltaY;

    if (! juce::exactlyEqual (sideways, 0.0f))
        scrollByWheel (sideways);

    if (! juce::exactlyEqual (upDown, 0.0f))
        setScrollY (scrollY - juce::roundToInt (getWheelNotches (upDown) * scrollPerNotch));
}

//==============================================================================
void Playlist::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &range)
    {
        lanes->repaint();

        // With FL Studio's keymap, the selected time range loops while it is selected, and then the song again.
        if (auto* e = getEdit(); e != nullptr && getCommands().getKeymap().selectionLoops)
        {
            if (! range.isEmpty())
                loopRange (*e, e->tempoSequence.toTime (range.getRange()));
            else if (loopsSelection)
                loopSong (*e);

            loopsSelection = ! range.isEmpty();
        }

        return;
    }

    updateHeaders();
    updateClips();
    lanes->repaint();

    if (focusMode)
        layOutRows(); // what is in focus changed

    // The clip selected last is the one the Draw and Paint tools place (not a section, which is a clip too).
    if (auto* clip = getSelectedClips (selection).getFirst())
        lastClip = clip;

    if (auto* track = selection.getFirstItemOfType<te::Track>())
        scrollToShow (getRowOf (track));
}

void Playlist::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (isTrackState (tree))
    {
        triggerAsyncUpdate(); // a name, color, height, mute or solo
    }
    else if (isTempoState (tree))
    {
        layOutClips(); // clips are placed in time, and the timeline is in beats
        ruler.repaint();
        lanes->repaint();
    }
    else if (const auto clipState = findClipState (tree); clipState.isValid())
    {
        clipChanged (clipState); // moved, trimmed or faded, or its notes changed
    }
}

void Playlist::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (isTrackState (child) || isContentClipState (child))
        triggerAsyncUpdate();
    else if (const auto clipState = findClipState (parent); clipState.isValid())
        clipChanged (clipState); // a note, say
}

void Playlist::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (isTrackState (child) || isContentClipState (child))
        triggerAsyncUpdate();
    else if (const auto clipState = findClipState (parent); clipState.isValid())
        clipChanged (clipState);
}

void Playlist::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == editState || isTrackState (parent))
        triggerAsyncUpdate();
}
} // namespace phazite::ui
