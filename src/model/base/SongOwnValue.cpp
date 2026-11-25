// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/SongOwnValue.h"
#include "model/base/Numbers.h"
#include "model/base/Song.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace phazite
{
namespace
{
    // How a grid is read: as Phazite's code always read the tree (readSavedGrid()), or as the song holds it (each property by
    // its description).
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

    // A text as the song holds one: as it is, but one of a description's hexadecimal digits only where it is of that form (the
    // loader's rule: a var that is text, of that form), else none.
    juce::String readText (const juce::ValueTree& element, const schema::PropertyDescription& property)
    {
        const auto& value = element[property.getName()];

        if (property.getHexDigits() > 0 && ! (value.isString() && schema::isVersionsID (value.toString(), property.getHexDigits())))
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
    std::optional<SavedKey> readKey (const juce::ValueTree& phazite)
    {
        const auto element = phazite.getChildWithName (schema::LASTKEY);

        if (! element.hasProperty (schema::keyRoot))
            return std::nullopt;

        SavedKey key;
        const auto& scale = element[schema::keyScale];
        const auto root = readHeldWhole (element[schema::keyRoot], schema::keyRoot);

        if (! root.has_value())
            return std::nullopt;

        key.root = *root;

        if (const auto word = scale.toString(); isOneOfWords (word, schema::keyScale))
            key.scale = word;

        return key;
    }

    using Described = const schema::PropertyDescription*;

    // A grid's five properties (Grid.h: makeSavedGrid()'s), in its order.
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

    //==============================================================================
    // Which of the three values a write is of.
    enum class SongOwnPart
    {
        own,
        standing,
        outside
    };

    // The writers: each property of one of the three values as Phazite writes it, handed to a function that writes it (the
    // name, and the value, nullptr for none: a property left out), in a fixed order. A text each where not empty; a switch
    // where its writer always wrote it.
    template <typename Write>
    void writeText (const schema::PropertyDescription& property, const juce::String& text, Write&& write)
    {
        if (text.isEmpty())
        {
            write (property.getName(), nullptr);
            return;
        }

        const juce::var value (text);
        write (property.getName(), &value);
    }

    template <typename Write>
    void writeIf (const schema::PropertyDescription& property, bool held, const juce::var& value, Write&& write)
    {
        write (property.getName(), held ? &value : nullptr);
    }

    // A grid's five (Grid.h: makeSavedGrid()'s), all of them, in its order; or none of them.
    template <typename Write>
    void writeGrid (const std::optional<SavedGrid>& grid, Write&& write)
    {
        const auto held = grid.has_value();
        const auto saved = grid.value_or (SavedGrid {});
        writeIf (schema::gridSnap, held, saved.snap, write);
        writeIf (schema::gridAdaptive, held, saved.adaptive, write);
        writeIf (schema::gridOffset, held, saved.offset, write);
        writeIf (schema::gridStep, held, saved.step, write);
        writeIf (schema::gridTriplets, held, saved.triplets, write);
    }

    // A value's properties on the root.
    template <typename Write>
    void writeRootProperties (SongOwnPart part, const SongOwnValue& own, const SongStanding& standing, const SongOutside& outside,
                              Write&& write)
    {
        switch (part)
        {
            case SongOwnPart::own:
                writeText (schema::outline, own.outline, write);
                writeText (schema::notes, own.notes, write);
                writeText (schema::idNumbering, own.idNumbering, write);
                writeIf (schema::patternsKeepTheirSpeed, own.patternsKeepTheirSpeed, true, write); // only while on
                return;

            case SongOwnPart::standing:
                writeText (schema::versionsBranch, standing.versionsBranch, write);
                writeText (schema::versionsBase, standing.versionsBase, write);
                return;

            case SongOwnPart::outside:
                writeIf (schema::loop, outside.loop, true, write); // only while on
                writeIf (schema::loopFollowsSong, outside.loopFollowsSong.has_value(), outside.loopFollowsSong.value_or (true), write);
                writeIf (schema::loopStart, outside.loopStart.has_value(), outside.loopStart.value_or (0.0), write);
                writeIf (schema::loopEnd, outside.loopEnd.has_value(), outside.loopEnd.value_or (0.0), write);
                writeIf (schema::playStart, outside.playStart > 0.0, outside.playStart, write); // only past the start
                writeIf (schema::metronome, outside.metronome, true, write);                    // only while on
                writeIf (schema::punch, outside.punch.has_value(), outside.punch.value_or (false), write);
                writeText (schema::lastInput, outside.lastInput, write);
                writeGrid (outside.playlistGrid, write);
                return;
        }
    }

    // Whether a value holds its element (MERGING, LASTKEY, PIANOROLL), and that element's properties.
    bool holdsPartElement (SongOwnPart part, const SongOwnValue& own, const SongStanding& standing, const SongOutside& outside)
    {
        switch (part)
        {
            case SongOwnPart::own:
                return own.merging.has_value();
            case SongOwnPart::standing:
                return standing.lastKey.has_value();
            case SongOwnPart::outside:
                return outside.pianoRollGrid.has_value();
        }

        return false;
    }

    template <typename Write>
    void writeElementProperties (SongOwnPart part, const SongOwnValue& own, const SongStanding& standing, const SongOutside& outside,
                                 Write&& write)
    {
        switch (part)
        {
            case SongOwnPart::own:
            {
                const auto record = own.merging.value_or (MergingRecord {});
                writeText (schema::mergingVersion, record.version, write);
                writeText (schema::mergingHead, record.head, write);
                writeText (schema::mergingBefore, record.before, write);
                writeText (schema::mergingChosen, record.chosen, write);
                writeText (schema::mergingRenumbered, record.renumbered, write);
                writeText (schema::mergingTranslated, record.translated, write);
                writeText (schema::mergingCrossed, record.crossed, write);
                return;
            }

            case SongOwnPart::standing:
            {
                const auto key = standing.lastKey.value_or (SavedKey {});
                writeIf (schema::keyRoot, standing.lastKey.has_value(), key.root, write);
                writeText (schema::keyScale, key.scale, write);
                return;
            }

            case SongOwnPart::outside:
                writeGrid (outside.pianoRollGrid, write);
                return;
        }
    }

    const juce::Identifier& getPartElement (SongOwnPart part)
    {
        switch (part)
        {
            case SongOwnPart::own:
                return schema::MERGING.getName();
            case SongOwnPart::standing:
                return schema::LASTKEY.getName();
            case SongOwnPart::outside:
                return schema::PIANOROLL.getName();
        }

        return schema::MERGING.getName();
    }

    //==============================================================================
    SongOwnValue readOwn (const juce::ValueTree& phazite)
    {
        SongOwnValue own;

        if (! phazite.hasType (schema::PHAZITE))
            return own;

        own.outline = readText (phazite, schema::outline);
        own.notes = readText (phazite, schema::notes);
        own.idNumbering = readText (phazite, schema::idNumbering);
        own.patternsKeepTheirSpeed = (bool) phazite.getProperty (schema::patternsKeepTheirSpeed, false);

        if (const auto merging = phazite.getChildWithName (schema::MERGING); merging.isValid())
            own.merging = readMergingRecord (merging);

        return own;
    }

    SongStanding readStanding (const juce::ValueTree& phazite)
    {
        SongStanding standing;

        if (! phazite.hasType (schema::PHAZITE))
            return standing;

        standing.lastKey = readKey (phazite);
        standing.versionsBranch = readText (phazite, schema::versionsBranch);
        standing.versionsBase = readText (phazite, schema::versionsBase);
        return standing;
    }

    SongOutside readOutside (const juce::ValueTree& phazite)
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
                *field = readHeldNumber (*number, *property);

        // A range looped on purpose is both its ends, the end past the start (Playback.cpp: getLoopBeats() loops the song
        // without either, and an empty range plays nothing sensible): else none, as the loader leaves it (keepSongOwnAsHeld()).
        if (! outside.loopStart.has_value() || ! outside.loopEnd.has_value() || ! (*outside.loopEnd > *outside.loopStart))
        {
            outside.loopStart.reset();
            outside.loopEnd.reset();
        }

        // Within its description's range (0 where it is no finite number, which the loader takes out). The project takes it
        // out as it opens.
        if (const auto* start = phazite.getPropertyPointer (schema::playStart))
            outside.playStart = readHeldNumber (*start, schema::playStart).value_or (0.0);

        if (const auto* punch = phazite.getPropertyPointer (schema::punch))
            outside.punch = (bool) *punch;

        outside.lastInput = phazite[schema::lastInput].toString();

        if (holdsGridOn (phazite, Reading::held))
            outside.playlistGrid = readGrid (phazite, Reading::held);

        // A PIANOROLL that holds none of a grid's properties holds no grid (the loader takes it out): the defaults, as a song
        // without one reads.
        if (const auto pianoRoll = phazite.getChildWithName (schema::PIANOROLL);
            pianoRoll.isValid() && holdsGridOn (pianoRoll, Reading::held))
            outside.pianoRollGrid = readGrid (pianoRoll, Reading::held);

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
SongOwnValue readHeldSongOwn (const juce::ValueTree& phazite)
{
    return readOwn (phazite);
}

SongStanding readHeldSongStanding (const juce::ValueTree& phazite)
{
    return readStanding (phazite);
}

SongOutside readHeldSongOutside (const juce::ValueTree& phazite)
{
    return readOutside (phazite);
}

namespace
{
    // A writer that sets each property written on an element.
    auto setOn (juce::ValueTree& element)
    {
        return [&element] (const juce::Identifier& name, const juce::var* value)
        {
            if (value != nullptr)
                element.setProperty (name, *value, nullptr);
        };
    }
} // namespace

void writeSongOwnPropertiesInto (juce::ValueTree& phazite, const SongOwnValue& own, const SongStanding& standing,
                                 const SongOutside& outside)
{
    for (const auto part : { SongOwnPart::own, SongOwnPart::standing, SongOwnPart::outside })
        writeRootProperties (part, own, standing, outside, setOn (phazite));
}

void writeSongOwnInto (juce::ValueTree& phazite, const SongOwnValue& own, const SongStanding& standing, const SongOutside& outside)
{
    // Each value's properties in their order, then the elements, LASTKEY, PIANOROLL and MERGING, each appended where held.
    writeSongOwnPropertiesInto (phazite, own, standing, outside);

    for (const auto part : { SongOwnPart::standing, SongOwnPart::outside, SongOwnPart::own })
    {
        if (! holdsPartElement (part, own, standing, outside))
            continue;

        juce::ValueTree element (getPartElement (part));
        writeElementProperties (part, own, standing, outside, setOn (element));
        phazite.appendChild (element, nullptr);
    }
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
    // other has none, through this undo manager. Whether any changed.
    bool keepAsWritten (juce::ValueTree& element, const juce::ValueTree& written, const std::vector<Described>& properties,
                        juce::UndoManager* undo)
    {
        auto changed = false;

        for (const auto* property : properties)
        {
            const auto* there = element.getPropertyPointer (property->getName());

            if (const auto* made = written.getPropertyPointer (property->getName()))
            {
                if (there == nullptr || ! isSameValue (*there, *made))
                {
                    element.setProperty (property->getName(), *made, undo);
                    changed = true;
                }
            }
            else if (there != nullptr)
            {
                element.removeProperty (property->getName(), undo);
                changed = true;
            }
        }

        return changed;
    }
} // namespace

bool isSongOwnProperty (const juce::Identifier& property)
{
    const auto& properties = getRootProperties();
    return std::any_of (properties.begin(), properties.end(), [&property] (Described each) { return each->getName() == property; });
}

bool isSongOwnElement (const juce::Identifier& type)
{
    return type == schema::LASTKEY.getName() || type == schema::PIANOROLL.getName() || type == schema::MERGING.getName();
}

bool isKnownRootProperty (const juce::Identifier& property)
{
    return schema::getTables().findProperty (schema::PHAZITE.getName(), property) != nullptr;
}

bool isKnownRootElement (const juce::Identifier& type)
{
    return schema::isKnownType (type);
}

bool keepSongOwnAsHeld (juce::ValueTree phazite)
{
    if (! phazite.hasType (schema::PHAZITE))
        return false;

    juce::ValueTree written (schema::PHAZITE);
    writeSongOwnInto (written, readHeldSongOwn (phazite), readHeldSongStanding (phazite), readHeldSongOutside (phazite));
    auto changed = keepAsWritten (phazite, written, getRootProperties(), nullptr);

    for (const auto* type : { &schema::LASTKEY, &schema::PIANOROLL, &schema::MERGING })
    {
        auto held = phazite.getChildWithName (*type);

        if (! held.isValid())
            continue;

        if (const auto made = written.getChildWithName (*type); made.isValid())
        {
            changed = keepAsWritten (held, made, getElementProperties (*type), nullptr) || changed;
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
const SongOwnValue& getSongOwn (const Song& song)
{
    return song.getValues()->songOwn.get();
}

const SongStanding& getSongStanding (const Song& song)
{
    return song.getValues()->songStanding.get();
}

const SongOutside& getSongOutside (const Song& song)
{
    return song.getValues()->songOutside.get();
}
} // namespace phazite
