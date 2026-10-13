// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CommandUI.h"
#include "LookAndFeel.h"
#include "PianoRollParts.h"
#include "model/Clips.h"
#include "model/LiveInput.h"
#include "model/Tracks.h"

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

    const juce::Identifier pianoRollStateType { "PIANOROLL" };

    bool isClipStateOfType (const juce::ValueTree& state)
    {
        return state.hasType (te::IDs::MIDICLIP);
    }
} // namespace

//==============================================================================
PianoRoll::PianoRoll (CommandRegistry& registry, te::SelectionManager& selectionManager)
    : TimelineEditor (registry, pianoRollStateType),
      selection (selectionManager),
      keys (std::make_unique<Keys> (*this)),
      grid (std::make_unique<NoteGrid> (*this)),
      velocityLane (std::make_unique<VelocityLane> (*this)),
      transformBar (std::make_unique<TransformBar> (*this))
{
    setTitle ("Piano roll");

    chordButton.setTitle ("Chord stamp");
    chordButton.onClick = [this] { showChordMenu(); };
    keyButton.setTitle ("Key");
    keyButton.onClick = [this] { showKeyMenu(); };
    transformButton.onClick = [this] { showTransformMenu(); };

    // These run their commands, so the palette, the keys and the buttons agree.
    for (auto [button, id] : { std::pair { &drumButton, "pianoRoll.drumMode" }, std::pair { &ghostButton, "pianoRoll.ghostNotes" } })
    {
        button->setClickingTogglesState (true);
        button->onClick = [this, commandID = juce::String (id)] { getCommands().invoke (commandID); };
    }

    clipLabel.setJustificationType (juce::Justification::centredRight);
    clipLabel.setFont (juce::FontOptions (TextSize::small));

    for (auto* c : std::initializer_list<juce::Component*> { &chordButton, &keyButton, &drumButton, &ghostButton, &transformButton,
                                                             &clipLabel, keys.get(), grid.get(), velocityLane.get() })
        addAndMakeVisible (c);

    addChildComponent (*transformBar);
    selection.addChangeListener (this);
    setTool (Tool::smart);
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

void PianoRoll::setClips (const juce::Array<te::MidiClip*>& newClips)
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

    // Drum mode follows the first clip's track.
    auto* track = newClips.isEmpty() ? nullptr : dynamic_cast<te::AudioTrack*> (newClips.getFirst()->getTrack());
    drumMode = track != nullptr && isDrumTrack (*track);

    if (transformOpen)
        closeTransform();

    clipsChanged();
    scrollToNotes();
    updateToolbar();
}

void PianoRoll::clipsChanged()
{
    if (auto* first = getClips().getFirst(); drumMode && first != nullptr)
        if (auto* track = dynamic_cast<te::AudioTrack*> (first->getTrack()))
            drumRows = getDrumRows (*track, getClips());

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

Key PianoRoll::getKey() const
{
    if (auto* e = getEdit())
        return getKeyAt (*e, getClips().isEmpty() ? te::BeatPosition() : getClips().getFirst()->getStartBeat());

    return {};
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
            return phazite::transpose (notes, transform.transposeSteps,
                                       transform.transposeInKey ? std::optional<Key> (getKey()) : std::optional<Key>());
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
    const auto hasClips = ! getClips().isEmpty();

    juce::String chordName ("Off");

    for (const auto& shape : getChordShapes())
        if (chordStamp == shape.type)
            chordName = shape.name;

    chordButton.setButtonText ("Chord: " + chordName);
    keyButton.setButtonText ("Key: " + describeKey (getKey()) + (snapToKey ? " (snap)" : ""));
    drumButton.setToggleState (drumMode, juce::dontSendNotification);
    ghostButton.setToggleState (showGhostNotes, juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &chordButton, &keyButton, &drumButton, &ghostButton, &transformButton })
        c->setEnabled (hasClips);

    // Which clips are shown, and on which track.
    juce::String text ("Select a MIDI clip in the playlist to edit its notes");

    if (const auto shown = getClips(); shown.size() == 1)
        text =
            shown.getFirst()->getName() + (shown.getFirst()->getTrack() != nullptr ? " on " + shown.getFirst()->getTrack()->getName() : "");
    else if (shown.size() > 1)
        text = juce::String (shown.size()) + " clips";

    clipLabel.setText (text, juce::dontSendNotification);
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
    juce::PopupMenu menu;
    menu.addSectionHeader ("The song is in " + describeKey (getKey()));
    menu.addItem ("Rows outside the key are shaded; the key can be changed in a later version", false, false, [] {});
    menu.addSeparator();
    addCommandItems (menu, getCommands(), { "pianoRoll.snapToKey" });
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
    if (auto* first = getClips().getFirst())
        if (auto* track = dynamic_cast<te::AudioTrack*> (first->getTrack()))
            setDrumTrack (*track, on);

    drumMode = on && ! getClips().isEmpty();
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
        return { te::BeatPosition(), te::BeatPosition::fromBeats (16.0) };

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
                    .description = "Shows the rows as drums, named after them, with notes as hits.",
                    .synonyms = { "drums", "beats", "pads", "drum map" },
                    .perform = [this] { setDrumMode (! drumMode); },
                    .isEnabled = [this] { return ! getClips().isEmpty(); },
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
                    .description = "Places new notes, and notes dragged up or down, only on the song's key.",
                    .synonyms = { "scale", "scale lock", "key lock", "in key" },
                    .perform =
                        [this]
                    {
                        snapToKey = ! snapToKey;
                        updateToolbar();
                    },
                    .isTicked = [this] { return snapToKey; } });

    refreshShortcutTexts();
}

void PianoRoll::refreshShortcutTexts()
{
    TimelineEditor::refreshShortcutTexts();
    chordButton.setTooltip ("Chord stamp: a click places a whole chord of this kind instead of one note");
    keyButton.setTooltip ("The song's key. Rows outside it are shaded; turn on snapping to place notes only on it");
    drumButton.setTooltip (withShortcut (getCommands(), "Drum mode: rows named after the drums, and notes as hits", "pianoRoll.drumMode"));
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

    for (auto [button, width] : { std::pair { &chordButton, 120 }, std::pair { &keyButton, 140 }, std::pair { &drumButton, 64 },
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
    // The MIDI clips selected in the playlist; with notes or anything else selected, the clips stay.
    juce::Array<te::MidiClip*> selectedClips;

    for (auto* clip : getSelectedClips (selection))
        if (auto* midi = dynamic_cast<te::MidiClip*> (clip))
            selectedClips.add (midi);

    if (! selectedClips.isEmpty())
        setClips (selectedClips);

    grid->repaint();
    velocityLane->repaint();
}

void PianoRoll::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
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
    // Clips that left the song go (a new note selection comes with the rest), and the rest repaints.
    const auto shown = getClips();

    if (selectedEvents == nullptr || shown.size() != (int) clips.size())
        setClips (shown);

    clipsChanged();
}
} // namespace phazite::ui
