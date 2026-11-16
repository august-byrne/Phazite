// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PlaylistParts.h"
#include "TrackHeights.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/TimelineModel.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>
#include <utility>

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
    const auto xOf = [&timeline] (BeatPosition beat) { return timeline.beatToColumn (beat); };

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

    // A folder's row, which holds no clips of its own: its tracks' clips, a thin lane for each track, so that the row shows
    // what the folder plays even while it is collapsed, as Ableton Live's and Bitwig's group tracks show theirs.
    for (const auto& row : playlist.rows)
    {
        const auto found = playlist.folderSummaries.find (row.track.id);
        const auto area = getRowArea (row).reduced (0, FolderTint::inset).withTrimmedBottom (1);

        if (found == playlist.folderSummaries.end() || found->second.lanes == 0 || ! area.intersects (clip))
            continue;

        const auto& summary = found->second;
        const auto laneHeight = std::min (FolderTint::tallestLane, (float) area.getHeight() / (float) summary.lanes);
        const auto top = (float) area.getCentreY() - laneHeight * (float) summary.lanes / 2.0f;

        for (const auto& folded : summary.clips)
        {
            // On whole pixels (columns, which move with the song), so that the bars' edges are crisp, with a pixel's gap after
            // each clip and under each lane.
            const auto left = (float) timeline.beatToColumn (folded.range.getStart());
            const auto right = (float) timeline.beatToColumn (folded.range.getEnd());

            if (right < (float) clip.getX() || left > (float) clip.getRight())
                continue;

            const auto laneTop = std::round (top + laneHeight * (float) folded.lane);
            const auto laneBottom = std::max (laneTop + 1.0f, std::round (top + laneHeight * (float) (folded.lane + 1)) - 1.0f);
            g.setColour (folded.colour.withAlpha (folded.muted ? FolderTint::mutedClip : FolderTint::clip));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (left, laneTop, std::max (left + 1.0f, right - 1.0f), laneBottom));
        }
    }

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
        const auto left = timeline.beatToColumn (shown.getStart());
        const auto right = timeline.beatToColumn (shown.getEnd());

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
        const auto x = timeline.beatToColumn (*playlist.fileDropBeat);
        const auto area = juce::isPositiveAndBelow (playlist.fileDropRow, (int) playlist.rows.size())
                              ? getRowArea (playlist.rows[(size_t) playlist.fileDropRow])
                              : getLocalBounds();
        g.setColour (p.accent);

        // On a track that doesn't suit it, it goes on a new track below: a line along the row's foot, where that track comes,
        // and the time from there down.
        if (playlist.fileDropOnNewTrack)
        {
            const auto shown = getShownTimeline (*this).toNearestInt();
            g.fillRect (shown.getX(), area.getBottom() - 2, shown.getWidth(), 3);
            g.fillRect (x - 1, area.getBottom() - 2, 3, area.getHeight() / 2);
        }
        else
        {
            g.fillRect (x - 1, area.getY(), 3, area.getHeight());
        }
    }

    // Where a device dragged from the browser would go: onto a track, or onto a new track under the last. Across what shows of the
    // lanes, so that its sides stay on the edges of their window while the song scrolls under it.
    if (playlist.deviceDropping)
    {
        auto lastRowBottom = 0;

        for (const auto& row : playlist.rows)
            lastRowBottom = std::max (lastRowBottom, getRowArea (row).getBottom());

        const auto area = juce::isPositiveAndBelow (playlist.fileDropRow, (int) playlist.rows.size())
                              ? getRowArea (playlist.rows[(size_t) playlist.fileDropRow])
                              : getLocalBounds().withTop (lastRowBottom);
        const auto shown = getShownTimeline (*this);
        g.setColour (p.accent);
        g.drawRect (area.toFloat().withLeft (shown.getX()).withRight (shown.getRight()), 2.0f);
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
        g.fillRect (timeline.beatToColumn (*target) - 1, 0, 3, getHeight());
    }
}

//==============================================================================
Playlist::AddTrackRow::AddTrackRow (Playlist& p)
    : juce::Button ("Add track at the end"),
      playlist (p)
{
    setTitle ("Add track at the end");
    setDescription ("Click to add a track at the end, or right-click for a folder (return tracks are the mixer's). Double-clicking under "
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
// The track headers' column's right edge, inside it: dragged, the column's width; double-clicked, its width again. Lit in the
// focus colour under the pointer and while dragged, as the panels' dividers are.
struct Playlist::HeaderEdge : public juce::Component
{
    explicit HeaderEdge (Playlist& p)
        : playlist (p)
    {
        setTitle ("Track headers' edge");
        setHelpText ("Drag to make the track headers wider or narrower; double-click to put them back as they were");
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        setRepaintsOnMouseActivity (true);
    }

    void paint (juce::Graphics& g) override
    {
        if (isMouseOverOrDragging())
            g.fillAll (getPalette (*this).focus.withAlpha (Shell::dividerLit));
    }

    void mouseDown (const juce::MouseEvent&) override { widthAtStart = playlist.getHeaderWidth(); }
    void mouseDrag (const juce::MouseEvent& e) override { playlist.setHeaderWidth (widthAtStart + e.getDistanceFromDragStartX()); }
    void mouseUp (const juce::MouseEvent&) override { playlist.keepHeaderWidth(); }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        playlist.setHeaderWidth (defaultHeaderWidth);
        playlist.keepHeaderWidth();
    }

    static constexpr int width = Space::xs;

    Playlist& playlist;
    int widthAtStart = 0;
};

//==============================================================================
Playlist::Playlist (CommandBridge& bridge, ProjectContext& projectContext)
    : TimelineEditor (bridge, projectContext, GridOwner::playlist),
      DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      context (projectContext),
      selection (projectContext.getSongSelection()),
      range (projectContext.getSongRange()),
      sectionsStrip (*this),
      tempoLane (*this),
      tempoLaneHeader (*this),
      chordLane (*this),
      chordLaneHeader (*this),
      addTrackRow (std::make_unique<AddTrackRow> (*this)),
      lanes (std::make_unique<Lanes> (*this)),
      headerEdge (std::make_unique<HeaderEdge> (*this))
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Playlist");
    toolRow.setTitle ("Playlist tools");

    // What lessons point at (Lessons::Step::target): controls by the commands they run, others by names of their own.
    setComponentID ("playlist");
    addTrackButton.setComponentID ("track.add");
    sectionsStrip.setComponentID ("sections");
    tempoLane.setComponentID ("tempoLane");
    chordLane.setComponentID ("chordLane");

    addTrackButton.setButtonText ("Track");
    addTrackButton.onClick = [this] { getCommands().invoke ("track.add"); };
    addTrackButton.setEnabled (false); // until a project is open
    collapseAllButton.setComponentID ("track.collapseAll");
    collapseAllButton.setQuiet (true);
    collapseAllButton.setMouseClickGrabsKeyboardFocus (false);
    collapseAllButton.onClick = [this] { getCommands().invoke ("track.collapseAll"); };
    collapseAllButton.setEnabled (false);

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

    // The parts that draw the time line, each through a window of its own (TimelineEditor::addTimelinePart()), among the others.
    addAndMakeVisible (addTrackButton);
    addAndMakeVisible (collapseAllButton);
    addTimelinePart (sectionsStrip);
    addAndMakeVisible (tempoLaneHeader);
    addTimelinePart (tempoLane);
    addAndMakeVisible (chordLaneHeader);
    addTimelinePart (chordLane);
    addAndMakeVisible (headerColumn);
    addTimelinePart (*lanes);
    addAndMakeVisible (*headerEdge);
    addAndMakeVisible (getOverview());

    // The overview along the time bar's top scrolls and zooms the time line, as FL Studio's scroll bar there does: no scroll bar
    // under the lanes. The rows' scroll bar lies over the lanes' right edge, thin and see-through.
    horizontalScrollBar.setVisible (false);
    updateScrollBarColours();

    // An empty song's hint over the lanes.
    emptyTitle.setText ("This song is empty", juce::dontSendNotification);
    emptyTitle.setFont (juce::FontOptions (TextSize::title, juce::Font::bold));
    emptyText.setText ("Drag an instrument or a sound here from the browser to make a track with it, or double-click here for an "
                       "empty one. Help > Example Songs opens finished songs to take apart.",
                       juce::dontSendNotification);
    emptyText.setFont (juce::FontOptions (TextSize::body));
    setThemeColour (emptyText, juce::Label::textColourId, &Palette::textMuted);

    for (auto* label : { &emptyTitle, &emptyText })
    {
        label->setJustificationType (juce::Justification::centredTop);
        label->setInterceptsMouseClicks (false, false);
        addChildComponent (label);
    }

    selection.addChangeListener (this);
    range.addChangeListener (this);
    context.addListener (this);
    showProject();
    toolChanged(); // the Smart Tool's pointer and hint
    addMouseListener (&clickWatcher, true);
}

Playlist::~Playlist()
{
    removeMouseListener (&clickWatcher);
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
    trackDrop.reset();
    headersFaded = false;
    selectionAnchor = {};
    lastClip = {};
    clipsSelectedBefore.clear();
    tracksSelectedBefore.clear();
    gesture = nullptr;
    setDragReadout ({});
    rubberBand = {};
    fileDropBeat.reset();
    headers.clear();
    laneHeaders.clear();
    laneViews.clear();
    knownLanes.reset(); // the new project's lanes show where they are
    clipComponents.clear();
    shownRecordings.clear();
    shownParts.clear();
    waveforms.clear();
    shownClips.clear();
    shownSubRows.clear();
    clipIndex.clear();
    overviewMarks.clear();
    rows.clear();
    rowOf.clear();
    cancelPendingUpdate();

    TimelineEditor::showProject();
    sectionsStrip.showProject();
    tempoLane.showProject();
    tempoLaneHeader.update();
    showTempoLaneWithChanges();
    chordLane.showProject();
    chordLaneHeader.update();
    showChordLaneWithChords();
    addTrackButton.setEnabled (open);
    collapseAllButton.setEnabled (open);
    addTrackRow->setVisible (open);
    rebuild();
    overviewContentChanged();
}

void Playlist::tick()
{
    showRecordings(); // what is being recorded, as it comes in

    if (ticksToSkip-- > 0)
        return;

    ticksToSkip = 2;
    const auto rendering = getClipsBeingRendered (context);
    const auto failed = getClipsWhoseRenderFailed (context);

    if (rendering.empty() && failed.empty() && ! showsRendering)
        return;

    showsRendering = ! rendering.empty() || ! failed.empty();

    for (auto& [id, component] : clipComponents)
    {
        const auto found = rendering.find (id);
        component->showRendering (found != rendering.end() ? std::optional (found->second) : std::nullopt);
        component->showRenderFailed (std::find (failed.begin(), failed.end(), id) != failed.end());
    }
}

void Playlist::songChanged (const SongChanges& changes)
{
    // The time bar's sections and the chord lane's chords, which the tempo places in time too.
    if (changes.sections || changes.tempo)
        sectionsStrip.repaint();

    // The tempo lane, while the song's tempo changes.
    if (changes.tempo)
    {
        showTempoLaneWithChanges();
        tempoLane.tempoChanged();
    }

    // The chord lane, while the song has chords. A track's key sets the new project's, which the lane is in until a key is
    // chosen for it.
    if (changes.chords)
        showChordLaneWithChords();

    if (changes.chords || changes.tempo || changes.tracks)
    {
        chordLane.repaint();
        chordLaneHeader.update();
    }

    // A change of a few clips (a note added, a clip dragged a step) shows those clips again, and their tracks' rows; any other
    // change of clips, or one that moves them all, shows every one again.
    const auto fewClips =
        changes.clips && ! changes.changedClips.any && ! changes.clipTracks.any && ! changes.tracks && ! changes.tempo && ! changes.devices;

    // The overview of the whole song: its tracks and their clips, after the rows show them (it takes theirs from them), read
    // again for the tracks a few clips were on, and painted again if what it shows of them moved.
    const auto showOverview = [this, &changes, fewClips]
    {
        if (changes.tracks || changes.tempo || (changes.clips && ! fewClips))
        {
            overviewMarks.clear();
            overviewContentChanged();
        }
        else if (fewClips && refreshOverviewMarks (changes.clipTracks.named))
        {
            overviewContentChanged();
        }
    };

    if (changes.tracks)
    {
        rebuild(); // a name, color, mute or solo, or tracks added, deleted or moved
        showOverview();
        return;
    }

    // Heights, collapsed and pinned tracks, and lanes: shown, collapsed, of devices gone or renamed, or of parameters that
    // gained or lost automation, which always shows a lane (the master's row with it).
    if (changes.view || changes.devices || changes.automation)
        trackHeightChanged();

    // Clips are placed in time, and the time line is in beats: a tempo change moves them on it.
    if (fewClips)
    {
        clipsBehind = false;
        showChangedClips (changes);
    }
    else if (changes.clips || changes.tempo || changes.devices)
    {
        clipsBehind = false;
        rebuildClips();
        layOutClips();
        updateClips();
    }

    if (changes.tempo)
        ruler.repaint();

    if ((changes.clips && ! fewClips) || changes.tempo || changes.sections)
        lanes->repaint();

    showOverview();
}

//==============================================================================
juce::Rectangle<int> Playlist::getTimelineArea() const
{
    return getTimelinePartArea (*lanes);
}

std::vector<TimelineEditor::OverviewMark> Playlist::getOverviewMarks() const
{
    // Each track that holds clips a row, in the playlist's order, folders' tracks too, however collapsed; its clips in its colour.
    std::vector<OverviewMark> marks;

    if (! context.hasProject())
        return marks;

    std::vector<TrackID> tracks;

    for (const auto track : getAllPlaylistTracks (context))
        if (canHoldClips (context, track))
            tracks.push_back (track);

    // What the rows hold, as they were read last, in their tracks' colours; the tracks in collapsed folders, which have no
    // rows, from the song.
    if (overviewMarks.empty())
    {
        for (const auto& row : rows)
            overviewMarks[row.track.id] = {};

        for (const auto& clip : shownClips)
            if (const auto row = getRowOf (clip.track); row >= 0)
                overviewMarks[clip.track].push_back ({ clip.range.getStart().inBeats(), clip.range.getEnd().inBeats(), 0.0f, 1.0f,
                                                       rows[(size_t) row].track.colour, clip.muted });
    }

    for (size_t row = 0; row < tracks.size(); ++row)
    {
        auto found = overviewMarks.find (tracks[row]);

        if (found == overviewMarks.end())
            found = overviewMarks.emplace (tracks[row], readOverviewMarks (tracks[row])).first;

        const auto top = (float) row / (float) tracks.size(), bottom = (float) (row + 1) / (float) tracks.size();

        for (auto mark : found->second)
        {
            mark.top = top;
            mark.bottom = bottom;
            marks.push_back (mark);
        }
    }

    return marks;
}

std::vector<TimelineEditor::OverviewMark> Playlist::readOverviewMarks (TrackID track) const
{
    // In its colour, its row's place in the overview set as the marks are given (getOverviewMarks()).
    std::vector<OverviewMark> marks;
    const auto shown = getTrackRow (context, track);
    const auto colour = shown.has_value() ? shown->colour : juce::Colours::grey;

    for (const auto& clip : getClips (context, track))
        marks.push_back ({ clip.range.getStart().inBeats(), clip.range.getEnd().inBeats(), 0.0f, 1.0f, colour, clip.muted });

    return marks;
}

bool Playlist::refreshOverviewMarks (const std::vector<TrackID>& tracks)
{
    const auto same = [] (const OverviewMark& a, const OverviewMark& b)
    { return juce::exactlyEqual (a.start, b.start) && juce::exactlyEqual (a.end, b.end) && a.colour == b.colour && a.faint == b.faint; };

    auto changed = false;

    for (const auto track : tracks)
    {
        const auto found = overviewMarks.find (track);

        // A track the overview hasn't read is read with the others.
        if (found == overviewMarks.end())
        {
            changed = true;
            continue;
        }

        auto fresh = readOverviewMarks (track);
        changed = changed || ! std::equal (fresh.begin(), fresh.end(), found->second.begin(), found->second.end(), same);
        found->second = std::move (fresh);
    }

    return changed;
}

BeatRange Playlist::getContentRange() const
{
    // An empty song still shows some bars. Everything on the time line, sections included, so that an outline longer
    // than the song so far can be scrolled to and fits Zoom to Fit.
    return { BeatPosition(), std::max (BeatPosition::fromBeats (16.0), getTimelineEnd (context)) };
}

void Playlist::timelineChanged()
{
    const MakingPass pass (*this); // a jump along the song makes its clips' components a part of a frame at a time
    // Where files dragged over the lanes would go: the beat under the pointer, which the view moving (following the playhead)
    // changes without a move of the mouse, which alone told it before, so that the line slid away from where a drop went.
    if (fileDropBeat.has_value())
        fileDropBeat = getFileDropBeat (fileDropX);

    layOutClips();
    lanes->repaint();
    sectionsStrip.viewChanged(); // a section's name being typed stays over it
    sectionsStrip.repaint();
    tempoLane.repaint();
    chordLane.repaint();
}

void Playlist::gridChanged()
{
    lanes->repaint();
}

void Playlist::frameComing()
{
    if (std::exchange (clipsBehind, false))
    {
        rebuildClips();
        layOutClips();
        updateClips();
    }
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
    paintTrackDrop (g);
}

void Playlist::resized()
{
    auto area = getLocalBounds();

    // The time bar (its tools are in its area's tab row: TimelineEditor::getToolRow()): the whole song in miniature with the part
    // in view over it, as FL Studio's scroll bar shows it along the top, then the song's sections over the bar numbers. Left of
    // them, over the track headers, the button that adds a track.
    auto timeBar = area.removeFromTop (overviewHeight + SectionsStrip::preferredHeight + rulerHeight);
    auto corner = timeBar.removeFromLeft (headerWidth).withTrimmedTop (overviewHeight).reduced (Space::s, Space::s);
    collapseAllButton.setBounds (corner.removeFromRight (corner.getHeight()));
    addTrackButton.setBounds (corner.withWidth (96));
    getOverview().setBounds (timeBar.removeFromTop (overviewHeight));
    placeTimelinePart (sectionsStrip, timeBar.removeFromTop (SectionsStrip::preferredHeight));
    placeTimelinePart (ruler, timeBar);

    // Under it, while the song's tempo changes, the tempo lane, and while it has chords, the chord lane, each with its header
    // over the track headers.
    auto tempoRow = area.removeFromTop (tempoLaneShown ? TempoLane::preferredHeight : 0);
    tempoLaneHeader.setBounds (tempoRow.removeFromLeft (headerWidth));
    placeTimelinePart (tempoLane, tempoRow);

    auto chordRow = area.removeFromTop (chordLaneShown ? ChordLane::preferredHeight : 0);
    chordLaneHeader.setBounds (chordRow.removeFromLeft (headerWidth));
    placeTimelinePart (chordLane, chordRow);

    headerColumn.setBounds (area.removeFromLeft (headerWidth));
    headerEdge->setBounds (headerColumn.getBounds().removeFromRight (HeaderEdge::width)); // over the headers, right of their controls
    placeTimelinePart (*lanes, area);

    // An empty song's hint, near the lanes' top, in their middle, no wider than a paragraph reads.
    const auto hintWidth = std::min (area.getWidth() - 2 * Space::l, emptyHintWidth);
    auto hint = area.withSizeKeepingCentre (std::max (0, hintWidth), area.getHeight()).withTrimmedTop (2 * Space::l);
    emptyTitle.setBounds (hint.removeFromTop (juce::roundToInt (TextSize::title) + Space::s));
    emptyText.setBounds (hint.removeFromTop (4 * (juce::roundToInt (TextSize::body) + Space::xs)));

    // The rows' scroll bar over the lanes' right edge, thin and see-through, as FL Studio's is: it takes no room of their own.
    verticalScrollBar.setBounds (area.removeFromRight (ScrollTint::overlayWidth));
    verticalScrollBar.toFront (false);

    layOutRows();
    setViewStart (getView().start);
}

void Playlist::showTempoLaneWithChanges()
{
    // A song of one tempo has it in the top bar alone, as other DAWs show their tempo tracks only once the tempo changes.
    const auto shown = hasTempoChanges (context);
    tempoLane.setVisible (shown);
    tempoLaneHeader.setVisible (shown);

    if (std::exchange (tempoLaneShown, shown) != shown)
        resized();
}

void Playlist::showChordLaneWithChords()
{
    // A guide to the song's chords, with nothing to show without them, as other DAWs' chord tracks show only once added.
    const auto shown = hasLaneChords (context);
    chordLane.setVisible (shown);
    chordLaneHeader.setVisible (shown);

    if (std::exchange (chordLaneShown, shown) != shown)
        resized();
}

void Playlist::setHeaderWidth (int width)
{
    width = juce::jlimit (minHeaderWidth, maxHeaderWidth, width);

    if (width != headerWidth)
    {
        headerWidth = width;
        resized();
        repaint();
    }
}

void Playlist::updateScrollBarColours()
{
    makeSeeThrough (verticalScrollBar);
}

void Playlist::keepHeaderWidth()
{
    if (onHeaderWidthChanged != nullptr)
        onHeaderWidthChanged (headerWidth);
}

void Playlist::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Ctrl+wheel zooms around the pointer, as in FL Studio.
    if (e.mods.isCommandDown())
    {
        zoomByWheel (e.getEventRelativeTo (lanes.get()).position.x, wheel.deltaY);
        return;
    }

    // Alt+wheel makes every track taller or shorter, by whole pixels, the parts of a notch a trackpad sends added up.
    if (e.mods.isAltDown())
    {
        if (const auto pixels = takeWheelSteps (wheel.deltaY, TrackHeight::step / 2.0, heightWheelRest);
            context.hasProject() && pixels != 0)
        {
            resizeTracks (context, getAllPlaylistTracks (context), pixels);
            trackHeightChanged();
        }

        return;
    }

    const auto sideways = e.mods.isShiftDown() ? wheel.deltaY : wheel.deltaX;
    const auto upDown = e.mods.isShiftDown() ? 0.0f : wheel.deltaY;

    if (! juce::exactlyEqual (sideways, 0.0f))
        scrollByWheel (sideways);

    if (! juce::exactlyEqual (upDown, 0.0f))
        setScrollY (scrollY - takeWheelSteps (upDown, scrollPerNotch, scrollWheelRest));
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

    // The clip selected last is the one the Draw and Paint tools place (not a section, which is a clip too): of those this
    // change selected, the last in the playlist's order. (The selection lists clips in that order, not as they were
    // selected: the first of them was taken, known bug 56 until 2026-11-14.)
    const auto clips = selection.getClips();

    for (const auto clip : clips)
        if (std::find (clipsSelectedBefore.begin(), clipsSelectedBefore.end(), clip) == clipsSelectedBefore.end())
            lastClip = clip;

    clipsSelectedBefore = clips;

    // A track a key or a command selects, not all of it in view, is brought whole into view; nothing is scrolled for a selection
    // the mouse made in the playlist, on what the user sees (a click, a Shift+click on a track far from the first selected), or
    // one that selected no track anew (a clip clicked, a track taken out). Every change scrolled back to the first selected
    // track, hiding what was clicked (known bug 54 until 2026-11-14).
    const auto tracks = selection.getTracks();
    std::vector<int> newRows;

    for (const auto track : tracks)
        if (std::find (tracksSelectedBefore.begin(), tracksSelectedBefore.end(), track) == tracksSelectedBefore.end())
            if (const auto row = getRowOf (track); row >= 0)
                newRows.push_back (row);

    tracksSelectedBefore = tracks;

    if (std::exchange (selectionByMouse, false) || newRows.empty())
        return;

    if (std::none_of (newRows.begin(), newRows.end(), [this] (int row) { return isRowFullyInView (row); }))
        scrollToShow (*std::min_element (newRows.begin(), newRows.end()));
}

void Playlist::ClickWatcher::mouseUp (const juce::MouseEvent&)
{
    // A click may select as the mouse goes up; a message later, after that change is heard (posted before this), what changes
    // the selection is a key or a command again.
    playlist.selectionByMouse = true;
    juce::MessageManager::callAsync (
        [safe = juce::Component::SafePointer<Playlist> (&playlist)]
        {
            if (safe != nullptr)
                safe->selectionByMouse = false;
        });
}
} // namespace phazite::ui
