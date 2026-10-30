// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectContext.h"
#include "internal/ProjectContext.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Clipboard.h"
#include "model/arrangement/internal/SongRange.h"
#include "model/arrangement/internal/SongSelection.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/mixer/internal/Freeze.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/internal/Notes.h"
#include "model/transport/internal/AudioRecorder.h"

namespace phazite
{
namespace
{
    // A track's element: Tracktion's (TRACK, FOLDERTRACK...), or the song's own in its track list (schema::TRACK).
    bool isTrackElement (const juce::ValueTree& element)
    {
        return te::TrackList::isTrack (element) || element.hasType (schema::TRACK);
    }

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
            ++version;
            state = std::move (newState);
            state.addListener (this);
        }

        int getVersion() const noexcept { return version; }

    private:
        juce::ListenerList<ProjectContext::Listener>& listeners;
        juce::ValueTree state;
        SongChanges pending;
        int version = 0; // ProjectContext::getSongVersion()

        // The group a change to this element, or to something in it, belongs to.
        void mark (const juce::ValueTree& changed)
        {
            for (auto element = changed; element.isValid(); element = element.getParent())
            {
                if (element.hasType (te::IDs::TRANSPORT) || element.hasType (te::IDs::INPUTDEVICES))
                    return;

                if (element.hasType (te::IDs::AUTOMATIONCURVE))
                    return add (pending.automation);

                // A track's modulators and macros, and what they modulate, in its devices' states.
                if (element.hasType (te::IDs::MODIFIERS) || element.hasType (te::IDs::MACROPARAMETERS)
                    || element.hasType (te::IDs::MODIFIERASSIGNMENTS))
                    return add (pending.modulation);

                if (schema::isTrackViewElement (element) || element.hasType (schema::MASTER))
                    return add (pending.view);

                if (element.hasType (schema::SECTIONS))
                    return add (pending.sections);

                if (element.hasType (schema::CHORDLANE))
                    return add (pending.chords);

                if (te::Clip::isClipState (element))
                    return add (pending.clips);

                // Devices, and containers' rack types (model/mixer/internal/Containers.h): their names and what they hold.
                if (element.hasType (te::IDs::PLUGIN) || element.hasType (te::IDs::MASTERPLUGINS) || element.hasType (te::IDs::RACK))
                    return add (pending.devices);

                if (element.hasType (schema::TEMPOMAP) || element.hasType (te::IDs::TEMPOSEQUENCE))
                    return add (pending.tempo);

                if (isTrackElement (element))
                    return add (pending.tracks);
            }
        }

        void add (bool& group)
        {
            group = true;
            ++version;
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
            else if (child.hasType (schema::SECTIONS))
                add (pending.sections);
            else if (child.hasType (schema::CHORDLANE))
                add (pending.chords);
            else if (te::Clip::isClipState (child))
                add (pending.clips);
            else if (child.hasType (te::IDs::PLUGIN) || child.hasType (te::IDs::RACK))
                add (pending.devices);
            else if (isTrackElement (child))
                add (pending.tracks);
            else
                mark (parent);
        }

        // A track or a device that moved past another, which changes their order. Tracktion sorts a track's elements now and
        // then (after a clip is added, say), which changes no order that matters: clips, sections and the tempo's changes
        // have their places in time.
        void markMoved (const juce::ValueTree& parent, int oldIndex, int newIndex)
        {
            // A device in a container moves with its place in the container's rack type.
            const auto moved = parent.getChild (newIndex);
            const auto isTrack = isTrackElement (moved);
            const auto isDevice = moved.hasType (te::IDs::PLUGIN) || moved.hasType (te::IDs::PLUGININSTANCE);

            // The elements it passed: now between its new place and its old one.
            const auto first = newIndex < oldIndex ? newIndex + 1 : oldIndex;
            const auto last = newIndex < oldIndex ? oldIndex : newIndex - 1;

            for (int i = first; i <= last && (isTrack || isDevice); ++i)
            {
                const auto passed = parent.getChild (i);

                if (isTrack && isTrackElement (passed))
                    return add (pending.tracks);

                if (isDevice && (passed.hasType (te::IDs::PLUGIN) || passed.hasType (te::IDs::PLUGININSTANCE)))
                    return add (pending.devices);
            }
        }

        void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
        {
            // A parameter's value is no change to what views show: controls read values as they show them, and a knob's drag
            // makes one at each move (docs/performance-audit.md, U1). Nor is where a device's window was, which no view shows.
            if (isParameterValue (tree, property) || (tree.hasType (te::IDs::PLUGIN) && schema::isDeviceViewProperty (property)))
                return;

            // How tall a track shows, and the like, is view state (Schema.h), not the song.
            if (tree.hasType (schema::TRACK) && schema::isTrackViewProperty (property))
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
    Selection selection;                          // what is selected, by IDs
    std::unique_ptr<SongSelection> songSelection; // the UI's view of selection
    RangeSelection rangeSelection;
    std::unique_ptr<SongRange> songRange; // a view of rangeSelection, by IDs
    juce::ListenerList<Listener> listeners;
    std::unique_ptr<NoteIDKeeper> noteIDs;                    // gives the notes added to the open Edit their IDs
    std::unique_ptr<FreezeKeeper> freezes;                    // unfreezes a frozen track whose clips or frozen devices change
    std::unique_ptr<DeviceModulationKeeper> deviceModulation; // plays what modulates Phazite's devices in each
    std::unique_ptr<ClipRenders> clipRenders;                 // renders what the clips play stretched, pitched or reversed
    std::unique_ptr<AudioRecorder> recorder;                  // records the armed tracks' audio inputs
    std::unique_ptr<AutomationWatcher> automation;            // the parameter touched last, and recorded automation's undo steps
    Clipboard clipboard;                                      // what Cut and Copy keep for Paste
    std::unique_ptr<SongWatcher> songWatcher;
    int projectNumber = 0;
};

ProjectContext::ProjectContext (EngineContext& engine, UIBehaviour* tracktionFocus)
    : impl (std::make_unique<Impl> (getEngine (engine), tracktionFocus))
{
}

ProjectContext::~ProjectContext()
{
    impl->songWatcher = nullptr;
    impl->automation = nullptr;
    impl->recorder = nullptr;
    impl->clipRenders = nullptr;
    impl->deviceModulation = nullptr;
    impl->freezes = nullptr;
    impl->noteIDs = nullptr;

    if (impl->focus != nullptr)
        impl->focus->setFocusedEdit (nullptr);
}

bool ProjectContext::hasProject() const noexcept
{
    return getEdit (*this) != nullptr;
}

int ProjectContext::getProjectNumber() const noexcept
{
    return impl->projectNumber;
}

int ProjectContext::getSongVersion() const noexcept
{
    return impl->songWatcher->getVersion();
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

const SongRange& ProjectContext::getSongRange() const noexcept
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

Selection& getSelection (ProjectContext& context)
{
    return context.getImpl().selection;
}

const Selection& getSelection (const ProjectContext& context)
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

ClipRenders* getClipRenders (const ProjectContext& context)
{
    return context.getImpl().clipRenders.get();
}

Clipboard& getClipboard (ProjectContext& context)
{
    return context.getImpl().clipboard;
}

void setEdit (ProjectContext& context, te::Edit* edit)
{
    auto& impl = context.getImpl();

    if (impl.focus != nullptr)
        impl.focus->setFocusedEdit (nullptr);

    // Another project: views forget what they showed, and the clipboard what it held, which named its files.
    if (edit != impl.selection.getEdit())
    {
        ++impl.projectNumber;
        impl.clipboard = {};
    }

    impl.songSelection->setNoteClips ({}); // the editors show the new project's clips, or none
    impl.rangeSelection.clear();
    impl.selection.setEdit (edit);                                                     // nothing of the previous one selected
    impl.automation = nullptr;                                                         // before the Edit it watches goes
    impl.deviceModulation = nullptr;                                                   // likewise
    impl.freezes = nullptr;                                                            // likewise
    impl.recorder = nullptr;                                                           // likewise, any recording dropped
    impl.clipRenders = nullptr;                                                        // likewise, its renders stopped
    impl.noteIDs = edit != nullptr ? std::make_unique<NoteIDKeeper> (*edit) : nullptr; // before the watcher hears what it writes
    impl.freezes = edit != nullptr ? std::make_unique<FreezeKeeper> (*edit) : nullptr;
    impl.deviceModulation = edit != nullptr ? std::make_unique<DeviceModulationKeeper> (*edit) : nullptr;
    impl.clipRenders = edit != nullptr ? std::make_unique<ClipRenders> (*edit) : nullptr;
    impl.recorder = edit != nullptr ? std::make_unique<AudioRecorder> (*edit) : nullptr;
    impl.automation = edit != nullptr ? std::make_unique<AutomationWatcher> (*edit) : nullptr;
    impl.songWatcher->setState (edit != nullptr ? edit->state : juce::ValueTree());

    if (impl.focus != nullptr)
        impl.focus->setFocusedEdit (edit);

    impl.listeners.call ([] (ProjectContext::Listener& l) { l.projectChanged(); });
}
} // namespace phazite
