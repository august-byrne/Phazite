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
#include "model/mixer/internal/Freeze.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/versions/internal/VersionHistory.h"

namespace phazite
{
namespace
{
    bool isTrackElement (const juce::ValueTree& element)
    {
        return element.hasType (schema::TRACK);
    }

    //==============================================================================
    // Listens to the open song and tells the context's listeners which groups changed, once per turn of the message loop. A
    // plug-in whose helper went, or that was reloaded, changes what its device shows (model/plugins/internal/PluginHost.h).
    struct SongWatcher : private juce::ValueTree::Listener,
                         private juce::ChangeListener,
                         private juce::AsyncUpdater
    {
        SongWatcher (juce::ListenerList<ProjectContext::Listener>& l, PluginHost& host)
            : listeners (l),
              plugins (host)
        {
            plugins.addChangeListener (this);
        }

        ~SongWatcher() override
        {
            plugins.removeChangeListener (this);
            setState ({});
        }

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
        PluginHost& plugins;
        juce::ValueTree state;
        SongChanges pending;
        int version = 0; // ProjectContext::getSongVersion()

        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            if (state.isValid())
                add (pending.devices);
        }

        // The group a change to this element, or to something in it, belongs to.
        void mark (const juce::ValueTree& changed)
        {
            for (auto element = changed; element.isValid(); element = element.getParent())
            {
                if (element.hasType (schema::AUTOMATION))
                    return add (pending.automation);

                // A track's modulators and macros, and what they modulate.
                if (element.hasType (schema::MODULATOR) || element.hasType (schema::MACRO) || element.hasType (schema::MODULATES))
                    return add (pending.modulation);

                if (schema::isTrackViewElement (element))
                    return add (pending.view);

                // The master's own element: its write mode (its lanes are view state, above).
                if (element.hasType (schema::MASTER))
                    return add (pending.tracks);

                if (element.hasType (schema::SECTIONS))
                    return add (pending.sections);

                if (element.hasType (schema::CHORDLANE))
                    return add (pending.chords);

                // Clips, with their notes.
                if (element.hasType (schema::CLIP))
                    return add (pending.clips);

                // Devices, on a track's element or the master's.
                if (element.hasType (schema::DEVICE) || element.hasType (schema::CONTAINER) || element.hasType (schema::SEND))
                    return add (pending.devices);

                if (element.hasType (schema::TEMPOMAP))
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
            if (child.hasType (schema::AUTOMATION) || child.hasType (schema::CURVE))
                add (pending.automation);
            else if (child.hasType (schema::MODULATOR) || child.hasType (schema::MACRO) || child.hasType (schema::MODULATES))
                add (pending.modulation);
            else if (schema::isTrackViewElement (child))
                add (pending.view);
            else if (child.hasType (schema::SECTIONS))
                add (pending.sections);
            else if (child.hasType (schema::CHORDLANE))
                add (pending.chords);
            else if (child.hasType (schema::CLIP))
                add (pending.clips);
            else if (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER) || child.hasType (schema::SEND))
                add (pending.devices);
            else if (isTrackElement (child))
                add (pending.tracks);
            else
                mark (parent);
        }

        // A track or a device that moved past another, which changes their order: clips, sections and the tempo's changes
        // have their places in time, which no order changes.
        void markMoved (const juce::ValueTree& parent, int oldIndex, int newIndex)
        {
            const auto moved = parent.getChild (newIndex);
            const auto isTrack = isTrackElement (moved);
            const auto isDevice = moved.hasType (schema::DEVICE) || moved.hasType (schema::CONTAINER) || moved.hasType (schema::SEND);

            // The elements it passed: now between its new place and its old one.
            const auto first = newIndex < oldIndex ? newIndex + 1 : oldIndex;
            const auto last = newIndex < oldIndex ? oldIndex : newIndex - 1;

            for (int i = first; i <= last && (isTrack || isDevice); ++i)
            {
                const auto passed = parent.getChild (i);

                if (isTrack && isTrackElement (passed))
                    return add (pending.tracks);

                if (isDevice && (passed.hasType (schema::DEVICE) || passed.hasType (schema::CONTAINER) || passed.hasType (schema::SEND)))
                    return add (pending.devices);
            }
        }

        void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
        {
            // A parameter's value is no change to what views show: controls read values as they show them, and a knob's drag
            // makes one at each move (docs/performance-audit.md, U1); a channel's volume and pan, a send's level and a
            // macro's value neither. Nor is where a device's window was, which no view shows.
            if (isParameterValue (tree, property) || tree.hasType (schema::VALUES) || tree.hasType (schema::VALUE)
                || (tree.hasType (schema::DEVICE) && schema::isDeviceViewProperty (property))
                || ((tree.hasType (schema::TRACK) || tree.hasType (schema::MASTER))
                    && (property == schema::channelVolume || property == schema::channelPan))
                || (tree.hasType (schema::SEND) && property == schema::sendLevel)
                || (tree.hasType (schema::MACRO) && property == schema::macroValue))
                return;

            // The project's notes, the song's own.
            if (tree == state && property == schema::notes)
                return add (pending.notes);

            // A track frozen or played live again changes what its devices show too (their controls are off while frozen).
            if (tree.hasType (schema::TRACK) && property == schema::trackFrozen)
                add (pending.devices);

            // How tall a track shows, and the like, is view state (Schema.h), not the song.
            if (tree.hasType (schema::TRACK) && schema::isTrackViewProperty (property))
                add (pending.view);
            else
                mark (tree);
        }

        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child) override
        {
            // A device's first value set by hand, as its other values, is no change that views show; nor its last one set back
            // to its default, which takes its VALUES away (keepValueSetByHand()).
            if (! child.hasType (schema::VALUES) && ! child.hasType (schema::VALUE))
                mark (child);
        }
        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
        {
            if (! child.hasType (schema::VALUES) && ! child.hasType (schema::VALUE))
                markRemoved (parent, child);
        }
        void valueTreeChildOrderChanged (juce::ValueTree& parent, int oldIndex, int newIndex) override
        {
            markMoved (parent, oldIndex, newIndex);
        }
    };

} // namespace

//==============================================================================
struct ProjectContext::Impl
{
    explicit Impl (EngineContext& e)
        : engine (e),
          songSelection (createSongSelection (selection)),
          songRange (createSongRange (rangeSelection, selection)),
          songWatcher (std::make_unique<SongWatcher> (listeners, getPluginHost (e)))
    {
    }

    EngineContext& engine; // the app's, which plays the song
    Song::Ptr song;
    Selection selection;                          // what is selected, by IDs
    std::unique_ptr<SongSelection> songSelection; // the UI's view of selection
    RangeSelection rangeSelection { selection };
    std::unique_ptr<SongRange> songRange; // a view of rangeSelection, by IDs
    juce::ListenerList<Listener> listeners;
    std::unique_ptr<FreezeKeeper> freezes;         // unfreezes a frozen track whose clips or frozen devices change
    std::unique_ptr<ClipRenders> clipRenders;      // renders what the clips play stretched, pitched or reversed
    std::unique_ptr<AutomationWatcher> automation; // the parameter touched last
    std::unique_ptr<PlaybackSession> playback;     // the song as Phazite's engine plays it
    std::unique_ptr<VersionHistory> versions;      // the project's versions, beside its file
    DeviceWindowHost* windows = nullptr;           // what shows the devices' windows, the app's
    Clipboard clipboard;                           // what Cut and Copy keep for Paste
    std::unique_ptr<SongWatcher> songWatcher;
    int projectNumber = 0;

    void dropSong()
    {
        versions = nullptr; // a version being written is finished first
        playback = nullptr;
        automation = nullptr;
        clipRenders = nullptr; // its renders stopped
        freezes = nullptr;
        song = nullptr;
    }
};

ProjectContext::ProjectContext (EngineContext& engine)
    : impl (std::make_unique<Impl> (engine))
{
}

ProjectContext::~ProjectContext()
{
    impl->songWatcher = nullptr;
    impl->dropSong();
}

bool ProjectContext::hasProject() const noexcept
{
    return impl->song != nullptr;
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
    return impl->song != nullptr ? &impl->song->getUndoManager() : nullptr;
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
Song* findSong (const ProjectContext& context)
{
    return context.getImpl().song.get();
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

EngineContext& getEngineContext (const ProjectContext& context)
{
    return context.getImpl().engine;
}

PlaybackSession* getSession (const ProjectContext& context)
{
    return context.getImpl().playback.get();
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

VersionHistory* getVersionHistory (const ProjectContext& context)
{
    return context.getImpl().versions.get();
}

void setDeviceWindows (ProjectContext& context, DeviceWindowHost* windows)
{
    context.getImpl().windows = windows;
}

DeviceWindowHost* getDeviceWindows (const ProjectContext& context)
{
    return context.getImpl().windows;
}

void setSong (ProjectContext& context, Song* song)
{
    auto& impl = context.getImpl();

    // Another project: views forget what they showed, and the clipboard what it held, which named its files.
    if (song != impl.song.get())
    {
        ++impl.projectNumber;
        impl.clipboard = {};
    }

    impl.songSelection->setNoteClips ({}); // the editors show the new project's clips, or none
    impl.rangeSelection.clear();
    impl.selection.setSong (song); // nothing of the previous one selected
    impl.dropSong();               // what played the previous one, any recording dropped
    impl.song = song;

    if (song != nullptr)
    {
        impl.freezes = std::make_unique<FreezeKeeper> (*song);
        impl.clipRenders = std::make_unique<ClipRenders> (*song);
        impl.automation = std::make_unique<AutomationWatcher> (*song);
        impl.playback = std::make_unique<PlaybackSession> (*song, getPlaybackHost (impl.engine));
        impl.versions = std::make_unique<VersionHistory> (
            *song, [&impl] { impl.listeners.call ([] (ProjectContext::Listener& l) { l.versionsChanged(); }); });
    }

    impl.songWatcher->setState (song != nullptr ? song->getState() : juce::ValueTree());
    impl.listeners.call ([] (ProjectContext::Listener& l) { l.projectChanged(); });
}

bool switchSongInPlace (ProjectContext& context, const std::function<bool()>& apply)
{
    auto& impl = context.getImpl();

    if (impl.song == nullptr || impl.freezes == nullptr || ! impl.freezes->keepThrough (apply))
        return false;

    impl.songSelection->setNoteClips ({});
    impl.rangeSelection.clear();
    impl.selection.deselectAll();
    impl.automation = std::make_unique<AutomationWatcher> (*impl.song);
    return true;
}
} // namespace phazite
