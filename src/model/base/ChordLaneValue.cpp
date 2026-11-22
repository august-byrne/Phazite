// SPDX-License-Identifier: AGPL-3.0-or-later
#include "model/base/internal/ChordLaneValue.h"
#include "model/base/internal/Schema.h"

#include <algorithm>

namespace phazite
{
namespace
{
    BeatPosition readStart (const juce::ValueTree& element)
    {
        return BeatPosition::fromBeats (std::max (0.0, (double) element[schema::chordStart]));
    }

    ChordEntry readChord (const juce::ValueTree& chord)
    {
        ChordEntry entry;
        entry.start = readStart (chord);
        entry.length = BeatDuration::fromBeats (std::max (0.0, (double) chord[schema::chordLength]));
        entry.degree = juce::jlimit (1, 7, (int) chord.getProperty (schema::chordDegree, 1));
        entry.accidental = juce::jlimit (-1, 1, (int) chord[schema::chordAccidental]);
        entry.inversion = (int) chord[schema::chordInversion];
        entry.type = chord[schema::chordType].toString();
        return entry;
    }

    KeyEntry readKey (const juce::ValueTree& key)
    {
        return { readStart (key), key.hasProperty (schema::keyRoot) ? juce::jlimit (0, 11, (int) key[schema::keyRoot]) : -1,
                 key[schema::keyScale].toString() };
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
    auto chords = immer::flex_vector<ChordEntry>().transient();
    auto keys = immer::flex_vector<KeyEntry>().transient();

    for (const auto& child : lane)
    {
        if (child.hasType (schema::CHORD))
            chords.push_back (readChord (child));
        else if (child.hasType (schema::CHORDKEY))
            keys.push_back (readKey (child));
    }

    return { chords.persistent(), keys.persistent() };
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

                         element.setProperty (schema::keyScale, key.scale, nullptr);
                         lane.appendChild (element, nullptr);
                     });
}
} // namespace phazite
