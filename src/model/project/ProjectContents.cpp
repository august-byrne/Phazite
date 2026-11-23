// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectContents.h"
#include "internal/ProjectContext.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/BackgroundJobs.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/mixer/internal/Mixer.h"
#include "model/plugins/internal/PluginInstances.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>

namespace phazite
{
namespace
{
    // Each channel, the tracks top to bottom (a folder before its tracks), then the master, by its ID, with its name. What
    // each holds is read by its ID: its clips (Clips.h, getClipValues()) and its devices (Mixer.h: getDeviceOutline(),
    // getDeviceSoundFiles()).
    struct Channel
    {
        TrackID id;
        juce::String name;
    };

    std::vector<Channel> getChannels (const Song& song)
    {
        std::vector<Channel> channels;

        for (const auto& state : getTrackStates (song))
        {
            const auto id = getTrackID (state);
            channels.push_back ({ id, getTrackName (song, id) });
        }

        const auto master = getMasterID (song);

        if (findTrackState (song, master).isValid())
            channels.push_back ({ master, "Master" });

        return channels;
    }

    PluginState toState (std::optional<PluginInstances::Status> status)
    {
        switch (status.value_or (PluginInstances::Status::playing))
        {
            case PluginInstances::Status::crashed:
            case PluginInstances::Status::crashedWhileLoading:
                return PluginState::crashed;
            case PluginInstances::Status::missing:
                return PluginState::missing;
            case PluginInstances::Status::off:
                return PluginState::off;
            case PluginInstances::Status::loading:
                return PluginState::loading;
            case PluginInstances::Status::playing:
            default:
                return PluginState::playing;
        }
    }
} // namespace

juce::String getProjectNotes (const ProjectContext& context)
{
    const auto* song = findSong (context);
    return song != nullptr ? song->getState()[schema::notes].toString() : juce::String();
}

void setProjectNotes (ProjectContext& context, const juce::String& text)
{
    auto* song = findSong (context);

    if (song == nullptr || text == getProjectNotes (context))
        return;

    const UndoStep step (*song, "Change Notes");

    if (text.isEmpty())
        song->getState().removeProperty (schema::notes, &song->getUndoManager());
    else
        song->getState().setProperty (schema::notes, text, &song->getUndoManager());
}

std::vector<ProjectAudioFile> getProjectAudioFiles (const ProjectContext& context, const std::set<juce::File>& missing)
{
    std::vector<ProjectAudioFile> files;
    const auto* song = findSong (context);

    if (song == nullptr)
        return files;

    std::map<juce::String, size_t> places; // each path's place among the files

    for (const auto& channel : getChannels (*song))
    {
        const auto use = [&] (const juce::String& path)
        {
            auto [place, isNew] = places.try_emplace (path, files.size());

            if (isNew)
            {
                const auto file = findProjectFile (*song, path);
                files.push_back ({ .path = path, .file = file, .missing = missing.count (file) > 0 });
            }

            auto& used = files[place->second];
            used.channels.addIfNotAlreadyThere (channel.name);
            ++used.uses;
        };

        // Its clips' files, in its clips' order, then its devices' sounds (a track that plays audio clips has no instrument).
        for (const auto& clip : getClipValues (*song, channel.id))
            if (clip.sound.file.isNotEmpty())
                use (clip.sound.file);

        for (const auto& sound : getDeviceSoundFiles (*song, channel.id))
            use (sound.path);
    }

    return files;
}

//==============================================================================
namespace
{
    std::atomic<int> diskQueriesOnMessageThread { 0 }, checksUnderWay { 0 };
} // namespace

void noteProjectDiskQuery() noexcept
{
    if (juce::MessageManager::existsAndIsCurrentThread())
        ++diskQueriesOnMessageThread;
}

int getProjectDiskQueriesOnMessageThread() noexcept
{
    return diskQueriesOnMessageThread.load();
}

// A check under way: told that its answer isn't wanted (a newer check waits: it stops at its next file, and says so), or
// that what asked for it has gone (it stops, and posts nothing).
struct MissingAudioCheck::Running
{
    std::atomic<bool> superseded { false }, gone { false };
};

MissingAudioCheck::MissingAudioCheck() = default;

MissingAudioCheck::~MissingAudioCheck()
{
    if (running != nullptr)
        running->gone = true; // left to end by itself
}

int MissingAudioCheck::getChecksUnderWay() noexcept
{
    return checksUnderWay.load();
}

void MissingAudioCheck::check (const juce::Array<juce::File>& files)
{
    checking = true;

    // One at a time: the one under way is asked to stop, and this one, the newest, waits for it.
    if (running != nullptr)
    {
        running->superseded = true;
        next = files;
        return;
    }

    start (files);
}

void MissingAudioCheck::start (const juce::Array<juce::File>& files)
{
    running = std::make_shared<Running>();
    ++started;
    ++checksUnderWay;

    // On a thread of its own, which nothing waits for: it owns what it uses (the files, the tests' hook, its state), and
    // once what asked for it has gone it posts nothing.
    auto checkFiles = [files, state = running, before = beforeChecking, weak = juce::WeakReference<MissingAudioCheck> (this)]
    {
        std::set<juce::File> found;
        auto stopped = false;

        for (const auto& file : files)
        {
            if (before != nullptr)
                before (file);

            if (state->superseded.load() || state->gone.load())
            {
                stopped = true;
                break;
            }

            if (file != juce::File() && ! file.existsAsFile())
                found.insert (file);
        }

        if (! state->gone.load())
            juce::MessageManager::callAsync (
                [weak, stopped, missingFound = std::move (found)]() mutable
                {
                    if (auto* self = weak.get())
                        self->ended (stopped, std::move (missingFound));
                });

        --checksUnderWay;
    };

    if (! runLeftBehind ("Phazite missing files", std::move (checkFiles)))
    {
        --checksUnderWay;
        running = nullptr;
        next.reset();
        checking = false;
    }
}

void MissingAudioCheck::ended (bool stopped, std::set<juce::File> found)
{
    running = nullptr;

    // The newest asked for while it ran, started now.
    if (next.has_value())
    {
        const auto files = std::move (*next);
        next.reset();
        start (files);
        return;
    }

    if (! stopped)
        missing = std::move (found);

    checking = false;

    if (onChecked != nullptr)
        onChecked();
}

//==============================================================================
bool locateAudioFile (ProjectContext& context, const juce::String& path, const juce::File& found)
{
    auto* song = findSong (context);

    noteProjectDiskQuery(); // the file found: one question, as the user chose it

    if (song == nullptr || path.isEmpty() || ! found.existsAsFile())
        return false;

    // Nothing to copy for a path the song doesn't play.
    const auto files = getProjectAudioFiles (context);

    if (std::none_of (files.begin(), files.end(), [&path] (const ProjectAudioFile& file) { return file.path == path; }))
        return false;

    // Copied into the project in the background, as an import copies a file (known bug 123); the path replaced once it is.
    return bringFilesIntoProject (*song, { .files = { found },
                                           .title = "Copying " + found.getFileName(),
                                           .problem = found.getFileName()
                                                      + " didn't take the missing file's place: the project changed while it "
                                                        "was copied in.",
                                           .apply = [&context, path, found] { return locateAudioFileNow (context, path, found); } });
}

bool locateAudioFileNow (ProjectContext& context, const juce::String& path, const juce::File& found)
{
    auto* song = findSong (context);

    noteProjectDiskQuery();

    if (song == nullptr || path.isEmpty() || ! found.existsAsFile())
        return false;

    // Where the song plays the path now: its clips and its devices' sounds, by their IDs.
    std::vector<ClipID> clips;
    std::vector<DeviceSoundFile> uses;

    for (const auto& channel : getChannels (*song))
    {
        for (const auto& clip : getClipValues (*song, channel.id))
            if (clip.sound.file == path)
                clips.push_back (clip.id);

        for (const auto& sound : getDeviceSoundFiles (*song, channel.id))
            if (sound.path == path)
                uses.push_back (sound);
    }

    if (clips.empty() && uses.empty())
        return false;

    // The project's own copy of the file found, by the path the song keeps for it.
    const auto newPath = getProjectPath (*song, copyIntoProject (*song, found));

    if (newPath == path)
        return false;

    const UndoStep step (*song, "Locate Audio File");

    setClipFiles (*song, path, newPath);

    for (const auto& sound : uses)
        setDeviceSound (*song, sound.device, sound.property, newPath);

    return true;
}

std::vector<ProjectPlugin> getProjectPlugins (const ProjectContext& context)
{
    std::vector<ProjectPlugin> plugins;
    const auto* song = findSong (context);

    if (song == nullptr)
        return plugins;

    for (const auto& channel : getChannels (*song))
    {
        for (const auto& device : getDeviceOutline (*song, channel.id))
        {
            if (device.form != DeviceValue::Form::plugin)
                continue;

            plugins.push_back ({ .device = device.id,
                                 .channel = channel.id,
                                 .channelName = channel.name,
                                 .name = device.plugin.name,
                                 .maker = device.plugin.maker,
                                 .format = device.plugin.format,
                                 .isInstrument = device.plugin.instrument,
                                 .state = toState (getPluginStatus (*song, device.id)) });
        }
    }

    return plugins;
}
} // namespace phazite
