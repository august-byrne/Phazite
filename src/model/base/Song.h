// SPDX-License-Identifier: AGPL-3.0-or-later
// The song: Phazite's record of a project, the PHAZITE element whose every name model/base/internal/Schema.h defines, with
// its undo history and its count of IDs. Every change to it is an undo step in its history (model/commands/internal/Undo.h).
// Its project owns it (model/project/internal/Project.h), and what plays it, its playback session, listens to it
// (model/playback/internal/PlaybackSession.h).
#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>

namespace phazite
{
struct ParameterRef;
class SongIndex;

class Song final : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<Song>;

    explicit Song (juce::ValueTree state); // a PHAZITE element
    ~Song() override;

    // Whatever listens to the song listens to this, its root (a juce::ValueTree::Listener added to what this returns), and
    // finds the element a change was made to from what the callback names: never to one of its elements' handles, nor
    // through a juce::Value or CachedValue of an element's property. The song's index (below) listens to its track list,
    // and JUCE tells a change to the listeners of the elements below first, so it hears of every change to the tracks,
    // clips and notes before anything else asks it about one (tools/check-architecture.sh counts: song-listeners).
    juce::ValueTree getState() const noexcept { return state; }

    // Where its tracks, clips and notes are, by their IDs, which the model's functions that take IDs find them by
    // (model/base/internal/SongIndex.h): kept from its state, which stays the only truth.
    const SongIndex& getIndex() const noexcept { return *index; }

    // Its undo history, which keeps the last steps up to the number set for songs made from then on (as the app starts, from
    // its settings: services/Settings.h, undoSteps), however much each step holds. What Phazite keeps of it beside JUCE's
    // (History) is read and written only by the undo code (model/commands/internal/Undo.h), which finds it from the undo
    // manager a command names.
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }
    const juce::UndoManager& getUndoManager() const noexcept { return undoManager; }

    static constexpr int defaultUndoSteps = 1000, minimumUndoSteps = 10, maximumUndoSteps = 100000;
    static void setUndoSteps (int steps); // limited to the range above. Message thread
    static int getUndoSteps();

    class History final : public juce::UndoManager
    {
    public:
        // How many of the redo steps JUCE holds, the last ones, are none of the song's: JUCE appends the redo steps it put
        // aside as a step began whenever a step is taken back alone (a drag's Escape, a trial put back), and those it put
        // aside long before are among them where no redo step was there as the step began (JUCE workaround W56
        // (docs/juce-workarounds.md)).
        int staleRedoSteps = 0;

        // The steps it keeps (Song::setUndoSteps() as the song was made), and how many more for a while (a version open:
        // Trial::holdHistory()).
        int stepsKept = defaultUndoSteps;
        void keepMoreSteps (int more) { setMaxNumberOfStoredUnits (1, stepsKept + more); }
    };

    // Where its files are: its project file, which the paths it keeps of the files it plays start from (none for a song
    // outside a project, which keeps whole paths); and the folder of what Phazite makes for it and can make again (renders
    // of its audio clips, its frozen tracks' files): its project's Cache folder, or one of its own in the temporary folder,
    // made as it is first asked for and deleted with the song.
    void setFile (const juce::File& projectFile) { file = projectFile; }
    const juce::File& getFile() const noexcept { return file; }
    void setCacheFolder (const juce::File&);
    juce::File getCacheFolder() const;

    // A new ID for an item of the song (a track, a channel's fader or VCA, a clip, a note, an alias group, a device, a
    // container, a send, a modulator, a macro, a section): the song's next (Schema.h: nextID), never given twice, nor
    // taken back by an undo.
    std::uint64_t makeID(); // in Schema.cpp, with the name of the count

    // A change to the project made outside the song's history: a plug-in's own settings, changed in its window, which the
    // song holds only once they are stored (setDeviceStateStore()). Each counts as an unsaved change. Counted for the song,
    // and for the device it was made in, by the device's ID: a plug-in whose stored settings differ counts as changed in the
    // versions only after it said it changed (model/versions/internal/VersionHistory.h), as some write other bytes each time.
    void markChangedOutsideHistory (std::uint64_t deviceID = 0)
    {
        ++changesOutsideHistory;

        if (deviceID != 0)
            ++deviceChangesOutsideHistory[deviceID];
    }

    int getChangesOutsideHistory() const noexcept { return changesOutsideHistory; }

    int getChangesOutsideHistory (std::uint64_t deviceID) const
    {
        const auto found = deviceChangesOutsideHistory.find (deviceID);
        return found != deviceChangesOutsideHistory.end() ? found->second : 0;
    }

    const std::map<std::uint64_t, int>& getDeviceChangesOutsideHistory() const noexcept { return deviceChangesOutsideHistory; }

    // The same counts as far as the states stored in the song hold them: a plug-in's stored state may be one it gave before
    // its last changes (a store waits for no plug-in: model/plugins/internal/PluginInstances.h), whose answer, written in
    // later, then counts as a change the versions show (known bug 166). The store sets a device's count as it writes its
    // state; one it didn't set counts as now.
    void setDeviceChangesHeld (std::uint64_t deviceID, int count) { deviceChangesHeld[deviceID] = count; }
    std::map<std::uint64_t, int> getDeviceChangesHeld() const;

    // Values set by hand as they are set: by a control, a command or a preset (setValueSetByHand(),
    // model/devices/internal/Parameters.h), or by a plug-in itself, in its window. For what plays the song, what writes its
    // automation while it plays and what remembers the parameter touched last. Message thread only.
    struct ParameterListener
    {
        virtual ~ParameterListener() = default;
        virtual void valueSetByHand (const ParameterRef&, float value, bool byPlugin) = 0;
    };

    void addParameterListener (ParameterListener*);
    void removeParameterListener (ParameterListener*);
    void tellValueSetByHand (const ParameterRef&, float value, bool byPlugin);

    // What the devices that play the song hold of their state outside it (a plug-in's own settings), stored into the
    // song's elements of them, outside the undo history, before the song is saved or copied: a track's devices (by the
    // track's ID, its folder's tracks' too), or every device for 0. What plays the song sets it
    // (model/plugins/internal/PluginInstances.h).
    void setDeviceStateStore (std::function<void (std::uint64_t trackID)> store);
    void storeDeviceStates (std::uint64_t trackID = 0); // nothing without one

    // Whether it shows another song for a while, in place, which goes again as it was (a version opened live:
    // model/versions/VersionsModel.h, openVersion(); the trial that shows it holds the undo history meanwhile:
    // model/commands/internal/Undo.h, Trial::holdHistory()). What changes in it meanwhile is no change of its own: its
    // project keeps its unsaved mark as it was and writes none of it (model/project/internal/Project.h). What hears the
    // start, its project, is told before the flag is set, so that it can take its mark as it is. Message thread.
    void setShowingOther (bool);
    bool isShowingOther() const noexcept { return showingOther; }
    void setShowingHeard (std::function<void (bool showing)> heard) { showingHeard = std::move (heard); }

private:
    juce::ValueTree state;
    std::unique_ptr<SongIndex> index;
    History undoManager;
    juce::File file, cacheFolder;
    mutable juce::File ownCacheFolder; // in the temporary folder, while no other is given
    int changesOutsideHistory = 0;
    std::map<std::uint64_t, int> deviceChangesOutsideHistory, deviceChangesHeld;
    juce::ListenerList<ParameterListener> parameterListeners;
    std::function<void (std::uint64_t)> deviceStateStore;
    std::function<void (bool)> showingHeard;
    bool showingOther = false;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Song)
    JUCE_DECLARE_NON_COPYABLE (Song)
};
} // namespace phazite
