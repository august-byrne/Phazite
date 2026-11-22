// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectContext.h"
#include "internal/ProjectContext.h"
#include "model/arrangement/internal/AudioWork.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Clipboard.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/SongRange.h"
#include "model/arrangement/internal/SongSelection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/arrangement/internal/Transients.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongValues.h"
#include "model/engine/internal/EngineContext.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/plugins/internal/PluginHost.h"
#include "model/versions/internal/VersionHistory.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

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
                         private SongValues::Listener,
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
            setSong (nullptr);
        }

        void setSong (Song* newSong)
        {
            if (song != nullptr)
                if (auto* values = song->getValues())
                    values->removeListener (this);

            state.removeListener (this);
            cancelPendingUpdate();
            pending = {};
            notesChanged.clear();
            ++version;
            song = newSong;
            state = song != nullptr ? song->getState() : juce::ValueTree();
            state.addListener (this);

            if (song != nullptr)
                if (auto* values = song->getValues())
                    values->addListener (this);
        }

        int getVersion() const noexcept { return version; }

    private:
        juce::ListenerList<ProjectContext::Listener>& listeners;
        PluginHost& plugins;
        juce::WeakReference<Song> song; // which may go before it lets go of it
        juce::ValueTree state;
        SongChanges pending;
        std::vector<ClipID> notesChanged; // named as the changes are delivered, with their tracks
        int version = 0;                  // ProjectContext::getSongVersion()

        // A clip's notes and controllers' values, which the song holds beside its tree (SongValues.h): a change in the clip,
        // as one of its elements was.
        void clipNotesChanged (const std::vector<ClipID>& clips) override
        {
            notesChanged.insert (notesChanged.end(), clips.begin(), clips.end());
            add (pending.clips);
        }

        // Clips added, taken out, moved or changed (SongValues.h): each named, with the track it was on and the one it is on,
        // as the change says them. Not a track's clips in another order, which changes nothing a view shows (their places in
        // time are their own: markMoved()).
        void clipsChanged (const std::vector<SongValues::ClipChange>& clips, const std::vector<TrackID>&) override
        {
            if (clips.empty())
                return;

            for (const auto& clip : clips)
            {
                name (pending.changedClips, clip.id);

                for (const auto track : { clip.before, clip.after })
                    if (track.isValid())
                        name (pending.clipTracks, track);
            }

            add (pending.clips);
        }

        // The tempo map (SongValues.h): its group. Its tree callbacks below leave its elements alone (isHeardAsKept()).
        void tempoMapChanged() override { add (pending.tempo); }

        // The chord lane (SongValues.h): its group, as a change in its element always was. Its tree callbacks below leave what
        // is in its element alone (isHeardAsKept()); the element's own coming and going they mark as before (mark()).
        void chordLaneChanged() override { add (pending.chords); }

        // Each clip whose notes changed named, on its track, as the song holds it now (a listener of the values reads nothing
        // of the song as it is told: this is later); one gone since was named as it was taken off its track.
        void nameNotesChanged()
        {
            const auto* values = song != nullptr ? song->getValues() : nullptr;

            for (const auto clip : std::exchange (notesChanged, {}))
            {
                if (const auto* value = values != nullptr ? values->findClip (clip) : nullptr)
                {
                    name (pending.changedClips, clip);
                    name (pending.clipTracks, value->track);
                }
            }
        }

        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            if (state.isValid())
            {
                pending.deviceChannels.any = true; // which plug-ins' channels isn't said
                add (pending.devices);
            }
        }

        //==============================================================================
        // Where the changes were (SongChanges::changedClips, clipTracks, deviceChannels): an item without an ID could be any.
        // A drag changes one item many times in a row, named once; the lists are made exact as they are delivered.
        template <typename ID>
        static void name (ChangedItems<ID>& items, ID id)
        {
            if (! id.isValid())
                items.any = true;
            else if (items.named.empty() || items.named.back() != id)
                items.named.push_back (id);
        }

        template <typename ID>
        static void makeExact (ChangedItems<ID>& items)
        {
            std::sort (items.named.begin(), items.named.end());
            items.named.erase (std::unique (items.named.begin(), items.named.end()), items.named.end());
        }

        // The channel whose element holds a device, a send or a container's chain, at any depth; and a send's return, whose
        // strip lists the tracks that send to it.
        void nameChannelOf (const juce::ValueTree& changed)
        {
            auto channel = changed;

            while (channel.isValid() && ! isTrackElement (channel) && ! channel.hasType (schema::MASTER))
                channel = channel.getParent();

            name (pending.deviceChannels, channel.isValid() ? getTrackID (channel) : TrackID());

            if (changed.hasType (schema::SEND))
                name (pending.deviceChannels, TrackID::fromRaw ((std::uint64_t) (juce::int64) changed[schema::sendTo]));
        }

        // Whether an element added or removed changes where sound can go: a send, or a device whose sidechain listens to a
        // track (in a container too). Every strip's send knobs follow the whole routing (a send that would make a loop has
        // none), so then any channel may show other sends: rare edits, not drags.
        static bool changesRouting (const juce::ValueTree& element)
        {
            if (element.hasType (schema::SEND) || element.hasProperty (schema::deviceSidechain))
                return true;

            for (const auto& child : element)
                if (changesRouting (child))
                    return true;

            return false;
        }

        // The group a change to this element, or to something in it, belongs to.
        void mark (const juce::ValueTree& changed)
        {
            for (auto element = changed; element.isValid(); element = element.getParent())
            {
                if (element.hasType (schema::AUTOMATION))
                    return add (pending.automation);

                if (element.hasType (schema::MIDILINKS))
                    return add (pending.midiLinks);

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

                // Devices, on a track's element or the master's.
                if (element.hasType (schema::DEVICE) || element.hasType (schema::CONTAINER) || element.hasType (schema::SEND))
                {
                    nameChannelOf (element);
                    return add (pending.devices);
                }

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
            nameNotesChanged();
            makeExact (pending.changedClips);
            makeExact (pending.clipTracks);
            makeExact (pending.deviceChannels);
            const auto changes = std::exchange (pending, {});
            listeners.call ([&changes] (ProjectContext::Listener& l) { l.songChanged (changes); });
        }

        // A child that is gone has no parent to say where it was: its own type does, or else its old parent's.
        void markRemoved (const juce::ValueTree& parent, const juce::ValueTree& child)
        {
            if (child.hasType (schema::AUTOMATION) || child.hasType (schema::CURVE))
                add (pending.automation);
            else if (child.hasType (schema::MIDILINKS) || child.hasType (schema::MIDILINK))
                add (pending.midiLinks);
            else if (child.hasType (schema::MODULATOR) || child.hasType (schema::MACRO) || child.hasType (schema::MODULATES))
                add (pending.modulation);
            else if (schema::isTrackViewElement (child))
                add (pending.view);
            else if (child.hasType (schema::SECTIONS))
                add (pending.sections);
            else if (child.hasType (schema::CHORDLANE))
                add (pending.chords);
            else if (child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER) || child.hasType (schema::SEND))
            {
                nameChannelOf (parent);

                if (child.hasType (schema::SEND))
                    name (pending.deviceChannels, TrackID::fromRaw ((std::uint64_t) (juce::int64) child[schema::sendTo]));

                if (changesRouting (child))
                    pending.deviceChannels.any = true;

                add (pending.devices);
            }
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
            const auto isDevice = moved.hasType (schema::DEVICE) || moved.hasType (schema::CONTAINER) || moved.hasType (schema::SEND)
                                  || moved.hasType (schema::CHAIN);

            // The elements it passed: now between its new place and its old one.
            const auto first = newIndex < oldIndex ? newIndex + 1 : oldIndex;
            const auto last = newIndex < oldIndex ? oldIndex : newIndex - 1;

            for (int i = first; i <= last && (isTrack || isDevice); ++i)
            {
                const auto passed = parent.getChild (i);

                if (isTrack && isTrackElement (passed))
                    return add (pending.tracks);

                if (isDevice
                    && (passed.hasType (schema::DEVICE) || passed.hasType (schema::CONTAINER) || passed.hasType (schema::SEND)
                        || passed.hasType (schema::CHAIN)))
                {
                    nameChannelOf (parent);
                    return add (pending.devices);
                }
            }
        }

        void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
        {
            if (isHeardAsKept (tree))
                return; // the areas': tempoMapChanged(), chordLaneChanged()

            // A device's value that turns what its sidechain hears on or off (a compressor's mode) changes what the mixer
            // shows of it.
            if (switchesSidechain (tree, property))
            {
                pending.deviceChannels.any = true; // where sound can go, as changesRouting()
                return add (pending.devices);
            }

            // A sidechain listening to another track, or a send to another return, changes where sound can go: any channel's
            // send knobs may change (changesRouting()).
            if ((tree.hasType (schema::DEVICE) && property == schema::deviceSidechain)
                || (tree.hasType (schema::SEND) && property == schema::sendTo))
                pending.deviceChannels.any = true;

            // A parameter's value is no change to what views show: controls read values as they show them, and a knob's drag
            // makes one at each move (docs/archive/performance-audit.md, U1); a channel's or a container's chain's volume and pan, a
            // chain's crossover, a send's level and a macro's value neither. Nor is where a device's window was, which no
            // view shows.
            if (isParameterValue (tree, property) || tree.hasType (schema::VALUES) || tree.hasType (schema::VALUE)
                || (tree.hasType (schema::DEVICE) && schema::isDeviceViewProperty (property))
                || ((tree.hasType (schema::TRACK) || tree.hasType (schema::MASTER) || tree.hasType (schema::CHAIN))
                    && (property == schema::channelVolume || property == schema::channelPan))
                || (tree.hasType (schema::CHAIN) && property == schema::chainCrossover)
                || (tree.hasType (schema::SEND) && property == schema::sendLevel)
                || (tree.hasType (schema::MACRO) && property == schema::macroValue))
                return;

            // The project's notes, the song's own.
            if (tree == state && property == schema::notes)
                return add (pending.notes);

            // A track frozen or played live again changes what its devices show too (their controls are off while frozen).
            if (tree.hasType (schema::TRACK) && property == schema::trackFrozen)
            {
                nameChannelOf (tree);
                add (pending.devices);
            }

            // How tall a track shows, and the like, is view state (Schema.h), not the song.
            if (tree.hasType (schema::TRACK) && schema::isTrackViewProperty (property))
                add (pending.view);
            else
                mark (tree);
        }

        void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
        {
            // A device's first value set by hand, as its other values, is no change that views show; nor its last one set back
            // to its default, which takes its VALUES away (keepValueSetByHand()). The areas' (isHeardAsKept()):
            // tempoMapChanged(), chordLaneChanged().
            if (child.hasType (schema::VALUES) || child.hasType (schema::VALUE) || isHeardAsKept (parent, child))
                return;

            if ((child.hasType (schema::DEVICE) || child.hasType (schema::CONTAINER) || child.hasType (schema::SEND))
                && changesRouting (child))
                pending.deviceChannels.any = true;

            mark (child);
        }
        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
        {
            if (! child.hasType (schema::VALUES) && ! child.hasType (schema::VALUE) && ! isHeardAsKept (parent, child))
                markRemoved (parent, child);
        }
        void valueTreeChildOrderChanged (juce::ValueTree& parent, int oldIndex, int newIndex) override
        {
            if (! isHeardAsKept (parent))
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
    std::unique_ptr<MidiLinker> midiLinks;         // what learns and turns its MIDI links
    std::unique_ptr<PlaybackSession> playback;     // the song as Phazite's engine plays it
    std::unique_ptr<VersionHistory> versions;      // the project's versions, beside its file
    DeviceWindowHost* windows = nullptr;           // what shows the devices' windows, the app's
    ProblemReporter problems;                      // what tells the user of a problem with the song, the app's
    Clipboard clipboard;                           // what Cut and Copy keep for Paste
    std::unique_ptr<SongWatcher> songWatcher;
    Transients transients;                // the hits found in audio files, whichever project plays them
    std::unique_ptr<AudioWork> audioWork; // what reads much of an audio file for a command, in the background
    int projectNumber = 0;

    void dropSong()
    {
        versions = nullptr;  // a version being written is finished first
        midiLinks = nullptr; // its burst kept, its learning ended, before its session goes
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
    impl->audioWork = std::make_unique<AudioWork> (*this);
}

ProjectContext::~ProjectContext()
{
    impl->audioWork = nullptr; // what it reads stopped, changing nothing
    impl->songWatcher = nullptr;
    impl->dropSong();
}

bool ProjectContext::hasProject() const noexcept
{
    return impl->song != nullptr;
}

void ProjectContext::withPluginStates (std::function<void (bool)> then)
{
    if (pluginStatesGate != nullptr)
        pluginStatesGate (std::move (then));
    else
        then (true);
}

std::unique_ptr<ProjectContext::WorkHold> ProjectContext::holdForWork (const juce::String& title, Cancel cancel, Progress progress)
{
    // Counted while it lives, the input held (the input holder's, the one set as it goes), and the app's wait in it.
    struct Counted final : WorkHold
    {
        Counted (std::shared_ptr<Held> h, std::unique_ptr<WorkHold> w)
            : held (std::move (h)),
              wait (std::move (w))
        {
            ++held->count;

            if (held->holder != nullptr)
                held->holder->holdInput();
        }

        ~Counted() override
        {
            wait = nullptr;
            --held->count;

            if (held->holder != nullptr)
                held->holder->releaseInput();
        }

        std::shared_ptr<Held> held;
        std::unique_ptr<WorkHold> wait;
    };

    return std::make_unique<Counted> (held, workHolder != nullptr ? workHolder (title, std::move (cancel), std::move (progress)) : nullptr);
}

void ProjectContext::setInputHolder (InputHolder* holder)
{
    // The holds alive taken over by the new holder: let go of on the old one, held on the new.
    for (int i = 0; i < held->count; ++i)
    {
        if (held->holder != nullptr)
            held->holder->releaseInput();

        if (holder != nullptr)
            holder->holdInput();
    }

    held->holder = holder;
    getAudioWork (*this).setInputHolder (holder); // the audio files' work holds the same one
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

const RangeSelection& getRangeSelection (const ProjectContext& context)
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

Transients& getTransientFinder (const ProjectContext& context)
{
    return context.getImpl().transients;
}

AudioWork& getAudioWork (const ProjectContext& context)
{
    return *context.getImpl().audioWork;
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

void setProblemReporter (ProjectContext& context, ProblemReporter reporter)
{
    context.getImpl().problems = std::move (reporter);
}

void reportProblem (const ProjectContext& context, const juce::String& title, const juce::String& message)
{
    if (const auto& problems = context.getImpl().problems; problems != nullptr)
        problems (title, message);
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

    // What reads audio for the project before stops at once (AudioWork.h), before its folder may go.
    impl.audioWork->songChanged (song);

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
        impl.playback->onProblem = [&impl] (const juce::String& title, const juce::String& message)
        {
            if (impl.problems != nullptr)
                impl.problems (title, message);
        };
        impl.midiLinks = std::make_unique<MidiLinker> (*song);
        impl.midiLinks->onChange = [&impl] { impl.listeners.call ([] (ProjectContext::Listener& l) { l.midiLearnChanged(); }); };
        // Not while the window is held for work, when no command that changes the project runs either (holdForWork()): the
        // value goes, and learning waits for the next one. Under a card or a dialog, a knob turns its parameter, as a
        // hardware control does.
        impl.playback->onMidiController = [&impl, &context] (int channel, int number, int value)
        {
            if (impl.midiLinks != nullptr && ! context.isHoldingForWork())
                impl.midiLinks->take ({ channel, number }, value);
        };
        impl.versions = std::make_unique<VersionHistory> (
            *song, [&impl] { impl.listeners.call ([] (ProjectContext::Listener& l) { l.versionsChanged(); }); });
    }

    impl.songWatcher->setSong (song);
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
