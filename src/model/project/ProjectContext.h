// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project, as commands, panels and dialogs find it: what is selected in it, by IDs, the time range selected across
// tracks, its undo manager, and what changes in it. The app's ProjectSession owns the one ProjectContext and is the only
// code (besides tests) that opens an Edit in it. Views listen to it, to show another project or none. Message thread
// only.
//
// No Tracktion here (architecture phase A3): the model, the app and tests reach the open Edit, Tracktion's selection
// manager and the range selection through internal/ProjectContext.h; model code given the selection manager may read
// selection.getEdit(), the same Edit. Tracktion's focused Edit and selection (UIBehaviour, which live MIDI and controller
// mappings use) follow them; Phazite's code never reads those.
#pragma once

#include "model/arrangement/SongRange.h"
#include "model/arrangement/SongSelection.h"

#include <juce_data_structures/juce_data_structures.h>

#include <memory>

namespace phazite
{
class EngineContext;
class UIBehaviour;

// What changed in the open project's song, in groups, for views to show it again. Changes made at once (in one turn of
// the message loop) come together, after it. View state (model/base/internal/Schema.h), such as how tall a track shows, is no
// change to the song: it has a group of its own. A drag can change the song as often as the mouse moves: views that
// take long to show a change show the last one at their next timer tick instead.
struct SongChanges
{
    bool tracks = false;   // tracks added, deleted, moved or changed
    bool devices = false;  // devices or sends added, deleted, moved or changed
    bool clips = false;    // clips added, deleted or changed, their notes included
    bool sections = false; // sections added, deleted or changed
    bool tempo = false;    // the tempo or the time signature
    bool view = false;     // how tracks show: their heights, collapsed and pinned ones, drum rows (view state)
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
    };

    // Keeps Tracktion's focus (the Engine's UIBehaviour) in step, if given one.
    explicit ProjectContext (EngineContext&, UIBehaviour* tracktionFocus = nullptr);
    ~ProjectContext();

    bool hasProject() const noexcept;

    // Which project is open, or none: a number that changes each time another one opens, so that a view told the
    // project changed can tell the same one again from another.
    int getProjectNumber() const noexcept;

    // The open project's undo manager, for the commands that change it (Command::undoTarget): none while no project is
    // open.
    juce::UndoManager* getUndoManager() const noexcept;

    SongSelection& getSongSelection() noexcept;
    const SongSelection& getSongSelection() const noexcept;
    SongRange& getSongRange() noexcept; // the time range selected across tracks, by beats and IDs

    void addListener (Listener*);
    void removeListener (Listener*);

    struct Impl; // the open Edit, Tracktion's selection manager and the range selection (internal/ProjectContext.h)
    Impl& getImpl() const noexcept { return *impl; }

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProjectContext)
};
} // namespace phazite
