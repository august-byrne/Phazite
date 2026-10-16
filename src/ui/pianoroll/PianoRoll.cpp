// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumRows.h"
#include "PianoRollParts.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Tracks.h"
#include "model/engine/LiveInput.h"
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

    bool isClipStateOfType (const juce::ValueTree& state)
    {
        return state.hasType (te::IDs::MIDICLIP);
    }
} // namespace

//==============================================================================
PianoRoll::PianoRoll (CommandBridge& bridge, te::SelectionManager& selectionManager)
    : TimelineEditor (bridge, GridOwner::pianoRoll),
      selection (selectionManager),
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
    updateToolbar();
}

PianoRoll::~PianoRoll()
{
    stopAudition();
    cancelPendingUpdate();
    selection.removeChangeListener (this);
    editState.removeListener (this);
    setClips ({}); // lets go of the note selection, and of the clips' listeners
}

void PianoRoll::setEdit (te::Edit* newEdit)
{
    if (newEdit == getEdit() && editState.isValid() == (newEdit != nullptr))
        return;

    stopAudition();
    closeTransform();
    setClips ({});
    emptyTrack = nullptr;
    editState.removeListener (this);
    editState = newEdit != nullptr ? newEdit->state : juce::ValueTree();
    editState.addListener (this);
    setTimelineEdit (newEdit);
    updateToolbar();
}

//==============================================================================
void PianoRoll::showClips (const juce::Array<te::MidiClip*>& toShow)
{
    setClips (toShow);
}

juce::Array<te::MidiClip*> PianoRoll::getClips() const
{
    juce::Array<te::MidiClip*> shown;

    for (const auto& c : clips)
        if (auto* clip = c.get(); clip != nullptr && clip->state.isAChildOf (editState))
            shown.add (clip);

    return shown;
}

te::AudioTrack* PianoRoll::getTrack() const
{
    if (auto* first = getClips().getFirst())
        return dynamic_cast<te::AudioTrack*> (first->getTrack());

    auto* track = emptyTrack.get();
    return track != nullptr && track->state.isAChildOf (editState) ? track : nullptr;
}

void PianoRoll::showTrack (te::AudioTrack& track)
{
    // The track's MIDI clip at the playhead, or the one nearest to it.
    const auto playheadBeat = getPlayheadBeat();
    te::MidiClip* nearest = nullptr;
    auto nearestDistance = std::numeric_limits<double>::max();
    auto holdsAudio = false;

    for (auto* clip : track.getClips())
    {
        holdsAudio = holdsAudio || dynamic_cast<te::AudioClipBase*> (clip) != nullptr;

        if (auto* midi = dynamic_cast<te::MidiClip*> (clip))
        {
            const auto range = midi->getEditBeatRange();
            const auto distance = range.contains (playheadBeat) ? 0.0
                                                                : std::min (std::abs ((range.getStart() - playheadBeat).inBeats()),
                                                                            std::abs ((range.getEnd() - playheadBeat).inBeats()));

            if (distance < nearestDistance)
            {
                nearest = midi;
                nearestDistance = distance;
            }
        }
    }

    if (nearest != nullptr)
    {
        setClips ({ nearest });
        return;
    }

    // None: the track from the bar the playhead is in, where the first note drawn makes a clip. Audio and notes
    // don't share a track (docs/design.md, section 5).
    setClips ({});
    emptyTrack = holdsAudio ? nullptr : &track;
    drumMode = ! holdsAudio && isDrumTrack (track);
    updateTimeOrigin (true);
    clipsChanged();
    scrollToNotes();
}

void PianoRoll::setClips (const juce::Array<te::MidiClip*>& newClips, bool keepView)
{
    // The same clips, all still there, change nothing.
    const auto same = clips.size() == (size_t) newClips.size()
                      && std::equal (clips.begin(), clips.end(), newClips.begin(),
                                     [] (const te::SafeSelectable<te::MidiClip>& a, te::MidiClip* b) { return a.get() == b; });

    if (same && (newClips.isEmpty() || selectedEvents != nullptr))
        return;

    stopAudition();

    // The note selection points to the old clips, so it goes first.
    if (selectedEvents != nullptr)
    {
        selectedEvents->deselect();
        selectedEvents = nullptr;
    }

    for (auto& c : clips)
        if (auto* clip = c.get())
            clip->removeSelectableListener (this);

    clips.clear();

    for (auto* clip : newClips)
    {
        clips.emplace_back (clip);
        clip->addSelectableListener (this);
    }

    if (! newClips.isEmpty())
        selectedEvents = std::make_unique<te::SelectedMidiEvents> (newClips);

    targetClip = newClips.getFirst();
    emptyTrack = nullptr;

    // Drum mode follows the track.
    auto* track = getTrack();
    drumMode = track != nullptr && isDrumTrack (*track);

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

    if (const auto shown = getClips(); ! shown.isEmpty())
    {
        origin = shown.getFirst()->getContentStartBeat();

        for (auto* clip : shown)
            origin = std::min (origin, clip->getContentStartBeat());
    }
    else if (auto* e = getEdit(); e != nullptr && getTrack() != nullptr)
    {
        const auto& tempo = e->tempoSequence;
        const auto bar = tempo.toBarsAndBeats (tempo.toTime (getPlayheadBeat())).bars;
        origin = tempo.toBeats (te::tempo::BarsAndBeats { std::max (0, bar), {}, 0 });
    }

    // A newly shown clip opens at bar 1. Otherwise the view stays on the same notes when the clip moves in the song.
    const auto offset = getView().start - getTimeOrigin();
    setTimeOrigin (origin);
    setViewStart (scrollToStart ? origin : origin + offset);
}

te::MidiClip* PianoRoll::makeClipForNewNotes()
{
    // A bar long from the time origin, on the track shown without clips; notes drawn after it make it longer, into the
    // empty space after it (Notes.h's addNote()).
    auto* track = getTrack();

    if (track == nullptr || ! getClips().isEmpty())
        return nullptr;

    auto* clip = insertBarLongMidiClip (*track, getTimeOrigin(), nullptr);

    if (clip != nullptr)
        setClips ({ clip }, true); // shown from now on, where the view is

    return clip;
}

void PianoRoll::clipsChanged()
{
    if (auto* track = getTrack(); drumMode && track != nullptr)
        drumRows = getDrumRows (getSamplerSounds (*track), getNoteNumbersPlayed (getClips()), getDrumRowOrder (*track),
                                getHiddenDrumRows (*track));

    updateVerticalScrollBar();
    updateScrollBar();
    updateToolbar();
    keys->repaint();
    grid->repaint();
    velocityLane->repaint();
}

std::vector<ClipNote> PianoRoll::getAllNotes() const
{
    std::vector<ClipNote> notes;

    for (auto* clip : getClips())
        for (auto* note : clip->getSequence().getNotes())
            notes.push_back ({ clip, note });

    return notes;
}

std::vector<ClipNote> PianoRoll::getSelectedNotesHere() const
{
    if (selectedEvents == nullptr || getSelectedMidiEvents (selection) != selectedEvents.get())
        return {};

    return getSelectedNotes (selection);
}

bool PianoRoll::isSelected (const te::MidiNote& note) const
{
    return selectedEvents != nullptr && getSelectedMidiEvents (selection) == selectedEvents.get() && selectedEvents->isSelected (&note);
}

void PianoRoll::selectNotes (const juce::Array<te::MidiNote*>& notes, bool addToSelection)
{
    if (selectedEvents == nullptr)
        return;

    // Adding to a selection that isn't these notes' starts a new one.
    const auto adding = addToSelection && getSelectedMidiEvents (selection) == selectedEvents.get();
    selectedEvents->setSelected (selection, notes, adding);
    grid->repaint();
    velocityLane->repaint();
}

void PianoRoll::selectAllNotes()
{
    juce::Array<te::MidiNote*> notes;

    for (const auto& n : getAllNotes())
        notes.add (n.note);

    selectNotes (notes, false);
}

te::MidiClip* PianoRoll::getClipForNewNote (te::BeatPosition beat) const
{
    const auto shown = getClips();

    // The target clip if the beat is in it, then any clip it is in, then the target clip if the beat comes after its start.
    if (auto* target = targetClip.get();
        target != nullptr && shown.contains (target) && beat >= target->getStartBeat() && beat < target->getEndBeat())
        return target;

    for (auto* clip : shown)
        if (beat >= clip->getStartBeat() && beat < clip->getEndBeat())
            return clip;

    if (auto* target = targetClip.get(); target != nullptr && shown.contains (target) && beat >= target->getStartBeat())
        return target;

    return nullptr;
}

std::optional<Key> PianoRoll::getKey() const
{
    if (auto* track = getTrack())
        return getTrackKey (*track);

    return std::nullopt;
}

void PianoRoll::setKey (const std::optional<Key>& key)
{
    if (auto* track = getTrack())
        chooseTrackKey (*track, key);

    updateToolbar();
    grid->repaint();
}

//==============================================================================
void PianoRoll::audition (te::MidiClip& clip, int pitch, int velocity)
{
    audition (clip, std::vector<int> { pitch }, velocity);
}

void PianoRoll::audition (te::MidiClip& clip, const std::vector<int>& pitches, int velocity)
{
    stopAudition();
    auto* track = dynamic_cast<te::AudioTrack*> (clip.getTrack());

    if (track == nullptr || ! hasInstrument (*track))
        return;

    // Guide notes go into the track as live MIDI does, so the playback graph has to be running.
    clip.edit.getTransport().ensureContextAllocated();
    const auto channel = clip.getMidiChannel().isValid() ? clip.getMidiChannel() : te::MidiChannel (1);

    for (const auto pitch : pitches)
        track->playGuideNote (pitch, channel, juce::jlimit (1, 127, velocity), false, true, false);

    auditionTrack = track;
}

void PianoRoll::stopAudition()
{
    if (auto* track = auditionTrack.get())
        track->turnOffGuideNotes();

    auditionTrack = nullptr;
}

//==============================================================================
bool PianoRoll::canUseTool (Tool toolToUse) const
{
    return toolToUse != Tool::slip; // notes have nothing inside to slip
}

bool PianoRoll::canNudgeSelection() const
{
    return ! getSelectedNotesHere().empty();
}

void PianoRoll::nudgeSelection (int direction)
{
    nudgeNotes (getSelectedNotesHere(), te::BeatDuration::fromBeats (direction * getGridStepInBeats()));
}

bool PianoRoll::canQuantizeSelection() const
{
    return ! getSelectedNotesHere().empty();
}

void PianoRoll::quantizeSelection()
{
    // With the strength and swing used last, to the grid as it is now.
    auto settings = transform.quantize;
    settings.gridBeats = getGridStepInBeats();
    const auto notes = getSelectedNotesHere();

    juce::Array<te::MidiNote*> added;

    for (const auto& n : replaceNotes (notes, quantize (getNoteData (notes), settings)))
        added.add (n.note);

    selectNotes (added, false);
}

juce::Rectangle<int> PianoRoll::getTimelineArea() const
{
    return grid->getBounds().getUnion (velocityLane->getBounds());
}

std::optional<double> PianoRoll::getLinkedPlayheadBeat (double songBeat) const
{
    // The notes shown play wherever a clip linked to theirs plays: the playhead shows where, in the clip shown.
    return getLinkedPlayBeat (getClips(), songBeat);
}

te::BeatRange PianoRoll::getContentRange() const
{
    const auto shown = getClips();

    if (shown.isEmpty())
        return { getTimeOrigin(), getTimeOrigin() + te::BeatDuration::fromBeats (16.0) };

    auto start = shown.getFirst()->getStartBeat(), end = shown.getFirst()->getEndBeat();

    for (auto* clip : shown)
    {
        start = std::min (start, clip->getStartBeat());
        end = std::max (end, clip->getEndBeat());
    }

    return { start, end };
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
        if (const auto notes = getSelectedNotesHere(); ! notes.empty())
        {
            changeNoteVelocities (notes, notches > 0.0 ? velocityPerNotch : -velocityPerNotch);
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
void PianoRoll::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The MIDI clips selected in the playlist, or a newly selected track's clip. With notes or anything else
    // selected, what is shown stays.
    juce::Array<te::MidiClip*> selectedClips;

    for (auto* clip : getSelectedClips (selection))
        if (auto* midi = dynamic_cast<te::MidiClip*> (clip))
            selectedClips.add (midi);

    if (! selectedClips.isEmpty())
        setClips (selectedClips);
    else if (auto* track = dynamic_cast<te::AudioTrack*> (getSelectedTracks (selection).getFirst());
             track != nullptr && track != getTrack())
        showTrack (*track);

    grid->repaint();
    velocityLane->repaint();
}

void PianoRoll::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    // The shown track's key, say.
    if (auto* track = getTrack(); track != nullptr && tree == track->state)
    {
        triggerAsyncUpdate();
        return;
    }

    // Notes are inside their clip's state; the tempo places clips in time.
    for (auto t = tree; t.isValid(); t = t.getParent())
    {
        if (isClipStateOfType (t) || t.hasType (te::IDs::TEMPOSEQUENCE) || t.hasType (te::IDs::PITCHSEQUENCE))
        {
            triggerAsyncUpdate();
            return;
        }
    }
}

void PianoRoll::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    valueTreePropertyChanged (parent, {});
}

void PianoRoll::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    // A clip shown here left the song: its notes can't stay selected.
    if (isClipStateOfType (child))
    {
        for (const auto& c : clips)
        {
            if (auto* clip = c.get(); clip != nullptr && clip->state == child && selectedEvents != nullptr)
            {
                selectedEvents->deselect();
                selectedEvents = nullptr;
            }
        }
    }

    valueTreePropertyChanged (parent, {});
}

void PianoRoll::selectableObjectAboutToBeDeleted (te::Selectable*)
{
    // The note selection holds pointers to its clips, so it goes now; the clips are sorted out after.
    if (selectedEvents != nullptr)
    {
        selectedEvents->deselect();
        selectedEvents = nullptr;
    }

    triggerAsyncUpdate();
}

void PianoRoll::handleAsyncUpdate()
{
    // Clips that left the song go (a new note selection comes with the rest), and the rest repaints. Bar 1
    // follows a clip moved in the song.
    const auto shown = getClips();

    if ((selectedEvents == nullptr && ! shown.isEmpty()) || shown.size() != (int) clips.size())
        setClips (shown, true);

    updateTimeOrigin (false);
    clipsChanged();
}
} // namespace phazite::ui
