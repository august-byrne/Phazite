// SPDX-License-Identifier: AGPL-3.0-or-later
// The time line's editors in windows, as the app shows them, for the tests that use their tools with the mouse
// (EditorToolTests.cpp) and change the song while the mouse is down (EditorDragTests.cpp): the playlist and the piano roll,
// each with the commands the app gives it and the keys that run them (ui/input/KeyDispatcher.h), the grid fixed at a beat, so
// that a test knows where every snap lands. What they act on is named through the model, by IDs.
#pragma once

#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/commands/internal/Undo.h"
#include "model/commands/internal/UndoCommands.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/playlist/ClipComponent.h"
#include "ui/playlist/Playlist.h"
#include "ui/playlist/TempoLane.h"
#include "ui/playlist/TrackHeights.h"

namespace phazite::test
{
inline BeatPosition beatAt (double beat)
{
    return BeatPosition::fromBeats (beat);
}

// What the app's message loop delivers between two of the user's events: the editors hear of what the song's changes moved,
// which a test's next press finds where it is.
inline void deliverChanges()
{
    dispatchPendingMessages();
}

// The grid at a beat, fixed, snapping.
inline GridSettings getBeatGrid()
{
    GridSettings grid;
    grid.adaptive = false;
    grid.fixedLevel = 9; // a beat
    return grid;
}

// The keys the tests press, as the default keymap has them (resources/keymaps/phazite.json).
inline juce::KeyPress getKey (const char* description)
{
    return juce::KeyPress::createFromDescription (description);
}

//==============================================================================
// A playlist in a window with three tracks, its grid a beat, with the commands the app gives it (undo, the selection's,
// the tracks', the tempo's and its own) and the keys that run them.
struct PlaylistScreen
{
    PlaylistScreen()
    {
        tracks = ensureTracks (*song, 3);
        song->getUndoManager().clearUndoHistory();

        addUndoCommands (registry, getSelection (context));
        addSelectionCommands (registry, getSelection (context), getRangeSelection (context));
        addTrackCommands (registry, getSelection (context));
        addTempoCommands (registry, getSelection (context));
        ui::addTimelineCommands (registry, [this] { return &playlist; });
        playlist.addCommands (registry);
        setSong (context, song.get());
        window.addKeyListener (&keys); // as the main window does
        playlist.setGrid (getBeatGrid());
        dispatchPendingMessages();

        lanes = findComponentWithTitle (playlist, "Tracks");
        REQUIRE (lanes != nullptr);
    }

    ~PlaylistScreen()
    {
        window.removeKeyListener (&keys);
        setSong (context, nullptr);
    }

    Song& getSong() const { return *song; }

    // How tall a track's row is, and where a beat is on it, in the lanes: its middle, and the lower half, where the Smart Tool
    // moves a clip, and the upper half, where it selects time.
    int heightOf (int track) const { return ui::getTrackHeight (getTrackRow (context, tracks[(size_t) track]).value_or (TrackRow())); }

    juce::Point<int> at (double beat, int track) const
    {
        auto y = 0;

        for (int i = 0; i < track; ++i)
            y += heightOf (i);

        return { juce::roundToInt (playlist.getView().beatToX (beatAt (beat))), y + heightOf (track) / 2 };
    }

    juce::Point<int> lower (double beat, int track) const { return at (beat, track).translated (0, heightOf (track) / 4); }
    juce::Point<int> upper (double beat, int track) const { return at (beat, track).translated (0, -heightOf (track) / 4); }

    // A beat's x, and how many pixels a number of beats is.
    int xOf (double beat) const { return juce::roundToInt (playlist.getView().beatToX (beatAt (beat))); }
    int widthOf (double beats) const { return juce::roundToInt (beats * playlist.getView().pixelsPerBeat); }

    ClipID addMidiClip (int track, double start, double length)
    {
        const auto clip = insertMidiClip (*song, tracks[(size_t) track], { beatAt (start), beatAt (start + length) }, nullptr);
        REQUIRE (clip.isValid());
        return clip;
    }

    // An audio clip of a sine on a track, from a beat, as long as its seconds are (at 120 BPM, two beats a second).
    ClipID addAudioClip (int track, double start, double seconds = 4.0)
    {
        const auto file = writeSound (temp.folder.getChildFile ("Sound " + juce::String (++sounds) + ".wav"), seconds);
        const auto imported = importFiles (context, { file.getFullPathName() }, tracks[(size_t) track], beatAt (start));
        REQUIRE (imported.size() == 1);
        return imported[0];
    }

    // Where a clip shows, in the lanes; nothing while it has no component (out of view).
    juce::Rectangle<int> boundsOf (ClipID clip)
    {
        for (auto* component : findAll<ui::ClipComponent> (playlist))
            if (component->getClip() == clip)
                return lanes->getLocalArea (component, component->getLocalBounds());

        return {};
    }

    ClipInfo get (ClipID clip) const
    {
        const auto info = getClip (context, clip);
        REQUIRE (info.has_value());
        return *info;
    }

    bool has (ClipID clip) const { return getClip (context, clip).has_value(); }

    std::vector<double> startsOn (int track) const
    {
        std::vector<double> starts;

        for (const auto& clip : getClips (context, tracks[(size_t) track]))
            starts.push_back (clip.range.getStart().inBeats());

        return starts;
    }

    juce::String describeUndo() const { return phazite::describeUndo (song.get()); }
    SongSelection& selection() { return context.getSongSelection(); }

    TemporaryFolder temp;
    int sounds = 0;
    Song::Ptr song = createSong(); // 120 BPM, 4/4
    std::vector<TrackID> tracks;
    ProjectContext context { getEngineContext() }; // the open project, as the app has it
    CommandRegistry registry;
    ui::CommandBridge commands { registry }; // with the default keymap
    ui::KeyDispatcher keys { commands };
    ui::Playlist playlist { commands, context };
    TestWindow window { playlist, 1000, 600 };
    juce::Component* lanes = nullptr;
};

//==============================================================================
// A piano roll in a window, with two tracks and a clip of 8 beats on the first, shown across the grid, its grid a beat, with
// the commands the app gives it and the keys that run them.
struct PianoRollScreen
{
    PianoRollScreen()
    {
        tracks = ensureTracks (*song, 2);
        clip = insertMidiClip (*song, tracks[0], { beatAt (0.0), beatAt (8.0) }, nullptr);
        REQUIRE (clip.isValid());
        song->getUndoManager().clearUndoHistory();

        addUndoCommands (registry, getSelection (context));
        addSelectionCommands (registry, getSelection (context), getRangeSelection (context));
        ui::addTimelineCommands (registry, [this] { return &pianoRoll; });
        pianoRoll.addCommands (registry);
        setSong (context, song.get());
        window.addKeyListener (&keys);
        getSelection (context).selectOnly (clip);
        dispatchPendingMessages();
        pianoRoll.zoomTo ({ beatAt (0.0), beatAt (8.0) });
        pianoRoll.setGrid (getBeatGrid());
        pianoRoll.setActive (true);

        notes = findComponentWithTitle (pianoRoll, "Notes");
        velocity = findComponentWithTitle (pianoRoll, "Velocity");
        REQUIRE (notes != nullptr);
        REQUIRE (velocity != nullptr);
    }

    ~PianoRollScreen()
    {
        window.removeKeyListener (&keys);
        setSong (context, nullptr);
    }

    Song& getSong() const { return *song; }

    // Where a beat is in the grid, on the middle row, or on the row of a pitch.
    juce::Point<int> at (double beat) const { return { xOf (beat), notes->getHeight() / 2 }; }
    int xOf (double beat) const { return juce::roundToInt (pianoRoll.getView().beatToX (beatAt (beat))); }
    int widthOf (double beats) const { return juce::roundToInt (beats * pianoRoll.getView().pixelsPerBeat); }
    // The rows, a pitch each from 127 down, as tall as the rows' scroll bar says (it spans the 128 of them).
    int rowHeight() const
    {
        for (auto* bar : findAll<juce::ScrollBar> (const_cast<ui::PianoRoll&> (pianoRoll)))
            if (bar->isVertical() && bar->findParentComponentOfClass<juce::Viewport>() == nullptr) // not the settings'
                return juce::roundToInt (bar->getMaximumRangeLimit() / 128.0);

        return 0;
    }

    int pitchAt (int y) const { return 127 - (y + pianoRoll.getScrollY()) / rowHeight(); }
    int yOf (int pitch) const { return (127 - pitch) * rowHeight() - pianoRoll.getScrollY() + rowHeight() / 2; }
    int middlePitch() const { return pitchAt (notes->getHeight() / 2); }

    // A note added through the model, on the middle row (or as many rows above it) from a beat, a beat long.
    NoteRef addNote (double start, int rowsUp = 0, double length = 1.0, int noteVelocity = 100)
    {
        const auto note =
            phazite::addNote (context, clip, middlePitch() + rowsUp, beatAt (start), BeatDuration::fromBeats (length), noteVelocity);
        REQUIRE (note.isValid());
        dispatchPendingMessages();
        return note;
    }

    NoteData get (NoteRef note) const
    {
        const auto data = getNote (context, note);
        REQUIRE (data.has_value());
        return *data;
    }

    bool has (NoteRef note) const { return getNote (context, note).has_value(); }
    std::vector<NoteData> getAll() const { return getNotes (context, { clip }); }
    std::vector<NoteRef> getSelected() const { return context.getSongSelection().getNotes(); }

    // The middle of a note, between its edges' zones.
    juce::Point<int> middleOf (NoteRef note) const
    {
        const auto data = get (note);
        return { xOf (data.start.inBeats() + data.length.inBeats() / 2.0), yOf (data.pitch) };
    }

    juce::String describeUndo() const { return phazite::describeUndo (song.get()); }
    SongSelection& selection() { return context.getSongSelection(); }

    Song::Ptr song = createSong(); // 120 BPM, 4/4
    std::vector<TrackID> tracks;
    ClipID clip;
    ProjectContext context { getEngineContext() };
    CommandRegistry registry;
    ui::CommandBridge commands { registry };
    ui::KeyDispatcher keys { commands };
    ui::PianoRoll pianoRoll { commands, context };
    TestWindow window { pianoRoll, 1176, 564 };
    juce::Component* notes = nullptr;
    juce::Component* velocity = nullptr;
};
} // namespace phazite::test
