// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project, as commands, panels and dialogs find it: what is selected in it, by IDs, the time range selected across
// tracks, its undo manager, and what changes in it. The app's ProjectSession owns the one ProjectContext and is the only
// code (besides tests) that opens a song in it. Views listen to it, to show another project or none. Message thread
// only. The model, the app and tests reach the open song, the selection and the range selection through
// internal/ProjectContext.h; model code given the selection may read selection.getSong(), the same song.
#pragma once

#include "model/arrangement/SongRange.h"
#include "model/arrangement/SongSelection.h"
#include "model/base/IDs.h"

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace phazite
{
class EngineContext;

// Which items of one kind a group of changes was in: those it names, each once, or, after a change that couldn't be tied
// to its items, any of them.
template <typename ItemID>
struct ChangedItems
{
    std::vector<ItemID> named;
    bool any = false;

    bool mayInclude (ItemID id) const { return any || std::find (named.begin(), named.end(), id) != named.end(); }
};

// What changed in the open project's song, in groups, for views to show it again. Changes made at once (in one turn of
// the message loop) come together, after it. View state (model/base/internal/Schema.h), such as how tall a track shows, is no
// change to the song: it has a group of its own. A drag can change the song as often as the mouse moves: views that
// take long to show a change show the last one at their next timer tick instead. A view of many items (the mixer's strips,
// the playlist's clips) shows again only those a change was in, as the groups of clips and devices name them.
struct SongChanges
{
    bool tracks = false;     // tracks added, deleted, moved or changed
    bool devices = false;    // devices or sends added, deleted, moved or changed
    bool clips = false;      // clips added, deleted or changed, their notes included
    bool automation = false; // automation points added, moved or deleted
    bool modulation = false; // modulators and macros added, deleted or changed, and what they modulate
    bool sections = false;   // sections added, deleted or changed
    bool chords = false;     // the chord lane's chords and keys (model/chords/ChordModel.h)
    bool tempo = false;      // the tempo or the time signature
    bool view = false;       // how tracks show: their heights, collapsed and pinned ones, drum rows, automation lanes (view state)
    bool notes = false;      // the project's notes (model/project/ProjectContents.h)

    // With clips: the clips added, changed (their notes included) or removed, and the tracks they were on or went to (both of
    // a clip moved to another track).
    ChangedItems<ClipID> changedClips;
    ChangedItems<TrackID> clipTracks;

    // With devices: the channels (tracks, folders, returns and the master) whose devices or sends changed, and the returns
    // those sends go to; any of them after a plug-in's helper went or one was reloaded, and after a change of where sound can
    // go (a send added or removed, a sidechain's source), which every strip's send knobs follow.
    ChangedItems<TrackID> deviceChannels;
};

class ProjectContext
{
public:
    // Hears when another project is open, or none, at once: before a project that closes goes. And what changes in it.
    // IDs (model/base/IDs.h) name the items of one project, and another project's items can have the same ones, so
    // views forget what they show by ID when the project changes.
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void projectChanged() = 0;
        virtual void songChanged (const SongChanges&) {}

        // The project's versions, its branches or the changes since its last version may have changed
        // (model/versions/VersionsModel.h): at most a few times a second.
        virtual void versionsChanged() {}
    };

    // Plays the song it holds on an engine context, which outlives it (model/engine/internal/EngineContext.h).
    explicit ProjectContext (EngineContext&);
    ~ProjectContext();

    bool hasProject() const noexcept;

    // Which project is open, or none: a number that changes each time another one opens, so that a view told the
    // project changed can tell the same one again from another.
    int getProjectNumber() const noexcept;

    // A number that changes at once with each change to the song that songChanged() will tell of (that comes once a turn
    // of the message loop), and with each project opened: for a view that keeps what it read from the song between
    // paints, and reads it again when this differs.
    int getSongVersion() const noexcept;

    // The open project's song's undo history, for the commands that change it (Command::undoTarget): none while no project
    // is open.
    juce::UndoManager* getUndoManager() const noexcept;

    SongSelection& getSongSelection() noexcept;
    const SongSelection& getSongSelection() const noexcept;
    SongRange& getSongRange() noexcept; // the time range selected across tracks, by beats and IDs
    const SongRange& getSongRange() const noexcept;

    void addListener (Listener*);
    void removeListener (Listener*);

    struct Impl; // the open song, the selection and the range selection (internal/ProjectContext.h)
    Impl& getImpl() const noexcept { return *impl; }

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProjectContext)
};
} // namespace phazite
