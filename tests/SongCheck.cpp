// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SongCheck.h"
#include "SongIDs.h"
#include "devices/core/DeviceInfo.h"
#include "devices/modulation/Modulators.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongIndex.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/modulation/internal/Modulation.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace phazite::test
{
namespace
{
    namespace schema = phazite::schema;

    // A value as a number: a number's own, or text that is a number and nothing else; none for anything else.
    std::optional<double> readNumber (const juce::var& value)
    {
        if (value.isInt() || value.isInt64() || value.isDouble() || value.isBool())
            return (double) value;

        if (! value.isString())
            return std::nullopt;

        const auto text = value.toString().trim().toStdString();
        double number = 0.0;
        const auto read = std::from_chars (text.data(), text.data() + text.size(), number);

        if (text.empty() || read.ec != std::errc() || read.ptr != text.data() + text.size())
        {
            // What from_chars reads as a number too large to hold, or as no number: "inf", "nan"... are no numbers here.
            if (read.ec == std::errc::result_out_of_range)
                return std::numeric_limits<double>::infinity();

            return std::nullopt;
        }

        return number;
    }

    juce::String describe (const juce::ValueTree& element)
    {
        auto text = element.getType().toString();

        if (element.hasProperty (schema::trackID))
            text << " " << element[schema::trackID].toString();

        return text;
    }

    struct Checker
    {
        const Song& song;
        juce::File folder;            // the project's, or none
        const schema::Tables& tables; // what Schema.h describes, or a test's changed copy of it
        std::vector<std::string> problems;

        void add (const char* rule, const juce::ValueTree& element, const juce::String& text)
        {
            problems.push_back (juce::String (juce::String (rule) + ": " + describe (element) + ": " + text).toStdString());
        }

        //==============================================================================
        void checkPlaces (const juce::ValueTree& parent)
        {
            const auto type = parent.getType().toString();

            for (const auto& child : parent)
            {
                const auto childType = child.getType().toString();

                if (! isKnownElementType (child))
                    continue; // one of a type Phazite doesn't know, which stays as it is

                const auto* described = tables.findElement (child.getType());

                if (described == nullptr)
                    continue; // one the tables given don't describe

                if (! described->goesIn (parent.getType()))
                    add ("place", child, "not a child of " + type);
                else if (childType == "TRACK" && type == "TRACK" && parent[schema::trackKind].toString() != schema::trackIsFolder)
                    add ("place", child, "a track in a track that isn't a folder");
                else if (childType == "CLIP" && parent[schema::trackKind].toString() != schema::trackIsTrack)
                    add ("place", child, "a clip in a folder or a return (only a track holds clips: PlaylistModel.cpp, holdsClips())");
                else if ((childType == "NOTE" || childType == "CONTROL") && parent[schema::clipKind].toString() != schema::clipIsMidi)
                    add ("place", child, "in a clip that isn't a MIDI clip");
                else if (childType == "WARP" && parent[schema::clipKind].toString() != schema::clipIsAudio)
                    add ("place", child, "in a clip that isn't an audio clip");

                checkPlaces (child); // in an element nothing goes in, any element of a type Phazite knows is out of place
            }
        }

        //==============================================================================
        void checkKeys (const juce::ValueTree& parent)
        {
            std::map<juce::String, int> keys;

            for (const auto& child : parent)
            {
                if (! isKnownElementType (child))
                    continue;

                // Items are keyed by their IDs, which the id rule checks across the whole song.
                if (! child.hasProperty (schema::trackID) || ! schema::holdsID (child, schema::trackID))
                {
                    const auto key = schema::getElementKey (child);

                    if (key.isNotEmpty() && ++keys[child.getType().toString() + "/" + key] == 2)
                        add ("key", child, "two with the key " + key);
                }

                checkKeys (child);
            }
        }

        //==============================================================================
        void checkNumber (const juce::ValueTree& element, const juce::Identifier& property, double low, double high, bool whole,
                          const juce::String& what)
        {
            const auto value = element[property];
            const auto number = readNumber (value);

            if (! number.has_value() || ! std::isfinite (*number))
                add ("range", element, what + " isn't a finite number: " + value.toString().substring (0, 40));
            else if (*number < low || *number > high)
                add ("range", element, what + " " + value.toString() + " beyond " + juce::String (low) + ".." + juce::String (high));
            else if (whole && ! juce::exactlyEqual (*number, std::round (*number)))
                add ("range", element, what + " " + value.toString() + " isn't whole");
        }

        // A value set by hand of a parameter of a description (a device's, a modulator's).
        void checkSetByHand (const juce::ValueTree& element, const juce::ValueTree& values,
                             const std::function<const devices::ParameterInfo*(std::string_view)>& find)
        {
            for (int i = 0; i < values.getNumProperties(); ++i)
            {
                const auto name = values.getPropertyName (i);
                const auto* parameter = find (std::string_view (name.getCharPointer().getAddress()));

                if (parameter == nullptr)
                {
                    add ("names", element, "a value of a parameter it hasn't: " + name.toString());
                    continue;
                }

                const auto saved = values[name];

                if (parameter->unit == devices::Unit::choice && saved.isString())
                {
                    if (! devices::findChoice (*parameter, saved.toString().toStdString()).has_value())
                        add ("range", element, name.toString() + " is no choice of its: " + saved.toString().substring (0, 40));

                    continue;
                }

                const auto number = readNumber (saved);

                if (! number.has_value() || ! std::isfinite (*number))
                    add ("range", element, name.toString() + " isn't a finite number: " + saved.toString().substring (0, 40));
                else if (const auto value = (float) *number; ! juce::exactlyEqual (devices::constrain (*parameter, value), value))
                    add ("range", element, name.toString() + " " + saved.toString() + " beyond its range");
            }
        }

        void checkPath (const juce::ValueTree& element, const juce::Identifier& property)
        {
            const auto path = element[property].toString();

            if (path.isEmpty() || folder == juce::File())
                return;

            // On a network share: never, which Windows would connect to as it looks.
            if (path.length() >= 2 && (path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/'))
            {
                add ("path", element, "a file on a network share: " + path);
                return;
            }

            // A full path of any computer's: Phazite writes one where it couldn't copy a file into the project (Import.h:
            // copyIntoProject()), as for a file of one of the library's folders. A ':' after anything but a drive's letter is
            // part of a file's name ("Loop 1:2.wav").
            const auto isFullPath = juce::File::isAbsolutePath (path) || path.startsWithChar ('\\') || path.startsWithChar ('/')
                                    || (path.length() >= 2 && juce::CharacterFunctions::isLetter (path[0]) && path[1] == ':');

            if (! isFullPath && ! folder.getChildFile (path).isAChildOf (folder))
                add ("path", element, "a file that leaves the project's folder: " + path);
        }

        void checkRanges (const juce::ValueTree& element)
        {
            const auto& described = tables.on (element.getType());

            for (const auto* property : described.ranged)
            {
                if (! element.hasProperty (*property))
                    continue;

                const auto& range = *property->getRange();
                checkNumber (element, *property, range.low, range.high, range.whole, property->toString());

                // Where it says so (a time signature's note value), a power of two.
                if (const auto number = readNumber (element[*property]);
                    range.powersOfTwo && number.has_value() && *number > 0.0
                    && ! juce::exactlyEqual (std::exp2 (std::round (std::log2 (*number))), *number))
                    add ("range", element, property->toString() + " " + element[*property].toString() + " isn't a power of two");
            }

            // Numbers between spaces (Form::numbers): a step sequencer's steps, -1 to 1; the sampler's slices, parts of its
            // sound, 0 to 1.
            for (const auto* property : described.lists)
            {
                for (const auto& token : juce::StringArray::fromTokens (element[*property].toString(), " ", {}))
                {
                    if (token.isEmpty())
                        continue;

                    juce::ValueTree one ("NUMBER");
                    one.setProperty (*property, token, nullptr);
                    checkNumber (one, *property, property->getRange()->low, property->getRange()->high, false,
                                 element.getType().toString() + " " + property->toString());
                }
            }

            // Ids written as text: so many lowercase hexadecimal digits, or none (where the song stands in the project's
            // versions, and the numbering its IDs are in).
            for (const auto* property : described.hexIDs)
                if (const auto text = element[*property].toString();
                    element.hasProperty (*property)
                    && (text.length() != property->getHexDigits() || ! text.containsOnly ("0123456789abcdef")))
                    add ("kind", element, property->toString() + " is no id: " + text.substring (0, 80));
        }

        // Every item with an ID of its own, and one the song can count on from.
        void checkItemIDs (const juce::ValueTree& tree)
        {
            for (const auto& child : tree)
            {
                if (! isKnownElementType (child))
                    continue;

                if (schema::holdsID (child, schema::trackID))
                    if (const auto id = readID (child[schema::trackID]); id == 0 || id >= schema::maxID)
                        add ("id", child, "an item without an ID the song can count on from");

                checkItemIDs (child);
            }
        }

        // The device of an ID, if the song has it.
        juce::ValueTree findDevice (const juce::ValueTree& tree, std::uint64_t id)
        {
            for (const auto& child : tree)
            {
                if (child.hasType (schema::DEVICE) && readID (child[schema::deviceID]) == id)
                    return child;

                if (const auto found = findDevice (child, id); found.isValid())
                    return found;
            }

            return {};
        }

        void checkValues (const juce::ValueTree& tree)
        {
            for (const auto& child : tree)
            {
                if (! isKnownElementType (child))
                    continue;

                checkRanges (child);

                for (const auto* property : tables.on (child.getType()).worded)
                {
                    const auto text = child[*property].toString();
                    const auto& words = property->getWords();

                    if (child.hasProperty (*property)
                            ? std::none_of (words.begin(), words.end(), [&text] (const char* word) { return text == word; })
                            : property->isAlways())
                        add ("kind", child, property->toString() + " is none of its words: " + text);
                }

                if (child.hasType (schema::DEVICE))
                {
                    const auto deviceType = child[schema::deviceType].toString();
                    const auto* info = findOwnDevice (deviceType.toStdString());

                    if (info == nullptr && deviceType != schema::deviceIsPlugin)
                        add ("kind", child, "a device of no type Phazite has: " + deviceType);

                    if (const auto values = child.getChildWithName (schema::VALUES); values.isValid() && info != nullptr)
                        checkSetByHand (child, values,
                                        [info] (std::string_view id) -> const devices::ParameterInfo*
                                        {
                                            const auto index = devices::findParameter (*info, id);
                                            return index >= 0 ? &info->parameters[(size_t) index] : nullptr;
                                        });
                    else if (values.isValid())
                        add ("place", child, "values by name on a device that isn't one of Phazite's own");
                }

                if (child.hasType (schema::MODULATOR))
                {
                    if (const auto kind = getModulatorKind (child); ! kind.has_value())
                        add ("kind", child, "a modulator of no type Phazite has: " + child[schema::modulatorType].toString());
                    else if (const auto values = child.getChildWithName (schema::VALUES); values.isValid())
                        checkSetByHand (child, values,
                                        [k = *kind] (std::string_view id) -> const devices::ParameterInfo*
                                        {
                                            const auto index = devices::findModulatorParameter (k, id);
                                            return index >= 0 ? &devices::getModulatorParameters (k)[(size_t) index] : nullptr;
                                        });
                }

                if (child.hasType (schema::POINT) && tree.hasType (schema::CURVE))
                {
                    const ParameterRef parameter { DeviceID::fromRaw (readID (tree[schema::curveDevice])),
                                                   tree[schema::curveParameter].toString() };

                    // A position along its parameter's control's travel. A plug-in tells which parameters it has only once it
                    // plays; a chain's crossover is kept while its container isn't split into bands or it is the first chain.
                    checkNumber (child, schema::pointValue, 0.0, 1.0, false, "value");

                    if (! describeParameter (song, parameter).has_value()
                        && findDevice (song.getState(), parameter.device.getRaw())[schema::deviceType].toString() != schema::deviceIsPlugin
                        && ! (findContainerChain (song, parameter.device).isValid() && parameter.parameter == schema::crossoverParameter))
                        add ("names", tree, "a curve of a parameter the song hasn't: " + parameter.parameter);
                }

                if (child.hasType (schema::MIDILINK))
                {
                    // A controller and a parameter the song has (a plug-in's while its element says it is one; a macro's
                    // value by the word "value", its ID being the macro's), one link of each parameter (one of each
                    // controller is its key's).
                    const auto device = DeviceID::fromRaw (readID (child[schema::midiLinkDevice]));
                    const auto isMacro = findModulatorState (song, ModulatorID::fromRaw (device.getRaw())).hasType (schema::MACRO);
                    const ParameterRef parameter { device,
                                                   isMacro ? juce::String (device.getRaw()) : child[schema::midiLinkParameter].toString() };

                    if (isMacro && child[schema::midiLinkParameter].toString() != schema::midiLinkMacroValue)
                        add ("names", child,
                             "a MIDI link of a macro that doesn't name its value: " + child[schema::midiLinkParameter].toString());

                    if (! child.hasProperty (schema::midiLinkChannel) || ! child.hasProperty (schema::midiLinkNumber))
                        add ("names", child, "a MIDI link without its controller");

                    if (! describeParameter (song, parameter).has_value()
                        && findDevice (song.getState(), parameter.device.getRaw())[schema::deviceType].toString() != schema::deviceIsPlugin)
                        add ("names", child, "a MIDI link of a parameter the song hasn't: " + parameter.parameter);

                    for (const auto& other : tree)
                        if (other != child && other.hasType (schema::MIDILINK)
                            && other[schema::midiLinkDevice] == child[schema::midiLinkDevice]
                            && other[schema::midiLinkParameter] == child[schema::midiLinkParameter])
                            add ("key", child, "a second MIDI link of a parameter: " + parameter.parameter);
                }

                if (child.hasType (schema::CLIP))
                {
                    checkPath (child, schema::clipFile);

                    if (child[schema::clipKind].toString() == schema::clipIsAudio && child[schema::clipFile].toString().isEmpty())
                        add ("path", child, "an audio clip without a file");
                }

                if (child.hasType (schema::SOUNDS))
                    for (int i = 0; i < child.getNumProperties(); ++i)
                        if (const auto property = child.getPropertyName (i); schema::isSoundFileProperty (property))
                            checkPath (child, property);

                checkValues (child);
            }
        }

        //==============================================================================
        // A clip's notes and controllers' values, in the order the clip holds them, each as its type and every property it
        // has, by name, with its value as text.
        static std::vector<juce::String> describeEvents (const juce::ValueTree& clip)
        {
            std::vector<juce::String> events;

            for (const auto& child : clip)
            {
                if (! child.hasType (schema::NOTE) && ! child.hasType (schema::CONTROL))
                    continue;

                juce::StringArray properties;

                for (int i = 0; i < child.getNumProperties(); ++i)
                    properties.add (child.getPropertyName (i).toString() + "=" + child[child.getPropertyName (i)].toString());

                properties.sort (false);
                events.push_back (child.getType().toString() + " " + properties.joinIntoString (" "));
            }

            return events;
        }

        // The song's clips that are in an alias group, by the group, each group's in the song's order.
        void collectGroups (const juce::ValueTree& tree, std::map<juce::String, std::vector<juce::ValueTree>>& groups)
        {
            for (const auto& child : tree)
            {
                if (! isKnownElementType (child))
                    continue;

                if (child.hasType (schema::CLIP) && child.hasProperty (schema::clipLink))
                    groups[juce::String (readID (child[schema::clipLink]))].push_back (child);

                if (child.hasType (schema::TRACKS) || child.hasType (schema::TRACK))
                    collectGroups (child, groups);
            }
        }

        // A clip's NOTE and CONTROL elements, in order.
        static std::vector<juce::ValueTree> getEvents (const juce::ValueTree& clip)
        {
            std::vector<juce::ValueTree> events;

            for (const auto& child : clip)
                if (child.hasType (schema::NOTE) || child.hasType (schema::CONTROL))
                    events.push_back (child);

            return events;
        }

        // Whether a clip holds the same events as the group's first, each equivalent to the first's in its place, the first's
        // compared with the clip's in that order (juce::ValueTree::isEquivalentTo(), whose values compare as juce::var's do,
        // which isn't the same both ways round for a number against text), as the loader compares them.
        static bool holdTheSameEvents (const juce::ValueTree& first, const juce::ValueTree& clip)
        {
            const auto a = getEvents (first);
            const auto b = getEvents (clip);

            return a.size() == b.size()
                   && std::equal (a.begin(), a.end(), b.begin(),
                                  [] (const juce::ValueTree& x, const juce::ValueTree& y) { return x.isEquivalentTo (y); });
        }

        // The clips of an alias group (Schema.h: clipLink) hold the same notes and controllers' values, every property of
        // each, in the same order; they are of one kind; and a group has two clips at least.
        // Clips are compared as the loader compares them (Project.cpp: keepAliasGroups()): as values, so that numbers that are
        // the same within JUCE's own tolerance (juce::var's, a double's against a number or text that reads as one) are the
        // same; the message describes each event as text.
        void checkAliases()
        {
            std::map<juce::String, std::vector<juce::ValueTree>> groups;
            collectGroups (song.getState(), groups);

            for (const auto& [link, clips] : groups)
            {
                const auto& first = clips.front();

                if (clips.size() < 2)
                {
                    add ("alias", first, "the only clip of its alias group " + link);
                    continue;
                }

                const auto events = describeEvents (first);

                for (size_t i = 1; i < clips.size(); ++i)
                {
                    const auto& clip = clips[i];
                    const auto clipText = "clip " + clip[schema::clipID].toString() + " of alias group " + link + ": ";

                    if (clip[schema::clipKind].toString() != first[schema::clipKind].toString())
                    {
                        add ("alias", clip,
                             clipText + "a " + clip[schema::clipKind].toString() + " clip in a group with a "
                                 + first[schema::clipKind].toString() + " clip");
                        continue;
                    }

                    if (holdTheSameEvents (first, clip))
                        continue;

                    const auto others = describeEvents (clip);

                    // Where they first differ, for the failure's message.
                    size_t at = 0;

                    while (at < events.size() && at < others.size() && events[at] == others[at])
                        ++at;

                    add ("alias", clip,
                         clipText + "its notes and values differ from clip " + first[schema::clipID].toString() + "'s at event "
                             + juce::String ((int) at + 1) + ": \"" + (at < others.size() ? others[at] : juce::String ("none"))
                             + "\" against \"" + (at < events.size() ? events[at] : juce::String ("none")) + "\" ("
                             + juce::String ((int) others.size()) + " against " + juce::String ((int) events.size()) + ")");
                }
            }
        }
    };
} // namespace

std::vector<std::string> findSongProblems (const Song& song)
{
    return findSongProblems (song, schema::getTables());
}

std::vector<std::string> findSongProblems (const Song& song, const schema::Tables& tables)
{
    const auto& state = song.getState();
    Checker checker { song, song.getFile() != juce::File() ? song.getFile().getParentDirectory() : juce::File(), tables, {} };

    if (! state.hasType (schema::PHAZITE))
        return { "place: the song isn't a PHAZITE element" };

    checker.checkPlaces (state);

    for (const auto& problem : findIDProblems (state))
        checker.problems.push_back (problem);

    const auto next = (std::uint64_t) (juce::int64) state.getProperty (schema::nextID, (juce::int64) 0);

    if (findNextID (state) > next)
        checker.problems.push_back ("id: the song's next ID is one it holds already");

    // Room to count on (Schema.h: renumberMark): a song's next ID below the mark is kept as it loads, and one that reaches
    // it, or an ID that does, makes the loader count on from below schema::maxCountedID; so a song's next ID is at most the
    // mark once it has loaded, 2^60 below schema::maxID.
    if (next > schema::renumberMark)
        checker.problems.push_back ("id: the song's next ID leaves no room before the largest an ID can be");

    checker.checkItemIDs (state);
    checker.checkKeys (state);
    checker.checkRanges (state);
    checker.checkValues (state);
    checker.checkAliases();
    return checker.problems;
}

void checkSong (const Song& song)
{
    for (const auto& problem : findSongProblems (song))
    {
        INFO (problem);
        CHECK (false);
    }

    // And what finds its items by their IDs holds what it does (model/base/internal/SongIndex.h).
    CHECK (song.getIndex().isExact());
}

Song::Ptr copyAsOpened (const Song& song)
{
    Song::Ptr copy (new Song (song.getState().createCopy()));
    forgetCurvesOfGoneDevices (*copy);
    copy->getState().removeProperty (schema::playStart, nullptr);
    std::set<std::uint64_t> items;
    findIDProblems (copy->getState(), &items);

    const std::function<void (juce::ValueTree)> visit = [&] (juce::ValueTree tree)
    {
        for (auto i = tree.getNumChildren(); --i >= 0;)
        {
            auto child = tree.getChild (i);

            if (child.hasType (schema::LANE) && items.count (readID (child[schema::laneDevice])) == 0)
            {
                tree.removeChild (i, nullptr);
                continue;
            }

            visit (child);
        }
    };

    visit (copy->getState());
    return copy;
}
} // namespace phazite::test
