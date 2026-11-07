// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Chords.h"
#include "Progressions.h"
#include "model/base/internal/Schema.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // Positions this close count as the same.
    constexpr double tiny = 1.0e-6;

    juce::UndoManager* getUndo (Song& song)
    {
        return &song.getUndoManager();
    }

    juce::ValueTree findLaneState (const Song& song)
    {
        return song.getState().getChildWithName (schema::CHORDLANE);
    }

    std::optional<Key> readKey (const juce::ValueTree& state)
    {
        if (! state.hasProperty (schema::keyRoot))
            return std::nullopt;

        return Key { juce::jlimit (0, 11, (int) state[schema::keyRoot]),
                     findScale (state[schema::keyScale].toString()).value_or (Scale::major) };
    }

    void writeKey (juce::ValueTree state, const Key& key, juce::UndoManager* undo)
    {
        state.setProperty (schema::keyRoot, key.root, undo);
        state.setProperty (schema::keyScale, getSavedScaleName (key.scale), undo);
    }

    // The key before any is written: the one new tracks get, which a new project sets, or else C major.
    Key getDefaultKey (const Song& song)
    {
        return readKey (song.getState().getChildWithName (schema::LASTKEY)).value_or (Key {});
    }

    BeatPosition getStart (const juce::ValueTree& state)
    {
        return BeatPosition::fromBeats (std::max (0.0, (double) state[schema::chordStart]));
    }

    BeatRange getRange (const juce::ValueTree& chord)
    {
        return { getStart (chord), BeatDuration::fromBeats (std::max (0.0, (double) chord[schema::chordLength])) };
    }

    void setRange (juce::ValueTree chord, BeatRange range, juce::UndoManager* undo)
    {
        chord.setProperty (schema::chordStart, range.getStart().inBeats(), undo);
        chord.setProperty (schema::chordLength, range.getLength().inBeats(), undo);
    }

    std::optional<Numeral> readNumeral (const juce::ValueTree& chord)
    {
        const auto type = findChordType (chord[schema::chordType].toString());

        if (! type.has_value())
            return std::nullopt;

        Numeral numeral;
        numeral.degree = juce::jlimit (1, 7, (int) chord.getProperty (schema::chordDegree, 1));
        numeral.accidental = juce::jlimit (-1, 1, (int) chord[schema::chordAccidental]);
        numeral.type = *type;
        numeral.inversion = juce::jlimit (0, (int) getChordIntervals (*type).size() - 1, (int) chord[schema::chordInversion]);
        return numeral;
    }

    // A chord's numeral: its accidental and inversion only when they aren't 0.
    void writeNumeral (juce::ValueTree chord, const Numeral& numeral, juce::UndoManager* undo)
    {
        chord.setProperty (schema::chordDegree, numeral.degree, undo);

        if (numeral.accidental != 0)
            chord.setProperty (schema::chordAccidental, numeral.accidental, undo);
        else
            chord.removeProperty (schema::chordAccidental, undo);

        chord.setProperty (schema::chordType, getSavedChordTypeName (numeral.type), undo);

        if (numeral.inversion != 0)
            chord.setProperty (schema::chordInversion, numeral.inversion, undo);
        else
            chord.removeProperty (schema::chordInversion, undo);
    }

    juce::ValueTree makeChordState (BeatRange range, const Numeral& numeral)
    {
        juce::ValueTree chord (schema::CHORD);
        setRange (chord, range, nullptr);
        writeNumeral (chord, numeral, nullptr);
        return chord;
    }

    // The lane's chord or key change that starts at a beat.
    juce::ValueTree findStartingAt (const Song& song, const juce::Identifier& type, BeatPosition start)
    {
        for (auto child : findLaneState (song))
            if (child.hasType (type) && std::abs ((getStart (child) - start).inBeats()) < 1.0e-4)
                return child;

        return {};
    }

    std::vector<juce::ValueTree> getChildren (const juce::ValueTree& lane, const juce::Identifier& type)
    {
        std::vector<juce::ValueTree> children;

        for (auto child : lane)
            if (child.hasType (type))
                children.push_back (child);

        return children;
    }

    int getBeatsPerBar (const Song& song, BeatPosition where)
    {
        return getBars (song).getBeatsPerBar (where);
    }

    //==============================================================================
    // Time edits.

    // Moves a chord's range, cutting it where it would start before the song.
    void moveChord (juce::ValueTree chord, BeatDuration distance, juce::UndoManager* undo)
    {
        const auto range = getRange (chord).movedBy (distance);
        const auto start = std::max (BeatPosition(), range.getStart());

        if (range.getEnd() <= start + BeatDuration::fromBeats (tiny))
            chord.getParent().removeChild (chord, undo);
        else
            setRange (chord, { start, range.getEnd() }, undo);
    }
} // namespace

//==============================================================================
std::vector<LaneKey> getLaneKeys (const Song& song)
{
    std::vector<LaneKey> keys;

    for (const auto& state : getChildren (findLaneState (song), schema::CHORDKEY))
        if (const auto key = readKey (state))
            keys.push_back ({ getStart (state), *key });

    std::stable_sort (keys.begin(), keys.end(), [] (const LaneKey& a, const LaneKey& b) { return a.start < b.start; });

    if (keys.empty() || keys.front().start > BeatPosition::fromBeats (tiny))
        keys.insert (keys.begin(), { BeatPosition(), getDefaultKey (song) });

    return keys;
}

Key getLaneKeyAt (const Song& song, BeatPosition where)
{
    const auto keys = getLaneKeys (song);
    auto key = keys.front().key;

    for (const auto& change : keys)
        if (change.start <= where + BeatDuration::fromBeats (tiny))
            key = change.key;

    return key;
}

std::vector<PlacedChord> getLaneChords (const Song& song)
{
    const auto keys = getLaneKeys (song);
    std::vector<PlacedChord> chords;

    for (const auto& state : getChildren (findLaneState (song), schema::CHORD))
    {
        const auto range = getRange (state);
        const auto numeral = readNumeral (state);

        if (range.isEmpty() || ! numeral.has_value())
            continue;

        auto key = keys.front().key;

        for (const auto& change : keys)
            if (change.start <= range.getStart() + BeatDuration::fromBeats (tiny))
                key = change.key;

        chords.push_back ({ range, *numeral, key });
    }

    std::stable_sort (chords.begin(), chords.end(),
                      [] (const PlacedChord& a, const PlacedChord& b) { return a.range.getStart() < b.range.getStart(); });
    return chords;
}

juce::ValueTree getLaneState (Song& song)
{
    auto phazite = song.getState();
    auto lane = phazite.getChildWithName (schema::CHORDLANE);

    if (! lane.isValid())
    {
        lane = juce::ValueTree (schema::CHORDLANE);
        juce::ValueTree key (schema::CHORDKEY);
        key.setProperty (schema::chordStart, 0.0, nullptr);
        writeKey (key, getDefaultKey (song), nullptr);
        lane.appendChild (key, nullptr);
        phazite.appendChild (lane, getUndo (song));
    }

    return lane;
}

//==============================================================================
void clearLaneRange (Song& song, BeatRange range)
{
    auto lane = findLaneState (song);

    if (! lane.isValid() || range.isEmpty())
        return;

    auto* undo = getUndo (song);
    const auto start = range.getStart(), end = range.getEnd();
    const auto margin = BeatDuration::fromBeats (tiny);

    for (auto chord : getChildren (lane, schema::CHORD))
    {
        const auto r = getRange (chord);

        if (r.getEnd() <= start + margin || r.getStart() >= end - margin)
            continue; // outside the range

        if (r.getStart() >= start - margin && r.getEnd() <= end + margin)
        {
            lane.removeChild (chord, undo);
        }
        else if (r.getStart() < start && r.getEnd() > end)
        {
            // Around the range: the part after it becomes a chord of its own.
            auto after = chord.createCopy();
            setRange (after, { end, r.getEnd() }, nullptr);
            setRange (chord, { r.getStart(), start }, undo);
            lane.appendChild (after, undo);
        }
        else if (r.getStart() < start)
        {
            setRange (chord, { r.getStart(), start }, undo);
        }
        else
        {
            setRange (chord, { end, r.getEnd() }, undo);
        }
    }
}

void putLaneChord (Song& song, BeatRange range, const Numeral& numeral)
{
    if (range.isEmpty())
        return;

    auto lane = getLaneState (song);
    clearLaneRange (song, range);
    lane.appendChild (makeChordState (range, numeral), getUndo (song));
}

bool hasLaneChordAt (const Song& song, BeatPosition start)
{
    return findStartingAt (song, schema::CHORD, start).isValid();
}

void setLaneChordNumeral (Song& song, BeatPosition start, const Numeral& numeral)
{
    if (auto chord = findStartingAt (song, schema::CHORD, start); chord.isValid())
        writeNumeral (chord, numeral, getUndo (song));
}

void removeLaneChord (Song& song, BeatPosition start)
{
    if (auto chord = findStartingAt (song, schema::CHORD, start); chord.isValid())
        chord.getParent().removeChild (chord, getUndo (song));
}

void setLaneChordEnd (Song& song, BeatPosition start, BeatPosition end)
{
    auto chord = findStartingAt (song, schema::CHORD, start);

    if (! chord.isValid() || end <= start + BeatDuration::fromBeats (tiny))
        return;

    // A copy goes in its place once the chords it grows over have made room.
    auto lane = chord.getParent();
    auto resized = chord.createCopy();
    setRange (resized, { getStart (chord), end }, nullptr);
    lane.removeChild (chord, getUndo (song));
    clearLaneRange (song, { start, end });
    lane.appendChild (resized, getUndo (song));
}

void setLaneKey (Song& song, BeatPosition from, const Key& key)
{
    auto lane = getLaneState (song);

    // The key change at the beat, or a new one there.
    if (auto change = findStartingAt (song, schema::CHORDKEY, from); change.isValid())
        return writeKey (change, key, getUndo (song));

    juce::ValueTree change (schema::CHORDKEY);
    change.setProperty (schema::chordStart, std::max (0.0, from.inBeats()), nullptr);
    writeKey (change, key, nullptr);
    lane.appendChild (change, getUndo (song));
}

void removeLaneKeyChange (Song& song, BeatPosition at)
{
    if (at <= BeatPosition::fromBeats (1.0e-4))
        return;

    if (auto change = findStartingAt (song, schema::CHORDKEY, at); change.isValid())
        change.getParent().removeChild (change, getUndo (song));
}

void putProgression (Song& song, const Progression& progression, BeatRange range)
{
    if (range.isEmpty() || progression.chords.empty())
        return;

    auto lane = getLaneState (song);
    clearLaneRange (song, range);

    auto beat = range.getStart();

    for (size_t i = 0; beat < range.getEnd() - BeatDuration::fromBeats (tiny); i = (i + 1) % progression.chords.size())
    {
        const auto& chord = progression.chords[i];
        const auto key = getLaneKeyAt (song, beat);
        const auto root = getRootIn (chord.numeral, getKeyForProgression (key, progression.scale));
        const auto length = BeatDuration::fromBeats (chord.bars * getBeatsPerBar (song, beat));
        const auto end = std::min (range.getEnd(), beat + length);

        lane.appendChild (makeChordState ({ beat, end }, toNumeral (root, chord.numeral.type, chord.numeral.inversion, key)),
                          getUndo (song));
        beat = end;
    }
}

//==============================================================================
void insertLaneTime (Song& song, BeatPosition at, BeatDuration length)
{
    auto lane = findLaneState (song);

    if (! lane.isValid() || length <= BeatDuration())
        return;

    auto* undo = getUndo (song);
    const auto margin = BeatDuration::fromBeats (tiny);

    for (auto state : lane)
    {
        if (state.hasType (schema::CHORDKEY))
        {
            if (const auto start = getStart (state); start > BeatPosition() + margin && start >= at - margin)
                state.setProperty (schema::chordStart, (start + length).inBeats(), undo);
        }
        else if (state.hasType (schema::CHORD))
        {
            const auto r = getRange (state);

            if (r.getStart() >= at - margin)
                setRange (state, r.movedBy (length), undo);
            else if (r.getEnd() > at + margin)
                setRange (state, { r.getStart(), r.getEnd() + length }, undo); // the time is inside it
        }
    }
}

void removeLaneTime (Song& song, BeatRange range)
{
    auto lane = findLaneState (song);

    if (! lane.isValid() || range.isEmpty())
        return;

    auto* undo = getUndo (song);
    const auto length = range.getLength();
    const auto start = range.getStart(), end = range.getEnd();
    const auto margin = BeatDuration::fromBeats (tiny);

    for (auto state : getChildren (lane, schema::CHORD))
    {
        const auto r = getRange (state);

        if (r.getEnd() <= start + margin)
            continue;

        if (r.getStart() >= end - margin)
            setRange (state, r.movedBy (-length), undo);
        else if (r.getStart() >= start - margin && r.getEnd() <= end + margin)
            lane.removeChild (state, undo);
        else if (r.getStart() < start && r.getEnd() > end)
            setRange (state, { r.getStart(), r.getEnd() - length }, undo);
        else if (r.getStart() < start)
            setRange (state, { r.getStart(), start }, undo);
        else
            setRange (state, { start, r.getEnd() - length }, undo);
    }

    // Key changes in the range go to its start, where what came after them now is; those after it move back.
    for (auto state : getChildren (lane, schema::CHORDKEY))
    {
        const auto keyStart = getStart (state);

        if (keyStart <= BeatPosition() + margin || keyStart < start)
            continue;

        state.setProperty (schema::chordStart, (keyStart >= end ? keyStart - length : start).inBeats(), undo);
    }
}

void moveLaneTime (Song& song, BeatRange range, BeatPosition destination)
{
    auto lane = findLaneState (song);
    const auto start = range.getStart(), end = range.getEnd();
    const auto length = range.getLength();
    const auto margin = BeatDuration::fromBeats (tiny);
    const auto later = destination >= end - margin;

    if (! lane.isValid() || range.isEmpty() || (destination > start + margin && destination < end - margin))
        return;

    // As moveTime() does: the chords are cut at the range's edges and the destination, then those in the range go to the
    // destination and those between it and the destination the other way.
    const auto changedStart = later ? start : destination;
    const auto changedEnd = later ? destination : end;
    const auto rangeShift = later ? destination - end : destination - start;
    const auto otherShift = later ? -length : length;

    for (const auto beat : { start, end, destination })
    {
        for (auto chord : getChildren (lane, schema::CHORD))
        {
            const auto r = getRange (chord);

            if (r.getStart() < beat - margin && r.getEnd() > beat + margin)
            {
                auto after = chord.createCopy();
                setRange (after, { beat, r.getEnd() }, nullptr);
                setRange (chord, { r.getStart(), beat }, getUndo (song));
                lane.appendChild (after, getUndo (song));
            }
        }
    }

    for (auto chord : getChildren (lane, schema::CHORD))
    {
        const auto chordStart = getRange (chord).getStart();

        if (chordStart >= changedStart - margin && chordStart < changedEnd - margin)
        {
            const auto inRange = chordStart >= start - margin && chordStart < end - margin;
            moveChord (chord, inRange ? rangeShift : otherShift, getUndo (song));
        }
    }
}

LaneCopy copyLaneChords (const Song& song, BeatRange range)
{
    LaneCopy copy;
    copy.rangeStart = range.getStart();

    for (const auto& chord : getChildren (findLaneState (song), schema::CHORD))
    {
        if (const auto r = getRange (chord).getIntersectionWith (range); r.getLength() > BeatDuration::fromBeats (tiny))
        {
            auto piece = chord.createCopy();
            setRange (piece, r, nullptr);
            copy.chords.push_back (piece);
        }
    }

    return copy;
}

void pasteLaneChords (Song& song, const LaneCopy& copy, BeatPosition destination)
{
    if (copy.chords.empty())
        return;

    auto lane = getLaneState (song);

    for (const auto& chord : copy.chords)
    {
        auto pasted = chord.createCopy();
        const auto range = getRange (chord).movedBy (destination - copy.rangeStart);
        setRange (pasted, range, nullptr);
        clearLaneRange (song, range); // a chord the inserted time made longer is cut around it
        lane.appendChild (pasted, getUndo (song));
    }
}
} // namespace phazite
