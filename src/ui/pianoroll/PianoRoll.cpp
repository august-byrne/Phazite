// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumRows.h"
#include "PianoRollParts.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int keysWidth = 72;
    constexpr int velocityLaneHeight = 72;
    constexpr int transformBarHeight = 40;
    constexpr int minRowHeight = 6, maxRowHeight = 32;
    constexpr int velocityPerNotch = 4; // Alt+wheel
} // namespace

//==============================================================================
PianoRoll::PianoRoll (CommandBridge& bridge, ProjectContext& projectContext)
    : TimelineEditor (bridge, projectContext, GridOwner::pianoRoll),
      context (projectContext),
      selection (projectContext.getSongSelection()),
      keys (std::make_unique<Keys> (*this)),
      grid (std::make_unique<NoteGrid> (*this)),
      velocityLane (std::make_unique<VelocityLane> (*this)),
      transformBar (std::make_unique<TransformBar> (*this))
{
    setTitle ("Piano roll");

    // The notes being edited stay in view while the song plays (the owner's choice, 2026-10-16); View > Follow
    // Playback turns following on, as the playlist has it.
    setFollowingPlayback (false);

    instrumentButton.setTitle ("Instrument");
    instrumentButton.onClick = [this] { instrumentClicked(); };
    chordButton.setTitle ("Chord stamp");
    chordButton.onClick = [this] { showChordMenu(); };
    keyButton.setTitle ("Key");
    keyButton.onClick = [this] { showKeyMenu(); };
    transformButton.onClick = [this] { showTransformMenu(); };

    // It runs its command, so the palette, the keys and the button agree. (Drum mode has no button: it comes
    // with drum tracks, and the grid's menu has it.)
    ghostButton.setClickingTogglesState (true);
    ghostButton.onClick = [this] { getCommands().invoke ("pianoRoll.ghostNotes"); };

    clipLabel.setJustificationType (juce::Justification::centredRight);
    clipLabel.setFont (juce::FontOptions (TextSize::small));

    for (auto* c : std::initializer_list<juce::Component*> { &instrumentButton, &chordButton, &keyButton, &ghostButton, &transformButton,
                                                             &clipLabel, keys.get(), grid.get(), velocityLane.get() })
        addAndMakeVisible (c);

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
    emptyTrack = {};
    shownProject = context.getProjectNumber();
    showProject();
    updateToolbar();
}

void PianoRoll::songChanged (const SongChanges& changes)
{
    // Notes and clips, where they are in time, a track's key or drum rows, the chords whose notes its rows show, and its
    // instrument (the toolbar shows it, and a drum sampler names the rows).
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
        scrollToNotes();
}

void PianoRoll::updateTimeOrigin (bool scrollToStart)
{
    // Bar 1 is where the first clip's notes count from, or for a track without clips, the bar the playhead is in.
    auto origin = getTimeOrigin();

    if (const auto shown = getShownClips(); ! shown.empty())
    {
        origin = shown.front().contentStart;

        for (const auto& clip : shown)
            origin = std::min (origin, clip.contentStart);
    }
    else if (hasProject() && getTrack().isValid())
    {
        origin = getBarStart (context, std::max (0, getBarAt (context, getPlayheadBeat())));
    }

    // A newly shown clip opens at bar 1. Otherwise the view stays on the same notes when the clip moves in the song.
    const auto offset = getView().start - getTimeOrigin();
    setTimeOrigin (origin);
    setViewStart (scrollToStart ? origin : origin + offset);
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
    if (const auto track = getTrack(); drumMode && track.isValid())
        drumRows = getDrumRows (getSamplerSounds (context, track), getNoteNumbersPlayed (context, clips), getDrumRowOrder (context, track),
                                getHiddenDrumRows (context, track));

    updateVerticalScrollBar();
    updateScrollBar();
    updateToolbar();
    keys->repaint();
    grid->repaint();
    velocityLane->repaint();
}

std::vector<NoteData> PianoRoll::getAllNotes() const
{
    return getNotes (context, clips);
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

    updateToolbar();
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

void PianoRoll::quantizeSelection()
{
    // With the strength and swing used last, to the grid as it is now.
    auto settings = transform.quantize;
    settings.gridBeats = getGridStepInBeats();
    const auto notes = getSelectedNotesHere();
    selectNotes (replaceNotes (context, getRefs (notes), quantize (notes, settings)), false);
}

juce::Rectangle<int> PianoRoll::getTimelineArea() const
{
    return grid->getBounds().getUnion (velocityLane->getBounds());
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
    g.setColour (p.surfaceRaised);
    g.fillRect (velocityLane->getBounds().withX (0).withWidth (keysWidth));
    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText ("Velocity", velocityLane->getBounds().withX (0).withWidth (keysWidth).reduced (Space::xs), juce::Justification::topLeft);
    g.setColour (p.border);
    g.fillRect (0, velocityLane->getY() - 1, getWidth(), 1);
}

void PianoRoll::resized()
{
    auto area = getLocalBounds();
    auto toolbar = layOutToolbarControls (area.removeFromTop (toolbarHeight).reduced (Space::s, Space::xs));

    for (auto [button, width] : { std::pair { &instrumentButton, 140 }, std::pair { &chordButton, 120 }, std::pair { &keyButton, 150 },
                                  std::pair { &ghostButton, 64 }, std::pair { &transformButton, 96 } })
    {
        button->setBounds (toolbar.removeFromLeft (width));
        toolbar.removeFromLeft (Space::xs);
    }

    clipLabel.setBounds (toolbar.withTrimmedLeft (Space::s));

    const auto verticalBarWidth = getVerticalScrollBarWidth();
    horizontalScrollBar.setBounds (area.removeFromBottom (scrollBarSize).withTrimmedLeft (keysWidth).withTrimmedRight (verticalBarWidth));

    if (transformOpen)
        transformBar->setBounds (area.removeFromBottom (transformBarHeight));

    auto velocityArea = area.removeFromBottom (velocityLaneHeight);
    auto rulerArea = area.removeFromTop (rulerHeight);
    verticalScrollBar.setBounds (area.removeFromRight (verticalBarWidth));
    keys->setBounds (area.removeFromLeft (keysWidth));
    grid->setBounds (area);
    ruler.setBounds (rulerArea.withTrimmedLeft (keysWidth).withTrimmedRight (verticalBarWidth));
    velocityLane->setBounds (velocityArea.withTrimmedLeft (keysWidth).withTrimmedRight (verticalBarWidth));

    setScrollY (scrollY);
    setViewStart (getView().start);
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto notches = getWheelNotches (wheel.deltaY);

    // Ctrl+Shift+wheel makes the rows taller or shorter; Ctrl+wheel zooms around the pointer, as in FL Studio.
    if (e.mods.isCommandDown() && e.mods.isShiftDown())
    {
        const auto row = getRowAt (e.getEventRelativeTo (grid.get()).y);
        rowHeight = juce::jlimit (minRowHeight, maxRowHeight, rowHeight + (notches > 0.0 ? 1 : -1));

        if (row >= 0)
            setScrollY (row * getRowHeight() - e.getEventRelativeTo (grid.get()).y);
        else
            setScrollY (scrollY);

        return;
    }

    if (e.mods.isCommandDown())
    {
        zoomByWheel (e.getEventRelativeTo (grid.get()).position.x, wheel.deltaY);
        return;
    }

    // Alt+wheel changes the selected notes' velocity, as in FL Studio.
    if (e.mods.isAltDown())
    {
        if (const auto notes = getSelectedRefsHere(); ! notes.empty())
            changeNoteVelocities (context, notes, notches > 0.0 ? velocityPerNotch : -velocityPerNotch);

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

    grid->repaint();
    velocityLane->repaint();
}
} // namespace phazite::ui
