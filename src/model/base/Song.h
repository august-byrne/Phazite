// SPDX-License-Identifier: AGPL-3.0-or-later
// The song: Phazite's record of a project, the PHAZITE element whose every name model/base/internal/Schema.h defines, with
// its undo history and its count of IDs. Every change to it is an undo step in its history (model/commands/internal/Undo.h).
// Its project owns it (model/project/internal/Project.h), and what plays it, its playback session, listens to it
// (model/playback/internal/PlaybackSession.h).
#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>

namespace phazite
{
struct ParameterRef;

class Song final : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<Song>;

    explicit Song (juce::ValueTree state); // a PHAZITE element
    ~Song() override;

    juce::ValueTree getState() const noexcept { return state; }

    // Its undo history: 30 steps at least.
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }
    const juce::UndoManager& getUndoManager() const noexcept { return undoManager; }

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
    // song holds only once they are stored (setDeviceStateStore()). Each counts as an unsaved change.
    void markChangedOutsideHistory() noexcept { ++changesOutsideHistory; }
    int getChangesOutsideHistory() const noexcept { return changesOutsideHistory; }

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

private:
    juce::ValueTree state;
    juce::UndoManager undoManager;
    juce::File file, cacheFolder;
    mutable juce::File ownCacheFolder; // in the temporary folder, while no other is given
    int changesOutsideHistory = 0;
    juce::ListenerList<ParameterListener> parameterListeners;
    std::function<void (std::uint64_t)> deviceStateStore;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Song)
    JUCE_DECLARE_NON_COPYABLE (Song)
};
} // namespace phazite
