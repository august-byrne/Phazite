// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SongCheck.h"
#include "SongIDs.h"
#include "devices/core/DeviceInfo.h"
#include "devices/modulation/Modulators.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongIndex.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Containers.h"
#include "model/modulation/internal/Modulation.h"

#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <optional>

namespace phazite::test
{
namespace
{
    namespace schema = phazite::schema;

    // Where each element goes, by its parent's type, as Schema.h's "Child of" says.
    const std::map<juce::String, juce::StringArray>& getPlaces()
    {
        static const std::map<juce::String, juce::StringArray> places {
            { "PHAZITE", { "LASTKEY", "TEMPOMAP", "PIANOROLL", "TRACKS", "MASTER", "AUTOMATION", "SECTIONS", "CHORDLANE", "MERGING" } },
            { "TEMPOMAP", { "TEMPO", "TIMESIGNATURE" } },
            { "TRACKS", { "TRACK" } },
            { "TRACK", { "TRACK", "LANES", "DEVICE", "CONTAINER", "SEND", "MODULATOR", "MACRO", "CLIP" } },
            { "MASTER", { "LANES", "DEVICE", "CONTAINER", "MODULATOR" } },
            { "LANES", { "LANE" } },
            { "DEVICE", { "VALUES", "SOUNDS", "VALUE" } },
            { "CONTAINER", { "CHAIN", "MACRO" } },
            { "CHAIN", { "DEVICE" } },
            { "MODULATOR", { "MODULATES", "VALUES" } },
            { "MACRO", { "MODULATES" } },
            { "AUTOMATION", { "CURVE" } },
            { "CURVE", { "POINT" } },
            { "SECTIONS", { "SECTION" } },
            { "CHORDLANE", { "CHORD", "CHORDKEY" } },
            { "CLIP", { "NOTE", "CONTROL", "WARP" } },
        };
        return places;
    }

    // A number Schema.h gives a range: from, to, and whether it is whole.
    struct Range
    {
        const char* type;
        const char* property;
        double low, high;
        bool whole = false;
    };

    const std::vector<Range>& getRanges()
    {
        constexpr double beat = maxBeat, seconds = 1.0e7, some = 1.0e-9;
        static const std::vector<Range> ranges {
            { "PHAZITE", "loopStart", 0.0, beat },
            { "PHAZITE", "loopEnd", 0.0, beat },
            { "PHAZITE", "playStart", 0.0, beat },
            { "PHAZITE", "gridOffset", -3.0, 3.0, true },
            { "PIANOROLL", "gridOffset", -3.0, 3.0, true },
            { "TEMPO", "beat", 0.0, beat },
            { "TEMPO", "bpm", 20.0, 300.0 },
            { "TEMPO", "curve", -1.0, 1.0 },
            { "TIMESIGNATURE", "beat", 0.0, beat },
            { "TIMESIGNATURE", "numerator", 1.0, 32.0, true },
            { "TIMESIGNATURE", "denominator", 2.0, 16.0, true },
            { "TRACK", "volume", -100.0, 6.0 },
            { "TRACK", "pan", -1.0, 1.0 },
            { "TRACK", "keyRoot", 0.0, 11.0, true },
            { "TRACK", "pad", 0.0, 127.0, true },
            { "TRACK", "height", 32.0, 320.0, true },
            { "MASTER", "volume", -100.0, 6.0 },
            { "MASTER", "pan", -1.0, 1.0 },
            { "MASTER", "height", 32.0, 320.0, true },
            { "LASTKEY", "keyRoot", 0.0, 11.0, true },
            { "LANE", "height", 32.0, 320.0, true },
            { "DEVICE", "mix", 0.0, 1.0 },
            { "VALUE", "value", 0.0, 1.0 },
            { "CONTAINER", "mix", 0.0, 1.0 },
            { "CHAIN", "volume", -100.0, 6.0 },
            { "CHAIN", "pan", -1.0, 1.0 },
            { "CHAIN", "crossover", 20.0, 20000.0 },
            { "CHAIN", "lowKey", 0.0, 127.0, true },
            { "CHAIN", "highKey", 0.0, 127.0, true },
            { "CHAIN", "lowVelocity", 1.0, 127.0, true },
            { "CHAIN", "highVelocity", 1.0, 127.0, true },
            { "SEND", "level", -100.0, 6.0 },
            { "MACRO", "value", 0.0, 1.0 },
            { "MODULATES", "amount", -1.0, 1.0 },
            { "POINT", "beat", 0.0, beat },
            { "POINT", "curve", -1.0, 1.0 },
            { "SECTION", "start", 0.0, beat },
            { "SECTION", "length", some, beat },
            { "SECTION", "energy", 0.0, 1.0 },
            { "CHORD", "start", 0.0, beat },
            { "CHORD", "length", some, beat },
            { "CHORD", "degree", 1.0, 7.0, true },
            { "CHORD", "accidental", -1.0, 1.0, true },
            { "CHORD", "inversion", 0.0, 16.0, true },
            { "CHORDKEY", "start", 0.0, beat },
            { "CHORDKEY", "keyRoot", 0.0, 11.0, true },
            { "CLIP", "start", 0.0, beat },
            { "CLIP", "length", some, beat },
            { "CLIP", "offset", 0.0, beat },
            { "CLIP", "fileStart", 0.0, seconds },
            { "CLIP", "fileEnd", 0.0, seconds },
            { "CLIP", "tempo", 20.0, 300.0 },
            { "CLIP", "pitch", -48.0, 48.0 },
            { "CLIP", "gain", -60.0, 24.0 },
            { "CLIP", "fadeIn", 0.0, seconds },
            { "CLIP", "fadeOut", 0.0, seconds },
            { "CLIP", "fadeInBend", -1.0, 1.0 },
            { "CLIP", "fadeOutBend", -1.0, 1.0 },
            { "CLIP", "takes", 2.0, 1.0e6, true },
            { "CLIP", "take", 1.0, 1.0e6, true },
            { "CLIP", "takeLength", some, seconds },
            { "NOTE", "pitch", 0.0, 127.0, true },
            { "NOTE", "start", -beat, beat },
            { "NOTE", "length", some, beat }, // more than 0: a tempo change scales notes, those that keep their speed below 1/64
            { "NOTE", "velocity", 1.0, 127.0, true },
            { "CONTROL", "number", 0.0, 127.0, true },
            { "CONTROL", "beat", -beat, beat },
            { "CONTROL", "value", 0.0, 16383.0, true },
            { "WARP", "time", 0.0, seconds },
            { "WARP", "playsAt", -seconds, seconds },
        };
        return ranges;
    }

    // Words a property must be one of, by its element's type.
    struct Words
    {
        const char* type;
        const char* property;
        juce::StringArray words;
        bool required = false;
    };

    const std::vector<Words>& getWords()
    {
        static const std::vector<Words> words {
            { "TRACK", "kind", { "track", "folder", "return" }, true },
            { "CLIP", "kind", { "midi", "audio" }, true },
            { "CONTROL", "kind", { "controller", "pitchWheel", "channelPressure", "aftertouch", "programChange" }, true },
            { "TRACK", "automationMode", { "touch", "latch", "write" } },
            { "MASTER", "automationMode", { "touch", "latch", "write" } },
            { "TRACK", "monitoring", { "always", "never" } },
            { "CONTAINER", "split", { "bands", "midSide", "leftRight" } },
            { "CLIP", "fadeInShape", { "curve", "sCurve" } },
            { "CLIP", "fadeOutShape", { "curve", "sCurve" } },
            { "TIMESIGNATURE", "denominator", { "2", "4", "8", "16" } },
        };
        return words;
    }

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
        juce::File folder; // the project's, or none
        std::vector<std::string> problems;

        void add (const char* rule, const juce::ValueTree& element, const juce::String& text)
        {
            problems.push_back (juce::String (juce::String (rule) + ": " + describe (element) + ": " + text).toStdString());
        }

        //==============================================================================
        void checkPlaces (const juce::ValueTree& parent)
        {
            const auto& places = getPlaces();
            const auto type = parent.getType().toString();
            const auto found = places.find (type);

            for (const auto& child : parent)
            {
                const auto childType = child.getType().toString();

                if (! isKnownElementType (child))
                    continue; // one of a type Phazite doesn't know, which stays as it is

                if (found == places.end() || ! found->second.contains (childType))
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
            for (const auto& range : getRanges())
                if (element.getType().toString() == range.type && element.hasProperty (range.property))
                    checkNumber (element, range.property, range.low, range.high, range.whole, range.property);

            // Numbers between spaces: a step sequencer's steps, -1 to 1; the sampler's slices, parts of its sound, 0 to 1.
            const auto checkList = [this, &element] (const juce::Identifier& property, double low, double high)
            {
                for (const auto& token : juce::StringArray::fromTokens (element[property].toString(), " ", {}))
                {
                    if (token.isEmpty())
                        continue;

                    juce::ValueTree one ("NUMBER");
                    one.setProperty (property, token, nullptr);
                    checkNumber (one, property, low, high, false, element.getType().toString() + " " + property.toString());
                }
            };

            if (element.hasType (schema::MODULATOR))
                checkList (schema::modulatorSteps, -1.0, 1.0);

            if (element.hasType (schema::SOUNDS))
                for (const auto* slices : { &schema::samplerSlices, &schema::samplerSlicesRemoved })
                    checkList (*slices, 0.0, 1.0);
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

                const auto type = child.getType().toString();
                checkRanges (child);

                for (const auto& words : getWords())
                {
                    if (type != words.type)
                        continue;

                    if (child.hasProperty (words.property) ? ! words.words.contains (child[words.property].toString()) : words.required)
                        add ("kind", child, juce::String (words.property) + " is none of its words: " + child[words.property].toString());
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

                    // A plug-in's parameters are from 0 to 1, and it tells which it has only once it plays.
                    if (const auto description = describeParameter (song, parameter))
                        checkNumber (child, schema::pointValue, description->range.start, description->range.end, false, "value");
                    else if (findDevice (song.getState(), parameter.device.getRaw())[schema::deviceType].toString()
                             == schema::deviceIsPlugin)
                        checkNumber (child, schema::pointValue, 0.0, 1.0, false, "value");
                    // A chain's crossover, while its container isn't split into bands or it is the first chain: kept.
                    else if (findContainerChain (song, parameter.device).isValid() && parameter.parameter == schema::crossoverParameter)
                        checkNumber (child, schema::pointValue, 20.0, 20000.0, false, "value");
                    else
                        add ("names", tree, "a curve of a parameter the song hasn't: " + parameter.parameter);
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
    };
} // namespace

std::vector<std::string> findSongProblems (const Song& song)
{
    const auto& state = song.getState();
    Checker checker { song, song.getFile() != juce::File() ? song.getFile().getParentDirectory() : juce::File(), {} };

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
} // namespace phazite::test
