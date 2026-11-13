// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumRows.h"
#include "PianoRollParts.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int keysWidth = 72;
    constexpr int settingsWidth = 176; // the settings down the left: "1/16 beat, velocity 127" fits
    constexpr int transformBarHeight = 40;
    constexpr int minRowHeight = 6, maxRowHeight = 32;
    constexpr int minimumNotesHeight = 48; // what dragging the velocity lane taller leaves the notes
    constexpr int velocityEdgeHeight = 6;  // the line between them takes a drag this tall
    constexpr int chevronSize = 16;        // the velocity lane's, which folds it

    // A clip opens zoomed to show it with a quarter of its length after it to draw on, and at least two bars of 4/4, but
    // never smaller than the editors' usual zoom, so that a quarter-beat note is wide enough to grab.
    constexpr double minimumBeatsShown = 8.0, roomAfterClips = 0.25;
    constexpr int minimumWidthToFit = 100; // pixels: narrower, the editor isn't laid out yet
} // namespace

//==============================================================================
// The line between the notes and the velocity lane: a drag up or down sizes the lane, as Ableton Live's lanes are sized by
// the divider over them, and a double-click puts it back at its first height.
struct PianoRoll::VelocityEdge : public juce::Component
{
    explicit VelocityEdge (PianoRoll& r)
        : roll (r)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        setTitle ("Velocity lane's edge");
        setHelpText ("Drag up or down to make the velocity lane taller or shorter; double-click for its first height");
    }

    void mouseDown (const juce::MouseEvent&) override { heightAtPress = roll.velocityHeight; }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        roll.velocityHeight = roll.clampVelocityHeight (heightAtPress - e.getDistanceFromDragStartY());
        roll.resized();
    }

    void mouseUp (const juce::MouseEvent&) override { roll.velocityLaneChanged(); }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        roll.velocityHeight = defaultVelocityHeight;
        roll.resized();
        roll.velocityLaneChanged();
    }

    PianoRoll& roll;
    int heightAtPress = defaultVelocityHeight;
};

//==============================================================================
PianoRoll::PianoRoll (CommandBridge& bridge, ProjectContext& projectContext)
    : TimelineEditor (bridge, projectContext, GridOwner::pianoRoll),
      context (projectContext),
      selection (projectContext.getSongSelection()),
      velocityEdge (std::make_unique<VelocityEdge> (*this)),
      keys (std::make_unique<Keys> (*this)),
      grid (std::make_unique<NoteGrid> (*this)),
      velocityLane (std::make_unique<VelocityLane> (*this)),
      transformBar (std::make_unique<TransformBar> (*this))
{
    setOpaque (true); // it fills itself, so what is behind it needn't repaint with it
    setTitle ("Piano roll");
    setComponentID ("pianoRoll"); // what lessons point at (Lessons::Step::target)
    toolRow.setTitle ("Piano roll tools");

    // The notes being edited stay in view while the song plays (the owner's choice, 2026-10-16); View > Follow
    // Playback turns following on, as the playlist has it.
    setFollowingPlayback (false);

    // The notes' menu is the More menu at the end of the tab row's tools, after what of them doesn't fit.
    toolRow.setMoreItems ([this] (juce::PopupMenu& menu) { grid->addMenuItems (menu); },
                          "More: the notes' menu, with the transforms (quantize, legato, chop, transpose, strum, humanize, "
                          "arpeggiate), slide and portamento notes, cut, copy and more. Shift+right-click on the notes shows it too");

    instrumentButton.setTitle ("Instrument");
    instrumentButton.onClick = [this] { instrumentClicked(); };
    nextNoteButton.setTitle ("Next note");
    nextNoteButton.onClick = [this] { showNextNoteMenu(); };
    chordButton.setTitle ("Chord stamp");
    chordButton.onClick = [this] { showChordMenu(); };
    keyButton.setTitle ("Key");
    keyButton.onClick = [this] { showKeyMenu(); };

    // It runs its command, so the palette, the keys and the button agree. (Drum mode has no button: it comes
    // with drum tracks, and the grid's menu has it.)
    ghostButton.setTitle ("Ghost notes");
    ghostButton.setClickingTogglesState (true);
    ghostButton.onClick = [this] { getCommands().invoke ("pianoRoll.ghostNotes"); };

    // Which clips, and where in the song: at the settings' top, its lines as many as there is room for.
    clipLabel.setJustificationType (juce::Justification::topLeft);
    clipLabel.setFont (juce::FontOptions (TextSize::small));
    setThemeColour (clipLabel, juce::Label::textColourId, &Palette::textMuted);

    for (auto* c :
         std::initializer_list<juce::Component*> { &clipLabel, &instrumentButton, &keyButton, &nextNoteButton, &chordButton, &ghostButton })
        settingsContent.addAndMakeVisible (c);

    settingsContent.setTitle ("Piano roll settings");
    settingsView.setViewedComponent (&settingsContent, false);
    settingsView.setScrollBarsShown (true, false);
    settingsView.setScrollBarThickness (ScrollTint::overlayWidth);
    settingsView.setTitle ("Piano roll settings");
    makeSeeThrough (settingsView.getVerticalScrollBar());
    addAndMakeVisible (settingsView);
    addAndMakeVisible (keys.get());

    // The velocity lane folds by its chevron to a thin row, and sizes by its edge.
    velocityFoldButton.setQuiet (true);
    velocityFoldButton.setMouseClickGrabsKeyboardFocus (false);
    velocityFoldButton.onClick = [this] { setVelocityFolded (! velocityFolded); };
    addAndMakeVisible (velocityFoldButton);
    addAndMakeVisible (*velocityEdge);

    // The notes and the velocities draw the time line, each through a window of its own (TimelineEditor::addTimelinePart()).
    addTimelinePart (*grid);
    addTimelinePart (*velocityLane);
    addAndMakeVisible (getOverview());

    // The overview of the clips' notes along the time bar's top scrolls and zooms the time line, as the playlist's does, as FL
    // Studio's piano roll has it: no scroll bar under the notes. The rows' scroll bar lies over the notes' right edge, thin and
    // see-through.
    horizontalScrollBar.setVisible (false);
    makeSeeThrough (verticalScrollBar);

    addChildComponent (*transformBar);
    selection.addChangeListener (this);
    context.addListener (this);
    projectChanged();
}

PianoRoll::~PianoRoll()
{
    stopAudition();
    context.removeListener (this);
    selection.removeChangeListener (this);
    setClips ({}); // lets go of the note selection
}

void PianoRoll::projectChanged()
{
    // The same project again (after a save, say) keeps the view as it is.
    if (shownProject == context.getProjectNumber())
        return;

    stopAudition();
    closeTransform();
    setClips ({});
    clipViews.clear(); // another project's clips can have the same IDs
    emptyTrack = {};
    shownProject = context.getProjectNumber();
    showProject();
    updateSettings();
}

void PianoRoll::songChanged (const SongChanges& changes)
{
    // Notes and clips, where they are in time, a track's key or drum rows, the chords whose notes its rows show, and its
    // instrument (its settings show it, and a drum sampler names the rows).
    if (! (changes.clips || changes.tempo || changes.tracks || changes.view || changes.devices || changes.chords))
        return;

    // Clips that left the song go, and the rest repaints. Bar 1 follows a clip moved in the song.
    if (const auto shown = getClips(); shown.size() != clips.size())
        setClips (shown, true);

    updateTimeOrigin (false);
    clipsChanged();
}

//==============================================================================
void PianoRoll::showClip (ClipID id)
{
    showClips ({ id });
}

void PianoRoll::showClips (const std::vector<ClipID>& toShow)
{
    std::vector<ClipID> midi;

    for (const auto& clip : getNoteClips (context, toShow))
        midi.push_back (clip.id);

    setClips (midi);
}

std::vector<ClipID> PianoRoll::getClips() const
{
    std::vector<ClipID> shown;

    for (const auto& clip : getShownClips())
        shown.push_back (clip.id);

    return shown;
}

std::vector<NoteClip> PianoRoll::getShownClips() const
{
    return getNoteClips (context, clips);
}

TrackID PianoRoll::getTrack() const
{
    if (const auto shown = getShownClips(); ! shown.empty())
        return shown.front().track;

    return canHoldNotes (context, emptyTrack) ? emptyTrack : TrackID();
}

void PianoRoll::showTrack (TrackID track)
{
    // The track's MIDI clip at the playhead, or the one nearest to it.
    if (const auto nearest = getMidiClipNearest (context, track, getPlayheadBeat()))
    {
        setClips ({ *nearest });
        return;
    }

    // None: the track from the bar the playhead is in, where the first note drawn makes a clip. Audio and notes
    // don't share a track (docs/design.md, section 5).
    const auto holdsAudio = hasAudioClips (context, track);
    setClips ({});
    emptyTrack = holdsAudio ? TrackID() : track;
    drumMode = ! holdsAudio && isDrumTrack (context, track);
    updateTimeOrigin (true);
    clipsChanged();
    scrollToNotes();
}

void PianoRoll::setClips (const std::vector<ClipID>& newClips, bool keepView)
{
    // The same clips change nothing.
    if (newClips == clips && ! newClips.empty())
        return;

    if (! keepView)
        rememberView();

    stopAudition();
    clips = newClips;
    selection.setNoteClips (clips); // the notes of the clips shown before are no longer selected
    targetClip = clips.empty() ? ClipID() : clips.front();
    emptyTrack = {};

    // Drum mode follows the track.
    const auto track = getTrack();
    drumMode = track.isValid() && isDrumTrack (context, track);

    if (transformOpen)
        closeTransform();

    updateTimeOrigin (! keepView);
    clipsChanged();

    if (! keepView)
        showNewClips();
}

void PianoRoll::rememberView()
{
    // A clip shown alone opens again as it was left (several shown at once open zoomed to fit).
    if (clips.size() == 1 && hasProject())
        clipViews[clips.front()] = { getView().pixelsPerBeat, (getView().start - getTimeOrigin()).inBeats(), scrollY };
}

void PianoRoll::showNewClips()
{
    if (clips.size() == 1)
    {
        if (const auto found = clipViews.find (clips.front()); found != clipViews.end())
        {
            const auto& left = found->second;
            const auto start = getTimeOrigin() + BeatDuration::fromBeats (left.offsetBeats);
            zoomTo ({ start, start + BeatDuration::fromBeats (std::max (1, getTimelineArea().getWidth()) / left.pixelsPerBeat) });
            setScrollY (left.scrollY);
            return;
        }
    }

    zoomToClips();
    scrollToNotes();
}

void PianoRoll::zoomToClips()
{
    const auto width = getTimelineArea().getWidth();
    const auto content = getContentRange();

    if (getShownClips().empty() || width < minimumWidthToFit)
        return;

    const auto beats = std::max (minimumBeatsShown, (content.getEnd() - getTimeOrigin()).inBeats() * (1.0 + roomAfterClips));
    const auto pixelsPerBeat = juce::jlimit (TimelineView::defaultPixelsPerBeat, TimelineView::maxPixelsPerBeat, width / beats);
    zoomTo ({ getTimeOrigin(), getTimeOrigin() + BeatDuration::fromBeats (width / pixelsPerBeat) });
}

void PianoRoll::updateTimeOrigin (bool scrollToStart)
{
    // Bar 1 is where the first clip's notes count from, or for a track without clips, the bar the playhead was in when the
    // track was shown: taken again at each change to the song while it played (a track muted), it moved bar 1 and the view
    // with the playhead.
    auto origin = getTimeOrigin();

    if (const auto shown = getShownClips(); ! shown.empty())
    {
        origin = shown.front().contentStart;

        for (const auto& clip : shown)
            origin = std::min (origin, clip.contentStart);
    }
    else if (hasProject() && getTrack().isValid() && scrollToStart)
    {
        origin = getBarStart (context, std::max (0, getBarAt (context, getPlayheadBeat())));
    }

    // A newly shown clip opens at bar 1. Otherwise the view stays on the same notes when the clip moves in the song: as many
    // whole pixels from bar 1 as it was, as the view starts on whole pixels (TimelineEditor::setViewStart()); counted from the
    // origin itself, each move added the rounding of the origin's place to the view's.
    const auto& timeline = getView();
    const auto pixelsFromOrigin = std::round ((timeline.start - getEarliestStart()).inBeats() * timeline.pixelsPerBeat);
    setTimeOrigin (origin);
    setViewStart (scrollToStart ? origin : getEarliestStart() + BeatDuration::fromBeats (pixelsFromOrigin / timeline.pixelsPerBeat));
}

ClipID PianoRoll::makeClipForNewNotes()
{
    // A bar long from the time origin, on the track shown without clips; notes drawn after it make it longer, into the
    // empty space after it (Notes.h's addNote()).
    const auto track = getTrack();

    if (! track.isValid() || ! getShownClips().empty())
        return {};

    const auto clip = addClipForNotes (context, track, getTimeOrigin());

    if (clip.isValid())
        setClips ({ clip }, true); // shown from now on, where the view is

    return clip;
}

void PianoRoll::clipsChanged()
{
    drawnNotes.reset();
    overviewContentChanged();

    if (const auto track = getTrack(); drumMode && track.isValid())
        drumRows = getDrumRows (getSamplerSounds (context, track), getNoteNumbersPlayed (context, clips), getDrumRowOrder (context, track),
                                getHiddenDrumRows (context, track));

    updateVerticalScrollBar();
    updateScrollBar();
    updateSettings();
    keys->repaint();
    grid->repaint();
    velocityLane->repaint();
}

std::vector<NoteData> PianoRoll::getAllNotes() const
{
    return getNotes (context, clips);
}

std::vector<TimelineEditor::OverviewMark> PianoRoll::getOverviewMarks() const
{
    // The clips' notes, each at its pitch between the lowest and the highest of them, in its clip's colour, faint while muted.
    std::vector<OverviewMark> marks;
    const auto& drawn = getDrawnNotes();

    if (drawn.notes.empty())
        return marks;

    auto lowest = 127, highest = 0;

    for (const auto& note : drawn.notes)
    {
        lowest = std::min (lowest, note.pitch);
        highest = std::max (highest, note.pitch);
    }

    const auto rows = (float) (highest - lowest + 1);

    for (const auto& note : drawn.notes)
    {
        const auto found = drawn.colours.find (note.clip);
        const auto top = (float) (highest - note.pitch) / rows;
        marks.push_back ({ note.start.inBeats(), (note.start + note.length).inBeats(), top, top + 1.0f / rows,
                           found != drawn.colours.end() ? found->second : juce::Colours::grey, note.muted });
    }

    return marks;
}

const PianoRoll::DrawnNotes& PianoRoll::getDrawnNotes() const
{
    if (! drawnNotes.has_value() || drawnSongVersion != context.getSongVersion())
    {
        auto notes = getAllNotes();
        auto colours = getNoteColours (context, notes);
        drawnNotes = DrawnNotes { std::move (notes), std::move (colours), getSelectedNoteRefs() };
        drawnSongVersion = context.getSongVersion();
    }

    return *drawnNotes;
}

std::unordered_set<NoteRef> PianoRoll::getSelectedNoteRefs() const
{
    std::unordered_set<NoteRef> selected;

    for (const auto& ref : selection.getNotes())
        if (std::find (clips.begin(), clips.end(), ref.clip) != clips.end())
            selected.insert (ref);

    return selected;
}

std::vector<NoteData> PianoRoll::getSelectedNotesHere() const
{
    const auto selected = getSelectedNoteRefs();
    std::vector<NoteData> notes;

    if (! selected.empty())
        for (const auto& note : getAllNotes())
            if (selected.contains (note.getRef()))
                notes.push_back (note);

    return notes;
}

std::vector<NoteRef> PianoRoll::getSelectedRefsHere() const
{
    return getRefs (getSelectedNotesHere());
}

void PianoRoll::selectNotes (const std::vector<NoteRef>& notes, bool addToSelection)
{
    selection.selectNotes (notes, addToSelection);
    drawnNotes.reset();
    grid->repaint();
    velocityLane->repaint();
}

void PianoRoll::selectAllNotes()
{
    selectNotes (getRefs (getAllNotes()), false);
}

ClipID PianoRoll::getClipForNewNote (BeatPosition beat) const
{
    const auto shown = getShownClips();
    const auto target = std::find_if (shown.begin(), shown.end(), [this] (const NoteClip& clip) { return clip.id == targetClip; });

    // The target clip if the beat is in it, then any clip it is in, then the target clip if the beat comes after its start.
    if (target != shown.end() && target->range.contains (beat))
        return target->id;

    for (const auto& clip : shown)
        if (clip.range.contains (beat))
            return clip.id;

    if (target != shown.end() && beat >= target->range.getStart())
        return target->id;

    return {};
}

std::optional<Key> PianoRoll::getKey() const
{
    return getTrackKey (context, getTrack());
}

void PianoRoll::setKey (const std::optional<Key>& key)
{
    if (const auto track = getTrack(); track.isValid())
        chooseTrackKey (context, track, key);

    updateSettings();
    grid->repaint();
}

//==============================================================================
void PianoRoll::audition (ClipID clip, int pitch, int velocity)
{
    audition (clip, std::vector<int> { pitch }, velocity);
}

void PianoRoll::audition (ClipID clip, const std::vector<int>& pitches, int velocity)
{
    stopAudition();
    auditionTrack = auditionNotes (context, clip, pitches, velocity);
}

void PianoRoll::stopAudition()
{
    if (auditionTrack.isValid())
        stopAuditioning (context, auditionTrack);

    auditionTrack = {};
}

//==============================================================================
bool PianoRoll::canUseTool (Tool toolToUse) const
{
    return toolToUse != Tool::slip; // notes have nothing inside to slip
}

bool PianoRoll::usesSmartToolFor (Tool toolToUse) const
{
    return toolToUse == Tool::draw;
}

bool PianoRoll::canNudgeSelection() const
{
    return ! getSelectedNoteRefs().empty();
}

void PianoRoll::nudgeSelection (int direction)
{
    nudgeNotes (context, getSelectedRefsHere(), BeatDuration::fromBeats (direction * getGridStepInBeats()));
}

bool PianoRoll::canQuantizeSelection() const
{
    return ! getSelectedNoteRefs().empty();
}

std::optional<BeatRange> PianoRoll::getSelectedRange() const
{
    std::optional<BeatRange> shown;

    for (const auto& note : getSelectedNotesHere())
        shown = shown.has_value() ? BeatRange (std::min (shown->getStart(), note.start), std::max (shown->getEnd(), note.getEnd()))
                                  : BeatRange (note.start, note.getEnd());

    return shown;
}

void PianoRoll::quantizeSelection()
{
    // With the strength and swing used last, which its name shows (getQuantizeName()), to the grid as it is now, counted from
    // bar 1 as the grid is.
    auto settings = transform.quantize;
    settings.gridBeats = getGridStepInBeats();
    settings.origin = getTimeOrigin();
    const auto notes = getSelectedNotesHere();
    selectNotes (replaceNotes (context, getRefs (notes), quantize (notes, settings)), false);
}

juce::String PianoRoll::getQuantizeName() const
{
    // What Quantize... was given last, which Quantize uses too, unless it is the plain strength and no swing.
    const auto& settings = transform.quantize;
    juce::StringArray parts;

    if (settings.strength < 0.995f)
        parts.add (juce::String (juce::roundToInt (settings.strength * 100.0f)) + "%");

    if (settings.swing > 0.005f)
        parts.add ("swing " + juce::String (juce::roundToInt (settings.swing * 100.0f)) + "%");

    if (settings.ends)
        parts.add ("ends too");

    return parts.isEmpty() ? juce::String ("Quantize") : "Quantize (" + parts.joinIntoString (", ") + ")";
}

juce::Rectangle<int> PianoRoll::getTimelineArea() const
{
    return getTimelinePartArea (*grid).getUnion (getTimelinePartArea (*velocityLane));
}

std::optional<double> PianoRoll::getLinkedPlayheadBeat (double songBeat) const
{
    // The notes shown play wherever a clip linked to theirs plays: the playhead shows where, in the clip shown.
    return getLinkedPlayBeat (context, clips, songBeat);
}

BeatRange PianoRoll::getContentRange() const
{
    const auto shown = getShownClips();

    if (shown.empty())
        return { getTimeOrigin(), getTimeOrigin() + BeatDuration::fromBeats (16.0) };

    auto range = shown.front().range;

    for (const auto& clip : shown)
        range = { std::min (range.getStart(), clip.range.getStart()), std::max (range.getEnd(), clip.range.getEnd()) };

    return range;
}

void PianoRoll::timelineChanged()
{
    grid->repaint();
    velocityLane->repaint();
}

void PianoRoll::toolChanged()
{
    grid->cancelGesture();
}

void PianoRoll::gridChanged()
{
    grid->repaint();
    nextNoteButton.setButtonText (describeNextNote()); // in drum mode, a hit is a step of the grid long

    if (transformOpen)
        transformBar->show (transform); // its text names the grid
}

void PianoRoll::verticalScrollBarMoved (double newRangeStart)
{
    setScrollY (juce::roundToInt (newRangeStart));
}

bool PianoRoll::scrollVerticallyBy (int pixels)
{
    const auto before = scrollY;
    setScrollY (scrollY + pixels);
    return scrollY != before;
}

//==============================================================================
void PianoRoll::paint (juce::Graphics& g)
{
    paintEditorBackground (g);
    const auto& p = getPalette (*this);

    // The settings down the left, with a line at their right, as the audio clip editor's.
    g.setColour (p.border);
    g.fillRect (settingsWidth - 1, 0, 1, getHeight());

    // The velocity lane's corner under the keys, its name after its chevron; folded, the lane is that row alone, all of it.
    g.setColour (p.surfaceRaised);
    g.fillRect (velocityFolded ? velocityRow : velocityRow.withWidth (keysWidth));
    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText ("Velocity",
                velocityRow.withWidth (keysWidth)
                    .withHeight (foldedVelocityHeight)
                    .withTrimmedLeft (velocityFoldButton.getRight() - velocityRow.getX()),
                juce::Justification::centredLeft);
    g.setColour (p.border);
    g.fillRect (velocityRow.getX(), velocityRow.getY() - 1, velocityRow.getWidth(), 1);
}

void PianoRoll::resized()
{
    // Down the left, the settings. Its tools are in its area's tab row (TimelineEditor::getToolRow()).
    auto area = getLocalBounds();
    settingsView.setBounds (area.removeFromLeft (settingsWidth));
    layOutSettings();

    if (transformOpen)
        transformBar->setBounds (area.removeFromBottom (transformBarHeight));

    // The overview of the notes over the bar numbers, over the notes; the velocities under them, or a thin row while folded.
    velocityRow = area.removeFromBottom (getVelocityLaneHeight());
    getOverview().setBounds (area.removeFromTop (overviewHeight).withTrimmedLeft (keysWidth));
    auto rulerArea = area.removeFromTop (rulerHeight);
    keys->setBounds (area.removeFromLeft (keysWidth));
    placeTimelinePart (*grid, area);
    placeTimelinePart (ruler, rulerArea.withTrimmedLeft (keysWidth));
    velocityLane->setVisible (! velocityFolded);
    placeTimelinePart (*velocityLane, velocityRow.withTrimmedLeft (keysWidth));

    // The lane's chevron at its top left, and its edge over the line above it, which a folded lane hasn't.
    const auto chevron = chevronSize;
    velocityFoldButton.setIcon (velocityFolded ? Icon::folded : Icon::expanded);
    velocityFoldButton.setTooltip (velocityFolded ? "Show the velocity lane" : "Fold the velocity lane to a thin row");
    velocityFoldButton.setBounds (velocityRow.getX() + Space::xs / 2, velocityRow.getY() + (foldedVelocityHeight - chevron) / 2, chevron,
                                  chevron);
    velocityEdge->setVisible (! velocityFolded);
    velocityEdge->setBounds (velocityRow.getX(), velocityRow.getY() - velocityEdgeHeight / 2, velocityRow.getWidth(), velocityEdgeHeight);
    velocityEdge->toFront (false);

    // The rows' scroll bar over the notes' right edge, thin and see-through: it takes no room of their own.
    verticalScrollBar.setBounds (area.removeFromRight (ScrollTint::overlayWidth));
    verticalScrollBar.toFront (false);

    setScrollY (scrollY);
    setViewStart (getView().start);
}

void PianoRoll::layOutSettings()
{
    // From the top: which clips, in up to three lines, then a row for each setting, as the audio clip editor's rows; a scroll
    // bar where the piano roll is too short for them all.
    constexpr int labelLines = 3;
    const auto lineHeight = juce::roundToInt (juce::Font (juce::FontOptions (TextSize::small)).getHeight());
    const auto buttons = { &instrumentButton, &keyButton, &nextNoteButton, &chordButton, &ghostButton };
    const auto height = 2 * Space::s + labelLines * lineHeight + (int) buttons.size() * (Space::xs + controlHeight);
    const auto scrolls = height > settingsView.getHeight();
    settingsContent.setSize (settingsView.getWidth() - (scrolls ? settingsView.getScrollBarThickness() : 0),
                             std::max (height, settingsView.getHeight()));

    auto area = settingsContent.getLocalBounds().reduced (Space::s);
    clipLabel.setBounds (area.removeFromTop (labelLines * lineHeight));

    for (auto* button : buttons)
    {
        area.removeFromTop (Space::xs);
        button->setBounds (area.removeFromTop (controlHeight));
    }
}

void PianoRoll::setVelocityLane (int height, bool folded)
{
    velocityHeight = std::max (minimumVelocityHeight, height);
    velocityFolded = folded;
    resized();
    repaint();
}

int PianoRoll::getVelocityLaneHeight() const noexcept
{
    return velocityFolded ? foldedVelocityHeight : clampVelocityHeight (velocityHeight);
}

int PianoRoll::clampVelocityHeight (int height) const
{
    const auto room = getHeight() - (transformOpen ? transformBarHeight : 0) - overviewHeight - rulerHeight - minimumNotesHeight;
    return juce::jlimit (minimumVelocityHeight, std::max (minimumVelocityHeight, room), height);
}

void PianoRoll::setVelocityFolded (bool shouldFold)
{
    velocityFolded = shouldFold;
    resized();
    repaint();
    velocityLaneChanged();
}

void PianoRoll::velocityLaneChanged()
{
    // The height as dragged, not as squeezed into a short piano roll: a taller one shows it all again.
    velocityHeight = std::max (minimumVelocityHeight, velocityHeight);

    if (onVelocityLaneChanged != nullptr)
        onVelocityLaneChanged (velocityHeight, velocityFolded);
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // As in the playlist: Ctrl+wheel zooms around the pointer, as in FL Studio, and Alt+wheel makes the rows taller or
    // shorter, as it makes the playlist's tracks. (Over the velocity lane, the wheel changes velocities.)
    if (e.mods.isCommandDown())
    {
        zoomByWheel (e.getEventRelativeTo (grid.get()).position.x, wheel.deltaY);
        return;
    }

    if (e.mods.isAltDown())
    {
        if (const auto notches = getWheelNotches (wheel.deltaY); ! juce::exactlyEqual (notches, 0.0))
        {
            const auto y = e.getEventRelativeTo (grid.get()).y;
            const auto row = getRowAt (y);
            rowHeight = juce::jlimit (minRowHeight, maxRowHeight, rowHeight + (notches > 0.0 ? 1 : -1));
            setScrollY (row >= 0 ? row * getRowHeight() - y : scrollY);
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

bool PianoRoll::keyPressed (const juce::KeyPress& key)
{
    // Escape during a drag takes back what it did, as a part of the drag, as Alt and Shift are: with the mouse down, no
    // command runs. Otherwise the key is the commands' (KeyDispatcher), Escape among them.
    if (key == juce::KeyPress::escapeKey && (grid->isDragging() || velocityLane->isDragging()))
    {
        grid->cancelDrag();
        velocityLane->cancelDrag();
        return true;
    }

    return false;
}

//==============================================================================
void PianoRoll::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The MIDI clips selected in the playlist, or a newly selected track's clip. With notes or anything else
    // selected, what is shown stays.
    std::vector<ClipID> selectedClips;

    for (const auto& clip : getNoteClips (context, selection.getClips()))
        selectedClips.push_back (clip.id);

    if (! selectedClips.empty())
    {
        setClips (selectedClips);
    }
    else if (const auto tracks = selection.getTracks(); ! tracks.empty())
    {
        if (const auto track = tracks.front(); canHoldNotes (context, track) && track != getTrack())
            showTrack (track);
    }

    drawnNotes.reset();
    grid->repaint();
    velocityLane->repaint();
}
} // namespace phazite::ui
