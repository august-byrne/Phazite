// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Chords.h"
#include "Progressions.h"
#include "model/base/internal/ChordLaneValue.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongPieces.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // Positions this close count as the same: the lane's nearness (ChordLaneValue.h), by which a lane that enters the song keeps
    // its chords apart as the writers here leave them (keepChordsApart()).
    constexpr double tiny = chordLaneNear;

    // How near a beat a chord or a key change starts that is "at" it (findStartingAt()).
    constexpr double nearAt = 1.0e-4;

    juce::ValueTree findLaneState (const Song& song)
    {
        return song.getState().getChildWithName (schema::CHORDLANE);
    }

    // The song's tree written as the song is written now: in the step in progress, or outside the history.
    juce::UndoManager* getTreeUndo (Song& song)
    {
        return song.isWritingOutsideHistory() ? nullptr : &song.getUndoManager();
    }

    // The key before any is written: the one new tracks get, which a new project sets (SongOwnValue.h: SongStanding's key
    // set last, as model/notes/Notes.cpp's getLastKey() reads it: its scale by its word, major for none), or else C major.
    Key getDefaultKey (const Song& song)
    {
        const auto saved = getSongStanding (song).lastKey;

        if (! saved.has_value())
            return Key {};

        return Key { .root = saved->root, .scale = findScale (saved->scale).value_or (Scale::major) };
    }

    BeatDuration getBarLength (const Song& song, BeatPosition where)
    {
        return getBars (song).getBarLength (where);
    }

    // A key change's key, as the lane's code reads one: none for one without a root.
    std::optional<Key> readKey (const KeyEntry& entry)
    {
        if (entry.root < 0)
            return std::nullopt;

        return Key { entry.root, findScale (entry.scale).value_or (Scale::major) };
    }

    // A chord's numeral, as the lane's code reads one: none for a type Phazite doesn't know.
    std::optional<Numeral> readNumeral (const ChordEntry& entry)
    {
        const auto type = findChordType (entry.type);

        if (! type.has_value())
            return std::nullopt;

        Numeral numeral;
        numeral.degree = entry.degree;
        numeral.accidental = entry.accidental;
        numeral.type = *type;
        numeral.inversion = juce::jlimit (0, (int) getChordIntervals (*type).size() - 1, entry.inversion);
        return numeral;
    }

    // The lane's keys from its value, by their starts, the default key from 0 where none starts there (getLaneKeys()).
    std::vector<LaneKey> getKeysOf (const Song& song, const ChordLaneValue& lane)
    {
        std::vector<LaneKey> keys;
        keys.reserve (lane.keys.size() + 1);

        for (const auto& entry : lane.keys)
            if (const auto key = readKey (entry))
                keys.push_back ({ entry.start, *key });

        std::stable_sort (keys.begin(), keys.end(), [] (const LaneKey& a, const LaneKey& b) { return a.start < b.start; });

        if (keys.empty() || keys.front().start > BeatPosition::fromBeats (tiny))
            keys.insert (keys.begin(), { BeatPosition(), getDefaultKey (song) });

        return keys;
    }

    // The key from the lane's keys (getKeysOf()) at a beat: the last that starts there or before.
    Key findKeyAt (const std::vector<LaneKey>& keys, BeatPosition where)
    {
        auto key = keys.front().key;

        for (const auto& change : keys)
            if (change.start <= where + BeatDuration::fromBeats (tiny))
                key = change.key;

        return key;
    }

    //==============================================================================
    // The lane as an edit works on it: its chords and its key changes, each kind in the lane's order, made from its value and
    // set as the next one once (setLane()).
    struct Lane
    {
        explicit Lane (const ChordLaneValue& value)
            : chords (value.chords.begin(), value.chords.end()),
              keys (value.keys.begin(), value.keys.end()),
              placed (value.placed)
        {
        }

        ChordLaneValue toValue() const
        {
            ChordLaneValue value;
            value.chords = immer::flex_vector<ChordEntry> (chords.begin(), chords.end());
            value.keys = immer::flex_vector<KeyEntry> (keys.begin(), keys.end());
            value.placed = placed;
            return value;
        }

        std::vector<ChordEntry> chords;
        std::vector<KeyEntry> keys;
        bool placed = false; // whether the song has the lane (ChordLaneValue.h): set where openLane() adds its element
    };

    // The song's chord lane made the next value: its area's, in the step in progress, or outside the history while the song
    // is written outside it (SongValues.h: setChordLane(), which writes nothing for the lane the song holds already).
    void setLane (Song& song, const Lane& lane)
    {
        // Each chord's start and length and each key change's start within their descriptions' ranges (SongText.h:
        // keepDescribed()), as the readers keep them; where one wasn't, its chords kept apart, as everything that enters the
        // lane is (keepChordsApart(): a chord kept at the song's last beat can come to overlap another there).
        const auto chordIsKept = [] (const ChordEntry& chord)
        {
            return chord.start == keepDescribed (chord.start, schema::chordStart)
                   && chord.length == keepDescribed (chord.length, schema::chordLength);
        };

        const auto keyIsKept = [] (const KeyEntry& key) { return key.start == keepDescribed (key.start, schema::chordStart); };
        auto value = lane.toValue();

        if (! std::all_of (lane.chords.begin(), lane.chords.end(), chordIsKept)
            || ! std::all_of (lane.keys.begin(), lane.keys.end(), keyIsKept))
        {
            auto kept = lane;

            for (auto& chord : kept.chords)
            {
                chord.start = keepDescribed (chord.start, schema::chordStart);
                chord.length = keepDescribed (chord.length, schema::chordLength);
            }

            // A key change kept at the last beat in place of one there (of two at one start, the later in the lane's order
            // stays, as a key set later at a beat replaces the one there: findStartingAt(), nearAt).
            std::vector<KeyEntry> keys;

            for (auto key : kept.keys)
            {
                key.start = keepDescribed (key.start, schema::chordStart);
                const auto isThere = [&key] (const KeyEntry& other) { return std::abs ((other.start - key.start).inBeats()) < nearAt; };
                keys.erase (std::remove_if (keys.begin(), keys.end(), isThere), keys.end());
                keys.push_back (key);
            }

            kept.keys = std::move (keys);
            value = keepChordsApart (kept.toValue());
        }

        if (auto* values = song.getValues())
            values->setChordLane (song, std::move (value));
        else
            jassertfalse; // the chord lane's edits are a song's that holds its values (model/project/internal/Project.h: makeSong())
    }

    // The lane an edit that writes a chord or a key works on: the song's, its element added first where it has none (an
    // action of the tree's in the step in progress), starting with a key from 0, the one getLaneKeys() gives until then. The
    // edit sets the value it works out (setLane()), the step's second action, which that key alone makes another value: so
    // Undo takes the lane's value back and its element away, and the keys are the key set last again. A song without values
    // gets no element: nothing is written (setLane() refuses the edit).
    Lane openLane (Song& song)
    {
        if (song.getValues() == nullptr || findLaneState (song).isValid())
            return Lane (getChordLaneValue (song));

        song.getState().appendChild (juce::ValueTree (schema::CHORDLANE), getTreeUndo (song));
        Lane lane (getChordLaneValue (song)); // empty: a song without a lane holds none (ChordLaneValue.h)
        lane.placed = true;
        const auto key = getDefaultKey (song);
        lane.keys.push_back ({ BeatPosition(), key.root, getSavedScaleName (key.scale) });
        return lane;
    }

    BeatRange getRange (const ChordEntry& chord)
    {
        return { chord.start, chord.length };
    }

    void setRange (ChordEntry& chord, BeatRange range)
    {
        chord.start = range.getStart();
        chord.length = range.getLength();
    }

    // A chord's numeral: its accidental and inversion 0 where it has none, as the lane writes them (writeChordLane()).
    void writeNumeral (ChordEntry& chord, const Numeral& numeral)
    {
        chord.degree = numeral.degree;
        chord.accidental = numeral.accidental;
        chord.type = getSavedChordTypeName (numeral.type);
        chord.inversion = numeral.inversion;
    }

    ChordEntry makeChord (BeatRange range, const Numeral& numeral)
    {
        ChordEntry chord;
        setRange (chord, range);
        writeNumeral (chord, numeral);
        return chord;
    }

    void writeKey (KeyEntry& change, const Key& key)
    {
        change.root = key.root;
        change.scale = getSavedScaleName (key.scale);
    }

    // The lane's first chord or key change (in the lane's order) that starts at a beat.
    template <typename Entry>
    std::optional<size_t> findStartingAt (const std::vector<Entry>& entries, BeatPosition start)
    {
        for (size_t i = 0; i < entries.size(); ++i)
            if (std::abs ((entries[i].start - start).inBeats()) < nearAt)
                return i;

        return std::nullopt;
    }

    // Room made for chords in a range (clearLaneRange()): those inside it go, and those that reach into it are cut at its
    // edges, each in its place; the part after the range of one around it a chord of its own, after the others.
    void clearChords (std::vector<ChordEntry>& chords, BeatRange range)
    {
        if (range.isEmpty())
            return;

        const auto start = range.getStart(), end = range.getEnd();
        const auto margin = BeatDuration::fromBeats (tiny);
        std::vector<ChordEntry> kept, after;
        kept.reserve (chords.size());

        for (auto chord : chords)
        {
            const auto r = getRange (chord);

            if (r.getEnd() <= start + margin || r.getStart() >= end - margin)
            {
                kept.push_back (chord); // outside the range
                continue;
            }

            if (r.getStart() >= start - margin && r.getEnd() <= end + margin)
                continue; // inside it: gone

            if (r.getStart() < start && r.getEnd() > end)
            {
                // Around the range: the part after it becomes a chord of its own.
                auto piece = chord;
                setRange (piece, { end, r.getEnd() });
                after.push_back (piece);
                setRange (chord, { r.getStart(), start });
            }
            else if (r.getStart() < start)
            {
                setRange (chord, { r.getStart(), start });
            }
            else
            {
                setRange (chord, { end, r.getEnd() });
            }

            kept.push_back (chord);
        }

        kept.insert (kept.end(), after.begin(), after.end());
        chords = std::move (kept);
    }

    //==============================================================================
    // Time edits.

    // A chord's range moved, cut where it would start before the song: whether anything of it is left (none: it goes).
    bool moveChord (ChordEntry& chord, BeatDuration distance)
    {
        const auto range = getRange (chord).movedBy (distance);
        const auto start = std::max (BeatPosition(), range.getStart());

        if (range.getEnd() <= start + BeatDuration::fromBeats (tiny))
            return false;

        setRange (chord, { start, range.getEnd() });
        return true;
    }
} // namespace

//==============================================================================
ChordLaneValue getChordLaneValue (const Song& song)
{
    if (const auto* values = song.getValues())
        return values->chordLane.get();

    return readChordLaneValue (findLaneState (song));
}

bool hasChordLane (const Song& song)
{
    if (const auto* values = song.getValues())
        return values->chordLane.get().placed;

    return findLaneState (song).isValid();
}

bool makeChordLaneAs (Song& song, const juce::ValueTree& lane)
{
    auto* values = song.getValues();

    if (values == nullptr || (lane.isValid() && ! lane.hasType (schema::CHORDLANE)))
        return false;

    // Its element first where it comes, its value, then its element away where it goes: the value never says more than the
    // element does (a song without a lane holds an empty one).
    auto state = song.getState();
    const auto live = findLaneState (song);
    const auto before = values->chordLane.get();
    auto wrote = false;

    if (lane.isValid() && ! live.isValid())
    {
        state.appendChild (juce::ValueTree (schema::CHORDLANE), getTreeUndo (song));
        wrote = true;
    }

    values->setChordLane (song, lane.isValid() ? readHeldChordLane (lane) : ChordLaneValue {});
    wrote = wrote || ! values->chordLane.get().isSameAs (before);

    if (! lane.isValid() && live.isValid())
    {
        state.removeChild (live, getTreeUndo (song));
        wrote = true;
    }

    return wrote;
}

std::vector<LaneKey> getLaneKeys (const Song& song)
{
    return getKeysOf (song, getChordLaneValue (song));
}

Key getLaneKeyAt (const Song& song, BeatPosition where)
{
    return findKeyAt (getLaneKeys (song), where);
}

std::vector<PlacedChord> getLaneChords (const Song& song)
{
    // The lane read once, for its keys and its chords.
    const auto lane = getChordLaneValue (song);
    const auto keys = getKeysOf (song, lane);
    std::vector<PlacedChord> chords;
    chords.reserve (lane.chords.size());

    for (const auto& entry : lane.chords)
    {
        const BeatRange range { entry.start, entry.length };
        const auto numeral = readNumeral (entry);

        if (range.isEmpty() || ! numeral.has_value())
            continue;

        chords.push_back ({ range, *numeral, findKeyAt (keys, range.getStart()) });
    }

    std::stable_sort (chords.begin(), chords.end(),
                      [] (const PlacedChord& a, const PlacedChord& b) { return a.range.getStart() < b.range.getStart(); });
    return chords;
}

//==============================================================================
void clearLaneRange (Song& song, BeatRange range)
{
    if (! hasChordLane (song) || range.isEmpty())
        return;

    Lane lane (getChordLaneValue (song));
    clearChords (lane.chords, range);
    setLane (song, lane);
}

void putLaneChord (Song& song, BeatRange range, const Numeral& numeral)
{
    if (range.isEmpty())
        return;

    auto lane = openLane (song);
    clearChords (lane.chords, range);
    lane.chords.push_back (makeChord (range, numeral));
    setLane (song, lane);
}

bool hasLaneChordAt (const Song& song, BeatPosition start)
{
    // As findStartingAt() finds one.
    const auto lane = getChordLaneValue (song);
    return std::any_of (lane.chords.begin(), lane.chords.end(),
                        [start] (const ChordEntry& chord) { return std::abs ((chord.start - start).inBeats()) < nearAt; });
}

void setLaneChordNumeral (Song& song, BeatPosition start, const Numeral& numeral)
{
    Lane lane (getChordLaneValue (song));

    if (const auto found = findStartingAt (lane.chords, start))
    {
        writeNumeral (lane.chords[*found], numeral);
        setLane (song, lane);
    }
}

void removeLaneChord (Song& song, BeatPosition start)
{
    Lane lane (getChordLaneValue (song));

    if (const auto found = findStartingAt (lane.chords, start))
    {
        lane.chords.erase (lane.chords.begin() + (std::ptrdiff_t) *found);
        setLane (song, lane);
    }
}

void setLaneChordEnd (Song& song, BeatPosition start, BeatPosition end)
{
    Lane lane (getChordLaneValue (song));
    const auto found = findStartingAt (lane.chords, start);

    if (! found.has_value() || end <= start + BeatDuration::fromBeats (tiny))
        return;

    // The chord resized goes after the others once the chords it grows over have made room.
    auto resized = lane.chords[*found];
    setRange (resized, { resized.start, end });
    lane.chords.erase (lane.chords.begin() + (std::ptrdiff_t) *found);
    clearChords (lane.chords, { start, end });
    lane.chords.push_back (resized);
    setLane (song, lane);
}

void setLaneKey (Song& song, BeatPosition from, const Key& key)
{
    auto lane = openLane (song);

    // The key change at the beat (from 0 to the song's last beat: SongText.h, keepDescribed()), or a new one there.
    const auto at = keepDescribed (from, schema::chordStart);

    if (const auto found = findStartingAt (lane.keys, at))
    {
        writeKey (lane.keys[*found], key);
    }
    else
    {
        KeyEntry change;
        change.start = at;
        writeKey (change, key);
        lane.keys.push_back (change);
    }

    setLane (song, lane);
}

void removeLaneKeyChange (Song& song, BeatPosition at)
{
    if (at <= BeatPosition::fromBeats (nearAt))
        return;

    Lane lane (getChordLaneValue (song));

    if (const auto found = findStartingAt (lane.keys, at))
    {
        lane.keys.erase (lane.keys.begin() + (std::ptrdiff_t) *found);
        setLane (song, lane);
    }
}

void putProgression (Song& song, const Progression& progression, BeatRange range)
{
    if (range.isEmpty() || progression.chords.empty())
        return;

    auto lane = openLane (song);
    clearChords (lane.chords, range);

    // The lane's keys read once: placing chords changes none of them.
    const auto keys = getKeysOf (song, lane.toValue());
    auto beat = range.getStart();

    for (size_t i = 0; beat < range.getEnd() - BeatDuration::fromBeats (tiny); i = (i + 1) % progression.chords.size())
    {
        const auto& chord = progression.chords[i];
        const auto key = findKeyAt (keys, beat);
        const auto root = getRootIn (chord.numeral, getKeyForProgression (key, progression.scale));
        const auto length = getBarLength (song, beat) * chord.bars;
        const auto end = std::min (range.getEnd(), beat + length);

        lane.chords.push_back (makeChord ({ beat, end }, toNumeral (root, chord.numeral.type, chord.numeral.inversion, key)));
        beat = end;
    }

    setLane (song, lane);
}

//==============================================================================
void insertLaneTime (Song& song, BeatPosition at, BeatDuration length)
{
    if (! hasChordLane (song) || length <= BeatDuration())
        return;

    Lane lane (getChordLaneValue (song));
    const auto margin = BeatDuration::fromBeats (tiny);

    for (auto& change : lane.keys)
        if (change.start > BeatPosition() + margin && change.start >= at - margin)
            change.start = change.start + length;

    for (auto& chord : lane.chords)
    {
        const auto r = getRange (chord);

        if (r.getStart() >= at - margin)
            setRange (chord, r.movedBy (length));
        else if (r.getEnd() > at + margin)
            setRange (chord, { r.getStart(), r.getEnd() + length }); // the time is inside it
    }

    setLane (song, lane);
}

void removeLaneTime (Song& song, BeatRange range)
{
    if (! hasChordLane (song) || range.isEmpty())
        return;

    Lane lane (getChordLaneValue (song));
    const auto length = range.getLength();
    const auto start = range.getStart(), end = range.getEnd();
    const auto margin = BeatDuration::fromBeats (tiny);
    // What is left of a chord once the range's time is taken out: none of one inside it.
    const auto cut = [&] (ChordEntry chord) -> std::optional<ChordEntry>
    {
        const auto r = getRange (chord);

        if (r.getEnd() <= start + margin)
            return chord;

        if (r.getStart() >= end - margin)
            setRange (chord, r.movedBy (-length));
        else if (r.getStart() >= start - margin && r.getEnd() <= end + margin)
            return std::nullopt;
        else if (r.getStart() < start && r.getEnd() > end)
            setRange (chord, { r.getStart(), r.getEnd() - length });
        else if (r.getStart() < start)
            setRange (chord, { r.getStart(), start });
        else
            setRange (chord, { start, r.getEnd() - length });

        return chord;
    };

    std::vector<ChordEntry> chords;
    chords.reserve (lane.chords.size());

    for (const auto& chord : lane.chords)
        if (const auto left = cut (chord))
            chords.push_back (*left);

    lane.chords = std::move (chords);

    // Key changes: what played after the range plays from its start, as the tempo's changes do (removeTempoMapTime()). The
    // last one in the range, or one where it ends, moves to its start; the others in it go, so that no two are on one beat;
    // those after it move back.
    std::optional<size_t> last;
    std::vector<bool> inRange (lane.keys.size(), false);

    for (size_t i = 0; i < lane.keys.size(); ++i)
    {
        const auto keyStart = lane.keys[i].start;

        if (keyStart < start - margin)
            continue;

        if (keyStart > end + margin)
        {
            lane.keys[i].start = keyStart - length;
            continue;
        }

        inRange[i] = true;

        if (! last.has_value() || keyStart >= lane.keys[*last].start)
            last = i;
    }

    std::vector<KeyEntry> keys;
    keys.reserve (lane.keys.size());

    for (size_t i = 0; i < lane.keys.size(); ++i)
    {
        if (inRange[i] && i != last)
            continue;

        keys.push_back (lane.keys[i]);

        if (i == last)
            keys.back().start = start;
    }

    lane.keys = std::move (keys);
    setLane (song, lane);
}

void moveLaneTime (Song& song, BeatRange range, BeatPosition destination)
{
    const auto start = range.getStart(), end = range.getEnd();
    const auto length = range.getLength();
    const auto margin = BeatDuration::fromBeats (tiny);
    const auto later = destination >= end - margin;

    if (! hasChordLane (song) || range.isEmpty() || (destination > start + margin && destination < end - margin))
        return;

    // As moveTime() does: the chords are cut at the range's edges and the destination, then those in the range go to the
    // destination and those between it and the destination the other way.
    const auto changedStart = later ? start : destination;
    const auto changedEnd = later ? destination : end;
    const auto rangeShift = later ? destination - end : destination - start;
    const auto otherShift = later ? -length : length;
    Lane lane (getChordLaneValue (song));

    for (const auto beat : { start, end, destination })
    {
        // The chords as they are at this beat's turn, those cut at an earlier one too; not the pieces cut at this one.
        const auto count = lane.chords.size();

        for (size_t i = 0; i < count; ++i)
        {
            const auto r = getRange (lane.chords[i]);

            if (r.getStart() < beat - margin && r.getEnd() > beat + margin)
            {
                auto after = lane.chords[i];
                setRange (after, { beat, r.getEnd() });
                setRange (lane.chords[i], { r.getStart(), beat });
                lane.chords.push_back (after);
            }
        }
    }

    std::vector<ChordEntry> moved;
    moved.reserve (lane.chords.size());

    for (auto chord : lane.chords)
    {
        const auto chordStart = chord.start;

        if (chordStart >= changedStart - margin && chordStart < changedEnd - margin)
        {
            const auto inRange = chordStart >= start - margin && chordStart < end - margin;

            if (! moveChord (chord, inRange ? rangeShift : otherShift))
                continue; // pushed before the song's start: gone
        }

        moved.push_back (chord);
    }

    lane.chords = std::move (moved);
    setLane (song, lane);
}

LaneCopy copyLaneChords (const Song& song, BeatRange range)
{
    LaneCopy copy;
    copy.rangeStart = range.getStart();

    for (const auto& chord : getChordLaneValue (song).chords)
    {
        if (const auto r = getRange (chord).getIntersectionWith (range); r.getLength() > BeatDuration::fromBeats (tiny))
        {
            auto piece = chord;
            setRange (piece, r);
            copy.chords.push_back (piece);
        }
    }

    return copy;
}

void pasteLaneChords (Song& song, const LaneCopy& copy, BeatPosition destination)
{
    if (copy.chords.empty())
        return;

    auto lane = openLane (song);

    for (const auto& chord : copy.chords)
    {
        auto pasted = chord;
        const auto range = getRange (chord).movedBy (destination - copy.rangeStart);
        setRange (pasted, range);
        clearChords (lane.chords, range); // a chord the inserted time made longer is cut around it
        lane.chords.push_back (pasted);
    }

    setLane (song, lane);
}

//==============================================================================
AreaChoice chooseChordLaneClash (Song& song, const MergeClash& clash, int value, const MergeSides&)
{
    // A path into the song's CHORDLANE, which the song holds as a value beside its tree (model/base/internal/SongValues.h),
    // its tree's element an empty placeholder: the lane merges whole (SongMerge.cpp: mergesWhole()), so its clash is the
    // whole element's, its path the lane's alone. The side chosen's lane made the song's, or none where that side had none
    // (makeChordLaneAs(): its element added or taken out, its value set, in the choice's step). Refused (nothing written) for
    // any other clash on its path, which a merge of two songs Phazite holds never makes, but a commit's song may come from
    // anyone (tests/FuzzTests.cpp chooses every clash of a damaged merge): a property, an element in it, an element of
    // another type, and one that stands in for what its version couldn't read.
    if (clash.path.isEmpty() || ! clash.path[0].startsWith (schema::CHORDLANE.toString() + ":"))
        return AreaChoice::notTheArea;

    const juce::ValueTree* elements[] = { &clash.baseElement, &clash.oursElement, &clash.theirsElement };

    if (clash.property.isValid() || clash.path.size() != 1 || ! juce::isPositiveAndBelow (value, 3))
        return AreaChoice::refused;

    const auto& chosen = *elements[value];

    if (chosen.isValid() && (! chosen.hasType (schema::CHORDLANE) || holdsStandIn (chosen)))
        return AreaChoice::refused;

    makeChordLaneAs (song, chosen);
    return AreaChoice::chosen;
}
} // namespace phazite
