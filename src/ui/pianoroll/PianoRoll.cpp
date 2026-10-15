// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumRows.h"
#include "PianoRollParts.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Tracks.h"
#include "model/engine/LiveInput.h"
#include "model/mixer/Mixer.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int keysWidth = 72;
    constexpr int velocityLaneHeight = 72;
    constexpr int transformBarHeight = 40;
    constexpr int minRowHeight = 6, maxRowHeight = 32, drumRowHeight = 22;
    constexpr int numPitches = 128;
    constexpr double scrollPerNotch = 48.0;
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
    auto* e = getEdit();

    if (track == nullptr || e == nullptr || ! getClips().isEmpty())
        return nullptr;

    const auto start = getTimeOrigin();
    const auto bar = te::BeatDuration::fromBeats (e->tempoSequence.getTimeSigAt (start).numerator.get());
    auto* clip = insertMidiClip (*track, { start, start + bar }, nullptr);

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
    {
        track->edit.getUndoManager().beginNewTransaction (key.has_value() ? "Set Key" : "Turn Key Off");
        setTrackKey (*track, key);
    }

    updateToolbar();
    grid->repaint();
}

//==============================================================================
std::vector<NoteData> PianoRoll::getPreview() const
{
    if (! transformOpen)
        return {};

    const auto notes = getNoteData (getSelectedNotesHere());

    switch (transform.kind)
    {
        case Transform::Kind::quantize:
        {
            auto settings = transform.quantize;
            settings.gridBeats = getGridStepInBeats(); // the grid as it is now
            return quantize (notes, settings);
        }

        case Transform::Kind::legato:
            return legato (notes, transform.legatoGapBeats);

        case Transform::Kind::chop:
            return chop (notes, getGridStepInBeats());

        case Transform::Kind::transpose:
        default:
            return phazite::transpose (notes, transform.transposeSteps, transform.transposeInKey ? getKey() : std::optional<Key>());
    }
}

juce::String PianoRoll::describeTransform() const
{
    switch (transform.kind)
    {
        case Transform::Kind::quantize:
            return "Quantize Notes";
        case Transform::Kind::legato:
            return "Legato";
        case Transform::Kind::chop:
            return "Chop Notes";
        case Transform::Kind::transpose:
        default:
            return "Transpose Notes";
    }
}

void PianoRoll::openTransform (Transform::Kind kind)
{
    if (getSelectedNotesHere().empty())
        return;

    transform.kind = kind;
    transformOpen = true;
    transformBar->show (transform);
    transformBar->setVisible (true);
    resized();
    grid->repaint();
}

void PianoRoll::closeTransform()
{
    if (! transformOpen)
        return;

    transformOpen = false;
    transformBar->setVisible (false);
    resized();
    grid->repaint();
}

void PianoRoll::applyTransform()
{
    auto* e = getEdit();
    const auto notes = getSelectedNotesHere();

    if (e == nullptr || ! transformOpen || notes.empty())
        return;

    const auto result = getPreview();
    e->getUndoManager().beginNewTransaction (describeTransform());

    juce::Array<te::MidiNote*> added;

    for (const auto& n : replaceNotes (notes, result))
        added.add (n.note);

    closeTransform();
    selectNotes (added, false);
}

//==============================================================================
int PianoRoll::getRowHeight() const
{
    return drumMode ? drumRowHeight : rowHeight;
}

int PianoRoll::getNumRows() const
{
    return drumMode ? (int) drumRows.size() : numPitches;
}

int PianoRoll::getRowForPitch (int pitch) const
{
    if (! drumMode)
        return juce::isPositiveAndBelow (pitch, numPitches) ? numPitches - 1 - pitch : -1;

    for (size_t i = 0; i < drumRows.size(); ++i)
        if (drumRows[i].note == pitch)
            return (int) i;

    return -1;
}

int PianoRoll::getPitchForRow (int row) const
{
    if (! juce::isPositiveAndBelow (row, getNumRows()))
        return -1;

    return drumMode ? drumRows[(size_t) row].note : numPitches - 1 - row;
}

int PianoRoll::getRowTop (int row) const
{
    return row * getRowHeight() - scrollY;
}

int PianoRoll::getRowAt (int y) const
{
    const auto position = y + scrollY;
    const auto row = position >= 0 ? position / getRowHeight() : -1;
    return juce::isPositiveAndBelow (row, getNumRows()) ? row : -1;
}

void PianoRoll::setScrollY (int newScrollY)
{
    const auto maxScroll = std::max (0, getNumRows() * getRowHeight() - grid->getHeight());
    scrollY = juce::jlimit (0, maxScroll, newScrollY);
    updateVerticalScrollBar();
    keys->repaint();
    grid->repaint();
}

void PianoRoll::scrollToNotes()
{
    // The middle of the notes' range, or middle C (or the first drum) without notes.
    auto lowest = 127, highest = 0;

    for (const auto& n : getAllNotes())
    {
        lowest = std::min (lowest, n.note->getNoteNumber());
        highest = std::max (highest, n.note->getNoteNumber());
    }

    if (drumMode)
    {
        setScrollY (0);
        return;
    }

    const auto middle = lowest <= highest ? (lowest + highest) / 2 : 60;
    setScrollY (getRowTop (getRowForPitch (middle)) + scrollY - grid->getHeight() / 2);
}

void PianoRoll::updateVerticalScrollBar()
{
    const auto total = std::max (1, getNumRows() * getRowHeight());
    verticalScrollBar.setRangeLimits (0.0, total, juce::dontSendNotification);
    verticalScrollBar.setCurrentRange (scrollY, std::max (1, grid->getHeight()), juce::dontSendNotification);
    verticalScrollBar.setSingleStepSize (scrollPerNotch);
}

juce::Rectangle<float> PianoRoll::getNoteBounds (const NoteData& note) const
{
    const auto row = getRowForPitch (note.pitch);

    if (row < 0)
        return {};

    const auto& timeline = getView();
    const auto top = (float) getRowTop (row) + 1.0f;
    const auto height = (float) getRowHeight() - 2.0f;
    const auto left = (float) timeline.beatToX (note.start);

    // In drum mode a note is a hit, drawn as a diamond where it starts.
    if (drumMode)
        return { left - height / 2.0f, top, height, height };

    const auto right = (float) timeline.beatToX (note.getEnd());
    return { left, top, std::max (3.0f, right - left), height };
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
void PianoRoll::updateToolbar()
{
    auto* track = getTrack();
    const auto key = getKey();

    juce::String chordName ("Off");

    for (const auto& shape : getChordShapes())
        if (chordStamp == shape.type)
            chordName = shape.name;

    chordButton.setButtonText ("Chord: " + chordName);
    keyButton.setButtonText ("Key: " + (key.has_value() ? describeKey (*key) + (snapToKey ? " (snap)" : "") : juce::String ("Off")));
    ghostButton.setToggleState (showGhostNotes, juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &chordButton, &keyButton, &ghostButton, &transformButton })
        c->setEnabled (track != nullptr);

    // The instrument that plays the notes, or a request for one: without it they make no sound.
    const auto& p = getPalette (*this);
    auto* instrument = track != nullptr ? getInstrument (*track) : nullptr;
    const auto silent = track != nullptr && instrument == nullptr;
    instrumentButton.setButtonText (instrument != nullptr ? instrument->getName() : juce::String ("Choose instrument"));
    instrumentButton.setEnabled (track != nullptr && getReturnBus (*track) < 0);
    instrumentButton.setColour (juce::TextButton::buttonColourId,
                                silent ? p.surfaceRaised.interpolatedWith (p.warning, attentionTint) : p.surfaceRaised);
    instrumentButton.setTooltip (silent
                                     ? "This track has no instrument, so its notes make no sound. Click to choose one: a synth or a sampler"
                                     : "The instrument that plays this track's notes. Click to open or replace it");

    // Which clips are shown, and where in the song: the ruler counts from the clip.
    const auto songBar = [this] (te::BeatPosition beat)
    {
        const auto& tempo = getEdit()->tempoSequence;
        return juce::String (tempo.toBarsAndBeats (tempo.toTime (beat)).bars + 1);
    };

    juce::String text ("Select a MIDI clip or a track in the playlist to edit notes");

    if (const auto shown = getClips(); shown.size() == 1)
        text = shown.getFirst()->getName() + (track != nullptr ? " on " + track->getName() : juce::String()) + ", at bar "
               + songBar (shown.getFirst()->getStartBeat()) + " of the song";
    else if (shown.size() > 1)
        text = juce::String (shown.size()) + " clips, from bar " + songBar (getTimeOrigin()) + " of the song";
    else if (track != nullptr)
        text = track->getName() + ": draw a note to start a clip at bar " + songBar (getTimeOrigin()) + " of the song";

    clipLabel.setText (text, juce::dontSendNotification);
}

void PianoRoll::instrumentClicked()
{
    auto* track = getTrack();

    if (track == nullptr)
        return;

    auto* instrument = getInstrument (*track);

    if (instrument == nullptr)
    {
        showInstrumentMenu (*track, instrumentButton);
        return;
    }

    const te::SafeSelectable<te::Plugin> safeInstrument (instrument);
    const te::SafeSelectable<te::AudioTrack> safeTrack (track);
    const juce::Component::SafePointer<juce::Component> safeButton (&instrumentButton);
    juce::PopupMenu menu;

    menu.addItem ("Open " + instrument->getName(),
                  [safeInstrument]
                  {
                      if (auto* opened = safeInstrument.get())
                          opened->showWindowExplicitly();
                  });

    menu.addItem ("Replace Instrument...",
                  [safeTrack, safeButton]
                  {
                      if (auto* t = safeTrack.get(); t != nullptr && safeButton != nullptr)
                          showInstrumentMenu (*t, *safeButton);
                  });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&instrumentButton));
}

void PianoRoll::showChordMenu()
{
    juce::PopupMenu menu;
    menu.addItem ("Off: one note per click", true, ! chordStamp.has_value(),
                  [this]
                  {
                      chordStamp.reset();
                      updateToolbar();
                  });
    menu.addSectionHeader ("A click places the chord, from the note clicked");

    for (const auto& shape : getChordShapes())
    {
        const auto type = shape.type;
        menu.addItem (shape.name, true, chordStamp == type,
                      [this, type]
                      {
                          chordStamp = type;
                          updateToolbar();
                      });
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&chordButton));
}

void PianoRoll::showKeyMenu()
{
    auto* track = getTrack();

    if (track == nullptr)
        return;

    // Off, or a root and a scale. Rows outside the key are shaded, and notes can snap to it.
    const auto key = getKey();
    juce::PopupMenu menu;
    menu.addSectionHeader ("The key of " + track->getName());
    menu.addItem ("Off: no shading or snapping", true, ! key.has_value(), [this] { setKey (std::nullopt); });

    for (int root = 0; root < 12; ++root)
    {
        juce::PopupMenu scales;

        for (const auto scale : getKeyScales())
        {
            const Key choice { root, scale };
            scales.addItem (te::Scale::getNameForType (scale), true, key.has_value() && key->root == root && key->scale == scale,
                            [this, choice] { setKey (choice); });
        }

        menu.addSubMenu (juce::MidiMessage::getMidiNoteName (root, true, false, 0), scales, true, nullptr,
                         key.has_value() && key->root == root);
    }

    menu.addSeparator();
    addCommandItems (menu, getCommands(), { "pianoRoll.snapToKey", "pianoRoll.keyForAllTracks" });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&keyButton));
}

void PianoRoll::showTransformMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Show what it does first");
    addCommandItems (menu, getCommands(),
                     { "note.quantizeSettings", "note.legatoSettings", "note.chopSettings", "note.transposeSettings" });
    menu.addSectionHeader ("Right away");
    addCommandItems (menu, getCommands(), { "edit.quantize", "note.legato", "note.chop" });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&transformButton));
}

void PianoRoll::setDrumMode (bool on)
{
    auto* track = getTrack();

    if (track != nullptr)
        setDrumTrack (*track, on);

    drumMode = on && track != nullptr;
    clipsChanged();
    scrollToNotes();
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
void PianoRoll::addCommands (CommandRegistry& registry)
{
    const auto hasNotes = [this] { return ! getSelectedNotesHere().empty(); };

    registry.add ({ .id = "note.selectAll",
                    .name = "Select All Notes",
                    .category = "Note",
                    .description = "Selects every note of the clips in the piano roll.",
                    .synonyms = { "all notes", "everything" },
                    .perform = [this] { selectAllNotes(); },
                    .isEnabled = [this] { return isActive() && ! getClips().isEmpty(); },
                    .keyPriority = Command::notePriority });

    struct TransformCommand
    {
        Transform::Kind kind;
        const char* id;
        const char* name;
        const char* description;
    };

    for (const auto& command :
         { TransformCommand { Transform::Kind::quantize, "note.quantizeSettings", "Quantize...",
                              "Shows how quantizing would move the selected notes, with its strength and swing, before you apply it." },
           TransformCommand { Transform::Kind::legato, "note.legatoSettings", "Legato...",
                              "Shows the selected notes lasting until the next ones start, with a gap you choose, before you apply it." },
           TransformCommand { Transform::Kind::chop, "note.chopSettings", "Chop...",
                              "Shows the selected notes cut into pieces of the grid's length before you apply it." },
           TransformCommand { Transform::Kind::transpose, "note.transposeSettings", "Transpose...",
                              "Shows the selected notes moved up or down, in semitones or steps of the key, before you apply it." } })
    {
        const auto kind = command.kind;
        registry.add ({ .id = command.id,
                        .name = command.name,
                        .category = "Note",
                        .description = command.description,
                        .synonyms = { "transform", "preview", "midi tools" },
                        .perform = [this, kind] { openTransform (kind); },
                        .isEnabled = hasNotes,
                        .keyPriority = Command::notePriority });
    }

    registry.add ({ .id = "note.applyTransform",
                    .name = "Apply Transform",
                    .category = "Note",
                    .description = "Makes the change the piano roll previews.",
                    .synonyms = { "commit", "ok" },
                    .perform = [this] { applyTransform(); },
                    .isEnabled = [this, hasNotes] { return transformOpen && hasNotes(); },
                    .keyPriority = Command::notePriority });

    registry.add ({ .id = "note.cancelTransform",
                    .name = "Cancel Transform",
                    .category = "Note",
                    .description = "Closes the transform the piano roll previews, without changing the notes.",
                    .synonyms = { "close", "discard" },
                    .perform = [this] { closeTransform(); },
                    .isEnabled = [this] { return transformOpen; },
                    .keyPriority = Command::notePriority });

    // View settings: changing them isn't an edit of the song.
    registry.add ({ .id = "pianoRoll.drumMode",
                    .name = "Drum Mode",
                    .category = "Piano Roll",
                    .description = "Shows the rows as drums, named after them, with notes as hits. Drum tracks start in it.",
                    .synonyms = { "drums", "beats", "pads", "drum map" },
                    .perform = [this] { setDrumMode (! drumMode); },
                    .isEnabled = [this] { return getTrack() != nullptr; },
                    .isTicked = [this] { return drumMode; } });

    registry.add ({ .id = "pianoRoll.ghostNotes",
                    .name = "Ghost Notes",
                    .category = "Piano Roll",
                    .description = "Shows faintly the notes of other tracks' clips. Double-right-click one to edit its clip.",
                    .synonyms = { "other tracks", "background notes", "ghost channels" },
                    .perform =
                        [this]
                    {
                        showGhostNotes = ! showGhostNotes;
                        updateToolbar();
                        grid->repaint();
                    },
                    .isTicked = [this] { return showGhostNotes; } });

    registry.add ({ .id = "pianoRoll.snapToKey",
                    .name = "Snap to Key",
                    .category = "Piano Roll",
                    .description = "Places new notes, and notes dragged up or down, only on the track's key.",
                    .synonyms = { "scale", "scale lock", "key lock", "in key" },
                    .perform =
                        [this]
                    {
                        snapToKey = ! snapToKey;
                        updateToolbar();
                    },
                    .isEnabled = [this] { return getKey().has_value(); },
                    .isTicked = [this] { return snapToKey && getKey().has_value(); } });

    registry.add ({ .id = "pianoRoll.keyForAllTracks",
                    .name = "Use This Key on Every Track",
                    .category = "Piano Roll",
                    .description = "Gives every track the key of the track in the piano roll.",
                    .synonyms = { "song key", "global key", "scale for all", "same key" },
                    .perform =
                        [this]
                    {
                        if (auto* e = getEdit())
                            setTrackKeys (getPlaylistTracks (*e), getKey());
                    },
                    .isEnabled = [this] { return getKey().has_value(); },
                    .undoTarget = [this] { return getEdit(); } });

    refreshShortcutTexts();
}

void PianoRoll::refreshShortcutTexts()
{
    TimelineEditor::refreshShortcutTexts();
    chordButton.setTooltip ("Chord stamp: a click places a whole chord of this kind instead of one note");
    keyButton.setTooltip ("The track's key, if you choose one: rows outside it are shaded, and notes can snap to it");
    ghostButton.setTooltip (withShortcut (getCommands(), "Ghost notes: other tracks' notes, shown faintly", "pianoRoll.ghostNotes"));
    transformButton.setTooltip ("Quantize, legato, chop and transpose the selected notes, with a preview first");
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

    horizontalScrollBar.setBounds (area.removeFromBottom (scrollBarSize).withTrimmedLeft (keysWidth).withTrimmedRight (scrollBarSize));

    if (transformOpen)
        transformBar->setBounds (area.removeFromBottom (transformBarHeight));

    auto velocityArea = area.removeFromBottom (velocityLaneHeight);
    auto rulerArea = area.removeFromTop (rulerHeight);
    verticalScrollBar.setBounds (area.removeFromRight (scrollBarSize));
    keys->setBounds (area.removeFromLeft (keysWidth));
    grid->setBounds (area);
    ruler.setBounds (rulerArea.withTrimmedLeft (keysWidth).withTrimmedRight (scrollBarSize));
    velocityLane->setBounds (velocityArea.withTrimmedLeft (keysWidth).withTrimmedRight (scrollBarSize));

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
            const auto change = notches > 0.0 ? velocityPerNotch : -velocityPerNotch;
            notes.front().clip->edit.getUndoManager().beginNewTransaction ("Change Velocity");

            for (const auto& n : notes)
                setNoteVelocities ({ n }, n.note->getVelocity() + change);
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
