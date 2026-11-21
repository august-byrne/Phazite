// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Notes.h"
#include "devices/drums/DrumSampler.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/Numbers.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongIndex.h"
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
#include <numeric>
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

    // Notes of one clip that start this close together are a chord (NoteValues.h).
    constexpr double chordTolerance = 1.0 / 128.0;

    // The indices of the chords' notes: the notes of each clip that start together, each chord's from its lowest note up,
    // the chords in the order they start. A note alone is a chord of one.
    std::vector<std::vector<size_t>> findChords (const std::vector<NoteData>& notes)
    {
        std::vector<size_t> order;

        for (size_t i = 0; i < notes.size(); ++i)
            order.push_back (i);

        std::stable_sort (order.begin(), order.end(),
                          [&notes] (size_t a, size_t b)
                          {
                              const auto& first = notes[a];
                              const auto& second = notes[b];

                              if (first.clip != second.clip)
                                  return first.clip.getRaw() < second.clip.getRaw();

                              return first.start < second.start;
                          });

        std::vector<std::vector<size_t>> groups;

        for (const auto index : order)
        {
            const auto& note = notes[index];

            if (groups.empty() || notes[groups.back().front()].clip != note.clip
                || (note.start - notes[groups.back().front()].start).inBeats() > chordTolerance)
                groups.emplace_back();

            groups.back().push_back (index);
        }

        for (auto& chord : groups)
            std::stable_sort (chord.begin(), chord.end(), [&notes] (size_t a, size_t b) { return notes[a].pitch < notes[b].pitch; });

        return groups;
    }

    // Where a chord starts: its earliest note's start.
    BeatPosition getChordStart (const std::vector<NoteData>& notes, const std::vector<size_t>& chord)
    {
        auto start = notes[chord.front()].start;

        for (const auto index : chord)
            start = std::min (start, notes[index].start);

        return start;
    }

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

    // By the song's index (model/base/internal/SongIndex.h), which looks through a clip outside the song.
    juce::ValueTree findNoteIn (const Song& song, const juce::ValueTree& clipState, NoteID id)
    {
        return song.getIndex().findNote (clipState, id.getRaw());
    }

    NoteID getNoteID (const juce::ValueTree& noteState)
    {
        return NoteID::fromRaw ((std::uint64_t) std::max<juce::int64> (0, (juce::int64) noteState[schema::noteID]));
    }

    std::uint64_t notesRead = 0; // message thread

    NoteData readNote (const juce::ValueTree& clipState, const juce::ValueTree& noteState)
    {
        ++notesRead;
        return { getClipID (clipState),
                 getNoteID (noteState),
                 (int) noteState[schema::notePitch],
                 getContentStart (clipState) + BeatDuration::fromBeats (noteState[schema::noteStart]),
                 BeatDuration::fromBeats (noteState[schema::noteLength]),
                 (int) noteState[schema::noteVelocity],
                 (bool) noteState[schema::noteMute],
                 (bool) noteState[schema::noteSlide],
                 (bool) noteState[schema::notePortamento],
                 readChannel (noteState, schema::noteChannel) };
    }

    // Changes the note with an ID in a clip, and in each of its aliases, which hold the same notes.
    void changeNote (Song& song, const NoteRef& ref, const std::function<void (juce::ValueTree&, juce::UndoManager*)>& change)
    {
        const auto clipState = findMidiClipState (song, ref.clip);

        if (! clipState.isValid())
            return;

        const auto aliases = getAliasStates (song, clipState);

        for (const auto& alias : aliases)
            if (auto note = findNoteIn (song, alias, ref.note); note.isValid())
                change (note, &song.getUndoManager());
    }

    // What a note is besides its place, pitch and velocity: muted, a slide note or a portamento note, each written on its
    // element only while it is (Schema.h); and its MIDI channel, written only for another than the first.
    struct NoteFlags
    {
        bool muted = false, slide = false, portamento = false;
        int channel = 1;
    };

    NoteFlags getFlags (const NoteData& data)
    {
        return { data.muted, data.slide, data.portamento, data.channel };
    }

    void writeFlags (juce::ValueTree& note, NoteFlags flags)
    {
        for (const auto& [property, on] : { std::pair { &schema::noteMute, flags.muted }, std::pair { &schema::noteSlide, flags.slide },
                                            std::pair { &schema::notePortamento, flags.portamento } })
            if (on)
                note.setProperty (*property, true, nullptr);

        writeChannel (note, schema::noteChannel, flags.channel, nullptr);
    }

    // Adds a note to a clip and to each of its aliases, at a beat among its notes, with the ID wanted if no note of the clip
    // has it, or else a new one of the song's. Returns its ID, or none if the clip isn't the song's.
    NoteID addNoteState (Song& song, const juce::ValueTree& clipState, NoteID wanted, int pitch, BeatPosition start, BeatDuration length,
                         int velocity, NoteFlags flags)
    {
        if (! clipState.isValid())
            return {};

        const auto id = wanted.isValid() && ! findNoteIn (song, clipState, wanted).isValid() ? (juce::int64) wanted.getRaw()
                                                                                             : (juce::int64) song.makeID();

        const auto aliases = getAliasStates (song, clipState);

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

        const auto aliases = getAliasStates (song, clipState);

        for (auto alias : aliases)
            if (const auto note = findNoteIn (song, alias, ref.note); note.isValid())
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

            if (! findNoteIn (song, clipState, ref.note).isValid())
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

    // Where each clip can grow to (findRoomEnd()), found once for an edit of many notes rather than once for each note, which
    // walked the track's clips for each (a step of a drag of 500 notes on a track of 200 clips looked at 100,000 clips). It
    // stays right for as long as the edit lasts: what bounds the room is the other clips on the track, which no note edit
    // moves, and a clip grown to hold a note (growToHold()) grows only into its room, which leaves where the room ends as it
    // was (a clip that another covers can't grow at all). By the clip's ID, as notes name their clips.
    class Rooms
    {
    public:
        explicit Rooms (const Song& s)
            : song (s)
        {
        }

        std::optional<BeatPosition> findEnd (const juce::ValueTree& clipState)
        {
            const auto id = getClipID (clipState).getRaw();

            if (const auto found = ends.find (id); found != ends.end())
                return found->second;

            return ends.emplace (id, findRoomEnd (song, clipState)).first->second;
        }

    private:
        const Song& song;
        std::unordered_map<std::uint64_t, std::optional<BeatPosition>> ends;
    };

    // Makes the clip end at the end of the bar a song beat is in, if the clip ends before the beat, but only into the
    // empty space after it: never past the start of the next clip on its track, so that a note edit never makes one clip
    // cover another.
    void growToHold (Song& song, Rooms& rooms, const juce::ValueTree& clipState, BeatPosition end)
    {
        const auto range = getClipRange (clipState);

        if (end <= range.getEnd() + tinyBeats)
            return;

        const auto bars = getBars (song);
        auto newEnd = bars.getBarStart (bars.getBarAt (end - tinyBeats) + 1);

        if (const auto roomEnd = rooms.findEnd (clipState))
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
std::uint64_t countNotesRead() noexcept
{
    return notesRead;
}

int readChannel (const juce::ValueTree& element, const juce::Identifier& property)
{
    return juce::jlimit (1, 16, (int) element.getProperty (property, 1));
}

void writeChannel (juce::ValueTree& element, const juce::Identifier& property, int channel, juce::UndoManager* undo)
{
    if (channel = juce::jlimit (1, 16, channel); channel == 1)
        element.removeProperty (property, undo);
    else
        element.setProperty (property, channel, undo);
}

juce::ValueTree findNoteState (const Song& song, const NoteRef& ref)
{
    return findNoteIn (song, findMidiClipState (song, ref.clip), ref.note);
}

std::optional<NoteData> getNoteData (const Song& song, const NoteRef& ref)
{
    const auto clipState = findMidiClipState (song, ref.clip);

    if (const auto note = findNoteIn (song, clipState, ref.note); note.isValid())
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
    SongIndex::countVisited ((std::uint64_t) clipState.getNumChildren()); // a walk of the clip's, for the speed gates

    for (const auto& child : clipState)
        if (child.hasType (schema::NOTE))
            data.push_back (readNote (clipState, child));

    return data;
}

namespace
{
    // Whether a clip plays a note: some of it is in the part of the song the clip covers.
    bool plays (const juce::ValueTree& clipState, const NoteData& note)
    {
        const auto range = getClipRange (clipState);
        return note.getEnd() > range.getStart() + tinyBeats && note.start < range.getEnd() - tinyBeats;
    }

    // How far back a note's start can go: to its clip's start, and not at all if it starts before it already.
    BeatPosition getEarliestStart (const juce::ValueTree& clipState, BeatPosition startNow)
    {
        return std::min (startNow, getClipRange (clipState).getStart());
    }

    // The latest a note of a clip can start and still play: the shortest note before where the clip can grow to, the next
    // clip on its track (growToHold()). None while nothing keeps the clip from growing to hold the note.
    std::optional<BeatPosition> findLatestStart (Rooms& rooms, const juce::ValueTree& clipState)
    {
        if (const auto roomEnd = rooms.findEnd (clipState))
            return std::max (*roomEnd, getClipRange (clipState).getEnd()) - getMinimumNoteLength();

        return std::nullopt;
    }

    // Where an edit puts a note's start, kept where its clip plays the note: no earlier than getEarliestStart() and no later
    // than findLatestStart(), nor than it is if it is later already. An edit of a note that plays never leaves it in its
    // clip without playing, where no editor would show it.
    BeatPosition keepPlayed (Rooms& rooms, const juce::ValueTree& clipState, BeatPosition startNow, BeatPosition wanted)
    {
        if (const auto latest = findLatestStart (rooms, clipState))
            wanted = std::min (wanted, std::max (startNow, *latest));

        return std::max (wanted, getEarliestStart (clipState, startNow));
    }

    // Whether a new note starting at a song beat would play: in the clip, or after it where it can grow to hold the note
    // (growToHold()): before the next clip on its track.
    bool canPlayAt (Rooms& rooms, const juce::ValueTree& clipState, BeatPosition start)
    {
        if (! clipState.isValid())
            return false;

        if (start < getClipRange (clipState).getEnd() - tinyBeats)
            return true;

        const auto roomEnd = rooms.findEnd (clipState);
        return ! roomEnd.has_value() || start < *roomEnd - tinyBeats;
    }
} // namespace

std::optional<BeatPosition> getLatestNoteStart (const Song& song, ClipID clip)
{
    const auto clipState = findMidiClipState (song, clip);
    Rooms rooms (song);
    return clipState.isValid() ? findLatestStart (rooms, clipState) : std::nullopt;
}

std::vector<NoteData> getClipNotesPlayed (const Song& song, ClipID clip)
{
    std::vector<NoteData> data;
    const auto clipState = findMidiClipState (song, clip);
    SongIndex::countVisited ((std::uint64_t) clipState.getNumChildren());

    for (const auto& child : clipState)
        if (child.hasType (schema::NOTE))
            if (const auto note = readNote (clipState, child); plays (clipState, note))
                data.push_back (note);

    return data;
}

bool isNotePlayed (const Song& song, const NoteRef& ref)
{
    const auto note = getNoteData (song, ref);
    return note.has_value() && plays (findMidiClipState (song, ref.clip), *note);
}

bool canPlayNoteAt (const Song& song, ClipID clip, BeatPosition start)
{
    Rooms rooms (song);
    return canPlayAt (rooms, findMidiClipState (song, clip), start);
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

    // Those still there that play (not one a trim has left outside its clip since it was selected), of the note clips, in
    // the order they were selected.
    for (const auto& ref : selection.getNotes())
        if (std::find (selection.getNoteClips().begin(), selection.getNoteClips().end(), ref.clip) != selection.getNoteClips().end())
            if (isNotePlayed (*song, ref))
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
    // Adds a note at a song beat, as addNote() says, with its flags, with the ID wanted if no note of the clip has it. It
    // starts no earlier than its clip, or than the earliest start given: where the note it replaces started, if that was
    // before the clip's start (moveNote()); and no later than the latest given, if any.
    NoteRef addNoteTo (Song& song, Rooms& rooms, ClipID clip, NoteID wanted, int pitch, BeatPosition start, BeatDuration length,
                       int velocity, NoteFlags flags, std::optional<BeatPosition> earliest = {}, std::optional<BeatPosition> latest = {})
    {
        const auto clipState = findMidiClipState (song, clip);

        if (! clipState.isValid())
            return {};

        start = std::max (std::min (start, latest.value_or (start)), earliest.value_or (getClipRange (clipState).getStart()));
        length = std::max (length, getMinimumNoteLength());
        growToHold (song, rooms, clipState, start + length);
        const auto id = addNoteState (song, clipState, wanted, clampPitch (pitch), toClipBeat (clipState, start), length,
                                      juce::jlimit (1, 127, velocity), flags);
        return id.isValid() ? NoteRef { clip, id } : NoteRef {};
    }
} // namespace

NoteRef addNote (Song& song, ClipID clip, int pitch, BeatPosition start, BeatDuration length, int velocity, int channel)
{
    Rooms rooms (song);
    NoteFlags flags;
    flags.channel = channel;
    return addNoteTo (song, rooms, clip, {}, pitch, start, length, velocity, flags);
}

const char* getSchemaKind (ControlKind kind) noexcept
{
    switch (kind)
    {
        case ControlKind::controller:
            return schema::controlIsController;
        case ControlKind::pitchWheel:
            return schema::controlIsPitchWheel;
        case ControlKind::channelPressure:
            return schema::controlIsChannelPressure;
        case ControlKind::aftertouch:
            return schema::controlIsAftertouch;
        case ControlKind::programChange:
            return schema::controlIsProgramChange;
    }

    return schema::controlIsController;
}

bool hasNumber (ControlKind kind) noexcept
{
    return kind == ControlKind::controller || kind == ControlKind::aftertouch;
}

int getHighestValue (ControlKind kind) noexcept
{
    return kind == ControlKind::pitchWheel ? 16383 : 127;
}

std::optional<ControlKind> findControlKind (const juce::ValueTree& control)
{
    const auto kind = control[schema::controlKind].toString();

    for (const auto each : { ControlKind::controller, ControlKind::pitchWheel, ControlKind::channelPressure, ControlKind::aftertouch,
                             ControlKind::programChange })
        if (kind == getSchemaKind (each))
            return each;

    return std::nullopt;
}

namespace
{
    juce::ValueTree makeControlState (const ControlValue& each, BeatPosition contentStart)
    {
        juce::ValueTree control (schema::CONTROL);
        control.setProperty (schema::controlKind, getSchemaKind (each.kind), nullptr);

        if (hasNumber (each.kind))
            control.setProperty (schema::controlNumber, juce::jlimit (0, 127, each.number), nullptr);

        control.setProperty (schema::controlBeat, std::max (0.0, (each.beat - contentStart).inBeats()), nullptr);
        control.setProperty (schema::controlValue, juce::jlimit (0, getHighestValue (each.kind), each.value), nullptr);
        writeChannel (control, schema::controlChannel, each.channel, nullptr);
        return control;
    }

    // Whether a clip's element is a value of a controller on a channel (by its number, if its kind has one).
    bool isControlOf (const juce::ValueTree& child, ControlKind kind, int number, int channel)
    {
        return child.hasType (schema::CONTROL) && findControlKind (child) == kind
               && (! hasNumber (kind) || (int) child[schema::controlNumber] == number)
               && readChannel (child, schema::controlChannel) == juce::jlimit (1, 16, channel);
    }
} // namespace

void addControl (Song& song, ClipID clip, BeatPosition beat, ControlKind kind, int number, int value, int channel)
{
    const auto clipState = findMidiClipState (song, clip);

    if (! clipState.isValid())
        return;

    for (auto alias : getAliasStates (song, clipState))
        alias.appendChild (makeControlState ({ beat, kind, number, value, channel }, getContentStart (clipState)), &song.getUndoManager());
}

void addControls (Song& song, ClipID clip, const std::vector<ControlValue>& values)
{
    const auto clipState = findMidiClipState (song, clip);

    if (! clipState.isValid() || values.empty())
        return;

    const auto contentStart = getContentStart (clipState);
    auto* undo = &song.getUndoManager();

    // Each alias filled as a copy, without the history, and put in its place, the same clip by its ID, in one removal and one
    // insertion: what listens to the song hears a recording's thousands of values (a bend's sweep) as one change, and Undo
    // takes them back with it. The original is never filled: the removal's undo brings it back as it was.
    for (auto alias : getAliasStates (song, clipState))
    {
        auto filled = alias.createCopy();

        for (const auto& each : values)
            filled.appendChild (makeControlState (each, contentStart), nullptr);

        auto parent = alias.getParent();
        const auto index = parent.indexOf (alias);
        parent.removeChild (index, undo);
        parent.addChild (filled, index, undo);
    }
}

void removeControls (Song& song, ClipID clip, ControlKind kind, int number, int channel, BeatPosition from, BeatPosition to)
{
    const auto clipState = findMidiClipState (song, clip);

    if (! clipState.isValid())
        return;

    constexpr auto near = 1.0e-9; // in beats
    const auto first = toClipBeat (clipState, from).inBeats() - near, last = toClipBeat (clipState, to).inBeats() + near;

    for (auto alias : getAliasStates (song, clipState))
    {
        for (auto i = alias.getNumChildren(); --i >= 0;)
        {
            const auto child = alias.getChild (i);

            if (! isControlOf (child, kind, number, channel))
                continue;

            if (const auto beat = (double) child[schema::controlBeat]; beat >= first && beat <= last)
                alias.removeChild (i, &song.getUndoManager());
        }
    }
}

void removeControl (Song& song, ClipID clip, const ControlValue& control)
{
    const auto clipState = findMidiClipState (song, clip);

    if (! clipState.isValid())
        return;

    constexpr auto near = 1.0e-9; // in beats
    const auto beat = toClipBeat (clipState, control.beat).inBeats();

    for (auto alias : getAliasStates (song, clipState))
    {
        for (auto i = alias.getNumChildren(); --i >= 0;)
        {
            const auto child = alias.getChild (i);

            if (isControlOf (child, control.kind, control.number, control.channel)
                && std::abs ((double) child[schema::controlBeat] - beat) <= near && (int) child[schema::controlValue] == control.value)
            {
                alias.removeChild (i, &song.getUndoManager());
                break;
            }
        }
    }
}

namespace
{
    // moveNote(), setNoteLength() and placeNote(), with where the clips can grow to found once for the edit (Rooms).
    void moveNoteIn (Song& song, Rooms& rooms, const NoteRef& ref, BeatPosition start, int pitch)
    {
        const auto clipState = findMidiClipState (song, ref.clip);
        const auto note = getNoteData (song, ref);

        if (! note.has_value())
            return;

        start = keepPlayed (rooms, clipState, note->start, start);
        growToHold (song, rooms, clipState, start + note->length);
        const auto clipBeat = toClipBeat (clipState, start);
        changeNote (song, ref,
                    [clipBeat, pitch] (juce::ValueTree& n, juce::UndoManager* undo)
                    {
                        n.setProperty (schema::noteStart, clipBeat.inBeats(), undo);
                        n.setProperty (schema::notePitch, clampPitch (pitch), undo);
                    });
    }

    void setNoteLengthIn (Song& song, Rooms& rooms, const NoteRef& ref, BeatDuration length)
    {
        const auto note = getNoteData (song, ref);

        if (! note.has_value())
            return;

        length = std::max (length, getMinimumNoteLength());
        growToHold (song, rooms, findMidiClipState (song, ref.clip), note->start + length);
        changeNote (song, ref,
                    [length] (juce::ValueTree& n, juce::UndoManager* undo) { n.setProperty (schema::noteLength, length.inBeats(), undo); });
    }

    void placeNoteIn (Song& song, Rooms& rooms, const NoteRef& ref, BeatPosition start, int pitch, BeatDuration length)
    {
        const auto clipState = findMidiClipState (song, ref.clip);
        const auto note = getNoteData (song, ref);

        if (! note.has_value())
            return;

        start = keepPlayed (rooms, clipState, note->start, start);
        length = std::max (length, getMinimumNoteLength());
        growToHold (song, rooms, clipState, start + length);
        const auto clipBeat = toClipBeat (clipState, start);
        changeNote (song, ref,
                    [clipBeat, pitch, length] (juce::ValueTree& n, juce::UndoManager* undo)
                    {
                        n.setProperty (schema::noteStart, clipBeat.inBeats(), undo);
                        n.setProperty (schema::notePitch, clampPitch (pitch), undo);
                        n.setProperty (schema::noteLength, length.inBeats(), undo);
                    });
    }
} // namespace

void moveNote (Song& song, const NoteRef& ref, BeatPosition start, int pitch)
{
    Rooms rooms (song);
    moveNoteIn (song, rooms, ref, start, pitch);
}

void setNoteLength (Song& song, const NoteRef& ref, BeatDuration length)
{
    Rooms rooms (song);
    setNoteLengthIn (song, rooms, ref, length);
}

void placeNote (Song& song, const NoteRef& ref, BeatPosition start, int pitch, BeatDuration length)
{
    Rooms rooms (song);
    placeNoteIn (song, rooms, ref, start, pitch, length);
}

void placeNotes (Song& song, const std::vector<NoteData>& notes)
{
    Rooms rooms (song);

    for (const auto& note : notes)
        placeNoteIn (song, rooms, note.getRef(), note.start, note.pitch, note.length);
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
    Rooms rooms (song);

    // The note nearest its clip's start decides how far back they can go: none at all with one that starts before it. And
    // the one nearest where its clip stops playing it, how far on (keepPlayed()).
    for (const auto& note : unique)
    {
        const auto clipState = findMidiClipState (song, note.clip);
        amount = keepPlayed (rooms, clipState, note.start, note.start + amount) - note.start;
    }

    for (const auto& note : unique)
        moveNoteIn (song, rooms, { note.clip, note.id }, note.start + amount, note.pitch);
}

void changeNoteLengths (Song& song, const std::vector<NoteRef>& notes, BeatDuration change)
{
    Rooms rooms (song);

    for (const auto& note : getNoteData (song, withoutAliasCopies (song, notes)))
        setNoteLengthIn (song, rooms, { note.clip, note.id }, note.length + change);
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

void setNoteChannels (Song& song, const std::vector<NoteRef>& notes, int channel)
{
    for (const auto& ref : withoutAliasCopies (song, notes))
        changeNote (song, ref, [channel] (juce::ValueTree& note, juce::UndoManager* undo)
                    { writeChannel (note, schema::noteChannel, channel, undo); });
}

void changeNoteVelocities (Song& song, const std::vector<NoteRef>& notes, int change)
{
    if (notes.empty())
        return;

    const UndoStep step (song, "Change Velocity");

    for (const auto& note : getNoteData (song, withoutAliasCopies (song, notes)))
        setNoteVelocities (song, { { note.clip, note.id } }, note.velocity + change);
}

std::vector<NoteRef> duplicateNotes (Song& song, const std::vector<NoteRef>& notes, const juce::String& stepName)
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

    auto made = replaceNotes (song, {}, copies);
    tellNotesMade (song, stepName, "Duplicate", copies.size(), made.size());
    return made;
}

void tellNotesMade (Song& song, const juce::String& asked, const juce::String& verb, size_t wanted, size_t made)
{
    if (made >= wanted)
        return;

    const auto count = [] (size_t n) { return n == 1 ? juce::String ("1 note") : juce::String ((int) n) + " notes"; };
    const auto name = made == 1 ? verb + " Note" : verb + " " + juce::String ((int) made) + " Notes";
    const auto left = wanted - made;

    UndoStep::tellOutcome (song, asked, name,
                           count (left) + " of " + juce::String ((int) wanted) + (left == 1 ? " wasn't" : " weren't")
                               + " made: " + (left == 1 ? "it" : "they")
                               + " would land past the clip's end, where the next clip on its track keeps it from growing.");
}

void setClipNotes (Song& song, ClipID clip, const std::vector<NoteData>& notes)
{
    const auto clipState = findMidiClipState (song, clip);

    if (! clipState.isValid())
        return;

    auto* undo = &song.getUndoManager();
    const auto contentStart = getContentStart (clipState);
    const auto aliases = getAliasStates (song, clipState);
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
    // Where the old ones started: what comes of one that started before its clip did (a note a split or a trim cut) goes no
    // earlier, rather than to its clip's start.
    std::unordered_map<NoteRef, BeatPosition> oldStarts;

    for (const auto& note : getNoteData (song, old))
        oldStarts[note.getRef()] = note.start;

    deleteNotes (song, old);
    std::vector<NoteRef> added;
    Rooms rooms (song);

    // A note that replaces one of the old ones (a quantized or transposed note, a chopped note's first piece) keeps that one's
    // ID, if no other note of the clip has it now, and stays where its clip plays it, as a moved note does (keepPlayed()). The
    // others that come of an old one (a chopped note's later pieces, with its ID, which they can't keep) go where they are
    // given, past the clip too, where the old note reached: nothing of it is lost, and the clip plays them once it covers
    // them. A new one (a copy, a pasted note), whose original is still there, gets an ID of its own and is added only where
    // it plays: none before its clip, or after it where the clip can't grow to hold it, where no editor would show it.
    for (const auto& data : replacements)
    {
        const auto clipState = findMidiClipState (song, data.clip);
        const auto oldStart = oldStarts.find (data.getRef());
        const auto comesOfOne = oldStart != oldStarts.end();
        const auto replacesOne = comesOfOne && ! findNoteState (song, data.getRef()).isValid();

        if (! comesOfOne
            && (data.getEnd() <= getClipRange (clipState).getStart() + tinyBeats || ! canPlayAt (rooms, clipState, data.start)))
            continue;

        std::optional<BeatPosition> earliest, latest;

        if (comesOfOne && clipState.isValid())
            earliest = getEarliestStart (clipState, oldStart->second);

        if (replacesOne && clipState.isValid())
            if (const auto latestPlayed = findLatestStart (rooms, clipState))
                latest = std::max (oldStart->second, *latestPlayed);

        if (const auto note = addNoteTo (song, rooms, data.clip, data.id, data.pitch, data.start, data.length, data.velocity,
                                         getFlags (data), earliest, latest);
            note.clip.isValid())
            added.push_back (note);
    }

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

    // The nearest grid line, counted from the origin as the piano roll's grid is, with every other one late by up to a third
    // of a step: at full swing, the second of each pair of steps lands two-thirds of the way through the pair, as in a
    // triplet.
    const auto origin = settings.origin.inBeats();

    const auto lineAt = [=] (double index)
    { return origin + index * grid + (std::fmod (std::abs (index), 2.0) > 0.5 ? swing * grid / 3.0 : 0.0); };

    // The nearest of the lines as they are swung: of the unswung lines' nearest, the swung one could be farther than its
    // neighbour.
    const auto quantizeBeat = [=] (double beat)
    {
        const auto nearest = std::round ((beat - origin) / grid);
        auto target = lineAt (nearest);

        for (const auto index : { nearest - 1.0, nearest + 1.0 })
            if (std::abs (lineAt (index) - beat) < std::abs (target - beat))
                target = lineAt (index);

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

juce::String describeLegato (double gapBeats)
{
    if (gapBeats < 1.0e-6)
        return "Legato";

    return "Legato (" + describeNoteLength (BeatDuration::fromBeats (gapBeats)) + " gap)";
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

            // The last there may be holds the rest of a note too long for that many, which would be lost.
            const auto last = i == maxPiecesPerNote - 1;
            piece.length = BeatDuration::fromBeats (last ? length - offset : std::min (pieceBeats, length - offset));
            pieces.push_back (piece);
        }
    }

    return pieces;
}

std::vector<NoteData> strum (std::vector<NoteData> notes, const StrumSettings& settings)
{
    const auto step = BeatDuration::fromBeats (std::max (0.0, settings.stepBeats));

    for (auto chord : findChords (notes))
    {
        if (chord.size() < 2)
            continue;

        if (settings.down)
            std::reverse (chord.begin(), chord.end());

        // Each note starts a step after the one before it, and ends where it did, so that the chord still ends together.
        const auto chordStart = getChordStart (notes, chord);

        for (size_t i = 0; i < chord.size(); ++i)
        {
            auto& note = notes[chord[i]];
            const auto end = note.getEnd();
            note.start = std::min (chordStart + step * (double) i, end - getMinimumNoteLength());
            note.length = end - note.start;
        }
    }

    return notes;
}

std::vector<NoteData> humanize (std::vector<NoteData> notes, const HumanizeSettings& settings)
{
    // Two draws for each note whatever the amounts, so that changing one amount keeps what the other does.
    juce::Random random (settings.seed);
    const auto timing = std::max (0.0, settings.timingBeats);
    const auto velocity = std::max (0, settings.velocity);

    for (auto& note : notes)
    {
        const auto timingDraw = random.nextDouble() * 2.0 - 1.0;
        const auto velocityDraw = random.nextDouble() * 2.0 - 1.0;
        note.start = BeatPosition::fromBeats (std::max (0.0, note.start.inBeats() + timing * timingDraw));
        note.velocity = juce::jlimit (1, 127, note.velocity + juce::roundToInt (velocity * velocityDraw));
    }

    return notes;
}

std::vector<NoteData> arpeggiate (std::vector<NoteData> notes, const ArpeggioSettings& settings)
{
    const auto step = settings.stepBeats;

    if (step <= getMinimumNoteLength().inBeats() / 2.0)
        return notes;

    const auto gate = (double) juce::jlimit (0.1f, 1.0f, settings.gate);
    std::vector<NoteData> result;

    for (const auto& chord : findChords (notes))
    {
        if (chord.size() < 2)
        {
            result.push_back (notes[chord.front()]);
            continue;
        }

        // The order the chord's notes play in: indices into the chord, which goes from its lowest note up.
        std::vector<size_t> order;

        for (size_t i = 0; i < chord.size(); ++i)
            order.push_back (i);

        if (settings.order == ArpeggioOrder::down)
            std::reverse (order.begin(), order.end());

        if (settings.order == ArpeggioOrder::upAndDown)
            for (auto i = chord.size() - 2; i > 0; --i)
                order.push_back (i);

        // A note at each step for as long as the chord lasts, each a piece of the note it plays: the first piece of each
        // note replaces it (replaceNotes() keeps its ID), and the rest are new.
        const auto chordStart = getChordStart (notes, chord);
        auto chordEnd = notes[chord.front()].getEnd();

        for (const auto index : chord)
            chordEnd = std::max (chordEnd, notes[index].getEnd());

        const auto maxSteps = maxPiecesPerNote * (int) chord.size();

        for (int i = 0; i < maxSteps; ++i)
        {
            const auto start = chordStart + BeatDuration::fromBeats (step * i);

            if ((chordEnd - start).inBeats() < 1.0e-6)
                break;

            // The last step there may be holds the rest of a chord too long for that many, which would be lost.
            auto piece = notes[chord[order[(size_t) i % order.size()]]];
            piece.start = start;
            piece.length = i == maxSteps - 1
                               ? std::max (getMinimumNoteLength(), chordEnd - start)
                               : std::max (getMinimumNoteLength(), std::min (BeatDuration::fromBeats (step * gate), chordEnd - start));
            result.push_back (piece);
        }
    }

    return result;
}

juce::String describeNoteLength (BeatDuration length)
{
    const auto beats = length.inBeats();
    const auto trimmed = [] (double value, int decimals)
    { return juce::String (value, decimals).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."); };

    // A part of a beat, as the grid names its steps ("1/4 beat"), if it is one; then whole beats, then decimals.
    if (beats < 1.0 - 1.0e-9)
    {
        for (const auto parts : { 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 })
        {
            const auto count = beats * parts;

            if (const auto whole = juce::roundToInt (count); whole > 0 && std::abs (count - whole) < 1.0e-6)
            {
                const auto divisor = std::gcd (whole, parts);
                return juce::String (whole / divisor) + "/" + juce::String (parts / divisor) + " beat";
            }
        }

        return trimmed (beats, 3) + " beat";
    }

    if (const auto whole = roundToNearestInt (beats); std::abs (beats - whole) < 1.0e-6)
        return juce::String (whole) + (whole == 1 ? " beat" : " beats");

    return trimmed (beats, 2) + " beats";
}

//==============================================================================
juce::String getScaleName (Scale scale)
{
    return findSavedScale (scale).shownName;
}

juce::String describeKey (const Key& key)
{
    return spellWithLetter (getKeyRootLetter (key), key.root) + " " + getScaleName (key.scale).toLowerCase();
}

int getParentMajor (const Key& key)
{
    switch (key.scale)
    {
        case Scale::major:
            return floorModulo (key.root, 12);
        case Scale::dorian:
            return floorModulo (key.root - 2, 12);
        case Scale::phrygian:
            return floorModulo (key.root - 4, 12);
        case Scale::lydian:
            return floorModulo (key.root - 5, 12);
        case Scale::mixolydian:
            return floorModulo (key.root - 7, 12);
        case Scale::locrian:
            return floorModulo (key.root - 11, 12);
        case Scale::minor:
        case Scale::harmonicMinor:
        case Scale::melodicMinor:
        default:
            return floorModulo (key.root + 3, 12);
    }
}

namespace
{
    constexpr const char* letterNames = "CDEFGAB";
    constexpr int letterPitches[] = { 0, 2, 4, 5, 7, 9, 11 };
} // namespace

int getKeyRootLetter (const Key& key)
{
    const auto parent = getParentMajor (key);
    const auto flats = parent == 5 || parent == 10 || parent == 3 || parent == 8 || parent == 1;
    constexpr int sharpLetters[] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 }; // C C# D D# E F F# G G# A A# B
    constexpr int flatLetters[] = { 0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6 };  // C Db D Eb E F Gb G Ab A Bb B
    return (flats ? flatLetters : sharpLetters)[floorModulo (key.root, 12)];
}

juce::String spellWithLetter (int letter, int pitchClass)
{
    letter = floorModulo (letter, 7);
    auto difference = floorModulo (pitchClass - letterPitches[letter], 12);

    if (difference > 6)
        difference -= 12;

    auto name = juce::String::charToString ((juce::juce_wchar) letterNames[letter]);
    name << juce::String::repeatedString (difference > 0 ? "#" : "b", std::abs (difference));
    return name;
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
    // A row for each of the drum sampler's pads that plays a sound, by their notes.
    for (const auto& device : getDeviceStates (findTrackState (song, track)))
    {
        if (isDrumSampler (device))
        {
            std::vector<DrumRow> pads;

            for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
                if (! isPadEmpty (song, device, pad))
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
        for (const auto& note : getClipNotesPlayed (song, clip))
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
void addNoteCommands (CommandRegistry& registry, Selection& selection, std::function<double()> getGridStepInBeats,
                      std::function<double()> getLegatoGapBeats)
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

    // With the gap Legato... was given last, which its name shows, so that what it does is never a surprise.
    const auto legatoGap = [getLegatoGapBeats] { return getLegatoGapBeats != nullptr ? std::max (0.0, getLegatoGapBeats()) : 0.0; };

    registry.add ({ .id = "note.legato",
                    .name = "Legato",
                    .category = "Note",
                    .description = "Makes each selected note last until the next one starts, less the gap Legato... was given last.",
                    .synonyms = { "connect", "glue", "smooth", "length" },
                    .perform =
                        [&selection, legatoGap]
                    {
                        replaceSelectedNotes (selection, [gap = legatoGap()] (std::vector<NoteData> notes)
                                              { return legato (std::move (notes), gap); });
                    },
                    .isEnabled = [&selection] { return hasSelectedNotes (selection); },
                    .undoTarget = undoTarget,
                    .currentName = [legatoGap] { return describeLegato (legatoGap()); },
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
