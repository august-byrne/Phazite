// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/SongOwnValue.h"
#include "model/base/Numbers.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace phazite
{
namespace
{
    // How a reader reads: as Phazite's code always read the tree, or as the song holds it (each by its description).
    enum class Reading
    {
        lenient,
        held
    };

    // A text that is one of a property's words (Schema.h: oneOf()).
    bool isOneOfWords (const juce::String& text, const schema::PropertyDescription& property)
    {
        const auto& words = property.getWords();
        return std::any_of (words.begin(), words.end(), [&text] (const char* word) { return text == word; });
    }

    // A text as the song holds one: as it is, or, held, only where it is of its description's hexadecimal digits (the loader's
    // rule: a var that is text, of that form), else none.
    juce::String readText (const juce::ValueTree& element, const schema::PropertyDescription& property, Reading reading)
    {
        const auto& value = element[property.getName()];

        if (reading == Reading::held && property.getHexDigits() > 0
            && ! (value.isString() && schema::isVersionsID (value.toString(), property.getHexDigits())))
            return {};

        return value.toString();
    }

    // A whole number within its description's range as the song holds one, made whole: none where it is no finite number (the
    // loader takes such a one out, these ranges giving no fallback).
    std::optional<int> readHeldWhole (const juce::var& value, const schema::PropertyDescription& property)
    {
        const auto number = readValueNumber (value);

        if (! number.has_value() || ! std::isfinite (*number))
            return std::nullopt;

        return roundToWhole<int> (readDescribed (value, property));
    }

    // A number within its description's range as the song holds one: none where it is no finite number.
    std::optional<double> readHeldNumber (const juce::var& value, const schema::PropertyDescription& property)
    {
        const auto number = readValueNumber (value);

        if (! number.has_value() || ! std::isfinite (*number))
            return std::nullopt;

        return readDescribed (value, property);
    }

    //==============================================================================
    std::optional<SavedKey> readKey (const juce::ValueTree& phazite, Reading reading)
    {
        const auto element = phazite.getChildWithName (schema::LASTKEY);

        if (! element.hasProperty (schema::keyRoot))
            return std::nullopt;

        SavedKey key;
        const auto& scale = element[schema::keyScale];

        if (reading == Reading::lenient)
        {
            // As model/notes/Notes.cpp read it: the root as an int, within 0 to 11; the scale as it is (major for none or any
            // other word).
            key.root = juce::jlimit (0, 11, toWhole<int> (element[schema::keyRoot]));
            key.scale = scale.toString();
            return key;
        }

        const auto root = readHeldWhole (element[schema::keyRoot], schema::keyRoot);

        if (! root.has_value())
            return std::nullopt;

        key.root = *root;

        if (const auto word = scale.toString(); isOneOfWords (word, schema::keyScale))
            key.scale = word;

        return key;
    }

    using Described = const schema::PropertyDescription*;

    // A grid's five properties (GridSettings::save()'s), in its order.
    const std::array<Described, 5>& getGridProperties()
    {
        static const std::array<Described, 5> properties { &schema::gridSnap, &schema::gridAdaptive, &schema::gridOffset, &schema::gridStep,
                                                           &schema::gridTriplets };
        return properties;
    }

    // Whether an element holds a grid: any of its five properties; held, only one its description keeps (an offset that is a
    // number, a step that is one of its words), as the loader leaves them (keepNumbers()) before it completes a grid.
    bool holdsGridOn (const juce::ValueTree& element, Reading reading)
    {
        for (const auto* property : getGridProperties())
        {
            const auto* value = element.getPropertyPointer (property->getName());

            if (value == nullptr)
                continue;

            if (reading == Reading::lenient)
                return true;

            if (property == &schema::gridOffset ? readHeldWhole (*value, schema::gridOffset).has_value()
                : property == &schema::gridStep ? isOneOfWords (value->toString(), schema::gridStep)
                                                : true)
                return true;
        }

        return false;
    }

    SavedGrid readGrid (const juce::ValueTree& element, Reading reading)
    {
        SavedGrid grid;
        grid.snap = element.getProperty (schema::gridSnap, grid.snap);
        grid.adaptive = element.getProperty (schema::gridAdaptive, grid.adaptive);
        grid.triplets = element.getProperty (schema::gridTriplets, grid.triplets);

        if (element.hasProperty (schema::gridOffset))
            grid.offset = reading == Reading::lenient ? toWhole<int> (element[schema::gridOffset])
                                                      : readHeldWhole (element[schema::gridOffset], schema::gridOffset).value_or (0);

        if (element.hasProperty (schema::gridStep))
            if (const auto step = element[schema::gridStep].toString();
                reading == Reading::lenient || isOneOfWords (step, schema::gridStep))
                grid.step = step;

        return grid;
    }

    void writeGrid (juce::ValueTree& element, const SavedGrid& grid)
    {
        // As GridSettings::save() writes them: all five, in its order.
        element.setProperty (schema::gridSnap, grid.snap, nullptr);
        element.setProperty (schema::gridAdaptive, grid.adaptive, nullptr);
        element.setProperty (schema::gridOffset, grid.offset, nullptr);
        element.setProperty (schema::gridStep, grid.step, nullptr);
        element.setProperty (schema::gridTriplets, grid.triplets, nullptr);
    }

    //==============================================================================
    SongOwnValue readOwn (const juce::ValueTree& phazite, Reading reading)
    {
        SongOwnValue own;

        if (! phazite.hasType (schema::PHAZITE))
            return own;

        own.outline = readText (phazite, schema::outline, reading);
        own.notes = readText (phazite, schema::notes, reading);
        own.idNumbering = readText (phazite, schema::idNumbering, reading);
        own.patternsKeepTheirSpeed = (bool) phazite.getProperty (schema::patternsKeepTheirSpeed, false);

        if (const auto merging = phazite.getChildWithName (schema::MERGING); merging.isValid())
            own.merging = readMergingRecord (merging);

        return own;
    }

    SongStanding readStanding (const juce::ValueTree& phazite, Reading reading)
    {
        SongStanding standing;

        if (! phazite.hasType (schema::PHAZITE))
            return standing;

        standing.lastKey = readKey (phazite, reading);
        standing.versionsBranch = readText (phazite, schema::versionsBranch, reading);
        standing.versionsBase = readText (phazite, schema::versionsBase, reading);
        return standing;
    }

    SongOutside readOutside (const juce::ValueTree& phazite, Reading reading)
    {
        SongOutside outside;

        if (! phazite.hasType (schema::PHAZITE))
            return outside;

        outside.loop = phazite.getProperty (schema::loop, false);
        outside.metronome = phazite.getProperty (schema::metronome, false);

        if (const auto* follows = phazite.getPropertyPointer (schema::loopFollowsSong))
            outside.loopFollowsSong = (bool) *follows;

        for (const auto& [property, field] :
             { std::pair (&schema::loopStart, &outside.loopStart), std::pair (&schema::loopEnd, &outside.loopEnd) })
            if (const auto* number = phazite.getPropertyPointer (*property))
                *field = reading == Reading::lenient ? std::optional ((double) *number) : readHeldNumber (*number, *property);

        // Held, a range looped on purpose is both its ends, the end past the start (Playback.cpp: getLoopBeats() loops the
        // song without either, and an empty range plays nothing sensible): else none, as the loader leaves it
        // (keepSongOwnAsHeld()).
        if (reading == Reading::held
            && (! outside.loopStart.has_value() || ! outside.loopEnd.has_value() || ! (*outside.loopEnd > *outside.loopStart)))
        {
            outside.loopStart.reset();
            outside.loopEnd.reset();
        }

        // Within its description's range, held (0 where it is no finite number, which the loader takes out); as Playback.cpp
        // read it otherwise. The project takes it out as it opens.
        if (const auto* start = phazite.getPropertyPointer (schema::playStart))
            outside.playStart = reading == Reading::lenient ? (double) *start : readHeldNumber (*start, schema::playStart).value_or (0.0);

        if (const auto* punch = phazite.getPropertyPointer (schema::punch))
            outside.punch = (bool) *punch;

        outside.lastInput = phazite[schema::lastInput].toString();

        if (holdsGridOn (phazite, reading))
            outside.playlistGrid = readGrid (phazite, reading);

        // A PIANOROLL that holds none of a grid's properties holds no grid (the loader takes it out): the defaults, as a song
        // without one reads.
        if (const auto pianoRoll = phazite.getChildWithName (schema::PIANOROLL); pianoRoll.isValid() && holdsGridOn (pianoRoll, reading))
            outside.pianoRollGrid = readGrid (pianoRoll, reading);

        return outside;
    }

    bool areExactlyEqual (const std::optional<double>& a, const std::optional<double>& b)
    {
        return a.has_value() && b.has_value() ? juce::exactlyEqual (*a, *b) : a.has_value() == b.has_value();
    }

    template <typename Field>
    void findChange (const Field& before, const Field& after, std::optional<std::pair<Field, Field>>& change)
    {
        if (! (before == after))
            change = std::pair (before, after);
    }

    template <typename Field>
    void moveChange (Field& now, const std::optional<std::pair<Field, Field>>& change, ChangeDirection direction)
    {
        if (change.has_value())
            now = direction == ChangeDirection::back ? change->first : change->second;
    }
} // namespace

//==============================================================================
SongStandingChanges findChanges (const SongStanding& before, const SongStanding& after)
{
    SongStandingChanges changes;
    findChange (before.lastKey, after.lastKey, changes.lastKey);
    findChange (before.versionsBranch, after.versionsBranch, changes.versionsBranch);
    findChange (before.versionsBase, after.versionsBase, changes.versionsBase);
    return changes;
}

SongStanding withChanges (SongStanding now, const SongStandingChanges& changes, ChangeDirection direction)
{
    moveChange (now.lastKey, changes.lastKey, direction);
    moveChange (now.versionsBranch, changes.versionsBranch, direction);
    moveChange (now.versionsBase, changes.versionsBase, direction);
    return now;
}

const juce::String& getDefaultGridStep()
{
    static const juce::String step (*schema::gridStep.getDefaultValue());
    return step;
}

bool SongOutside::operator== (const SongOutside& other) const
{
    return loop == other.loop && metronome == other.metronome && loopFollowsSong == other.loopFollowsSong
           && areExactlyEqual (loopStart, other.loopStart) && areExactlyEqual (loopEnd, other.loopEnd)
           && juce::exactlyEqual (playStart, other.playStart) && punch == other.punch && lastInput == other.lastInput
           && playlistGrid == other.playlistGrid && pianoRollGrid == other.pianoRollGrid;
}

//==============================================================================
SongOwnValue readSongOwnValue (const juce::ValueTree& phazite)
{
    return readOwn (phazite, Reading::lenient);
}

SongStanding readSongStanding (const juce::ValueTree& phazite)
{
    return readStanding (phazite, Reading::lenient);
}

SongOutside readSongOutside (const juce::ValueTree& phazite)
{
    return readOutside (phazite, Reading::lenient);
}

SongOwnValue readHeldSongOwn (const juce::ValueTree& phazite)
{
    return readOwn (phazite, Reading::held);
}

SongStanding readHeldSongStanding (const juce::ValueTree& phazite)
{
    return readStanding (phazite, Reading::held);
}

SongOutside readHeldSongOutside (const juce::ValueTree& phazite)
{
    return readOutside (phazite, Reading::held);
}

void writeSongOwnInto (juce::ValueTree& phazite, const SongOwnValue& own, const SongStanding& standing, const SongOutside& outside)
{
    const auto setText = [&phazite] (const schema::PropertyDescription& property, const juce::String& text)
    {
        if (text.isNotEmpty())
            phazite.setProperty (property.getName(), text, nullptr);
    };

    setText (schema::outline, own.outline);
    setText (schema::notes, own.notes);
    setText (schema::idNumbering, own.idNumbering);

    if (own.patternsKeepTheirSpeed)
        phazite.setProperty (schema::patternsKeepTheirSpeed, true, nullptr);

    setText (schema::versionsBranch, standing.versionsBranch);
    setText (schema::versionsBase, standing.versionsBase);

    if (outside.loop)
        phazite.setProperty (schema::loop, true, nullptr);

    if (outside.loopFollowsSong.has_value())
        phazite.setProperty (schema::loopFollowsSong, *outside.loopFollowsSong, nullptr);

    if (outside.loopStart.has_value())
        phazite.setProperty (schema::loopStart, *outside.loopStart, nullptr);

    if (outside.loopEnd.has_value())
        phazite.setProperty (schema::loopEnd, *outside.loopEnd, nullptr);

    if (outside.playStart > 0.0)
        phazite.setProperty (schema::playStart, outside.playStart, nullptr);

    if (outside.metronome)
        phazite.setProperty (schema::metronome, true, nullptr);

    if (outside.punch.has_value())
        phazite.setProperty (schema::punch, *outside.punch, nullptr);

    setText (schema::lastInput, outside.lastInput);

    if (outside.playlistGrid.has_value())
        writeGrid (phazite, *outside.playlistGrid);

    if (standing.lastKey.has_value())
    {
        juce::ValueTree key (schema::LASTKEY);
        key.setProperty (schema::keyRoot, standing.lastKey->root, nullptr);

        if (standing.lastKey->scale.isNotEmpty())
            key.setProperty (schema::keyScale, standing.lastKey->scale, nullptr);

        phazite.appendChild (key, nullptr);
    }

    if (outside.pianoRollGrid.has_value())
    {
        juce::ValueTree pianoRoll (schema::PIANOROLL);
        writeGrid (pianoRoll, *outside.pianoRollGrid);
        phazite.appendChild (pianoRoll, nullptr);
    }

    if (own.merging.has_value())
        phazite.appendChild (writeMergingElement (*own.merging), nullptr);
}

MergingRecord readMergingRecord (const juce::ValueTree& merging)
{
    MergingRecord record;
    record.version = merging[schema::mergingVersion].toString();
    record.head = merging[schema::mergingHead].toString();
    record.before = merging[schema::mergingBefore].toString();
    record.chosen = merging[schema::mergingChosen].toString();
    record.renumbered = merging[schema::mergingRenumbered].toString();
    record.translated = merging[schema::mergingTranslated].toString();
    record.crossed = merging[schema::mergingCrossed].toString();
    return record;
}

juce::ValueTree writeMergingElement (const MergingRecord& record)
{
    juce::ValueTree element (schema::MERGING);

    for (const auto& [property, text] :
         { std::pair (&schema::mergingVersion, &record.version), std::pair (&schema::mergingHead, &record.head),
           std::pair (&schema::mergingBefore, &record.before), std::pair (&schema::mergingChosen, &record.chosen),
           std::pair (&schema::mergingRenumbered, &record.renumbered), std::pair (&schema::mergingTranslated, &record.translated),
           std::pair (&schema::mergingCrossed, &record.crossed) })
        if (text->isNotEmpty())
            element.setProperty (*property, *text, nullptr);

    return element;
}

namespace
{
    // The properties the conversion reads and writes, on the root and on each of its elements: what keepSongOwnAsHeld() keeps
    // as written, nothing else (nextID, and what Phazite doesn't know, stay as they are).
    const std::vector<Described>& getRootProperties()
    {
        static const std::vector<Described> properties {
            &schema::outline,        &schema::notes,        &schema::idNumbering, &schema::patternsKeepTheirSpeed,
            &schema::versionsBranch, &schema::versionsBase, &schema::loop,        &schema::loopFollowsSong,
            &schema::loopStart,      &schema::loopEnd,      &schema::playStart,   &schema::metronome,
            &schema::punch,          &schema::lastInput,    &schema::gridSnap,    &schema::gridAdaptive,
            &schema::gridOffset,     &schema::gridStep,     &schema::gridTriplets
        };
        return properties;
    }

    const std::vector<Described>& getElementProperties (const juce::Identifier& type)
    {
        static const std::vector<Described> key { &schema::keyRoot, &schema::keyScale };
        static const std::vector<Described> grid (getGridProperties().begin(), getGridProperties().end());
        static const std::vector<Described> merging { &schema::mergingVersion, &schema::mergingHead,       &schema::mergingBefore,
                                                      &schema::mergingChosen,  &schema::mergingRenumbered, &schema::mergingTranslated,
                                                      &schema::mergingCrossed };
        return type == schema::LASTKEY.getName() ? key : type == schema::PIANOROLL.getName() ? grid : merging;
    }

    // Each of those properties of an element as another holds it (as its text: SongText.h, isSameValue()), or none where the
    // other has none. Whether any changed.
    bool keepAsWritten (juce::ValueTree& element, const juce::ValueTree& written, const std::vector<Described>& properties)
    {
        auto changed = false;

        for (const auto* property : properties)
        {
            const auto* there = element.getPropertyPointer (property->getName());

            if (const auto* made = written.getPropertyPointer (property->getName()))
            {
                if (there == nullptr || ! isSameValue (*there, *made))
                {
                    element.setProperty (property->getName(), *made, nullptr);
                    changed = true;
                }
            }
            else if (there != nullptr)
            {
                element.removeProperty (property->getName(), nullptr);
                changed = true;
            }
        }

        return changed;
    }
} // namespace

bool keepSongOwnAsHeld (juce::ValueTree phazite)
{
    if (! phazite.hasType (schema::PHAZITE))
        return false;

    juce::ValueTree written (schema::PHAZITE);
    writeSongOwnInto (written, readHeldSongOwn (phazite), readHeldSongStanding (phazite), readHeldSongOutside (phazite));
    auto changed = keepAsWritten (phazite, written, getRootProperties());

    for (const auto* type : { &schema::LASTKEY, &schema::PIANOROLL, &schema::MERGING })
    {
        auto held = phazite.getChildWithName (*type);

        if (! held.isValid())
            continue;

        if (const auto made = written.getChildWithName (*type); made.isValid())
        {
            changed = keepAsWritten (held, made, getElementProperties (*type)) || changed;
        }
        else
        {
            phazite.removeChild (held, nullptr); // it holds none of what it is for
            changed = true;
        }
    }

    return changed;
}

SavedGrid readSavedGrid (const juce::ValueTree& gridHolder)
{
    return readGrid (gridHolder, Reading::lenient);
}

//==============================================================================
SongOwnValue getSongOwn (const Song& song)
{
    return readHeldSongOwn (song.getState());
}

SongStanding getSongStanding (const Song& song)
{
    return readHeldSongStanding (song.getState());
}

SongOutside getSongOutside (const Song& song)
{
    return readHeldSongOutside (song.getState());
}
} // namespace phazite
