// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/ChordLaneValue.h"
#include "model/base/Numbers.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace phazite
{
namespace
{
    // A property's number as the song reads one (SongText.h: readValueNumber(), the loader's reading, so that a text reads as
    // the same number wherever it is read), or the one given where the element has none, or what it has is no finite number.
    double readOr (const juce::var& held, double absent)
    {
        const auto number = readValueNumber (held);
        return number.has_value() && std::isfinite (*number) ? *number : absent;
    }

    //==============================================================================
    // As the chord lane's code has always read them (readChordLaneValue()).
    BeatPosition readStart (const juce::ValueTree& element)
    {
        return BeatPosition::fromBeats (std::max (0.0, readOr (element[schema::chordStart], 0.0)));
    }

    ChordEntry readChord (const juce::ValueTree& chord)
    {
        ChordEntry entry;
        entry.start = readStart (chord);
        entry.length = BeatDuration::fromBeats (std::max (0.0, readOr (chord[schema::chordLength], 0.0)));
        entry.degree = juce::jlimit (1, 7, toWhole<int> (readOr (chord[schema::chordDegree], 1.0)));
        entry.accidental = juce::jlimit (-1, 1, toWhole<int> (readOr (chord[schema::chordAccidental], 0.0)));
        entry.inversion = toWhole<int> (readOr (chord[schema::chordInversion], 0.0));
        entry.type = chord[schema::chordType].toString();
        return entry;
    }

    KeyEntry readKey (const juce::ValueTree& key)
    {
        return { readStart (key),
                 key.hasProperty (schema::keyRoot) ? juce::jlimit (0, 11, toWhole<int> (readOr (key[schema::keyRoot], 0.0))) : -1,
                 key[schema::keyScale].toString() };
    }

    //==============================================================================
    // As the song holds them (readHeldChordLane()): each number by its description, a whole one made whole.
    int readWhole (const juce::ValueTree& element, const schema::PropertyDescription& property)
    {
        return roundToWhole<int> (readDescribed (element, property));
    }

    ChordEntry readHeldChord (const juce::ValueTree& chord)
    {
        ChordEntry entry;
        entry.start = BeatPosition::fromBeats (readDescribed (chord, schema::chordStart));
        entry.length = BeatDuration::fromBeats (readDescribed (chord, schema::chordLength));
        entry.degree = readWhole (chord, schema::chordDegree);
        entry.accidental = readWhole (chord, schema::chordAccidental);
        entry.inversion = readWhole (chord, schema::chordInversion);
        entry.type = chord[schema::chordType].toString();
        return entry;
    }

    KeyEntry readHeldKey (const juce::ValueTree& key)
    {
        // A root that is no finite number the loader takes away (its description gives no fallback): none.
        const auto root = readValueNumber (key[schema::keyRoot]);
        return { BeatPosition::fromBeats (readDescribed (key, schema::chordStart)),
                 root.has_value() && std::isfinite (*root) ? readWhole (key, schema::keyRoot) : -1, key[schema::keyScale].toString() };
    }

    template <typename ReadChord, typename ReadKey>
    ChordLaneValue readLane (const juce::ValueTree& lane, ReadChord&& chordOf, ReadKey&& keyOf)
    {
        auto chords = immer::flex_vector<ChordEntry>().transient();
        auto keys = immer::flex_vector<KeyEntry>().transient();

        for (const auto& child : lane)
        {
            if (child.hasType (schema::CHORD))
                chords.push_back (chordOf (child));
            else if (child.hasType (schema::CHORDKEY))
                keys.push_back (keyOf (child));
        }

        return { chords.persistent(), keys.persistent() };
    }
} // namespace

bool ChordEntry::operator== (const ChordEntry& other) const noexcept
{
    return start == other.start && length == other.length && degree == other.degree && accidental == other.accidental
           && inversion == other.inversion && type == other.type;
}

bool KeyEntry::operator== (const KeyEntry& other) const noexcept
{
    return start == other.start && root == other.root && scale == other.scale;
}

bool ChordLaneValue::isSameAs (const ChordLaneValue& other) const noexcept
{
    return chords.identity() == other.chords.identity() && keys.identity() == other.keys.identity();
}

bool ChordLaneValue::operator== (const ChordLaneValue& other) const
{
    return chords == other.chords && keys == other.keys;
}

ChordLaneValue readChordLaneValue (const juce::ValueTree& lane)
{
    return readLane (lane, readChord, readKey);
}

ChordLaneValue readHeldChordLane (const juce::ValueTree& lane)
{
    return keepChordsApart (readLane (lane, readHeldChord, readHeldKey));
}

ChordLaneValue withEntryTaken (const ChordLaneValue& value, const juce::ValueTree& entry)
{
    if (entry.hasType (schema::CHORD))
        return keepChordsApart ({ value.chords.push_back (readHeldChord (entry)), value.keys });

    if (entry.hasType (schema::CHORDKEY))
        return { value.chords, value.keys.push_back (readHeldKey (entry)) };

    return value;
}

ChordLaneValue keepChordsApart (const ChordLaneValue& lane)
{
    const auto near = BeatDuration::fromBeats (chordLaneNear);

    // The chords with a length of their own, by their starts, two at one start in the lane's order.
    std::vector<size_t> order;
    order.reserve (lane.chords.size());

    for (size_t i = 0; i < lane.chords.size(); ++i)
        if (lane.chords[i].length > near)
            order.push_back (i);

    std::stable_sort (order.begin(), order.end(), [&lane] (size_t a, size_t b) { return lane.chords[a].start < lane.chords[b].start; });

    // From the last: each cut at the start of the next one kept, or gone where nothing of it would be left.
    auto chords = lane.chords;
    std::vector<bool> gone (lane.chords.size(), false);
    std::optional<BeatPosition> next;
    auto changed = false;

    for (auto each = order.rbegin(); each != order.rend(); ++each)
    {
        const auto& chord = lane.chords[*each];

        if (next.has_value() && chord.start + chord.length > *next + near)
        {
            changed = true;

            if (*next - chord.start <= near)
            {
                gone[*each] = true;
                continue;
            }

            auto cut = chord;
            cut.length = *next - chord.start;
            chords = chords.set (*each, cut);
        }

        next = chord.start;
    }

    if (! changed)
        return lane;

    auto kept = immer::flex_vector<ChordEntry>().transient();

    for (size_t i = 0; i < chords.size(); ++i)
        if (! gone[i])
            kept.push_back (chords[i]);

    return { kept.persistent(), lane.keys };
}

bool holdsChordsApart (const ChordLaneValue& lane)
{
    return keepChordsApart (lane) == lane;
}

void writeChordLane (const ChordLaneValue& value, juce::ValueTree& lane)
{
    jassert (lane.hasType (schema::CHORDLANE));

    immer::for_each (value.chords,
                     [&lane] (const ChordEntry& chord)
                     {
                         juce::ValueTree element (schema::CHORD, { { schema::chordStart, chord.start.inBeats() },
                                                                   { schema::chordLength, chord.length.inBeats() },
                                                                   { schema::chordDegree, chord.degree } });

                         if (chord.accidental != 0)
                             element.setProperty (schema::chordAccidental, chord.accidental, nullptr);

                         if (chord.type.isNotEmpty())
                             element.setProperty (schema::chordType, chord.type, nullptr);

                         if (chord.inversion != 0)
                             element.setProperty (schema::chordInversion, chord.inversion, nullptr);

                         lane.appendChild (element, nullptr);
                     });

    immer::for_each (value.keys,
                     [&lane] (const KeyEntry& key)
                     {
                         juce::ValueTree element (schema::CHORDKEY, { { schema::chordStart, key.start.inBeats() } });

                         if (key.root >= 0)
                             element.setProperty (schema::keyRoot, key.root, nullptr);

                         if (key.scale.isNotEmpty())
                             element.setProperty (schema::keyScale, key.scale, nullptr);

                         lane.appendChild (element, nullptr);
                     });
}
} // namespace phazite
