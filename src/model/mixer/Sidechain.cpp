// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Sidechain.h"
#include "model/commands/internal/Undo.h"

namespace phazite
{
namespace
{
    bool listensToAny (const te::Plugin& plugin, const juce::Array<te::Track*>& tracks)
    {
        const auto source = plugin.getSidechainSourceID();

        if (source.isValid())
            for (auto* track : tracks)
                if (track->itemID == source)
                    return true;

        return false;
    }

    // Whether a track's sound comes from any of these tracks: a bus mixes the tracks inside it, a return takes in what
    // is sent to its bus, and a device can listen to a sidechain. (Returns and sends are read from their aux plug-ins,
    // as Mixer.h makes them: Mixer.h depends on this file, through deleting tracks.)
    bool isFedBy (const te::Track& track, const juce::Array<te::Track*>& feeding)
    {
        if (const auto* folder = dynamic_cast<const te::FolderTrack*> (&track))
            for (auto* inside : folder->getAllSubTracks (false))
                if (feeding.contains (inside))
                    return true;

        for (auto* input : track.pluginList.getPluginsOfType<te::AuxReturnPlugin>())
            for (auto* sender : feeding)
                for (auto* send : sender->pluginList.getPluginsOfType<te::AuxSendPlugin>())
                    if (send->getBusNumber() == input->busNumber.get())
                        return true;

        for (auto* plugin : track.pluginList)
            if (listensToAny (*plugin, feeding))
                return true;

        return false;
    }

    // The tracks whose sound comes from a track, the track itself first: those it feeds, those they feed, and so on.
    juce::Array<te::Track*> getTracksFedBy (te::Track& track)
    {
        juce::Array<te::Track*> fed { &track };
        const auto all = te::getAllTracks (track.edit);

        for (auto grew = true; grew;)
        {
            grew = false;

            for (auto* other : all)
            {
                if (! fed.contains (other) && isFedBy (*other, fed))
                {
                    fed.add (other);
                    grew = true;
                }
            }
        }

        return fed;
    }
} // namespace

bool hasSidechain (te::Plugin& plugin)
{
    return plugin.canSidechain();
}

juce::Array<te::AudioTrack*> getSidechainSources (te::Plugin& plugin)
{
    juce::Array<te::AudioTrack*> sources;

    if (! hasSidechain (plugin))
        return sources;

    // A device on the master, which no track's sound comes from, can listen to any track.
    juce::Array<te::Track*> fed;

    if (auto* owner = plugin.getOwnerTrack())
        fed = getTracksFedBy (*owner);

    for (auto* track : te::getAudioTracks (plugin.edit))
        if (! fed.contains (track))
            sources.add (track);

    return sources;
}

te::AudioTrack* getSidechainSource (te::Plugin& plugin)
{
    const auto source = plugin.getSidechainSourceID();
    return source.isValid() ? dynamic_cast<te::AudioTrack*> (te::findTrackForID (plugin.edit, source)) : nullptr;
}

void setSidechainSource (te::Plugin& plugin, te::AudioTrack* source)
{
    if (source == getSidechainSource (plugin) || (source != nullptr && ! getSidechainSources (plugin).contains (source)))
        return;

    const UndoStep step (plugin.edit, source != nullptr ? "Sidechain from " + source->getName() : juce::String ("Turn Sidechain Off"));
    plugin.setSidechainSourceID (source != nullptr ? source->itemID : te::EditItemID());

    // The source's two channels go to the sidechain's inputs: the routing Tracktion guesses for a device's inputs.
    if (source != nullptr && plugin.getNumWires() == 0)
        plugin.guessSidechainRouting();
}

void forgetSidechainSources (const juce::Array<te::Track*>& tracks)
{
    if (tracks.isEmpty())
        return;

    juce::Array<te::Track*> going;

    for (auto* track : tracks)
    {
        going.addIfNotAlreadyThere (track);

        for (auto* inside : track->getAllSubTracks (true))
            going.addIfNotAlreadyThere (inside);
    }

    for (auto* plugin : tracks.getFirst()->edit.getPluginCache().getPlugins())
        if (listensToAny (*plugin, going))
            plugin->setSidechainSourceID ({});
}
} // namespace phazite
