// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Notes.h"
#include "devices/drums/DrumSampler.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace phazite
{
namespace
{
    // The scales Phazite offers, in the order its menus show them: the names projects save them by and menus show, and the
    // semitones of their notes above the root.
    struct SavedScale
    {
        Scale scale;
        const char* name;
        const char* shownName;
        std::array<int, 7> steps;
    };

    // clang-format off
    constexpr SavedScale savedScales[] = {
        { Scale::major,         "major",         "Major",          { 0, 2, 4, 5, 7, 9, 11 } },
        { Scale::minor,         "minor",         "Minor",          { 0, 2, 3, 5, 7, 8, 10 } },
        { Scale::dorian,        "dorian",        "Dorian",         { 0, 2, 3, 5, 7, 9, 10 } },
        { Scale::phrygian,      "phrygian",      "Phrygian",       { 0, 1, 3, 5, 7, 8, 10 } },
        { Scale::lydian,        "lydian",        "Lydian",         { 0, 2, 4, 6, 7, 9, 11 } },
        { Scale::mixolydian,    "mixolydian",    "Mixolydian",     { 0, 2, 4, 5, 7, 9, 10 } },
        { Scale::locrian,       "locrian",       "Locrian",        { 0, 1, 3, 5, 6, 8, 10 } },
        { Scale::harmonicMinor, "harmonicMinor", "Harmonic Minor", { 0, 2, 3, 5, 7, 8, 11 } },
        { Scale::melodicMinor,  "melodicMinor",  "Melodic Minor",  { 0, 2, 3, 5, 7, 9, 11 } }
    };
    // clang-format on

    const SavedScale& findSavedScale (Scale scale)
    {
        for (const auto& saved : savedScales)
            if (saved.scale == scale)
                return saved;

        return savedScales[0];
    }

    // The chords the piano roll stamps, with their notes' semitones above the root.
    struct ChordStamp
    {
        ChordType type;
        std::vector<int> intervals;
        const char* name;
    };

    // clang-format off
    const ChordStamp chords[] = { { ChordType::major,                 { 0, 4, 7 },          "Major" },
                                  { ChordType::minor,                 { 0, 3, 7 },          "Minor" },
                                  { ChordType::dominantSeventh,       { 0, 4, 7, 10 },      "7" },
                                  { ChordType::majorSeventh,          { 0, 4, 7, 11 },      "Maj7" },
                                  { ChordType::minorSeventh,          { 0, 3, 7, 10 },      "m7" },
                                  { ChordType::suspendedSecond,       { 0, 2, 7 },          "Sus2" },
                                  { ChordType::suspendedFourth,       { 0, 5, 7 },          "Sus4" },
                                  { ChordType::diminished,            { 0, 3, 6 },          "Dim" },
                                  { ChordType::augmented,             { 0, 4, 8 },          "Aug" },
                                  { ChordType::majorSixth,            { 0, 4, 7, 9 },       "6" },
                                  { ChordType::minorSixth,            { 0, 3, 7, 9 },       "m6" },
                                  { ChordType::dominantNinth,         { 0, 4, 7, 10, 14 },  "9" },
                                  { ChordType::majorNinth,            { 0, 4, 7, 11, 14 },  "Maj9" },
                                  { ChordType::power,                 { 0, 7 },             "Power" },
                                  { ChordType::halfDiminishedSeventh, { 0, 3, 6, 10 },      "m7b5" },
                                  { ChordType::diminishedSeventh,     { 0, 3, 6, 9 },       "Dim7" },
                                  { ChordType::minorNinth,            { 0, 3, 7, 10, 14 },  "m9" } };
    // clang-format on

    // A key, on a track's element or on LASTKEY in the song's Phazite state (model/base/internal/Schema.h). A key exists exactly when
    // its root does.
    std::optional<Key> readKey (const juce::ValueTree& state)
    {
        if (! state.hasProperty (schema::keyRoot))
            return std::nullopt;

        return Key { juce::jlimit (0, 11, (int) state[schema::keyRoot]),
                     findScale (state[schema::keyScale].toString()).value_or (Scale::major) };
    }

    void writeKey (juce::ValueTree state, const std::optional<Key>& key, juce::UndoManager* undo)
    {
        if (! key.has_value())
        {
            state.removeProperty (schema::keyRoot, undo);
            state.removeProperty (schema::keyScale, undo);
            return;
        }

        state.setProperty (schema::keyRoot, key->root, undo);
        state.setProperty (schema::keyScale, getSavedScaleName (key->scale), undo);
    }

    const auto tinyBeats = BeatDuration::fromBeats (1.0e-6);
    constexpr int maxPiecesPerNote = 256; // chopping into tinier pieces than this makes no musical sense

    // A MIDI clip's element, or invalid.
    juce::ValueTree findMidiClipState (const Song& song, ClipID clip)
    {
        const auto clipState = findClipState (song, clip);
        return isMidiState (clipState) ? clipState : juce::ValueTree();
    }

    // A song beat as a position among the clip's notes, which count from its content's start.
    BeatPosition toClipBeat (const juce::ValueTree& clipState, BeatPosition songBeat)
    {
        return BeatPosition::fromBeats ((songBeat - getContentStart (clipState)).inBeats());
    }

    //==============================================================================
    // The song's notes: each a NOTE in its clip's element (Schema.h), and in each alias's, with the same ID.

    juce::ValueTree findNoteIn (const juce::ValueTree& clipState, NoteID id)
    {
        for (const auto& child : clipState)
            if (child.hasType (schema::NOTE) && (juce::int64) child[schema::noteID] == (juce::int64) id.getRaw())
                return child;

        return {};
    }

    NoteID getNoteID (const juce::ValueTree& noteState)
    {
        return NoteID::fromRaw ((std::uint64_t) std::max<juce::int64> (0, (juce::int64) noteState[schema::noteID]));
    }

    NoteData readNote (const juce::ValueTree& clipState, const juce::ValueTree& noteState)
    {
        return { getClipID (clipState),
                 getNoteID (noteState),
                 (int) noteState[schema::notePitch],
                 getContentStart (clipState) + BeatDuration::fromBeats (noteState[schema::noteStart]),
                 BeatDuration::fromBeats (noteState[schema::noteLength]),
                 (int) noteState[schema::noteVelocity],
                 (bool) noteState[schema::noteMute],
                 (bool) noteState[schema::noteSlide],
                 (bool) noteState[schema::notePortamento] };
    }

    // Changes the note with an ID in a clip, and in each of its aliases, which hold the same notes.
    void changeNote (Song& song, const NoteRef& ref, const std::function<void (juce::ValueTree&, juce::UndoManager*)>& change)
    {
        const auto clipState = findMidiClipState (song, ref.clip);

        if (! clipState.isValid())
            return;

        const auto aliases = getAliasStates (clipState);

        for (const auto& alias : aliases)
            if (auto note = findNoteIn (alias, ref.note); note.isValid())
                change (note, &song.getUndoManager());
    }

    // What a note is besides its place, pitch and velocity: muted, a slide note or a portamento note, each written on its
    // element only while it is (Schema.h).
    struct NoteFlags
    {
        bool muted = false, slide = false, portamento = false;
    };

    NoteFlags getFlags (const NoteData& data)
    {
        return { data.muted, data.slide, data.portamento };
    }

    void writeFlags (juce::ValueTree& note, NoteFlags flags)
    {
        for (const auto& [property, on] : { std::pair { &schema::noteMute, flags.muted }, std::pair { &schema::noteSlide, flags.slide },
                                            std::pair { &schema::notePortamento, flags.portamento } })
            if (on)
                note.setProperty (*property, true, nullptr);
    }

    // Adds a note to a clip and to each of its aliases, at a beat among its notes, with the ID wanted if no note of the clip
    // has it, or else a new one of the song's. Returns its ID, or none if the clip isn't the song's.
    NoteID addNoteState (Song& song, const juce::ValueTree& clipState, NoteID wanted, int pitch, BeatPosition start, BeatDuration length,
                         int velocity, NoteFlags flags)
    {
        if (! clipState.isValid())
            return {};

        const auto id =
            wanted.isValid() && ! findNoteIn (clipState, wanted).isValid() ? (juce::int64) wanted.getRaw() : (juce::int64) song.makeID();

        const auto aliases = getAliasStates (clipState);

        for (auto alias : aliases)
        {
            juce::ValueTree note (schema::NOTE);
            note.setProperty (schema::noteID, id, nullptr);
            note.setProperty (schema::notePitch, pitch, nullptr);
            note.setProperty (schema::noteStart, start.inBeats(), nullptr);
            note.setProperty (schema::noteLength, length.inBeats(), nullptr);
            note.setProperty (schema::noteVelocity, velocity, nullptr);
            writeFlags (note, flags);
            alias.appendChild (note, &song.getUndoManager());
        }

        return NoteID::fromRaw ((std::uint64_t) id);
    }

    void removeNoteState (Song& song, const NoteRef& ref)
    {
        const auto clipState = findMidiClipState (song, ref.clip);

        if (! clipState.isValid())
            return;

        const auto aliases = getAliasStates (clipState);

        for (auto alias : aliases)
            if (const auto note = findNoteIn (alias, ref.note); note.isValid())
                alias.removeChild (note, &song.getUndoManager());
    }

    // The notes there, each once: of a note in a clip and its copies in the clip's aliases, the first one, since an edit to it
    // is made to each of them.
    std::vector<NoteRef> withoutAliasCopies (const Song& song, const std::vector<NoteRef>& notes)
    {
        std::vector<NoteRef> unique;
        std::set<std::pair<juce::String, juce::int64>> seen;

        for (const auto& ref : notes)
        {
            const auto clipState = findMidiClipState (song, ref.clip);

            if (! findNoteIn (clipState, ref.note).isValid())
                continue;

            const auto group = clipState[schema::clipLink].toString();
            const auto key = group.isNotEmpty() ? group : "clip " + juce::String ((juce::int64) ref.clip.getRaw());

            if (seen.insert ({ key, (juce::int64) ref.note.getRaw() }).second)
                unique.push_back (ref);
        }

        return unique;
    }

    // Where a clip can grow to without covering another clip on its track: the start of the first clip that reaches past
    // its end, or its own end if another clip already covers that. Nothing if nothing on the track reaches past its end.
    std::optional<BeatPosition> findRoomEnd (const Song& song, const juce::ValueTree& clipState)
    {
        const auto end = getClipRange (song, clipState).getEnd();
        std::optional<BeatPosition> roomEnd;

        for (const auto& other : getClipStates (clipState.getParent()))
        {
            const auto range = getClipRange (song, other);

            if (other == clipState || range.getEnd() <= end + tinyBeats)
                continue;

            const auto covered = std::max (range.getStart(), end);
            roomEnd = roomEnd.has_value() ? std::min (*roomEnd, covered) : covered;
        }

        return roomEnd;
    }

    // Makes the clip end at the end of the bar a song beat is in, if the clip ends before the beat, but only into the
    // empty space after it: never past the start of the next clip on its track, so that a note edit never makes one clip
    // cover another.
    void growToHold (Song& song, const juce::ValueTree& clipState, BeatPosition end)
    {
        const auto range = getClipRange (clipState);

        if (end <= range.getEnd() + tinyBeats)
            return;

        const auto bars = getBars (song);
        auto newEnd = bars.getBarStart (bars.getBarAt (end - tinyBeats) + 1);

        if (const auto roomEnd = findRoomEnd (song, clipState))
            newEnd = std::min (newEnd, *roomEnd);

        if (newEnd > range.getEnd() + tinyBeats)
            setClipPlace (song, clipState, range.getStart(), newEnd - range.getStart(), getClipOffset (clipState));
    }

    int clampPitch (int pitch)
    {
        return juce::jlimit (0, 127, pitch);
    }

    // Integer division and remainder that round down, so that they also work below 0.
    int floorDivide (int a, int b)
    {
        return a >= 0 ? a / b : -((-a + b - 1) / b);
    }

    int floorModulo (int a, int b)
    {
        return a - b * floorDivide (a, b);
    }

    std::vector<int> parseNotes (const juce::var& value)
    {
        std::vector<int> notes;

        for (const auto& token : juce::StringArray::fromTokens (value.toString(), ",", {}))
            if (token.trim().isNotEmpty())
                notes.push_back (token.getIntValue());

        return notes;
    }

    juce::String joinNotes (const std::vector<int>& notes)
    {
        juce::StringArray tokens;

        for (const auto note : notes)
            tokens.add (juce::String (note));

        return tokens.joinIntoString (",");
    }
} // namespace

//==============================================================================
juce::ValueTree findNoteState (const Song& song, const NoteRef& ref)
{
    return findNoteIn (findMidiClipState (song, ref.clip), ref.note);
}

std::optional<NoteData> getNoteData (const Song& song, const NoteRef& ref)
{
    const auto clipState = findMidiClipState (song, ref.clip);

    if (const auto note = findNoteIn (clipState, ref.note); note.isValid())
        return readNote (clipState, note);

    return std::nullopt;
}

std::vector<NoteData> getNoteData (const Song& song, const std::vector<NoteRef>& notes)
{
    std::vector<NoteData> data;

    for (const auto& ref : notes)
        if (const auto note = getNoteData (song, ref))
            data.push_back (*note);

    return data;
}

std::vector<NoteData> getClipNotes (const Song& song, ClipID clip)
{
    std::vector<NoteData> data;
    const auto clipState = findMidiClipState (song, clip);

    for (const auto& child : clipState)
        if (child.hasType (schema::NOTE))
            data.push_back (readNote (clipState, child));

    return data;
}

BeatDuration getMinimumNoteLength()
{
    return BeatDuration::fromBeats (1.0 / 64.0);
}

//==============================================================================
std::vector<NoteRef> getSelectedNotes (const Selection& selection)
{
    std::vector<NoteRef> notes;
    auto* song = selection.getSong();

    if (song == nullptr || selection.getNotes().empty())
        return notes;

    // Those still there, of the note clips, in the order they were selected.
    for (const auto& ref : selection.getNotes())
        if (std::find (selection.getNoteClips().begin(), selection.getNoteClips().end(), ref.clip) != selection.getNoteClips().end())
            if (findNoteState (*song, ref).isValid())
                notes.push_back (ref);

    return notes;
}

void selectNotes (Selection& selection, const std::vector<NoteRef>& notes)
{
    selection.selectNotes (notes);
}

bool hasSelectedNotes (const Selection& selection)
{
    return ! getSelectedNotes (selection).empty();
}

//==============================================================================
namespace
{
    // Adds a note at a song beat, as addNote() says, with its flags, with the ID wanted if no note of the clip has it.
    NoteRef addNoteTo (Song& song, ClipID clip, NoteID wanted, int pitch, BeatPosition start, BeatDuration length, int velocity,
                       NoteFlags flags)
    {
        const auto clipState = findMidiClipState (song, clip);

        if (! clipState.isValid())
            return {};

        start = std::max (start, getClipRange (clipState).getStart());
        length = std::max (length, getMinimumNoteLength());
        growToHold (song, clipState, start + length);
        const auto id = addNoteState (song, clipState, wanted, clampPitch (pitch), toClipBeat (clipState, start), length,
                                      juce::jlimit (1, 127, velocity), flags);
        return id.isValid() ? NoteRef { clip, id } : NoteRef {};
    }
} // namespace

NoteRef addNote (Song& song, ClipID clip, int pitch, BeatPosition start, BeatDuration length, int velocity)
{
    return addNoteTo (song, clip, {}, pitch, start, length, velocity, {});
}

void moveNote (Song& song, const NoteRef& ref, BeatPosition start, int pitch)
{
    const auto clipState = findMidiClipState (song, ref.clip);
    const auto note = getNoteData (song, ref);

    if (! note.has_value())
        return;

    start = std::max (start, getClipRange (clipState).getStart());
    growToHold (song, clipState, start + note->length);
    const auto clipBeat = toClipBeat (clipState, start);
    changeNote (song, ref,
                [clipBeat, pitch] (juce::ValueTree& n, juce::UndoManager* undo)
                {
                    n.setProperty (schema::noteStart, clipBeat.inBeats(), undo);
                    n.setProperty (schema::notePitch, clampPitch (pitch), undo);
                });
}

void setNoteLength (Song& song, const NoteRef& ref, BeatDuration length)
{
    const auto note = getNoteData (song, ref);

    if (! note.has_value())
        return;

    length = std::max (length, getMinimumNoteLength());
    growToHold (song, findMidiClipState (song, ref.clip), note->start + length);
    changeNote (song, ref,
                [length] (juce::ValueTree& n, juce::UndoManager* undo) { n.setProperty (schema::noteLength, length.inBeats(), undo); });
}

void deleteNotes (Song& song, const std::vector<NoteRef>& notes)
{
    // Each by its ID, all found before any goes; a note's copies in its clip's aliases go with it.
    for (const auto& ref : withoutAliasCopies (song, notes))
        removeNoteState (song, ref);
}

void transposeNotes (Song& song, const std::vector<NoteRef>& notes, int semitones)
{
    const auto unique = getNoteData (song, withoutAliasCopies (song, notes));

    // As far as the highest or lowest note can go, so that chords keep their shape.
    for (const auto& note : unique)
        semitones = juce::jlimit (-note.pitch, 127 - note.pitch, semitones);

    if (semitones == 0)
        return;

    for (const auto& note : unique)
    {
        const auto pitch = note.pitch + semitones;
        changeNote (song, { note.clip, note.id },
                    [pitch] (juce::ValueTree& n, juce::UndoManager* undo) { n.setProperty (schema::notePitch, pitch, undo); });
    }
}

void nudgeNotes (Song& song, const std::vector<NoteRef>& notes, BeatDuration amount)
{
    const auto unique = getNoteData (song, withoutAliasCopies (song, notes));

    // The note nearest its clip's start decides how far back they can go.
    for (const auto& note : unique)
        amount = std::max (amount, getClipRange (findMidiClipState (song, note.clip)).getStart() - note.start);

    for (const auto& note : unique)
        moveNote (song, { note.clip, note.id }, note.start + amount, note.pitch);
}

void changeNoteLengths (Song& song, const std::vector<NoteRef>& notes, BeatDuration change)
{
    for (const auto& note : getNoteData (song, withoutAliasCopies (song, notes)))
        setNoteLength (song, { note.clip, note.id }, note.length + change);
}

bool areAllNotesMuted (const Song& song, const std::vector<NoteRef>& notes)
{
    return areAllOn (notes, [&song] (const NoteRef& ref) { return (bool) findNoteState (song, ref)[schema::noteMute]; });
}

namespace
{
    bool hasFlag (const Song& song, const NoteRef& ref, const juce::Identifier& flag)
    {
        return (bool) findNoteState (song, ref)[flag];
    }

    void toggleNoteFlags (Song& song, const std::vector<NoteRef>& notes, const juce::Identifier& flag)
    {
        toggleAll (
            withoutAliasCopies (song, notes), [&song, &flag] (const NoteRef& ref) { return hasFlag (song, ref, flag); },
            [&song, &flag] (const NoteRef& ref, bool on)
            {
                changeNote (song, ref,
                            [on, &flag] (juce::ValueTree& note, juce::UndoManager* undo)
                            {
                                if (on)
                                    note.setProperty (flag, true, undo);
                                else
                                    note.removeProperty (flag, undo);
                            });
            });
    }
} // namespace

void toggleNotesMuted (Song& song, const std::vector<NoteRef>& notes)
{
    toggleNoteFlags (song, notes, schema::noteMute);
}

bool areAllNotesSlides (const Song& song, const std::vector<NoteRef>& notes)
{
    return areAllOn (notes, [&song] (const NoteRef& ref) { return hasFlag (song, ref, schema::noteSlide); });
}

void toggleNotesSlide (Song& song, const std::vector<NoteRef>& notes)
{
    toggleNoteFlags (song, notes, schema::noteSlide);
}

bool areAllNotesPortamento (const Song& song, const std::vector<NoteRef>& notes)
{
    return areAllOn (notes, [&song] (const NoteRef& ref) { return hasFlag (song, ref, schema::notePortamento); });
}

void toggleNotesPortamento (Song& song, const std::vector<NoteRef>& notes)
{
    toggleNoteFlags (song, notes, schema::notePortamento);
}

void setNoteVelocities (Song& song, const std::vector<NoteRef>& notes, int velocity)
{
    const auto clamped = juce::jlimit (1, 127, velocity);

    for (const auto& ref : withoutAliasCopies (song, notes))
        changeNote (song, ref,
                    [clamped] (juce::ValueTree& note, juce::UndoManager* undo) { note.setProperty (schema::noteVelocity, clamped, undo); });
}

void changeNoteVelocities (Song& song, const std::vector<NoteRef>& notes, int change)
{
    if (notes.empty())
        return;

    const UndoStep step (song, "Change Velocity");

    for (const auto& note : getNoteData (song, withoutAliasCopies (song, notes)))
        setNoteVelocities (song, { { note.clip, note.id } }, note.velocity + change);
}

std::vector<NoteRef> duplicateNotes (Song& song, const std::vector<NoteRef>& notes)
{
    const auto unique = getNoteData (song, withoutAliasCopies (song, notes));

    if (unique.empty())
        return {};

    // The copies go right after the whole selection.
    auto start = unique.front().start, end = start;

    for (const auto& note : unique)
    {
        start = std::min (start, note.start);
        end = std::max (end, note.getEnd());
    }

    auto copies = unique;

    for (auto& copy : copies)
        copy.start = copy.start + (end - start);

    return replaceNotes (song, {}, copies);
}

void setClipNotes (Song& song, ClipID clip, const std::vector<NoteData>& notes)
{
    const auto clipState = findMidiClipState (song, clip);

    if (! clipState.isValid())
        return;

    auto* undo = &song.getUndoManager();
    const auto contentStart = getContentStart (clipState);
    const auto aliases = getAliasStates (clipState);
    std::vector<juce::int64> ids;

    for (size_t i = 0; i < notes.size(); ++i)
        ids.push_back ((juce::int64) song.makeID());

    for (auto alias : aliases)
    {
        for (int i = alias.getNumChildren(); --i >= 0;)
            if (alias.getChild (i).hasType (schema::NOTE))
                alias.removeChild (i, undo);

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& data = notes[i];
            juce::ValueTree note (schema::NOTE);
            note.setProperty (schema::noteID, ids[i], nullptr);
            note.setProperty (schema::notePitch, clampPitch (data.pitch), nullptr);
            note.setProperty (schema::noteStart, (data.start - contentStart).inBeats(), nullptr);
            note.setProperty (schema::noteLength, data.length.inBeats(), nullptr);
            note.setProperty (schema::noteVelocity, juce::jlimit (1, 127, data.velocity), nullptr);
            writeFlags (note, getFlags (data));
            alias.appendChild (note, undo);
        }
    }
}

std::vector<NoteRef> replaceNotes (Song& song, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements)
{
    deleteNotes (song, old);
    std::vector<NoteRef> added;

    // A note that replaces one of the old ones (a quantized or transposed note, a chopped note's first piece) keeps that one's
    // ID, if no other note of the clip has it now; a copy, whose original is still there, gets its own.
    for (const auto& data : replacements)
        if (const auto note = addNoteTo (song, data.clip, data.id, data.pitch, data.start, data.length, data.velocity, getFlags (data));
            note.clip.isValid())
            added.push_back (note);

    return added;
}

std::vector<NoteRef> replaceNotes (Song& song, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements,
                                   const juce::String& undoName)
{
    if (old.empty() && replacements.empty())
        return {};

    const UndoStep step (song, undoName);
    return replaceNotes (song, old, replacements);
}

std::vector<NoteRef> replaceSelectedNotes (Selection& selection,
                                           const std::function<std::vector<NoteData> (std::vector<NoteData>)>& transform)
{
    auto* song = selection.getSong();

    if (song == nullptr)
        return {};

    const auto notes = withoutAliasCopies (*song, getSelectedNotes (selection));

    if (notes.empty())
        return {};

    auto added = replaceNotes (*song, notes, transform (getNoteData (*song, notes)));
    selectNotes (selection, added);
    return added;
}

//==============================================================================
std::vector<NoteData> quantize (std::vector<NoteData> notes, const QuantizeSettings& settings)
{
    const auto grid = settings.gridBeats;
    const auto strength = (double) juce::jlimit (0.0f, 1.0f, settings.strength);
    const auto swing = (double) juce::jlimit (0.0f, 1.0f, settings.swing);

    if (grid <= 0.0)
        return notes;

    // The nearest grid line, with every other one late by up to a third of a step: at full swing, the second
    // of each pair of steps lands two-thirds of the way through the pair, as in a triplet.
    const auto quantizeBeat = [=] (double beat)
    {
        const auto index = std::round (beat / grid);
        auto target = index * grid;

        if (std::fmod (std::abs (index), 2.0) > 0.5)
            target += swing * grid / 3.0;

        return std::max (0.0, beat + (target - beat) * strength);
    };

    for (auto& note : notes)
    {
        const auto end = note.getEnd().inBeats();
        note.start = BeatPosition::fromBeats (quantizeBeat (note.start.inBeats()));

        if (settings.ends)
            note.length = std::max (getMinimumNoteLength(), BeatDuration::fromBeats (quantizeBeat (end) - note.start.inBeats()));
    }

    return notes;
}

std::vector<NoteData> legato (std::vector<NoteData> notes, double gapBeats)
{
    // The starts in each clip, in order; a note lasts until the next start after its own.
    std::map<ClipID, std::vector<double>> starts;

    for (const auto& note : notes)
        starts[note.clip].push_back (note.start.inBeats());

    for (auto& [clip, beats] : starts)
        std::sort (beats.begin(), beats.end());

    for (auto& note : notes)
    {
        const auto& clipStarts = starts[note.clip];
        const auto next = std::upper_bound (clipStarts.begin(), clipStarts.end(), note.start.inBeats() + 1.0e-6);

        if (next != clipStarts.end())
            note.length = std::max (getMinimumNoteLength(), BeatDuration::fromBeats (*next - note.start.inBeats() - gapBeats));
    }

    return notes;
}

std::vector<NoteData> chop (std::vector<NoteData> notes, double pieceBeats)
{
    if (pieceBeats <= getMinimumNoteLength().inBeats() / 2.0)
        return notes;

    std::vector<NoteData> pieces;

    for (const auto& note : notes)
    {
        const auto length = note.length.inBeats();
        auto offset = 0.0;

        for (int i = 0; offset < length - 1.0e-6 && i < maxPiecesPerNote; ++i, offset += pieceBeats)
        {
            auto piece = note; // the first piece replaces the note (replaceNotes() keeps its ID), and the rest are new
            piece.start = note.start + BeatDuration::fromBeats (offset);
            piece.length = BeatDuration::fromBeats (std::min (pieceBeats, length - offset));
            pieces.push_back (piece);
        }
    }

    return pieces;
}

//==============================================================================
juce::String getScaleName (Scale scale)
{
    return findSavedScale (scale).shownName;
}

juce::String describeKey (const Key& key)
{
    return juce::MidiMessage::getMidiNoteName (key.root, true, false, 0) + " " + getScaleName (key.scale).toLowerCase();
}

std::vector<int> getScaleSteps (Scale scale)
{
    const auto& steps = findSavedScale (scale).steps;
    return { steps.begin(), steps.end() };
}

bool isInKey (int pitch, const Key& key)
{
    const auto steps = getScaleSteps (key.scale);
    return std::find (steps.begin(), steps.end(), floorModulo (pitch - key.root, 12)) != steps.end();
}

const std::vector<Scale>& getKeyScales()
{
    static const auto scales = []
    {
        std::vector<Scale> all;

        for (const auto& saved : savedScales)
            all.push_back (saved.scale);

        return all;
    }();

    return scales;
}

juce::String getSavedScaleName (Scale scale)
{
    return findSavedScale (scale).name;
}

std::optional<Scale> findScale (const juce::String& savedName)
{
    for (const auto& saved : savedScales)
        if (savedName == saved.name)
            return saved.scale;

    return std::nullopt;
}

std::optional<Key> getTrackKey (const Song& song, TrackID track)
{
    return readKey (findTrackState (song, track));
}

void setTrackKey (Song& song, TrackID track, const std::optional<Key>& key)
{
    writeKey (findTrackState (song, track), key, &song.getUndoManager());

    // What new tracks get; not an edit to undo.
    if (key.has_value())
        writeKey (song.getState().getOrCreateChildWithName (schema::LASTKEY, nullptr), key, nullptr);
}

void setTrackKeys (Song& song, const std::vector<TrackID>& tracks, const std::optional<Key>& key)
{
    for (const auto track : tracks)
        setTrackKey (song, track, key);
}

void chooseTrackKey (Song& song, TrackID track, const std::optional<Key>& key)
{
    const UndoStep step (song, key.has_value() ? "Set Key" : "Turn Key Off");
    setTrackKey (song, track, key);
}

void applyLastKey (Song& song, TrackID track)
{
    if (const auto last = readKey (song.getState().getChildWithName (schema::LASTKEY)))
        writeKey (findTrackState (song, track), last, nullptr);
}

void setLastKey (Song& song, const Key& key)
{
    writeKey (song.getState().getOrCreateChildWithName (schema::LASTKEY, nullptr), key, nullptr);
}

int snapToKey (int pitch, const Key& key)
{
    for (int distance = 0; distance < 12; ++distance)
    {
        if (isInKey (pitch - distance, key))
            return pitch - distance;

        if (isInKey (pitch + distance, key))
            return pitch + distance;
    }

    return pitch;
}

std::vector<NoteData> transpose (std::vector<NoteData> notes, int steps, const std::optional<Key>& diatonicIn)
{
    for (auto& note : notes)
    {
        if (! diatonicIn.has_value())
        {
            note.pitch = clampPitch (note.pitch + steps);
            continue;
        }

        // Count scale steps: from the note's degree, octaves wrap around the scale.
        const auto& key = *diatonicIn;
        const auto scale = getScaleSteps (key.scale);
        const auto size = (int) scale.size();
        const auto inKey = snapToKey (note.pitch, key);
        const auto fromRoot = inKey - key.root;
        const auto degree = (int) (std::find (scale.begin(), scale.end(), floorModulo (fromRoot, 12)) - scale.begin());
        const auto target = degree + steps;
        const auto octave = floorDivide (fromRoot, 12) + floorDivide (target, size);
        note.pitch = clampPitch (key.root + 12 * octave + scale[(size_t) floorModulo (target, size)]);
    }

    return notes;
}

//==============================================================================
const std::vector<ChordShape>& getChordShapes()
{
    static const auto shapes = []
    {
        std::vector<ChordShape> all;

        for (const auto& chord : chords)
            all.push_back ({ chord.name, chord.type });

        return all;
    }();

    return shapes;
}

std::vector<int> getChordIntervals (ChordType type)
{
    for (const auto& chord : chords)
        if (chord.type == type)
            return chord.intervals;

    return { 0, 4, 7 };
}

std::vector<int> getChordPitches (int root, ChordType type)
{
    std::vector<int> pitches;

    for (const auto step : getChordIntervals (type))
        if (juce::isPositiveAndBelow (root + step, 128))
            pitches.push_back (root + step);

    return pitches;
}

//==============================================================================
juce::String getDrumName (int note)
{
    // Producers' names for the drums used most, and General MIDI's for the rest.
    struct Name
    {
        int note;
        const char* name;
    };

    static constexpr Name shortNames[] = { { 35, "Kick 2" },   { 36, "Kick" },    { 37, "Rim" },        { 38, "Snare" },
                                           { 39, "Clap" },     { 40, "Snare 2" }, { 42, "Closed Hat" }, { 44, "Pedal Hat" },
                                           { 46, "Open Hat" }, { 49, "Crash" },   { 51, "Ride" } };

    for (const auto& entry : shortNames)
        if (entry.note == note)
            return entry.name;

    if (note >= 35 && note <= 81)
        return juce::MidiMessage::getRhythmInstrumentName (note);

    return juce::MidiMessage::getMidiNoteName (note, true, true, 4);
}

bool isDrumTrack (const Song& song, TrackID track)
{
    const auto state = findTrackState (song, track);

    if (state.hasProperty (schema::drumMode))
        return state[schema::drumMode];

    for (const auto& device : getDeviceStates (state))
        if (isInstrumentState (device))
            return isDrumSampler (device);

    return false;
}

void setDrumTrack (Song& song, TrackID track, bool isDrums)
{
    if (auto state = findTrackState (song, track); state.isValid())
        state.setProperty (schema::drumMode, isDrums, nullptr);
}

std::optional<std::vector<DrumRow>> getSamplerSounds (const Song& song, TrackID track)
{
    // A row for each of the drum sampler's pads.
    for (const auto& device : getDeviceStates (findTrackState (song, track)))
    {
        if (isDrumSampler (device))
        {
            std::vector<DrumRow> pads;

            for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
                pads.push_back ({ devices::DrumSampler::firstNote + pad, getPadName (song, device, pad) });

            return pads;
        }
    }

    return std::nullopt;
}

std::vector<int> getNoteNumbersPlayed (const Song& song, const std::vector<ClipID>& clips)
{
    std::set<int> played;

    for (const auto clip : clips)
        for (const auto& note : getClipNotes (song, clip))
            played.insert (note.pitch);

    return std::vector<int> (played.begin(), played.end());
}

std::vector<int> getDrumRowOrder (const Song& song, TrackID track)
{
    return parseNotes (findTrackState (song, track)[schema::drumOrder]);
}

void setDrumRowOrder (Song& song, TrackID track, const std::vector<int>& notes)
{
    if (auto state = findTrackState (song, track); state.isValid())
        state.setProperty (schema::drumOrder, joinNotes (notes), nullptr);
}

std::vector<int> getHiddenDrumRows (const Song& song, TrackID track)
{
    return parseNotes (findTrackState (song, track)[schema::drumHidden]);
}

void hideDrumRow (Song& song, TrackID track, int note)
{
    auto hidden = getHiddenDrumRows (song, track);

    if (std::find (hidden.begin(), hidden.end(), note) == hidden.end())
        hidden.push_back (note);

    if (auto state = findTrackState (song, track); state.isValid())
        state.setProperty (schema::drumHidden, joinNotes (hidden), nullptr);
}

void showAllDrumRows (Song& song, TrackID track)
{
    if (auto state = findTrackState (song, track); state.isValid())
        state.removeProperty (schema::drumHidden, nullptr);
}

//==============================================================================
void addNoteCommands (CommandRegistry& registry, Selection& selection, std::function<double()> getGridStepInBeats)
{
    const auto undoTarget = undoTargetOf (selection);

    struct Simple
    {
        const char* id;
        const char* name;
        const char* description;
        juce::StringArray synonyms;
        std::function<void (Song&, const std::vector<NoteRef>&)> action;
    };

    const auto gridStep = [getGridStepInBeats] { return BeatDuration::fromBeats (getGridStepInBeats()); };

    for (auto& simple :
         { Simple { "note.transposeUp",
                    "Transpose Up",
                    "Moves the selected notes up a semitone.",
                    { "pitch up", "semitone", "raise" },
                    [] (Song& song, const std::vector<NoteRef>& notes) { transposeNotes (song, notes, 1); } },
           Simple { "note.transposeDown",
                    "Transpose Down",
                    "Moves the selected notes down a semitone.",
                    { "pitch down", "semitone", "lower" },
                    [] (Song& song, const std::vector<NoteRef>& notes) { transposeNotes (song, notes, -1); } },
           Simple { "note.octaveUp",
                    "Octave Up",
                    "Moves the selected notes up an octave.",
                    { "transpose", "pitch up", "raise" },
                    [] (Song& song, const std::vector<NoteRef>& notes) { transposeNotes (song, notes, 12); } },
           Simple { "note.octaveDown",
                    "Octave Down",
                    "Moves the selected notes down an octave.",
                    { "transpose", "pitch down", "lower" },
                    [] (Song& song, const std::vector<NoteRef>& notes) { transposeNotes (song, notes, -12); } },
           Simple { "note.lengthen",
                    "Lengthen Notes",
                    "Makes the selected notes a step of the grid longer.",
                    { "longer", "extend", "length" },
                    [gridStep] (Song& song, const std::vector<NoteRef>& notes) { changeNoteLengths (song, notes, gridStep()); } },
           Simple { "note.shorten",
                    "Shorten Notes",
                    "Makes the selected notes a step of the grid shorter.",
                    { "shorter", "length" },
                    [gridStep] (Song& song, const std::vector<NoteRef>& notes)
                    { changeNoteLengths (song, notes, BeatDuration() - gridStep()); } } })
    {
        registry.add ({ .id = simple.id,
                        .name = simple.name,
                        .category = "Note",
                        .description = simple.description,
                        .synonyms = simple.synonyms,
                        .perform = withSong (selection, [&selection, action = simple.action] (Song& song)
                                             { action (song, getSelectedNotes (selection)); }),
                        .isEnabled = [&selection] { return hasSelectedNotes (selection); },
                        .undoTarget = undoTarget,
                        .keyPriority = Command::notePriority,
                        .repeatsWhileHeld = true });
    }

    registry.add ({ .id = "note.legato",
                    .name = "Legato",
                    .category = "Note",
                    .description = "Makes each selected note last until the next one starts.",
                    .synonyms = { "connect", "glue", "smooth", "length" },
                    .perform = [&selection]
                    { replaceSelectedNotes (selection, [] (std::vector<NoteData> notes) { return legato (std::move (notes)); }); },
                    .isEnabled = [&selection] { return hasSelectedNotes (selection); },
                    .undoTarget = undoTarget,
                    .keyPriority = Command::notePriority });

    // Slide and portamento notes, as FL Studio has them.
    registry.add ({ .id = "note.slide",
                    .name = "Slide Note",
                    .category = "Note",
                    .description = "Makes the selected notes slide notes, which play nothing themselves but glide the notes sounding as "
                                   "they start to their pitch over their length, as an 808 glides; or plain notes again. Phazite's own "
                                   "instruments play them.",
                    .synonyms = { "glide", "808", "pitch slide", "bend" },
                    .perform = withSong (selection, [&selection] (Song& song) { toggleNotesSlide (song, getSelectedNotes (selection)); }),
                    .isEnabled = [&selection] { return hasSelectedNotes (selection); },
                    .undoTarget = undoTarget,
                    .isTicked = ifSong (selection,
                                        [&selection] (const Song& song) { return areAllNotesSlides (song, getSelectedNotes (selection)); }),
                    .keyPriority = Command::notePriority });

    registry.add (
        { .id = "note.portamento",
          .name = "Portamento",
          .category = "Note",
          .description = "Makes the selected notes glide in from the note played before each, or start on their own pitch "
                         "again. Phazite's own instruments play the glide, over their Glide time.",
          .synonyms = { "porta", "glide", "legato", "slide in" },
          .perform = withSong (selection, [&selection] (Song& song) { toggleNotesPortamento (song, getSelectedNotes (selection)); }),
          .isEnabled = [&selection] { return hasSelectedNotes (selection); },
          .undoTarget = undoTarget,
          .isTicked =
              ifSong (selection, [&selection] (const Song& song) { return areAllNotesPortamento (song, getSelectedNotes (selection)); }),
          .keyPriority = Command::notePriority });

    registry.add ({ .id = "note.chop",
                    .name = "Chop",
                    .category = "Note",
                    .description = "Cuts the selected notes into pieces a step of the grid long.",
                    .synonyms = { "slice", "cut", "split", "stutter" },
                    .perform =
                        [&selection, getGridStepInBeats]
                    {
                        replaceSelectedNotes (selection, [&getGridStepInBeats] (std::vector<NoteData> notes)
                                              { return chop (std::move (notes), getGridStepInBeats()); });
                    },
                    .isEnabled = [&selection] { return hasSelectedNotes (selection); },
                    .undoTarget = undoTarget,
                    .keyPriority = Command::notePriority });
}
} // namespace phazite
