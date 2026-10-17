// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectContext.h"
#include "internal/ProjectContext.h"
#include "model/arrangement/internal/SongRange.h"
#include "model/arrangement/internal/SongSelection.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/notes/internal/Notes.h"

namespace phazite
{
namespace
{
    //==============================================================================
    // Listens to the open Edit's state and tells the context's listeners which groups changed, once per turn of the message
    // loop. What Tracktion writes as it plays (the transport, the input devices) is no change to the song.
    struct SongWatcher : private juce::ValueTree::Listener,
                         private juce::AsyncUpdater
    {
        explicit SongWatcher (juce::ListenerList<ProjectContext::Listener>& l)
            : listeners (l)
        {
        }

        ~SongWatcher() override { setState ({}); }

        void setState (juce::ValueTree newState)
        {
            state.removeListener (this);
            cancelPendingUpdate();
            pending = {};
            state = std::move (newState);
            state.addListener (this);
        }

    private:
        juce::ListenerList<ProjectContext::Listener>& listeners;
        juce::ValueTree state;
        SongChanges pending;

        // The group a change to this element, or to something in it, belongs to.
        void mark (const juce::ValueTree& changed)
        {
            for (auto element = changed; element.isValid(); element = element.getParent())
            {
                if (element.hasType (te::IDs::TRANSPORT) || element.hasType (te::IDs::INPUTDEVICES))
                    return;

                if (element.hasType (te::IDs::AUTOMATIONCURVE))
                    return add (pending.automation);

                if (schema::isTrackViewElement (element))
                    return add (pending.view);

                if (element.hasType (te::IDs::ARRANGERCLIP))
                    return add (pending.sections);

                if (te::Clip::isClipState (element))
                    return add (pending.clips);

                if (element.hasType (te::IDs::PLUGIN) || element.hasType (te::IDs::MASTERPLUGINS))
                    return add (pending.devices);

                if (element.hasType (te::IDs::TEMPOSEQUENCE))
                    return add (pending.tempo);

                if (te::TrackList::isTrack (element))
                    return add (pending.tracks);
            }
        }

        void add (bool& group)
        {
            group = true;
            triggerAsyncUpdate();
        }

        void handleAsyncUpdate() override
        {
            const auto changes = std::exchange (pending, {});
            listeners.call ([&changes] (ProjectContext::Listener& l) { l.songChanged (changes); });
        }

        // A child that is gone has no parent to say where it was: its own type does, or else its old parent's.
        void markRemoved (const juce::ValueTree& parent, const juce::ValueTree& child)
        {
            if (child.hasType (te::IDs::AUTOMATIONCURVE))
                add (pending.automation);
            else if (schema::isTrackViewElement (child))
                add (pending.view);
            else if (child.hasType (te::IDs::ARRANGERCLIP))
                add (pending.sections);
            else if (te::Clip::isClipState (child))
                add (pending.clips);
            else if (child.hasType (te::IDs::PLUGIN))
                add (pending.devices);
            else if (te::TrackList::isTrack (child))
                add (pending.tracks);
            else
                mark (parent);
        }

        // A track or a device that moved past another, which changes their order. Tracktion sorts a track's elements now and
        // then (after a clip is added, say), which changes no order that matters: clips, sections and the tempo's changes
        // have their places in time.
        void markMoved (const juce::ValueTree& parent, int oldIndex, int newIndex)
        {
            const auto moved = parent.getChild (newIndex);
            const auto isTrack = te::TrackList::isTrack (moved), isDevice = moved.hasType (te::IDs::PLUGIN);

            // The elements it passed: now between its new place and its old one.
            const auto first = newIndex < oldIndex ? newIndex + 1 : oldIndex;
            const auto last = newIndex < oldIndex ? oldIndex : newIndex - 1;

            for (int i = first; i <= last && (isTrack || isDevice); ++i)
            {
                const auto passed = parent.getChild (i);

                if (isTrack && te::TrackList::isTrack (passed))
                    return add (pending.tracks);

                if (isDevice && passed.hasType (te::IDs::PLUGIN))
                    return add (pending.devices);
            }
        }

        void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
        {
            // The value of a parameter with automation, which Tracktion writes as the automation plays, is no change.
            if (isAutomatedValue (tree, property))
                return;

            // How tall a track shows, and the like, is view state (Schema.h), not the song.
            if (te::TrackList::isTrack (tree) && schema::isTrackViewProperty (property))
                add (pending.view);
            else
                mark (tree);
        }

        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child) override { mark (child); }
        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { markRemoved (parent, child); }
        void valueTreeChildOrderChanged (juce::ValueTree& parent, int oldIndex, int newIndex) override
        {
            markMoved (parent, oldIndex, newIndex);
        }
    };

} // namespace

//==============================================================================
struct ProjectContext::Impl
{
    Impl (te::Engine& engine, UIBehaviour* tracktionFocus)
        : focus (tracktionFocus),
          selection (engine),
          songSelection (createSongSelection (selection)),
          songRange (createSongRange (rangeSelection, selection)),
          songWatcher (std::make_unique<SongWatcher> (listeners))
    {
    }

    UIBehaviour* const focus;
    te::SelectionManager selection;
    std::unique_ptr<SongSelection> songSelection; // a view of selection, by IDs
    RangeSelection rangeSelection;
    std::unique_ptr<SongRange> songRange; // a view of rangeSelection, by IDs
    juce::ListenerList<Listener> listeners;
    std::unique_ptr<NoteIDKeeper> noteIDs;         // gives the notes added to the open Edit their IDs
    std::unique_ptr<AutomationWatcher> automation; // the parameter touched last, and recorded automation's undo steps
    std::unique_ptr<SongWatcher> songWatcher;
    int projectNumber = 0;
};

ProjectContext::ProjectContext (EngineContext& engine, UIBehaviour* tracktionFocus)
    : impl (std::make_unique<Impl> (getEngine (engine), tracktionFocus))
{
    if (impl->focus != nullptr)
        impl->focus->setSelectionManager (&impl->selection);
}

ProjectContext::~ProjectContext()
{
    impl->songWatcher = nullptr;
    impl->automation = nullptr;
    impl->noteIDs = nullptr;

    if (impl->focus != nullptr)
    {
        impl->focus->setFocusedEdit (nullptr);
        impl->focus->setSelectionManager (nullptr);
    }
}

bool ProjectContext::hasProject() const noexcept
{
    return getEdit (*this) != nullptr;
}

int ProjectContext::getProjectNumber() const noexcept
{
    return impl->projectNumber;
}

juce::UndoManager* ProjectContext::getUndoManager() const noexcept
{
    auto* edit = getEdit (*this);
    return edit != nullptr ? &edit->getUndoManager() : nullptr;
}

SongSelection& ProjectContext::getSongSelection() noexcept
{
    return *impl->songSelection;
}

const SongSelection& ProjectContext::getSongSelection() const noexcept
{
    return *impl->songSelection;
}

SongRange& ProjectContext::getSongRange() noexcept
{
    return *impl->songRange;
}

void ProjectContext::addListener (Listener* listener)
{
    impl->listeners.add (listener);
}

void ProjectContext::removeListener (Listener* listener)
{
    impl->listeners.remove (listener);
}

//==============================================================================
te::Edit* getEdit (const ProjectContext& context)
{
    return context.getImpl().selection.getEdit();
}

te::SelectionManager& getSelection (ProjectContext& context)
{
    return context.getImpl().selection;
}

const te::SelectionManager& getSelection (const ProjectContext& context)
{
    return context.getImpl().selection;
}

RangeSelection& getRangeSelection (ProjectContext& context)
{
    return context.getImpl().rangeSelection;
}

AutomationWatcher* getAutomationWatcher (const ProjectContext& context)
{
    return context.getImpl().automation.get();
}

void setEdit (ProjectContext& context, te::Edit* edit)
{
    auto& impl = context.getImpl();

    if (impl.focus != nullptr)
        impl.focus->setFocusedEdit (nullptr);

    if (edit != impl.selection.getEdit())
        ++impl.projectNumber;

    impl.songSelection->setNoteClips ({}); // the editors show the new project's clips, or none
    impl.selection.deselectAll();
    impl.selection.edit = nullptr;
    impl.rangeSelection.clear();
    impl.selection.edit = edit;
    impl.automation = nullptr;                                                         // before the Edit it watches goes
    impl.noteIDs = edit != nullptr ? std::make_unique<NoteIDKeeper> (*edit) : nullptr; // before the watcher hears what it writes
    impl.automation = edit != nullptr ? std::make_unique<AutomationWatcher> (*edit) : nullptr;
    impl.songWatcher->setState (edit != nullptr ? edit->state : juce::ValueTree());

    if (impl.focus != nullptr)
        impl.focus->setFocusedEdit (edit);

    impl.listeners.call ([] (ProjectContext::Listener& l) { l.projectChanged(); });
}
} // namespace phazite
