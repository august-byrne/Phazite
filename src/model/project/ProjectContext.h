// SPDX-License-Identifier: AGPL-3.0-or-later
// The open project, as commands, panels and dialogs find it: its Edit, what is selected in it, and the time range
// selected across tracks. The app's ProjectSession owns the one ProjectContext and is the only code (besides tests) that
// calls setEdit(). Code that holds the context reads getEdit(); model code given the selection may read
// selection.getEdit(): the same Edit. Tracktion's focused Edit and selection (UIBehaviour, which live MIDI and
// controller mappings use) follow this; Phazite's code never reads them. Views listen to it, to show another project
// or none. Message thread only.
//
// Architecture phase A3 moves its Tracktion side out of this header: views use getSongSelection(), which names items by
// their IDs, rather than getSelection().
#pragma once

#include "model/arrangement/Selection.h"
#include "model/arrangement/SongSelection.h"

namespace phazite
{
class UIBehaviour;

// What changed in the open project's song, in groups, for views to show it again. Changes made at once (in one turn of
// the message loop) come together, after it. View state (model/base/Schema.h), such as how tall a track shows, is no
// change to the song. A drag can change the song as often as the mouse moves: views that take long to show a change
// show the last one at their next timer tick instead.
struct SongChanges
{
    bool tracks = false;   // tracks added, deleted, moved or changed
    bool devices = false;  // devices or sends added, deleted, moved or changed
    bool clips = false;    // clips added, deleted or changed, their notes included
    bool sections = false; // sections added, deleted or changed
    bool tempo = false;    // the tempo or the time signature
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
    explicit ProjectContext (te::Engine&, UIBehaviour* tracktionFocus = nullptr);
    ~ProjectContext();

    te::Edit* getEdit() const noexcept { return selection.getEdit(); } // nullptr while no project is open
    bool hasProject() const noexcept { return getEdit() != nullptr; }

    te::SelectionManager& getSelection() noexcept { return selection; }
    const te::SelectionManager& getSelection() const noexcept { return selection; }
    SongSelection& getSongSelection() noexcept { return *songSelection; }
    const SongSelection& getSongSelection() const noexcept { return *songSelection; }
    RangeSelection& getRangeSelection() noexcept { return rangeSelection; }

    // The Edit open now, or none. Nothing of the previous Edit stays selected. Then tells the listeners.
    void setEdit (te::Edit*);

    void addListener (Listener*);
    void removeListener (Listener*);

private:
    struct SongWatcher; // tells the listeners what changes in the open Edit

    UIBehaviour* const focus;
    te::SelectionManager selection;
    std::unique_ptr<SongSelection> songSelection; // a view of selection, by IDs
    RangeSelection rangeSelection;
    juce::ListenerList<Listener> listeners;
    std::unique_ptr<SongWatcher> songWatcher;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProjectContext)
};
} // namespace phazite
