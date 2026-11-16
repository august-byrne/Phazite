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
#include <functional>
#include <memory>
#include <vector>

namespace phazite
{
class EngineContext;

// What holds the window's input while work runs (an audio file's work, model/arrangement/internal/AudioWork.h; a hold for
// work to come, ProjectContext::holdForWork()): the app's one (src/app/InputHold.h), which the context hands both
// (ProjectContext::setInputHolder()); none in most tests. Each holdInput() is matched by a releaseInput(); holds are counted.
class InputHolder
{
public:
    virtual ~InputHolder() = default;
    virtual void holdInput() = 0;
    virtual void releaseInput() = 0;
};

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

    // What copies, saves, replaces or records what the plug-ins hold of their own (a preset loaded or saved, a chain
    // duplicated, a track dragged into the browser, every command with Command::needsPluginStates) does it once those that
    // said they changed have given their states: then runs with true to go on, false if it is dropped (the wait's Cancel,
    // another project opened), once (known bug 166). The app's session sets how (asking them, the window holding input,
    // and a wait of more than a second in its progress window); without one, then runs at once. The command registry's
    // gate is this one.
    using PluginStatesGate = std::function<void (std::function<void (bool go)>)>;
    void setPluginStatesGate (PluginStatesGate gate) { pluginStatesGate = std::move (gate); }
    void withPluginStates (std::function<void (bool go)> then);

    // The one way to hold the window while a command waits for work on another thread before it changes the song (the
    // versions' commands that read whole songs: model/versions/internal/VersionHistory.h, startReading(); known bug 126):
    // - From holdForWork() on, the window takes no input: the hold is the input holder's (setInputHolder(): the app's one,
    //   src/app/InputHold.h, which an audio file's work holds too, AudioWork.h), counted with every other, which passes only
    //   the keys that run while a mouse button is down (play and stop, the tools, the editors' zoom) and keys let go; the
    //   message loop runs on; no command that changes a project runs (CommandRegistry::setBusyCheck()), nor does a file
    //   dropped on the window (JUCE gives a drop to no component a modal one blocks).
    // - After a second a progress window shows title, what is waited for ("Reading branch 'idea-2'"), with progress's value
    //   (0 to 1, read at the window's ticks on the message thread: an atomic the work sets; none, or below 0, shows it busy),
    //   through the app's render queue, so that it waits its turn behind another's window, never on top of it. Its Cancel, and
    //   Escape (in the first second too, before it shows: app/ProjectSession.h, cancelHeldWork()), call cancel with no why:
    //   the caller stops its work, lets go of the hold and changes nothing. A quit or the computer's session ending calls
    //   cancel too (ProjectSession::cancelWork(), which gives up an audio file's work alike), with why ('Stopped, as Phazite
    //   was closing'), which the caller says where it says its problems, and the close goes on to its question. cancel is
    //   called on the message thread, at most once, and never after the hold went.
    // - The wait is over as what holdForWork() returns goes. Holds can follow one another, the next made before the one before
    //   goes, without the window taking input in between.
    // The app's session shows the wait (setWorkHolder()); without one (as in most tests), nothing shows, and the hold is still
    //   counted, the input holder's too where there is one. Whether one is held now.
    struct WorkHold
    {
        virtual ~WorkHold() = default;
    };

    using Cancel = std::function<void (const juce::String& why)>;
    using Progress = std::function<float()>;
    using WorkHolder = std::function<std::unique_ptr<WorkHold> (const juce::String& title, Cancel cancel, Progress progress)>;
    void setWorkHolder (WorkHolder holder) { workHolder = std::move (holder); }
    std::unique_ptr<WorkHold> holdForWork (const juce::String& title, Cancel cancel, Progress progress = {});
    bool isHoldingForWork() const noexcept { return held->count > 0; }

    // What holds the window's input, for the work of the model's that holds it (holdForWork(), and an audio file's: AudioWork.h):
    // the app's one, which the main window sets; none in most tests. Another set takes over the holds of the one before.
    void setInputHolder (InputHolder*);
    InputHolder* getInputHolder() const noexcept { return held->holder; }

    struct Impl; // the open song, the selection and the range selection (internal/ProjectContext.h)
    Impl& getImpl() const noexcept { return *impl; }

private:
    std::unique_ptr<Impl> impl;
    PluginStatesGate pluginStatesGate;
    WorkHolder workHolder;
    // How many holds for work are alive, and the input holder they hold, which they let go of as they go.
    struct Held
    {
        int count = 0;
        InputHolder* holder = nullptr;
    };

    std::shared_ptr<Held> held = std::make_shared<Held>();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProjectContext)
};
} // namespace phazite
