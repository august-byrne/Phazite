// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's transforms (quantize, legato, chop and transpose, with a preview), its toolbar's menus, and its commands.
#include "PianoRollParts.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Tracks.h"
#include "model/mixer/MixerModel.h"
#include "ui/devices/DeviceMenus.h"
#include "ui/input/CommandUI.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
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
    juce::Array<te::MidiNote*> added;

    for (const auto& n : replaceNotes (*e, notes, result, describeTransform()))
        added.add (n.note);

    closeTransform();
    selectNotes (added, false);
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
    const auto trackID = track != nullptr ? getTrackID (*track) : TrackID();
    const auto instrument = getInstrument (context, trackID);
    const auto silent = track != nullptr && ! instrument.has_value();
    instrumentButton.setButtonText (instrument.has_value() ? instrument->name : juce::String ("Choose instrument"));
    instrumentButton.setEnabled (canHoldInstrument (context, trackID));
    instrumentButton.setColour (juce::TextButton::buttonColourId,
                                silent ? p.surfaceRaised.interpolatedWith (p.warning, attentionTint) : p.surfaceRaised);
    instrumentButton.setTooltip (silent
                                     ? "This track has no instrument, so its notes make no sound. Click to choose one: a synth or a sampler"
                                     : "The instrument that plays this track's notes. Click to open or replace it");

    // Which clips are shown, and where in the song: the ruler counts from the clip.
    const auto songBar = [this] (BeatPosition beat) { return juce::String (getBarAt (context, beat) + 1); };

    juce::String text ("Select a MIDI clip or a track in the playlist to edit notes");

    if (const auto shown = getClips(); shown.size() == 1)
        text = shown.getFirst()->getName() + (track != nullptr ? " on " + track->getName() : juce::String()) + ", at bar "
               + songBar (fromTracktion (shown.getFirst()->getStartBeat())) + " of the song";
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

    const auto trackID = getTrackID (*track);
    const auto instrument = getInstrument (context, trackID);

    if (! instrument.has_value())
    {
        showInstrumentMenu (context, trackID, instrumentButton);
        return;
    }

    const juce::Component::SafePointer<juce::Component> safeButton (&instrumentButton);
    juce::PopupMenu menu;
    menu.addItem ("Open " + instrument->name, [&c = context, id = instrument->id] { showDeviceWindow (c, id); });
    menu.addItem ("Replace Instrument...",
                  [&c = context, trackID, safeButton]
                  {
                      if (safeButton != nullptr)
                          showInstrumentMenu (c, trackID, *safeButton);
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
            scales.addItem (getScaleName (scale), true, key.has_value() && key->root == root && key->scale == scale,
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
} // namespace phazite::ui
