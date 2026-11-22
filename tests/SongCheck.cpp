// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SongCheck.h"
#include "SongIDs.h"
#include "devices/core/DeviceInfo.h"
#include "devices/modulation/Modulators.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/ChordLaneValue.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/MidiLinksValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/SongIndex.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/modulation/internal/Modulation.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
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
        juce::ValueTree state;        // the song whole (TestHelpers.h: copyWholeSong()), which the rules walk
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
                        && findDevice (state, parameter.device.getRaw())[schema::deviceType].toString() != schema::deviceIsPlugin
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
                        && findDevice (state, parameter.device.getRaw())[schema::deviceType].toString() != schema::deviceIsPlugin)
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
            collectGroups (state, groups);

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

        //==============================================================================
        // The audio clips of an alias group (Schema.h: clipLink) play one file alike: they share the file (its path as the
        // song holds it), its tempo, pitch and direction, each as read (its default where a clip has none: no tempo, no
        // pitch, forwards), and its warp markers (WARP), the same in the same order. Compared as the loader compares
        // them (Project.cpp: keepAliasGroups()): the
        // group's first clip's against each other's, as values (juce::var's, and juce::ValueTree::isEquivalentTo() for the
        // markers).
        static juce::String describeMarker (const juce::ValueTree& marker)
        {
            return marker[schema::warpTime].toString() + " plays at " + marker[schema::warpPlaysAt].toString();
        }

        void checkAudioAliases()
        {
            struct Shared
            {
                juce::Identifier name;
                juce::var none;
            };

            const std::vector<Shared> shared { { schema::clipFile, juce::String() },
                                               { schema::clipTempo, 0.0 },
                                               { schema::clipPitch, 0.0 },
                                               { schema::clipReversed, false } };
            const auto markersOf = [] (const juce::ValueTree& clip)
            {
                std::vector<juce::ValueTree> markers;

                for (const auto& child : clip)
                    if (child.hasType (schema::WARP))
                        markers.push_back (child);

                return markers;
            };

            std::map<juce::String, std::vector<juce::ValueTree>> groups;
            collectGroups (state, groups);

            for (const auto& [link, clips] : groups)
            {
                const auto& first = clips.front();

                if (first[schema::clipKind].toString() != schema::clipIsAudio)
                    continue;

                const auto firstMarkers = markersOf (first);

                for (size_t i = 1; i < clips.size(); ++i)
                {
                    const auto& clip = clips[i];

                    if (clip[schema::clipKind].toString() != schema::clipIsAudio) // the kind's rule says so (checkAliases())
                        continue;

                    const auto clipText = "clip " + clip[schema::clipID].toString() + " of alias group " + link + ": ";

                    for (const auto& [name, none] : shared)
                        if (const auto a = first.getProperty (name, none), b = clip.getProperty (name, none); a != b)
                            add ("alias", clip,
                                 clipText + "its " + name.toString() + " " + b.toString() + " against clip "
                                     + first[schema::clipID].toString() + "'s " + a.toString());

                    const auto markers = markersOf (clip);
                    size_t at = 0;

                    while (at < markers.size() && at < firstMarkers.size() && firstMarkers[at].isEquivalentTo (markers[at]))
                        ++at;

                    if (at < markers.size() || at < firstMarkers.size())
                        add ("alias", clip,
                             clipText + "its warp markers differ from clip " + first[schema::clipID].toString() + "'s at marker "
                                 + juce::String ((int) at + 1) + ": \""
                                 + (at < markers.size() ? describeMarker (markers[at]) : juce::String ("none")) + "\" against \""
                                 + (at < firstMarkers.size() ? describeMarker (firstMarkers[at]) : juce::String ("none")) + "\" ("
                                 + juce::String ((int) markers.size()) + " against " + juce::String ((int) firstMarkers.size()) + ")");
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
    // The song whole, with what it keeps beside its tree: what its file would hold.
    const auto state = test::copyWholeSong (song);
    Checker checker { song, state, song.getFile() != juce::File() ? song.getFile().getParentDirectory() : juce::File(), tables, {} };

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
    checker.checkAudioAliases();

    for (auto& problem : findValueProblems (song))
        checker.problems.push_back (std::move (problem));

    return checker.problems;
}

namespace
{
    std::string clipText (std::uint64_t id)
    {
        return "values: clip " + std::to_string (id);
    }

    // The clips' area: no clip in the song's tree (its clips are the area's); the area's lists agree with its clips, and no
    // alias group is of one clip; each clip on a track of the song that can hold it; and the audio clips of an alias group
    // hold ONE sound (the same, not two alike).
    void checkClipsValue (const Song& song, const SongValues& values, std::vector<std::string>& problems)
    {
        const auto& clips = values.clips.get();

        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::CLIP))
                    problems.push_back (clipText ((std::uint64_t) (juce::int64) child[schema::clipID])
                                        + " is an element in the song's tree");

                visit (child);
            }
        };

        visit (song.getState());

        if (! holdsItsLists (clips))
            problems.push_back ("values: the clips' lists (each track's, each alias group's) don't hold the clips the area holds");

        if (hasGroupsOfOne (clips))
            problems.push_back ("values: an alias group holds one clip alone");

        std::map<std::uint64_t, ClipID> firstOfGroups;

        for (const auto& [id, clip] : clips.clips)
        {
            if (const auto track = findTrackState (song, clip.track);
                ! track.hasType (schema::TRACK) || isFolderState (track) || isReturnState (track))
                problems.push_back (clipText (id) + " is on track " + std::to_string (clip.track.getRaw())
                                    + ", which isn't a track of the song's that holds clips");

            if (clip.link == 0)
                continue;

            const auto [first, isFirst] = firstOfGroups.try_emplace (clip.link, clip.id);

            if (isFirst)
                continue;

            const auto* other = clips.find (first->second);

            if (other == nullptr || other->kind != clip.kind || clip.isMidi())
                continue; // the alias rule says it of the whole copy; a MIDI group's notes are the notes' rule's

            if (! clip.sound.isSameAs (other->sound))
                problems.push_back (clipText (id) + " of alias group " + std::to_string (clip.link) + " holds "
                                    + (clip.sound == other->sound ? "a sound alike" : "another sound") + ", not clip "
                                    + std::to_string (other->id.getRaw()) + "'s");
        }
    }

    // The notes' area: a MIDI clip with its notes, an audio clip without; the MIDI clips of an alias group hold ONE value of
    // notes (the same, not two alike); and the area holds the MIDI clips' values alone.
    void checkNotesValue (const Song&, const SongValues& values, std::vector<std::string>& problems)
    {
        const auto& clips = values.clips.get();
        const auto& notes = values.notes.get();
        std::map<std::uint64_t, ClipID> firstOfGroups;

        for (const auto& [id, clip] : clips.clips)
        {
            const auto* value = notes.find (clip.id);

            if (clip.isAudio() && value != nullptr)
                problems.push_back (clipText (id) + ", an audio clip, has a value of notes");

            if (clip.isMidi() && value == nullptr)
                problems.push_back (clipText (id) + ", a MIDI clip, has no value of notes");

            if (clip.link == 0)
                continue;

            const auto [first, isFirst] = firstOfGroups.try_emplace (clip.link, clip.id);

            if (isFirst)
                continue;

            const auto* other = clips.find (first->second);

            if (other == nullptr || other->kind != clip.kind || ! clip.isMidi())
                continue; // the alias rule says it of the whole copy; an audio group's sound is the clips' rule's

            if (const auto* firstValue = notes.find (other->id);
                firstValue != nullptr && value != nullptr && ! firstValue->isSameAs (*value))
                problems.push_back (clipText (id) + " of alias group " + std::to_string (clip.link) + " holds "
                                    + (*firstValue == *value ? "a value alike" : "another value") + ", not clip "
                                    + std::to_string (other->id.getRaw()) + "'s");
        }

        for (const auto& entry : notes.clips)
        {
            const auto* clip = clips.find (ClipID::fromRaw (entry.first));

            if (clip == nullptr || ! clip->isMidi())
                problems.push_back ("values: a value of notes for clip " + std::to_string (entry.first)
                                    + ", which isn't a MIDI clip of the song's");
        }
    }

    // The tempo map's area: no entry of it in the song's tree, its TEMPOMAP there an empty place for it; each kind not empty
    // and in the order of its beats; every number within its description's range (Schema.h), read here from the description.
    void checkTempoMapValue (const Song& song, const SongValues& values, std::vector<std::string>& problems)
    {
        const std::function<void (const juce::ValueTree&)> findEntries = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::TEMPO) || child.hasType (schema::TIMESIGNATURE))
                    problems.push_back ("values: a " + child.getType().toString().toStdString() + " is an element in the song's tree");

                findEntries (child);
            }
        };

        findEntries (song.getState());

        if (const auto map = song.getState().getChildWithName (schema::TEMPOMAP);
            ! map.isValid() || map.getNumChildren() > 0 || map.getNumProperties() > 0)
            problems.push_back ("values: the song's tree has no empty TEMPOMAP to write its tempo map in");

        const auto& tempoMap = values.tempoMap.get();
        const auto isWithin = [] (double number, const schema::PropertyDescription& property)
        {
            const auto& range = property.getRange();

            if (! std::isfinite (number) || ! range.has_value())
                return std::isfinite (number);

            if (number < range->low || number > range->high || (range->whole && ! juce::exactlyEqual (std::round (number), number)))
                return false;

            return ! range->powersOfTwo || juce::exactlyEqual (std::exp2 (std::round (std::log2 (number))), number);
        };

        const auto checkKind = [&problems] (const auto& entries, const char* kind, const auto& isKept)
        {
            if (entries.empty())
                problems.push_back (std::string ("values: the tempo map holds no ") + kind);

            std::optional<BeatPosition> last;
            size_t place = 0;

            for (const auto& entry : entries)
            {
                if (last.has_value() && entry.beat < *last)
                    problems.push_back (std::string ("values: the tempo map's ") + kind + " " + std::to_string (place)
                                        + " is before the one before it");

                if (! isKept (entry))
                    problems.push_back (std::string ("values: the tempo map's ") + kind + " " + std::to_string (place)
                                        + " holds a number outside its description's range");

                last = entry.beat;
                ++place;
            }
        };

        checkKind (tempoMap.tempos, "tempo",
                   [&isWithin] (const TempoEntry& tempo)
                   {
                       return isWithin (tempo.beat.inBeats(), schema::mapBeat) && isWithin (tempo.bpm, schema::bpm)
                              && isWithin ((double) tempo.curve, schema::tempoCurve);
                   });
        checkKind (tempoMap.signatures, "time signature",
                   [&isWithin] (const SignatureEntry& signature)
                   {
                       return isWithin (signature.beat.inBeats(), schema::mapBeat)
                              && isWithin ((double) signature.numerator, schema::numerator)
                              && isWithin ((double) signature.denominator, schema::denominator);
                   });
    }

    // The chord lane's area: no chord or key of it in the song's tree, its CHORDLANE there (where the song has a lane) an empty
    // placeholder, and no chord or key held without one (a song without a lane takes its keys from the key set last); every
    // number within its description's range (Schema.h), read here from the description; and what the lane's readers rely on:
    // chords that don't overlap, and a key's root a key's (0 to 11) or none (-1).
    void checkChordLaneValue (const Song& song, const SongValues& values, std::vector<std::string>& problems)
    {
        const std::function<void (const juce::ValueTree&)> findEntries = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::CHORD) || child.hasType (schema::CHORDKEY))
                    problems.push_back ("values: a " + child.getType().toString().toStdString() + " is an element in the song's tree");

                findEntries (child);
            }
        };

        findEntries (song.getState());
        const auto& lane = values.chordLane.get();

        if (const auto element = song.getState().getChildWithName (schema::CHORDLANE); ! element.isValid())
        {
            if (! lane.chords.empty() || ! lane.keys.empty())
                problems.push_back ("values: the song holds chords or keys but has no CHORDLANE, the sign of a chord lane");
        }
        else if (element.getNumChildren() > 0 || element.getNumProperties() > 0)
        {
            problems.push_back ("values: the song's CHORDLANE isn't an empty placeholder for its chord lane");
        }

        const auto isWithin = [] (double number, const schema::PropertyDescription& property)
        {
            const auto& range = property.getRange();

            if (! std::isfinite (number) || ! range.has_value())
                return std::isfinite (number);

            return number >= range->low && number <= range->high && (! range->whole || juce::exactlyEqual (std::round (number), number));
        };

        size_t place = 0;

        for (const auto& chord : lane.chords)
        {
            if (! isWithin (chord.start.inBeats(), schema::chordStart) || ! isWithin (chord.length.inBeats(), schema::chordLength)
                || ! isWithin ((double) chord.degree, schema::chordDegree)
                || ! isWithin ((double) chord.accidental, schema::chordAccidental)
                || ! isWithin ((double) chord.inversion, schema::chordInversion))
                problems.push_back ("values: the chord lane's chord " + std::to_string (place)
                                    + " holds a number outside its description's range");

            ++place;
        }

        // Chords that don't overlap, as the lane's writers leave them and as a lane that enters the song is kept
        // (ChordLaneValue.h: holdsChordsApart()).
        if (! holdsChordsApart (lane))
            problems.push_back ("values: the chord lane holds chords that overlap");

        place = 0;

        for (const auto& key : lane.keys)
        {
            if (! isWithin (key.start.inBeats(), schema::chordStart) || (key.root != -1 && ! isWithin ((double) key.root, schema::keyRoot)))
                problems.push_back ("values: the chord lane's key change " + std::to_string (place)
                                    + " holds a number outside its description's range");

            ++place;
        }
    }

    // The MIDI links' area: no MIDILINK in the song's tree, its MIDILINKS there (where the song has one) an empty
    // placeholder, and no link held without one (a whole copy writes the links in it); every number within its description's
    // range (Schema.h), read here from the description, and a parameter named (a device's ID and a text); and one link of each
    // controller and one of each parameter (MidiLinksValue.h: holdsOneLinkEach()). That each names a parameter the song has
    // is the rule above, on the song whole (checkValues()'s MIDILINK).
    void checkMidiLinksValue (const Song& song, const SongValues& values, std::vector<std::string>& problems)
    {
        const std::function<void (const juce::ValueTree&)> findLinks = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::MIDILINK))
                    problems.push_back ("values: a MIDILINK is an element in the song's tree");

                findLinks (child);
            }
        };

        findLinks (song.getState());
        const auto& links = values.midiLinks.get();

        if (const auto element = song.getState().getChildWithName (schema::MIDILINKS); ! element.isValid())
        {
            if (! links.links.empty())
                problems.push_back ("values: the song holds MIDI links but has no MIDILINKS, the place of its links");
        }
        else if (element.getNumChildren() > 0 || element.getNumProperties() > 0)
        {
            problems.push_back ("values: the song's MIDILINKS isn't an empty placeholder for its MIDI links");
        }

        const auto isWithin = [] (int number, const schema::PropertyDescription& property)
        {
            const auto& range = property.getRange();
            return range.has_value() && (double) number >= range->low && (double) number <= range->high;
        };

        size_t place = 0;

        for (const auto& link : links.links)
        {
            if (! isWithin (link.channel, schema::midiLinkChannel) || ! isWithin (link.number, schema::midiLinkNumber))
                problems.push_back ("values: the MIDI link " + std::to_string (place)
                                    + " holds a controller outside its description's range");

            if (link.device == 0 || link.parameter.isEmpty())
                problems.push_back ("values: the MIDI link " + std::to_string (place) + " names no parameter");

            ++place;
        }

        if (! holdsOneLinkEach (links))
            problems.push_back ("values: the MIDI links hold two of one controller or of one parameter");
    }

    // The curves' area: no CURVE nor POINT in the song's tree, its AUTOMATION there, one, an empty place (every song that holds
    // its values has one); the curves' order holding each parameter once, the map the same parameters (CurveValues.h:
    // holdsItsOrder()), each naming a parameter (a device's ID and a text); every point's numbers within their descriptions'
    // ranges (Schema.h), read here from the descriptions; and every point's key one the song gave (none is 0, each below the
    // song's next), each once in the song. Points out of time order are no problem (a file from others may hold them so, and
    // they are kept). That each names a parameter the song has is the rule above, on the song whole (checkValues()'s POINT).
    void checkCurvesValue (const Song& song, const SongValues& values, std::vector<std::string>& problems)
    {
        const std::function<void (const juce::ValueTree&)> findCurves = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::CURVE) || child.hasType (schema::POINT))
                    problems.push_back ("values: a " + child.getType().toString().toStdString() + " is an element in the song's tree");

                findCurves (child);
            }
        };

        findCurves (song.getState());

        if (const auto element = song.getState().getChildWithName (schema::AUTOMATION); ! element.isValid())
            problems.push_back ("values: the song has no AUTOMATION, the place of its curves");
        else if (element.getNumChildren() > 0 || element.getNumProperties() > 0)
            problems.push_back ("values: the song's AUTOMATION isn't an empty place for its curves");

        // One place: a second would have the curves written into it too (the keeper refuses one: CurvesArea.cpp).
        auto numAutomations = 0;

        for (const auto& child : song.getState())
            numAutomations += child.hasType (schema::AUTOMATION) ? 1 : 0;

        if (numAutomations > 1)
            problems.push_back ("values: the song has more than one AUTOMATION");

        const auto& curves = values.curves.get();

        if (! holdsItsOrder (curves))
            problems.push_back ("values: the curves' order and their map don't hold the same parameters, each once");

        const auto isWithin = [] (double number, const schema::PropertyDescription& property)
        {
            const auto& range = property.getRange();
            return std::isfinite (number) && (! range.has_value() || (number >= range->low && number <= range->high));
        };

        std::vector<std::uint64_t> keys;

        for (const auto& parameter : curves.order)
        {
            const auto* curve = curves.find (parameter);
            const auto name = getCurveKeyText (parameter).toStdString();

            if (! parameter.device.isValid() || parameter.parameter.isEmpty())
                problems.push_back ("values: the curve " + name + " names no parameter");

            if (curve == nullptr)
                continue;

            size_t place = 0;

            for (const auto& point : curve->points)
            {
                if (! isWithin (point.beat.inBeats(), schema::pointBeat) || ! isWithin ((double) point.position, schema::pointValue)
                    || ! isWithin ((double) point.curve, schema::pointCurve))
                    problems.push_back ("values: the curve " + name + "'s point " + std::to_string (place)
                                        + " holds a number outside its description's range");

                if (point.key == 0 || point.key >= values.getNextPointKey())
                    problems.push_back ("values: the curve " + name + "'s point " + std::to_string (place)
                                        + " holds a key the song didn't give");

                keys.push_back (point.key);
                ++place;
            }
        }

        std::sort (keys.begin(), keys.end());

        if (std::adjacent_find (keys.begin(), keys.end()) != keys.end())
            problems.push_back ("values: two of the song's points hold one key");
    }

    // The sections' area: no SECTION in the song's tree, its SECTIONS there (where the song has one) an empty placeholder, and
    // no section held without one (a whole copy writes the sections in it); every section with an ID the song can count on
    // from, one of each (SectionsValue.h: holdsSectionsHeld()); and every number within its description's range (Schema.h),
    // read here from the description, a length more than 0. That each ID is held once across the song, and the song's next ID
    // past it, is the id rule above, on the song whole (findIDProblems(), which reads the sections written into whole state).
    void checkSectionsValue (const Song& song, const SongValues& values, std::vector<std::string>& problems)
    {
        const std::function<void (const juce::ValueTree&)> findSections = [&] (const juce::ValueTree& element)
        {
            for (const auto& child : element)
            {
                if (child.hasType (schema::SECTION))
                    problems.push_back ("values: a SECTION is an element in the song's tree");

                findSections (child);
            }
        };

        findSections (song.getState());
        const auto& sections = values.sections.get();

        if (const auto element = song.getState().getChildWithName (schema::SECTIONS); ! element.isValid())
        {
            if (! sections.sections.empty())
                problems.push_back ("values: the song holds sections but has no SECTIONS, the place of its sections");
        }
        else if (element.getNumChildren() > 0 || element.getNumProperties() > 0)
        {
            problems.push_back ("values: the song's SECTIONS isn't an empty placeholder for its sections");
        }

        if (! holdsSectionsHeld (sections))
            problems.push_back ("values: the sections hold two of one ID, or one without an ID the song can count on from");

        const auto isWithin = [] (double number, const schema::PropertyDescription& property)
        {
            const auto& range = property.getRange();
            return range.has_value() && number >= range->low && number <= range->high;
        };

        size_t place = 0;

        for (const auto& section : sections.sections)
        {
            if (! isWithin (section.start.inBeats(), schema::sectionStart) || ! isWithin (section.length.inBeats(), schema::sectionLength)
                || ! isWithin ((double) section.energy, schema::sectionEnergy))
                problems.push_back ("values: the section " + std::to_string (place) + " holds a number outside its description's range");

            ++place;
        }
    }
} // namespace

std::vector<std::string> findValueProblems (const Song& song)
{
    std::vector<std::string> problems;
    const auto* values = song.getValues();

    if (values == nullptr)
        return problems;

    // Each area's rule, a line each (SongValues.h's recipe), in this order: the clips, their notes, the tempo map, the chord
    // lane, the MIDI links, the curves, the sections.
    checkClipsValue (song, *values, problems);
    checkNotesValue (song, *values, problems);
    checkTempoMapValue (song, *values, problems);
    checkChordLaneValue (song, *values, problems);
    checkMidiLinksValue (song, *values, problems);
    checkCurvesValue (song, *values, problems);
    checkSectionsValue (song, *values, problems);
    return problems;
}

void checkValues (const Song& song)
{
    for (const auto& problem : findValueProblems (song))
    {
        INFO (problem);
        CHECK (false);
    }
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
    Song::Ptr copy (new Song (phazite::test::copyWholeSong (song)));
    forgetCurvesOfGoneDevices (*copy);

    // A song left without curves is written without an AUTOMATION (CurvesArea.h: writeCurvesIn()), as the song opened is.
    if (auto automation = copy->getState().getChildWithName (schema::AUTOMATION); automation.isValid() && automation.getNumChildren() == 0)
        copy->getState().removeChild (automation, nullptr);
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
