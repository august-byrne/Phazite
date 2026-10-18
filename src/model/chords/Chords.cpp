// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Chords.h"
#include "Progressions.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"

#include <algorithm>

namespace phazite
{
namespace
{
    // Positions this close count as the same.
    constexpr double tiny = 1.0e-6;

    juce::UndoManager* getUndo (te::Edit& edit)
    {
        return &edit.getUndoManager();
    }

    juce::ValueTree findLaneState (const te::Edit& edit)
    {
        return findPhaziteState (edit).getChildWithName (schema::CHORDLANE);
    }

    std::optional<Key> readKey (const juce::ValueTree& state)
    {
        if (! state.hasProperty (schema::phaziteKeyRoot))
            return std::nullopt;

        return Key { juce::jlimit (0, 11, (int) state[schema::phaziteKeyRoot]),
                     findScale (state[schema::phaziteKeyScale].toString()).value_or (Scale::major) };
    }

    void writeKey (juce::ValueTree state, const Key& key, juce::UndoManager* undo)
    {
        state.setProperty (schema::phaziteKeyRoot, key.root, undo);
        state.setProperty (schema::phaziteKeyScale, getSavedScaleName (key.scale), undo);
    }

    // The key before any is written: the one new tracks get, which a new project sets, or else C major.
    Key getDefaultKey (const te::Edit& edit)
    {
        return readKey (findPhaziteState (edit).getChildWithName (schema::LASTKEY)).value_or (Key {});
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
    juce::ValueTree findStartingAt (const te::Edit& edit, const juce::Identifier& type, BeatPosition start)
    {
        for (auto child : findLaneState (edit))
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

    int getBeatsPerBar (const te::Edit& edit, BeatPosition where)
    {
        return std::max (1, edit.tempoSequence.getTimeSigAt (toTracktion (where)).numerator.get());
    }

    //==============================================================================
    // Parts.

    // A clip's notes as plain values, in song beats.
    std::vector<NoteData> getClipNotes (const te::MidiClip& clip)
    {
        std::vector<NoteData> notes;

        for (auto* note : clip.getSequence().getNotes())
            notes.push_back (getNoteData (clip, *note));

        return notes;
    }

    // Whether two lists hold the same notes, in any order, whatever their IDs.
    bool areSameNotes (std::vector<NoteData> a, std::vector<NoteData> b)
    {
        if (a.size() != b.size())
            return false;

        const auto earlier = [] (const NoteData& x, const NoteData& y)
        {
            if (std::abs ((x.start - y.start).inBeats()) > tiny)
                return x.start < y.start;

            return x.pitch < y.pitch;
        };

        std::sort (a.begin(), a.end(), earlier);
        std::sort (b.begin(), b.end(), earlier);

        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].pitch != b[i].pitch || std::abs ((a[i].start - b[i].start).inBeats()) > 1.0e-4
                || std::abs ((a[i].length - b[i].length).inBeats()) > 1.0e-4 || a[i].velocity != b[i].velocity || a[i].muted != b[i].muted)
                return false;

        return true;
    }

    void setPartNotes (te::MidiClip& clip, const std::vector<NoteData>& notes)
    {
        auto* undo = getUndo (clip.edit);
        auto& sequence = clip.getSequence();
        sequence.removeAllNotes (undo);

        const auto contentStart = clip.getContentStartBeat();

        for (const auto& note : notes)
            sequence.addNote (note.pitch, te::BeatPosition::fromBeats ((toTracktion (note.start) - contentStart).inBeats()),
                              toTracktion (note.length), note.velocity, 0, undo);
    }

    // The sound of each kind of part on Phazite's synth.
    struct Setting
    {
        const char* id;
        float value;
    };

    // Chords: a soft pad of two detuned saws that swells in and fades out slowly.
    constexpr Setting padSound[] = { { "osc2Detune", 12.0f },     { "cutoff", 1800.0f }, { "resonance", 0.1f },
                                     { "filterEnvelope", 12.0f }, { "attack", 150.0f },  { "decay", 600.0f },
                                     { "sustain", 0.75f },        { "release", 900.0f }, { "level", -14.0f } };

    // Bass: one voice at a time, a sub under the saws, and a short filter pluck.
    constexpr Setting bassSound[] = { { "subLevel", 0.5f },        { "cutoff", 600.0f },      { "resonance", 0.2f },
                                      { "filterEnvelope", 18.0f }, { "filterDecay", 180.0f }, { "filterSustain", 0.15f },
                                      { "attack", 2.0f },          { "decay", 250.0f },       { "sustain", 0.7f },
                                      { "release", 120.0f },       { "voiceMode", 1.0f },     { "level", -10.0f } };

    // Arpeggio: the starter project's pluck.
    constexpr Setting pluckSound[] = { { "cutoff", 700.0f },      { "resonance", 0.25f },    { "filterEnvelope", 30.0f },
                                       { "filterDecay", 250.0f }, { "filterSustain", 0.1f }, { "decay", 400.0f },
                                       { "sustain", 0.4f },       { "release", 250.0f },     { "osc2Detune", 10.0f },
                                       { "level", -12.0f } };

    te::Plugin::Ptr createSynth (te::Edit& edit, PartKind kind)
    {
        auto synth = edit.getPluginCache().createNewPlugin (devices::Synth::typeID, {});

        if (auto* device = dynamic_cast<DevicePlugin*> (synth.get()))
        {
            const auto apply = [device] (const auto& settings)
            {
                for (const auto& setting : settings)
                    if (auto* parameter = device->findParameter (setting.id))
                        parameter->setParameter (setting.value, juce::sendNotification);
            };

            if (kind == PartKind::bass)
                apply (bassSound);
            else if (kind == PartKind::arpeggio)
                apply (pluckSound);
            else
                apply (padSound);
        }

        return synth;
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
std::vector<LaneKey> getLaneKeys (const te::Edit& edit)
{
    std::vector<LaneKey> keys;

    for (const auto& state : getChildren (findLaneState (edit), schema::CHORDKEY))
        if (const auto key = readKey (state))
            keys.push_back ({ getStart (state), *key });

    std::stable_sort (keys.begin(), keys.end(), [] (const LaneKey& a, const LaneKey& b) { return a.start < b.start; });

    if (keys.empty() || keys.front().start > BeatPosition::fromBeats (tiny))
        keys.insert (keys.begin(), { BeatPosition(), getDefaultKey (edit) });

    return keys;
}

Key getLaneKeyAt (const te::Edit& edit, BeatPosition where)
{
    const auto keys = getLaneKeys (edit);
    auto key = keys.front().key;

    for (const auto& change : keys)
        if (change.start <= where + BeatDuration::fromBeats (tiny))
            key = change.key;

    return key;
}

std::vector<PlacedChord> getLaneChords (const te::Edit& edit)
{
    const auto keys = getLaneKeys (edit);
    std::vector<PlacedChord> chords;

    for (const auto& state : getChildren (findLaneState (edit), schema::CHORD))
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

juce::ValueTree getLaneState (te::Edit& edit)
{
    auto phazite = getPhaziteState (edit);
    auto lane = phazite.getChildWithName (schema::CHORDLANE);

    if (! lane.isValid())
    {
        lane = juce::ValueTree (schema::CHORDLANE);
        juce::ValueTree key (schema::CHORDKEY);
        key.setProperty (schema::chordStart, 0.0, nullptr);
        writeKey (key, getDefaultKey (edit), nullptr);
        lane.appendChild (key, nullptr);
        phazite.appendChild (lane, getUndo (edit));
    }

    return lane;
}

//==============================================================================
void clearLaneRange (te::Edit& edit, BeatRange range)
{
    auto lane = findLaneState (edit);

    if (! lane.isValid() || range.isEmpty())
        return;

    auto* undo = getUndo (edit);
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

void putLaneChord (te::Edit& edit, BeatRange range, const Numeral& numeral)
{
    if (range.isEmpty())
        return;

    auto lane = getLaneState (edit);
    clearLaneRange (edit, range);
    lane.appendChild (makeChordState (range, numeral), getUndo (edit));
}

bool hasLaneChordAt (const te::Edit& edit, BeatPosition start)
{
    return findStartingAt (edit, schema::CHORD, start).isValid();
}

void setLaneChordNumeral (te::Edit& edit, BeatPosition start, const Numeral& numeral)
{
    if (auto chord = findStartingAt (edit, schema::CHORD, start); chord.isValid())
        writeNumeral (chord, numeral, getUndo (edit));
}

void removeLaneChord (te::Edit& edit, BeatPosition start)
{
    if (auto chord = findStartingAt (edit, schema::CHORD, start); chord.isValid())
        chord.getParent().removeChild (chord, getUndo (edit));
}

void setLaneChordEnd (te::Edit& edit, BeatPosition start, BeatPosition end)
{
    auto chord = findStartingAt (edit, schema::CHORD, start);

    if (! chord.isValid() || end <= start + BeatDuration::fromBeats (tiny))
        return;

    // A copy goes in its place once the chords it grows over have made room.
    auto lane = chord.getParent();
    auto resized = chord.createCopy();
    setRange (resized, { getStart (chord), end }, nullptr);
    lane.removeChild (chord, getUndo (edit));
    clearLaneRange (edit, { start, end });
    lane.appendChild (resized, getUndo (edit));
}

void setLaneKey (te::Edit& edit, BeatPosition from, const Key& key)
{
    auto lane = getLaneState (edit);

    // The key change at the beat, or a new one there.
    if (auto change = findStartingAt (edit, schema::CHORDKEY, from); change.isValid())
        return writeKey (change, key, getUndo (edit));

    juce::ValueTree change (schema::CHORDKEY);
    change.setProperty (schema::chordStart, std::max (0.0, from.inBeats()), nullptr);
    writeKey (change, key, nullptr);
    lane.appendChild (change, getUndo (edit));
}

void removeLaneKeyChange (te::Edit& edit, BeatPosition at)
{
    if (at <= BeatPosition::fromBeats (1.0e-4))
        return;

    if (auto change = findStartingAt (edit, schema::CHORDKEY, at); change.isValid())
        change.getParent().removeChild (change, getUndo (edit));
}

void putProgression (te::Edit& edit, const Progression& progression, BeatRange range)
{
    if (range.isEmpty() || progression.chords.empty())
        return;

    auto lane = getLaneState (edit);
    clearLaneRange (edit, range);

    auto beat = range.getStart();

    for (size_t i = 0; beat < range.getEnd() - BeatDuration::fromBeats (tiny); i = (i + 1) % progression.chords.size())
    {
        const auto& chord = progression.chords[i];
        const auto key = getLaneKeyAt (edit, beat);
        const auto root = getRootIn (chord.numeral, getKeyForProgression (key, progression.scale));
        const auto length = BeatDuration::fromBeats (chord.bars * getBeatsPerBar (edit, beat));
        const auto end = std::min (range.getEnd(), beat + length);

        lane.appendChild (makeChordState ({ beat, end }, toNumeral (root, chord.numeral.type, chord.numeral.inversion, key)),
                          getUndo (edit));
        beat = end;
    }
}

void changeLane (te::Edit& edit, const std::function<void()>& change)
{
    struct Part
    {
        juce::ReferenceCountedObjectPtr<te::MidiClip> clip;
        PartStyle style;
    };

    std::vector<Part> following;

    for (auto* track : te::getAudioTracks (edit))
        for (auto* clip : track->getClips())
            if (auto* midi = dynamic_cast<te::MidiClip*> (clip))
                if (const auto style = getChordPartStyle (*midi); style.has_value() && followsChords (*midi))
                    following.push_back ({ midi, *style });

    change();

    for (const auto& part : following)
        setPartNotes (*part.clip, makeChordPartNotes (*part.clip, part.style));
}

//==============================================================================
void insertLaneTime (te::Edit& edit, BeatPosition at, BeatDuration length)
{
    auto lane = findLaneState (edit);

    if (! lane.isValid() || length <= BeatDuration())
        return;

    auto* undo = getUndo (edit);
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

void removeLaneTime (te::Edit& edit, BeatRange range)
{
    auto lane = findLaneState (edit);

    if (! lane.isValid() || range.isEmpty())
        return;

    auto* undo = getUndo (edit);
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

void moveLaneTime (te::Edit& edit, BeatRange range, BeatPosition destination)
{
    auto lane = findLaneState (edit);
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
                setRange (chord, { r.getStart(), beat }, getUndo (edit));
                lane.appendChild (after, getUndo (edit));
            }
        }
    }

    for (auto chord : getChildren (lane, schema::CHORD))
    {
        const auto chordStart = getRange (chord).getStart();

        if (chordStart >= changedStart - margin && chordStart < changedEnd - margin)
        {
            const auto inRange = chordStart >= start - margin && chordStart < end - margin;
            moveChord (chord, inRange ? rangeShift : otherShift, getUndo (edit));
        }
    }
}

LaneCopy copyLaneChords (const te::Edit& edit, BeatRange range)
{
    LaneCopy copy;
    copy.rangeStart = range.getStart();

    for (const auto& chord : getChildren (findLaneState (edit), schema::CHORD))
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

void pasteLaneChords (te::Edit& edit, const LaneCopy& copy, BeatPosition destination)
{
    if (copy.chords.empty())
        return;

    auto lane = getLaneState (edit);

    for (const auto& chord : copy.chords)
    {
        auto pasted = chord.createCopy();
        const auto range = getRange (chord).movedBy (destination - copy.rangeStart);
        setRange (pasted, range, nullptr);
        clearLaneRange (edit, range); // a chord the inserted time made longer is cut around it
        lane.appendChild (pasted, getUndo (edit));
    }
}

//==============================================================================
std::optional<PartStyle> getChordPartStyle (const te::Clip& clip)
{
    return findPartStyle (clip.state[schema::phaziteChordPart].toString());
}

std::vector<NoteData> makeChordPartNotes (const te::MidiClip& clip, PartStyle style)
{
    const auto range = BeatRange (fromTracktion (clip.getStartBeat()), fromTracktion (clip.getEndBeat()));
    return makePartNotes (getLaneChords (clip.edit), style, range, getClipID (clip));
}

bool followsChords (const te::MidiClip& clip)
{
    const auto style = getChordPartStyle (clip);
    return style.has_value() && areSameNotes (getClipNotes (clip), makeChordPartNotes (clip, *style));
}

void setChordPart (te::MidiClip& clip, std::optional<PartStyle> style)
{
    auto* undo = getUndo (clip.edit);

    if (! style.has_value())
    {
        clip.state.removeProperty (schema::phaziteChordPart, undo);
        return;
    }

    clip.state.setProperty (schema::phaziteChordPart, getSavedPartStyleName (*style), undo);
    setPartNotes (clip, makeChordPartNotes (clip, *style));
}

te::MidiClip* addChordPart (te::Edit& edit, PartStyle style, BeatRange range, te::SelectionManager* selection)
{
    if (range.isEmpty())
        return nullptr;

    auto* track = insertTrack (edit, nullptr, selection);

    if (track == nullptr)
        return nullptr;

    const auto kind = getPartKind (style);
    track->setName (getPartKindName (kind));

    if (const auto colour = getColourForName (track->getName()))
        track->setColour (*colour);

    setInstrument (*track, createSynth (edit, kind));

    auto* clip = insertMidiClip (*track, toTracktion (range), nullptr);

    if (clip != nullptr)
        setChordPart (*clip, style);

    return clip;
}
} // namespace phazite
