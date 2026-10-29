// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PlaylistParts.h"
#include "TrackHeights.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/TimelineModel.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
//==============================================================================
Playlist::Lanes::Lanes (Playlist& p)
    : playlist (p)
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Tracks");
    setWantsKeyboardFocus (true); // a click here takes the keyboard back from text fields and sliders
}

void Playlist::Lanes::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto clip = g.getClipBounds();
    g.fillAll (p.background);

    if (! playlist.context.hasProject())
        return;

    // The song's sections, as ghosts behind the tracks: a faint wash of each one's color.
    const auto sections = getSongSections (playlist.context);
    const auto& timeline = playlist.getView();
    const auto xOf = [&timeline] (BeatPosition beat) { return juce::roundToInt (timeline.beatToX (beat)); };

    for (const auto& section : sections)
    {
        const auto left = xOf (section.range.getStart());
        const auto wash =
            juce::Rectangle<int> (left, clip.getY(), xOf (section.range.getEnd()) - left, clip.getHeight()).getIntersection (clip);

        if (! wash.isEmpty())
        {
            g.setColour (
                section.colour.withAlpha (playlist.selection.isSelected (section.id) ? SectionTint::selectedWash : SectionTint::wash));
            g.fillRect (wash);
        }
    }

    // The selected tracks' rows a shade lighter. The rows aren't in their tracks' colors: the headers and the clips
    // carry them.
    for (const auto& row : playlist.rows)
    {
        const auto area = getRowArea (row).getIntersection (clip);

        if (area.isEmpty() || ! playlist.selection.isSelected (row.track.id))
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
    for (const auto& section : sections)
    {
        g.setColour (section.colour.withAlpha (SectionTint::edge));
        g.fillRect (xOf (section.range.getStart()), clip.getY(), 1, clip.getHeight());
    }

    if (! sections.empty())
    {
        g.setColour (sections.back().colour.withAlpha (SectionTint::edge));
        g.fillRect (xOf (sections.back().range.getEnd()), clip.getY(), 1, clip.getHeight());
    }
}

juce::Rectangle<int> Playlist::Lanes::getRowArea (const Row& row) const
{
    const juce::Rectangle<int> rowArea (0, playlist.getRowTop (row), getWidth(), row.height);
    const auto band = getLocalBounds().withHeight (playlist.pinnedHeight);
    return rowArea.getIntersection (row.track.pinned ? band : getLocalBounds().withTrimmedTop (playlist.pinnedHeight));
}

void Playlist::Lanes::paintOverChildren (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto& timeline = playlist.getView();

    // The range, on each of its tracks' rows.
    if (! playlist.range.isEmpty())
    {
        const auto shown = playlist.range.getRange();
        const auto left = juce::roundToInt (timeline.beatToX (shown.getStart()));
        const auto right = juce::roundToInt (timeline.beatToX (shown.getEnd()));

        for (const auto track : playlist.range.getTracks())
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
    : TimelineEditor (bridge, projectContext, GridOwner::playlist),
      DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      context (projectContext),
      selection (projectContext.getSongSelection()),
      range (projectContext.getSongRange()),
      sectionsStrip (*this),
      chordLane (*this),
      chordLaneHeader (*this),
      addTrackRow (std::make_unique<AddTrackRow> (*this)),
      lanes (std::make_unique<Lanes> (*this))
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
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

    // The clips' layers let the mouse through to the lanes, which handle it, and so do the clips; the automation lanes in
    // them take it.
    for (auto* layer : { &scrollingClips, &pinnedClips })
    {
        layer->setInterceptsMouseClicks (false, true);
        lanes->addAndMakeVisible (layer);
    }

    for (auto* c : std::initializer_list<juce::Component*> { &addTrackButton, &sectionsStrip, &chordLaneHeader, &chordLane, &headerColumn,
                                                             lanes.get() })
        addAndMakeVisible (c);

    selection.addChangeListener (this);
    range.addChangeListener (this);
    context.addListener (this);
    showProject();
    toolChanged(); // the Smart Tool's pointer and hint
}

Playlist::~Playlist()
{
    context.removeListener (this);
    range.removeChangeListener (this);
    selection.removeChangeListener (this);
    headers.clear(); // before headerContent, which holds them
    laneHeaders.clear();
    laneViews.clear();
    clipComponents.clear(); // and the lanes
    waveforms.clear();      // after the components that draw them, and before the lanes they repaint
}

void Playlist::showProject()
{
    // The same project again (after a save, say) keeps the view as it is.
    if (shownProject == context.getProjectNumber())
        return;

    shownProject = context.getProjectNumber();
    const auto open = context.hasProject();

    // Another project's tracks and clips can have the same IDs: nothing of what was shown stays.
    scrollY = 0;
    dropRow = -1;
    selectionAnchor = {};
    lastClip = {};
    gesture = nullptr;
    rubberBand = {};
    fileDropBeat.reset();
    headers.clear();
    laneHeaders.clear();
    laneViews.clear();
    knownLanes.reset(); // the new project's lanes show where they are
    clipComponents.clear();
    waveforms.clear();
    shownClips.clear();
    clipIndex.clear();
    rows.clear();
    rowOf.clear();
    cancelPendingUpdate();

    TimelineEditor::showProject();
    sectionsStrip.showProject();
    chordLane.showProject();
    chordLaneHeader.update();
    addTrackButton.setEnabled (open);
    addTrackRow->setVisible (open);
    rebuild();
}

void Playlist::tick()
{
    const auto rendering = getClipsBeingRendered (context);

    if (rendering.empty() && ! showsRendering)
        return;

    showsRendering = ! rendering.empty();

    for (auto& [id, component] : clipComponents)
    {
        const auto found = rendering.find (id);
        component->showRendering (found != rendering.end() ? std::optional (found->second) : std::nullopt);
    }
}

void Playlist::songChanged (const SongChanges& changes)
{
    // The time bar's sections and the chord lane's chords, which the tempo places in time too.
    if (changes.sections || changes.tempo)
        sectionsStrip.repaint();

    // A track's key sets the new project's, which an empty lane is in until a key is chosen for it.
    if (changes.chords || changes.tempo || changes.tracks)
    {
        chordLane.repaint();
        chordLaneHeader.update();
    }

    if (changes.tracks)
    {
        rebuild(); // a name, color, mute or solo, or tracks added, deleted or moved
        return;
    }

    // Heights, collapsed and pinned tracks, and lanes: shown, collapsed, of devices gone or renamed, or of parameters that
    // gained or lost automation, which always shows a lane (the master's row with it).
    if (changes.view || changes.devices || changes.automation)
        trackHeightChanged();

    // Clips are placed in time, and the time line is in beats: a tempo change moves them on it.
    if (changes.clips || changes.tempo || changes.devices)
    {
        rebuildClips();
        layOutClips();
        updateClips();
    }

    if (changes.tempo)
        ruler.repaint();

    if (changes.clips || changes.tempo || changes.sections)
        lanes->repaint();
}

//==============================================================================
juce::Rectangle<int> Playlist::getTimelineArea() const
{
    return lanes->getBounds();
}

BeatRange Playlist::getContentRange() const
{
    // An empty song still shows some bars. Everything on the time line, sections included, so that an outline longer
    // than the song so far can be scrolled to and fits Zoom to Fit.
    return { BeatPosition(), std::max (BeatPosition::fromBeats (16.0), getTimelineEnd (context)) };
}

void Playlist::timelineChanged()
{
    layOutClips();
    lanes->repaint();
    sectionsStrip.repaint();
    chordLane.repaint();
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

    // Under it, the chord lane, with its header over the track headers.
    auto chordRow = area.removeFromTop (ChordLane::preferredHeight).withTrimmedRight (verticalBarWidth);
    chordLaneHeader.setBounds (chordRow.removeFromLeft (headerWidth));
    chordLane.setBounds (chordRow);

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
        if (context.hasProject())
        {
            resizeTracks (context, getAllPlaylistTracks (context),
                          juce::roundToInt (getWheelNotches (wheel.deltaY) * TrackHeight::step / 2));
            trackHeightChanged();
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
    if (range.isSource (source))
    {
        lanes->repaint();

        // With FL Studio's keymap, the selected time range loops while it is selected, and then the song again.
        if (context.hasProject() && getCommands().getKeymap().selectionLoops)
        {
            if (! range.isEmpty())
                loopRange (context, range.getRange());
            else if (loopsSelection)
                loopSong (context);

            loopsSelection = ! range.isEmpty();
        }

        return;
    }

    updateHeaders();
    updateClips();
    lanes->repaint(); // and the lanes' selected points

    if (focusMode)
        layOutRows(); // what is in focus changed

    // The clip selected last is the one the Draw and Paint tools place (not a section, which is a clip too).
    if (const auto clips = selection.getClips(); ! clips.empty())
        lastClip = clips.front();

    if (const auto tracks = selection.getTracks(); ! tracks.empty())
        scrollToShow (getRowOf (tracks.front()));
}
} // namespace phazite::ui
