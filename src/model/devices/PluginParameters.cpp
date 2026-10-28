// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/PluginParameters.h"
#include "model/base/internal/Schema.h"

#include <map>
#include <optional>
#include <set>

namespace phazite
{
namespace
{
    // What a reference to a parameter becomes: another ID, or nothing (it goes).
    using Rename = std::function<std::optional<juce::String> (const juce::String& id)>;

    // The plug-in's own ID for one of its parameters that Tracktion knows by its place in the list: a parameter a plug-in
    // format hosts (a VST3's). One with an ID of its own in JUCE Tracktion knows by that already.
    std::optional<juce::String> getOwnID (const juce::AudioProcessorParameter& parameter)
    {
        if (dynamic_cast<const juce::AudioProcessorParameterWithID*> (&parameter) != nullptr)
            return {};

        if (const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*> (&parameter))
            return hosted->getParameterID();

        return {};
    }

    // Sets a property without an undo manager, only if its value is another.
    void store (juce::ValueTree& state, const juce::Identifier& property, const juce::var& value)
    {
        if (const auto* stored = state.getPropertyPointer (property); stored == nullptr || *stored != value)
            state.setProperty (property, value, nullptr);
    }

    // The children of a tree, renamed, whose ID property names a parameter; those renamed to nothing go.
    void renameChildren (juce::ValueTree tree, const juce::Identifier& property, const Rename& rename,
                         const std::function<bool (const juce::ValueTree&)>& isReference)
    {
        for (int i = tree.getNumChildren(); --i >= 0;)
        {
            auto child = tree.getChild (i);

            if (! isReference (child))
                continue;

            if (const auto renamed = rename (child[property].toString()))
                store (child, property, *renamed);
            else
                tree.removeChild (i, nullptr);
        }
    }

    // The values set by hand of parameters that automation moves (te::IDs::parameters: each one's ID, then its value, as
    // AutomatableEditItem::saveChangedParametersToState() writes them).
    void renameValuesSetByHand (juce::ValueTree& state, const Rename& rename)
    {
        const auto* saved = state[te::IDs::parameters].getBinaryData();

        if (saved == nullptr)
            return;

        juce::MemoryInputStream in (*saved, false);
        juce::MemoryOutputStream out;

        while (! in.isExhausted())
        {
            const auto id = in.readString();
            const auto value = in.readFloat();

            if (const auto renamed = rename (id))
            {
                out.writeString (*renamed);
                out.writeFloat (value);
            }
        }

        if (out.getDataSize() > 0)
            store (state, te::IDs::parameters, out.getMemoryBlock());
        else
            state.removeProperty (te::IDs::parameters, nullptr);
    }

    // A device's automation lanes, renamed, on whichever track shows them (a container's devices' on the container's track);
    // tracks are in the Edit, folders and the tracks in them, and nowhere else.
    void renameLanes (const juce::ValueTree& element, const juce::String& device, const Rename& rename)
    {
        for (const auto& child : element)
        {
            if (child.hasType (te::IDs::TRACK) || child.hasType (te::IDs::FOLDERTRACK) || child.hasType (te::IDs::MASTERTRACK))
            {
                renameChildren (child, schema::laneParameter, rename, [&device] (const juce::ValueTree& lane)
                                { return lane.hasType (schema::PHAZITELANE) && lane[schema::laneDevice].toString() == device; });
                renameLanes (child, device, rename);
            }
        }
    }

    // Everything in the project that names one of the plug-in's parameters, renamed: its automation curves, what modulates
    // them (modulators and macros are assigned in the state of what they move), its values set by hand, its quick control,
    // and its automation lanes.
    void renameReferences (te::ExternalPlugin& plugin, const Rename& rename)
    {
        auto& state = plugin.state;
        renameChildren (state, te::IDs::paramID, rename,
                        [] (const juce::ValueTree& child) { return child.hasType (te::IDs::AUTOMATIONCURVE); });
        renameChildren (state.getChildWithName (te::IDs::MODIFIERASSIGNMENTS), te::IDs::paramID, rename,
                        [] (const juce::ValueTree& child) { return child.hasProperty (te::IDs::paramID); });
        renameValuesSetByHand (state, rename);

        if (const auto quick = state[te::IDs::quickParamName].toString(); quick.isNotEmpty())
        {
            if (const auto renamed = rename (quick))
                store (state, te::IDs::quickParamName, *renamed);
            else
                state.removeProperty (te::IDs::quickParamName, nullptr);
        }

        renameLanes (plugin.edit.state, state[te::IDs::id].toString(), rename);
    }

    // What phaziteParameterIDs holds: each Tracktion ID with the plug-in's own ID for it.
    std::map<juce::String, juce::String> readStoredIDs (const juce::ValueTree& state)
    {
        std::map<juce::String, juce::String> stored;

        for (const auto& pair : juce::StringArray::fromTokens (state[schema::phaziteParameterIDs].toString(), " ", {}))
            if (const auto at = pair.indexOfChar ('='); at > 0)
                stored.emplace (pair.substring (0, at), pair.substring (at + 1));

        return stored;
    }

    void writeStoredIDs (juce::ValueTree& state, const std::map<juce::String, juce::String>& stored)
    {
        juce::StringArray pairs;

        for (const auto& [id, own] : stored)
            pairs.add (id + "=" + own);

        if (pairs.isEmpty())
            state.removeProperty (schema::phaziteParameterIDs, nullptr);
        else
            store (state, schema::phaziteParameterIDs, pairs.joinIntoString (" "));
    }
} // namespace

void storeParameterIDs (te::ExternalPlugin& plugin)
{
    auto* instance = plugin.getAudioPluginInstance();

    if (instance == nullptr)
        return;

    std::set<juce::String> referredTo;
    renameReferences (plugin,
                      [&referredTo] (const juce::String& id)
                      {
                          referredTo.insert (id);
                          return id;
                      });

    // An ID that is a place in the list, where the plug-in has a parameter that Tracktion knows by its place.
    const auto& parameters = instance->getParameters();
    std::map<juce::String, juce::String> stored;

    for (const auto& id : referredTo)
        if (const auto index = id.getIntValue(); id == juce::String (index) && juce::isPositiveAndBelow (index, parameters.size()))
            if (const auto own = getOwnID (*parameters.getUnchecked (index)))
                stored.emplace (id, *own);

    writeStoredIDs (plugin.state, stored);
}

void findMovedParameters (te::ExternalPlugin& plugin)
{
    const auto stored = readStoredIDs (plugin.state);
    auto* instance = plugin.getAudioPluginInstance();

    if (stored.empty() || instance == nullptr)
        return;

    // Where each own ID is now, as Tracktion will name it: by its place.
    std::map<juce::String, juce::String> now;
    const auto& parameters = instance->getParameters();

    for (int i = 0; i < parameters.size(); ++i)
        if (const auto own = getOwnID (*parameters.getUnchecked (i)))
            now.emplace (*own, juce::String (i));

    // Each stored ID that moved, to where, or to nothing if the parameter is gone.
    std::map<juce::String, std::optional<juce::String>> moves;
    std::map<juce::String, juce::String> nowStored;
    juce::StringArray gone;

    for (const auto& [id, own] : stored)
    {
        if (const auto found = now.find (own); found != now.end())
        {
            nowStored.emplace (found->second, own);

            if (found->second != id)
                moves.emplace (id, found->second);
        }
        else
        {
            moves.emplace (id, std::nullopt);
            gone.add (own);
        }
    }

    if (moves.empty() || nowStored.empty())
        return;

    // All at once, as two parameters can swap places.
    renameReferences (plugin,
                      [&moves] (const juce::String& id) -> std::optional<juce::String>
                      {
                          const auto move = moves.find (id);
                          return move != moves.end() ? move->second : id;
                      });
    writeStoredIDs (plugin.state, nowStored);

    if (! gone.isEmpty())
        juce::Logger::writeToLog ("The plug-in " + plugin.getName() + " no longer has the parameters " + gone.joinIntoString (", ")
                                  + " the project automated or modulated: that automation and modulation went.");
}
} // namespace phazite
