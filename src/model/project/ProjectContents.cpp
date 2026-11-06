// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ProjectContents.h"
#include "internal/ProjectContext.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DeviceFiles.h"
#include "model/mixer/internal/Mixer.h"
#include "model/plugins/internal/PluginInstances.h"

#include <functional>
#include <map>

namespace phazite
{
namespace
{
    // Each channel's element, the tracks top to bottom (a folder before its tracks), then the master's, with its name.
    struct Channel
    {
        TrackID id;
        juce::String name;
        juce::ValueTree state;
    };

    std::vector<Channel> getChannels (const Song& song)
    {
        std::vector<Channel> channels;

        for (const auto& state : getTrackStates (song))
        {
            const auto id = getTrackID (state);
            channels.push_back ({ id, getTrackName (song, id), state });
        }

        const auto master = getMasterID (song);

        if (const auto state = findTrackState (song, master); state.isValid())
            channels.push_back ({ master, "Master", state });

        return channels;
    }

    // Everything in a channel's element, but the tracks a folder holds, which are channels of their own.
    void forEachIn (const juce::ValueTree& element, const std::function<void (const juce::ValueTree&)>& visit)
    {
        for (const auto& child : element)
        {
            if (child.hasType (schema::TRACK))
                continue;

            visit (child);
            forEachIn (child, visit);
        }
    }

    // Each path an element names an audio file by: a clip's file, or a device's sound file (a pad's or an oscillator's).
    void forEachAudioPath (const juce::ValueTree& element, const std::function<void (const juce::Identifier&)>& visit)
    {
        if (element.hasType (schema::CLIP) && element[schema::clipFile].toString().isNotEmpty())
            visit (schema::clipFile);

        if (element.hasType (schema::SOUNDS))
            for (int i = 0; i < element.getNumProperties(); ++i)
                if (const auto property = element.getPropertyName (i);
                    schema::isSoundFileProperty (property) && element[property].toString().isNotEmpty())
                    visit (property);
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

std::vector<ProjectAudioFile> getProjectAudioFiles (const ProjectContext& context)
{
    std::vector<ProjectAudioFile> files;
    const auto* song = findSong (context);

    if (song == nullptr)
        return files;

    std::map<juce::String, size_t> places; // each path's place among the files

    for (const auto& channel : getChannels (*song))
    {
        forEachIn (channel.state,
                   [&] (const juce::ValueTree& element)
                   {
                       forEachAudioPath (element,
                                         [&] (const juce::Identifier& property)
                                         {
                                             const auto path = element[property].toString();
                                             auto [place, isNew] = places.try_emplace (path, files.size());

                                             if (isNew)
                                             {
                                                 const auto file = findProjectFile (*song, path);
                                                 files.push_back ({ .path = path, .file = file, .missing = ! file.existsAsFile() });
                                             }

                                             auto& used = files[place->second];
                                             used.channels.addIfNotAlreadyThere (channel.name);
                                             ++used.uses;
                                         });
                   });
    }

    return files;
}

bool locateAudioFile (ProjectContext& context, const juce::String& path, const juce::File& found)
{
    auto* song = findSong (context);

    if (song == nullptr || path.isEmpty() || ! found.existsAsFile())
        return false;

    // Where the song plays the path now.
    std::vector<std::pair<juce::ValueTree, juce::Identifier>> uses;

    for (const auto& channel : getChannels (*song))
        forEachIn (channel.state,
                   [&uses, &path] (const juce::ValueTree& element)
                   {
                       forEachAudioPath (element,
                                         [&] (const juce::Identifier& property)
                                         {
                                             if (element[property].toString() == path)
                                                 uses.emplace_back (element, property);
                                         });
                   });

    if (uses.empty())
        return false;

    // The project's own copy of the file found, by the path the song keeps for it.
    const auto newPath = getProjectPath (*song, copyIntoProject (*song, found));

    if (newPath == path)
        return false;

    const UndoStep step (*song, "Locate Audio File");

    for (auto& [element, property] : uses)
        element.setProperty (property, newPath, &song->getUndoManager());

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
        forEachIn (channel.state,
                   [&] (const juce::ValueTree& element)
                   {
                       if (! element.hasType (schema::DEVICE) || element[schema::deviceType].toString() != schema::deviceIsPlugin)
                           return;

                       const auto id = getDeviceID (element);
                       plugins.push_back ({ .device = id,
                                            .channel = channel.id,
                                            .channelName = channel.name,
                                            .name = getDeviceName (element),
                                            .maker = element[schema::pluginMaker].toString(),
                                            .format = element[schema::pluginFormat].toString(),
                                            .isInstrument = (bool) element[schema::pluginInstrument],
                                            .state = toState (getPluginStatus (*song, id)) });
                   });
    }

    return plugins;
}
} // namespace phazite
