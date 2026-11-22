// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Notes.h"
#include "devices/drums/DrumSampler.h"
#include "internal/NoteElements.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/Numbers.h"
#include "model/base/Toggles.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongIndex.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/mixer/internal/Mixer.h"
#include "model/transport/internal/Tempo.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

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

    // A MIDI clip's value, or none.
    std::optional<ClipValue> findMidiClip (const Song& song, ClipID clip)
    {
        if (auto value = getClipValue (song, clip); value.has_value() && value->isMidi())
            return value;

        return std::nullopt;
    }

    // The MIDI clips that an edit, or a reading of many notes, names: each found in the song's clips' area by its ID, without a
    // copy (SongValues.h: SongClips), the area as it was as this was made held meanwhile, so that what is found stays valid
    // while the edit changes the song; a clip's place, length, offset and alias group read from there once for all of its
    // notes, not once for each. What grows a clip it found (growToHold()) reads it again (readAgain()).
    class MidiClips
    {
    public:
        explicit MidiClips (const Song& s)
            : song (s),
              held (s.getValues() != nullptr ? s.getValues()->clips.get() : SongClips())
        {
        }

        // None for a clip that isn't a MIDI clip of the song's. Valid for as long as this is.
        const ClipValue* find (ClipID clip) const
        {
            SongIndex::countVisited (1); // a look, for the speed gates, as the clips' storage counts one (Clips.cpp)

            if (const auto found = grown.find (clip.getRaw()); found != grown.end())
                return &found->second;

            const auto* value = held.find (clip);
            return value != nullptr && value->isMidi() ? value : nullptr;
        }

        // A clip as the song holds it now, after this changed it: none for one that isn't a MIDI clip of the song's any more.
        const ClipValue* readAgain (ClipID clip)
        {
            if (auto now = findMidiClip (song, clip))
            {
                auto& kept = grown[clip.getRaw()];
                kept = std::move (*now);
                return &kept;
            }

            return nullptr;
        }

    private:
        const Song& song;
        const SongClips held;                               // the song's clips as this was made: what it finds in
        std::unordered_map<std::uint64_t, ClipValue> grown; // those read again since
    };

    // A song beat as a position among the clip's notes, which count from its content's start; and back.
    BeatPosition toClipBeat (const ClipValue& clip, BeatPosition songBeat)
    {
        return BeatPosition::fromBeats ((songBeat - getContentStart (clip)).inBeats());
    }

    BeatPosition toSongBeat (const ClipValue& clip, BeatPosition clipBeat)
    {
        return getContentStart (clip) + BeatDuration::fromBeats (clipBeat.inBeats());
    }

    //==============================================================================
    // The song's notes: each clip's value (SongValues.h: ClipNotes), the same in each of its aliases, which the song holds
    // beside its tree (getClipNotesValue(), setClipNotesValue()); one note alone is found in its clip's value by its ID.

    std::uint64_t notesRead = 0; // message thread

    // A clip's note as the piano roll shows it, in song beats.
    NoteData toNoteData (const ClipValue& clip, const NoteValues& note)
    {
        ++notesRead;
        return { clip.id,    note.id,         note.pitch,  toSongBeat (clip, note.start), note.length, note.velocity, note.muted,
                 note.slide, note.portamento, note.channel };
    }

    // One note of a clip as it holds it now: none for a note or a clip that is gone.
    std::optional<NoteValues> findNoteValues (const Song& song, const ClipValue* clip, NoteID id)
    {
        if (clip == nullptr)
            return std::nullopt;

        if (const auto* note = getClipNotesValue (song, clip->id).find (id))
            return *note;

        return std::nullopt;
    }

    // What names a clip's alias group: its link, or the clip's own ID for a clip in none.
    using GroupKey = std::pair<bool, std::uint64_t>; // whether a link, and the link or the clip's ID

    GroupKey getGroupKey (const ClipValue& clip)
    {
        return clip.link != 0 ? GroupKey { true, clip.link } : GroupKey { false, clip.id.getRaw() };
    }

    // One edit's clips' notes: each alias group's value taken once, from the first of its clips the edit names, changed as a
    // batch, and set once for the group as the edit ends (commit()). The clips the edit names are read once (MidiClips); what
    // changes them (their places, as growToHold() changes them) changes them in the song as the edit goes.
    class NotesEdit
    {
    public:
        explicit NotesEdit (Song& s)
            : song (s),
              clips (s)
        {
        }

        struct Group
        {
            ClipID read; // the clip the value was taken from
            ClipNotes before, after;
        };

        // A MIDI clip of the song's, as this edit read it: none for one that isn't. And what reads them.
        const ClipValue* findClip (ClipID clip) const { return clips.find (clip); }
        MidiClips& getClips() { return clips; }

        // The value of a clip's group, for this edit: nullptr for a clip that isn't a MIDI clip of the song's.
        Group* find (const ClipValue* clip)
        {
            if (clip == nullptr)
                return nullptr;

            const auto key = getGroupKey (*clip);

            if (const auto found = groups.find (key); found != groups.end())
                return &found->second;

            Group group;
            group.read = clip->id;
            group.before = getClipNotesValue (song, group.read);
            group.after = group.before;
            order.push_back (key);
            return &groups.emplace (key, std::move (group)).first->second;
        }

        Group* find (ClipID clip) { return find (findClip (clip)); }

        // Each group's value set, in the order the groups were first named, as setClipNotesValue() sets it. Once, as the edit
        // ends.
        void commit()
        {
            for (const auto& key : order)
                if (const auto& group = groups.at (key); ! group.after.isSameAs (group.before))
                    setClipNotesValue (song, group.read, group.after);

            groups.clear();
            order.clear();
        }

    private:
        Song& song;
        MidiClips clips;
        std::map<GroupKey, Group> groups;
        std::vector<GroupKey> order;
    };

    // A note's values as its clip holds them, but its ID, its pitch, its start and its length, which the edits work out. Its
    // channel kept within 1 to 16, as a note's value holds it (its velocity is kept within its range as it is added).
    NoteValues getValuesKept (const NoteData& data)
    {
        return { .velocity = data.velocity,
                 .muted = data.muted,
                 .slide = data.slide,
                 .portamento = data.portamento,
                 .channel = keepChannel (data.channel) };
    }

    // The notes there, each once: of a note in a clip and its copies in the clip's aliases, the first one named, since an
    // edit to it is made to each of them.
    std::vector<NoteRef> withoutAliasCopies (const Song& song, const std::vector<NoteRef>& notes)
    {
        std::vector<NoteRef> unique;
        std::set<std::pair<GroupKey, std::uint64_t>> seen;
        MidiClips clips (song);

        for (const auto& ref : notes)
        {
            const auto* clip = clips.find (ref.clip);

            if (! findNoteValues (song, clip, ref.note).has_value())
                continue;

            if (seen.insert ({ getGroupKey (*clip), ref.note.getRaw() }).second)
                unique.push_back (ref);
        }

        return unique;
    }

    // The notes there, each once as withoutAliasCopies() names it, with every clip of its group it was named through, the
    // first named first.
    struct NamedNote
    {
        NoteRef ref;
        std::vector<ClipID> clips;
    };

    std::vector<NamedNote> nameEachOnce (const Song& song, const std::vector<NoteRef>& notes)
    {
        std::vector<NamedNote> named;
        std::map<std::pair<GroupKey, std::uint64_t>, size_t> places;
        MidiClips read (song);

        for (const auto& ref : notes)
        {
            const auto* clip = read.find (ref.clip);

            if (! findNoteValues (song, clip, ref.note).has_value())
                continue;

            const auto [found, isNew] = places.try_emplace ({ getGroupKey (*clip), ref.note.getRaw() }, named.size());

            if (isNew)
                named.push_back ({ ref, { ref.clip } });
            else if (auto& clips = named[found->second].clips; std::find (clips.begin(), clips.end(), ref.clip) == clips.end())
                clips.push_back (ref.clip);
        }

        return named;
    }

    // Where a clip can grow to without covering another clip on its track: the start of the first clip that reaches past
    // its end, or its own end if another clip already covers that. Nothing if nothing on the track reaches past its end.
    // The other clips read by their places alone (Clips.h: getClipPlacements()), once for each clip an edit grows.
    std::optional<BeatPosition> findRoomEnd (const Song& song, const ClipValue& clip)
    {
        jassert (clip.isMidi());
        const auto end = getClipRange (clip).getEnd();
        std::optional<BeatPosition> roomEnd;

        for (const auto& other : getClipPlacements (song, clip.track))
        {
            const auto range = getClipRange (song, other);

            if (other.id == clip.id || range.getEnd() <= end + tinyBeats)
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

        std::optional<BeatPosition> findEnd (const ClipValue& clip)
        {
            const auto id = clip.id.getRaw();

            if (const auto found = ends.find (id); found != ends.end())
                return found->second;

            return ends.emplace (id, findRoomEnd (song, clip)).first->second;
        }

    private:
        const Song& song;
        std::unordered_map<std::uint64_t, std::optional<BeatPosition>> ends;
    };

    // Makes the clip end at the end of the bar a song beat is in, if the clip ends before the beat, but only into the
    // empty space after it: never past the start of the next clip on its track, so that a note edit never makes one clip
    // cover another. The clip read again where it grew (clip then points at what the song holds now).
    void growToHold (Song& song, Rooms& rooms, MidiClips& clips, const ClipValue*& clip, BeatPosition end)
    {
        const auto range = getClipRange (*clip);

        if (end <= range.getEnd() + tinyBeats)
            return;

        const auto bars = getBars (song);
        auto newEnd = bars.getBarStart (bars.getBarAt (end - tinyBeats) + 1);

        if (const auto roomEnd = rooms.findEnd (*clip))
            newEnd = std::min (newEnd, *roomEnd);

        if (newEnd > range.getEnd() + tinyBeats)
        {
            setClipPlace (song, clip->id, range.getStart(), newEnd - range.getStart(), clip->offset);

            if (const auto* now = clips.readAgain (clip->id))
                clip = now;
        }
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

ClipNotes getClipNotesValue (const Song& song, ClipID clip)
{
    // None of a song that keeps everything in its tree (one made to be read or checked: Song::getValues()), whose clips are no
    // clips of the song's (Clips.h).
    const auto* values = song.getValues();
    const auto* value = values != nullptr ? values->findClipNotes (clip) : nullptr;
    return value != nullptr ? *value : ClipNotes();
}

void setClipNotesValue (Song& song, ClipID clip, const ClipNotes& value)
{
    auto* values = song.getValues();
    jassert (values != nullptr); // a song made to be read or checked isn't edited

    if (values != nullptr && isMidiClip (song, clip))
        values->setClipNotes (song, clip, value);
}

namespace
{
    // A note as getNoteData() gives it, its clip read once for all the notes read with the same clips.
    std::optional<NoteData> readNoteData (const Song& song, MidiClips& clips, const NoteRef& ref)
    {
        const auto* clip = clips.find (ref.clip);

        if (const auto note = findNoteValues (song, clip, ref.note))
            return toNoteData (*clip, *note);

        return std::nullopt;
    }
} // namespace

std::optional<NoteData> getNoteData (const Song& song, const NoteRef& ref)
{
    MidiClips clips (song);
    return readNoteData (song, clips, ref);
}

std::vector<NoteData> getNoteData (const Song& song, const std::vector<NoteRef>& notes)
{
    std::vector<NoteData> data;
    MidiClips clips (song);

    for (const auto& ref : notes)
        if (const auto note = readNoteData (song, clips, ref))
            data.push_back (*note);

    return data;
}

std::vector<NoteData> getClipNotes (const Song& song, ClipID clip)
{
    std::vector<NoteData> data;

    if (const auto value = findMidiClip (song, clip))
        forEachNote (getClipNotesValue (song, clip), [&] (const NoteValues& note) { data.push_back (toNoteData (*value, note)); });

    return data;
}

namespace
{
    // Whether a clip plays a note: some of it is in the part of the song the clip covers.
    bool plays (const ClipValue& clip, const NoteData& note)
    {
        const auto range = getClipRange (clip);
        return note.getEnd() > range.getStart() + tinyBeats && note.start < range.getEnd() - tinyBeats;
    }

    // How far back a note's start can go: to its clip's start, and not at all if it starts before it already.
    BeatPosition getEarliestStart (const ClipValue& clip, BeatPosition startNow)
    {
        return std::min (startNow, clip.start);
    }

    // The latest a note of a clip can start and still play: the shortest note before where the clip can grow to, the next
    // clip on its track (growToHold()). None while nothing keeps the clip from growing to hold the note.
    std::optional<BeatPosition> findLatestStart (Rooms& rooms, const ClipValue& clip)
    {
        if (const auto roomEnd = rooms.findEnd (clip))
            return std::max (*roomEnd, getClipRange (clip).getEnd()) - getMinimumNoteLength();

        return std::nullopt;
    }

    // Where an edit puts a note's start, kept where its clip plays the note: no earlier than getEarliestStart() and no later
    // than findLatestStart(), nor than it is if it is later already. An edit of a note that plays never leaves it in its
    // clip without playing, where no editor would show it.
    BeatPosition keepPlayed (Rooms& rooms, const ClipValue& clip, BeatPosition startNow, BeatPosition wanted)
    {
        if (const auto latest = findLatestStart (rooms, clip))
            wanted = std::min (wanted, std::max (startNow, *latest));

        return std::max (wanted, getEarliestStart (clip, startNow));
    }

    // Whether a new note starting at a song beat would play: in the clip, or after it where it can grow to hold the note
    // (growToHold()): before the next clip on its track.
    bool canPlayAt (Rooms& rooms, const ClipValue* clip, BeatPosition start)
    {
        if (clip == nullptr)
            return false;

        if (start < getClipRange (*clip).getEnd() - tinyBeats)
            return true;

        const auto roomEnd = rooms.findEnd (*clip);
        return ! roomEnd.has_value() || start < *roomEnd - tinyBeats;
    }
} // namespace

std::optional<BeatPosition> getLatestNoteStart (const Song& song, ClipID clip)
{
    const auto value = findMidiClip (song, clip);
    return value.has_value() ? getLatestNoteStart (song, *value) : std::nullopt;
}

std::optional<BeatPosition> getLatestNoteStart (const Song& song, const ClipValue& clip)
{
    Rooms rooms (song);
    return findLatestStart (rooms, clip);
}

std::vector<NoteData> getClipNotesPlayed (const Song& song, ClipID clip)
{
    std::vector<NoteData> data;
    const auto value = findMidiClip (song, clip);

    if (! value.has_value())
        return data;

    forEachNote (getClipNotesValue (song, clip),
                 [&] (const NoteValues& values)
                 {
                     if (const auto note = toNoteData (*value, values); plays (*value, note))
                         data.push_back (note);
                 });

    return data;
}

namespace
{
    bool isPlayedIn (const Song& song, MidiClips& clips, const NoteRef& ref)
    {
        const auto note = readNoteData (song, clips, ref);
        return note.has_value() && plays (*clips.find (ref.clip), *note);
    }
} // namespace

bool isNotePlayed (const Song& song, const NoteRef& ref)
{
    MidiClips clips (song);
    return isPlayedIn (song, clips, ref);
}

bool canPlayNoteAt (const Song& song, ClipID clip, BeatPosition start)
{
    Rooms rooms (song);
    const auto value = findMidiClip (song, clip);
    return canPlayAt (rooms, value.has_value() ? &*value : nullptr, start);
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
    MidiClips clips (*song);

    for (const auto& ref : selection.getNotes())
        if (std::find (selection.getNoteClips().begin(), selection.getNoteClips().end(), ref.clip) != selection.getNoteClips().end())
            if (isPlayedIn (*song, clips, ref))
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
    // Of the values given, the velocity, the flags and the channel are the note's (getValuesKept()).
    NoteRef addNoteTo (Song& song, NotesEdit& edit, Rooms& rooms, ClipID clip, NoteID wanted, int pitch, BeatPosition start,
                       BeatDuration length, NoteValues values, std::optional<BeatPosition> earliest = {},
                       std::optional<BeatPosition> latest = {})
    {
        const auto* value = edit.findClip (clip);
        auto* group = edit.find (value);

        if (group == nullptr)
            return {};

        start = std::max (std::min (start, latest.value_or (start)), earliest.value_or (value->start));
        length = std::max (length, getMinimumNoteLength());
        growToHold (song, rooms, edit.getClips(), value, start + length);
        values.id = wanted.isValid() && group->after.find (wanted) == nullptr ? wanted : NoteID::fromRaw (song.makeID());
        values.pitch = clampPitch (pitch);
        values.start = toClipBeat (*value, start);
        values.length = length;
        values.velocity = juce::jlimit (1, 127, values.velocity);
        values.channel = keepChannel (values.channel);
        group->after = withNoteAdded (group->after, values);
        return { clip, values.id };
    }
} // namespace

NoteRef addNote (Song& song, ClipID clip, int pitch, BeatPosition start, BeatDuration length, int velocity, int channel)
{
    NotesEdit edit (song);
    Rooms rooms (song);
    const auto note = addNoteTo (song, edit, rooms, clip, {}, pitch, start, length, { .velocity = velocity, .channel = channel });
    edit.commit();
    return note;
}

std::vector<NoteRef> addNotes (Song& song, const std::vector<NoteData>& notes)
{
    // As addNote() adds each, one after another: where each clip can grow to is the same for all of them (Rooms), and each
    // alias group's value is read once and set once.
    NotesEdit edit (song);
    Rooms rooms (song);
    std::vector<NoteRef> added;
    added.reserve (notes.size());

    for (const auto& note : notes)
        added.push_back (addNoteTo (song, edit, rooms, note.clip, {}, note.pitch, note.start, note.length, getValuesKept (note)));

    edit.commit();
    return added;
}

void growClipForNote (Song& song, ClipID clip, BeatPosition start, BeatDuration length)
{
    MidiClips clips (song);
    const auto* value = clips.find (clip);

    if (value == nullptr)
        return;

    // Where addNoteTo() puts the note, and what it grows the clip to hold.
    Rooms rooms (song);
    growToHold (song, rooms, clips, value, std::max (start, value->start) + std::max (length, getMinimumNoteLength()));
}

juce::String getControllerName (int number)
{
    // The names players know, where JUCE's are old or long ("Modulation Wheel (coarse)", "Hold Pedal (on/off)").
    switch (number)
    {
        case 1:
            return "Mod Wheel";
        case 7:
            return "Volume";
        case 10:
            return "Pan";
        case 11:
            return "Expression";
        case 64:
            return "Sustain Pedal";
        case 66:
            return "Sostenuto Pedal";
        default:
            break;
    }

    // JUCE's otherwise: a coarse value and a switch named for what they control alone, a fine one so ("Breath controller
    // Fine"), and the sound controllers without the word every one of them has ("Brightness", not "Sound Brightness").
    const auto* text = juce::MidiMessage::getControllerName (number);

    if (text == nullptr)
        return {};

    auto name = juce::String (text).replace (" (coarse)", {}).replace (" (on/off)", {}).replace (" (fine)", " Fine");

    if (name.startsWith ("Sound ") && ! name.startsWith ("Sound Control"))
        name = name.fromFirstOccurrenceOf ("Sound ", false, false);

    return name;
}

juce::String ControlLane::getShortName() const
{
    switch (kind)
    {
        case ControlKind::controller:
        {
            const auto name = getControllerName (number);
            return name.isNotEmpty() ? name : "CC " + juce::String (number);
        }
        case ControlKind::pitchWheel:
            return "Pitch Bend";
        case ControlKind::channelPressure:
            return "Channel Pressure";
        case ControlKind::aftertouch:
            return "Note Pressure";
        case ControlKind::programChange:
            return "Program Change";
    }

    return {};
}

juce::String ControlLane::getName() const
{
    auto name = getShortName();

    if (kind == ControlKind::controller && getControllerName (number).isNotEmpty())
        name << " (CC " << number << ")";

    if (channel != 1)
        name << ", Channel " << channel;

    return name;
}

int ControlLane::getHighestValue() const noexcept
{
    return phazite::getHighestValue (kind);
}

int ControlLane::getRest() const noexcept
{
    return kind == ControlKind::pitchWheel ? 8192 : 0;
}

bool ControlLane::canDraw() const noexcept
{
    return kind != ControlKind::aftertouch;
}

juce::String ControlLane::describeValue (int value) const
{
    value = juce::jlimit (0, getHighestValue(), value);

    switch (kind)
    {
        case ControlKind::pitchWheel:
            return getShortName() + " " + juce::String (juce::jlimit (-100, 100, juce::roundToInt ((value - 8192) * 100.0 / 8192.0))) + "%";
        case ControlKind::programChange:
            return "Program " + juce::String (value + 1);
        case ControlKind::controller:
        case ControlKind::channelPressure:
        case ControlKind::aftertouch:
            break;
    }

    return getShortName() + " " + juce::String (value);
}

bool operator< (const ControlLane& a, const ControlLane& b) noexcept
{
    return std::tuple (a.channel, static_cast<int> (a.kind), a.number) < std::tuple (b.channel, static_cast<int> (b.kind), b.number);
}

ControlLane makeControlLane (ControlKind kind, int number, int channel)
{
    return { kind, kind == ControlKind::controller ? juce::jlimit (0, 127, number) : 0, juce::jlimit (1, 16, channel) };
}

std::vector<ControlLane> getUsualControlLanes()
{
    return { { ControlKind::pitchWheel, 0, 1 },      { ControlKind::controller, 1, 1 },   { ControlKind::controller, 64, 1 },
             { ControlKind::controller, 11, 1 },     { ControlKind::controller, 7, 1 },   { ControlKind::controller, 10, 1 },
             { ControlKind::channelPressure, 0, 1 }, { ControlKind::programChange, 0, 1 } };
}

BeatDuration getShortestControlGap()
{
    return BeatDuration::fromBeats (1.0 / 64.0);
}

std::vector<ControlPoint> makeControlRamp (const ControlLane& lane, BeatPosition from, int fromValue, BeatPosition to, int toValue)
{
    if (to < from)
    {
        std::swap (from, to);
        std::swap (fromValue, toValue);
    }

    fromValue = juce::jlimit (0, lane.getHighestValue(), fromValue);
    toValue = juce::jlimit (0, lane.getHighestValue(), toValue);
    std::vector<ControlPoint> points;

    const auto add = [&points] (BeatPosition beat, int value)
    {
        if (points.empty() || points.back().value != value)
            points.push_back ({ {}, beat, value, 0 });
    };

    // A value at each gap from its start, the last a gap or more before its end, then the end's: none closer than a gap.
    const auto length = (to - from).inBeats();
    const auto gap = getShortestControlGap().inBeats();
    const auto steps = length > 0.0 ? (int) std::floor (length / gap + 1.0e-9) : 0;

    for (int i = 0; i < steps; ++i)
        add (from + BeatDuration::fromBeats (i * gap), juce::roundToInt (fromValue + (toValue - fromValue) * (i * gap / length)));

    add (to, toValue);
    return points;
}

namespace
{
    // A value as its clip would hold it, its beat among the clip's notes.
    ControlValues makeControlValues (const ControlValue& each, BeatPosition contentStart)
    {
        return keepWithinKind ({ each.kind, each.number, (each.beat - contentStart).inBeats(), each.value, each.channel });
    }

    // Whether a clip's value is one of a controller on a channel (by its number, if its kind has one).
    bool isControlOf (const ControlValues& control, ControlKind kind, int number, int channel)
    {
        return control.kind == kind && (! hasNumber (kind) || control.number == number) && control.channel == juce::jlimit (1, 16, channel);
    }

} // namespace

void addControl (Song& song, ClipID clip, BeatPosition beat, ControlKind kind, int number, int value, int channel)
{
    NotesEdit edit (song);
    const auto* clipValue = edit.findClip (clip);

    if (auto* group = edit.find (clipValue))
    {
        group->after =
            withControlsAdded (group->after, { makeControlValues ({ beat, kind, number, value, channel }, getContentStart (*clipValue)) });
        edit.commit();
    }
}

void addControls (Song& song, ClipID clip, const std::vector<ControlValue>& values)
{
    const auto clipValue = findMidiClip (song, clip);

    if (! clipValue.has_value() || values.empty())
        return;

    const auto contentStart = getContentStart (*clipValue);
    std::vector<ControlValues> added;
    added.reserve (values.size());

    for (const auto& each : values)
        added.push_back (makeControlValues (each, contentStart));

    setClipNotesValue (song, clip, withControlsAdded (getClipNotesValue (song, clip), added));
}

void removeControls (Song& song, ClipID clip, ControlKind kind, int number, int channel, BeatPosition from, BeatPosition to)
{
    NotesEdit edit (song);
    const auto* clipValue = edit.findClip (clip);
    auto* group = edit.find (clipValue);

    if (group == nullptr)
        return;

    constexpr auto near = 1.0e-9; // in beats
    const auto first = toClipBeat (*clipValue, from).inBeats() - near, last = toClipBeat (*clipValue, to).inBeats() + near;

    group->after =
        withoutControls (group->after, [&] (size_t, const ControlValues& control)
                         { return isControlOf (control, kind, number, channel) && control.beat >= first && control.beat <= last; });
    edit.commit();
}

void removeControl (Song& song, ClipID clip, const ControlValue& control)
{
    NotesEdit edit (song);
    const auto* clipValue = edit.findClip (clip);
    auto* group = edit.find (clipValue);

    if (group == nullptr)
        return;

    constexpr auto near = 1.0e-9; // in beats
    const auto beat = toClipBeat (*clipValue, control.beat).inBeats();
    std::optional<size_t> found; // the last that matches

    forEachControl (group->after,
                    [&] (size_t place, const ControlValues& each)
                    {
                        if (isControlOf (each, control.kind, control.number, control.channel) && std::abs (each.beat - beat) <= near
                            && each.value == control.value)
                            found = place;
                    });

    if (found.has_value())
    {
        group->after = withoutControls (group->after, [&found] (size_t place, const ControlValues&) { return place == *found; });
        edit.commit();
    }
}

std::vector<ControlValue> getClipControls (const Song& song, ClipID clip)
{
    std::vector<ControlValue> values;
    const auto clipValue = findMidiClip (song, clip);

    if (! clipValue.has_value())
        return values;

    const auto contentStart = getContentStart (*clipValue);

    forEachControl (getClipNotesValue (song, clip),
                    [&] (size_t, const ControlValues& control)
                    {
                        values.push_back ({ BeatPosition::fromBeats (contentStart.inBeats() + control.beat), control.kind, control.number,
                                            control.value, control.channel });
                    });

    return values;
}

std::optional<int> findControlValueBefore (const Song& song, ClipID clip, ControlKind kind, int number, int channel, BeatPosition beat)
{
    const auto clipValue = findMidiClip (song, clip);

    if (! clipValue.has_value())
        return std::nullopt;

    const auto contentStart = getContentStart (*clipValue).inBeats();
    std::optional<std::pair<double, int>> last;

    forEachControl (getClipNotesValue (song, clip),
                    [&] (size_t, const ControlValues& control)
                    {
                        if (control.kind != kind || (hasNumber (kind) && control.number != number) || control.channel != channel)
                            return;

                        const auto at = contentStart + control.beat;

                        if (at <= beat.inBeats() + 1.0e-9 && (! last.has_value() || at >= last->first))
                            last = std::pair (at, control.value);
                    });

    return last.has_value() ? std::optional (last->second) : std::nullopt;
}

void scaleClipContent (Song& song, const std::vector<ClipID>& clips, double factor)
{
    NotesEdit edit (song);
    std::set<NotesEdit::Group*> scaled; // each alias group's notes once

    for (const auto clip : clips)
    {
        auto* group = edit.find (clip);

        if (group == nullptr || ! scaled.insert (group).second)
            continue;

        group->after = withEachChanged (
            group->after,
            [factor] (NoteValues& note)
            {
                note.start = BeatPosition::fromBeats (note.start.inBeats() * factor);
                note.length = BeatDuration::fromBeats (note.length.inBeats() * factor);
            },
            [factor] (ControlValues& control) { control.beat *= factor; });
    }

    edit.commit();
}

//==============================================================================
void makeClipNotesLike (Song& song, ClipID clip, const juce::ValueTree& otherClip)
{
    // The other's value, as one value of the clip's group: nothing where it holds that already.
    if (! isMidiClip (song, clip))
        return;

    if (const auto other = readClipNotes (otherClip); ! (getClipNotesValue (song, clip) == other))
        setClipNotesValue (song, clip, other);
}

bool setNoteValue (Song& song, const NoteRef& ref, const juce::Identifier& property, const juce::var& value)
{
    NotesEdit edit (song);
    auto* group = edit.find (ref.clip);
    const auto* note = group != nullptr ? group->after.find (ref.note) : nullptr;

    if (note == nullptr)
        return false;

    // A value of a note's (never its ID, which names it): a property of another name has no place in the note's value.
    auto changed = *note;

    if (property == schema::noteID || ! setNoteProperty (changed, property, value))
        return false;

    group->after = withNotesChanged (group->after, { ref.note }, [&changed] (NoteValues& each) { each = changed; });
    edit.commit();
    return true;
}

void removeClipNotesAlone (Song& song, ClipID clip, const std::vector<NoteID>& notes)
{
    const auto value = findMidiClip (song, clip);

    if (! value.has_value())
        return;

    const std::set<NoteID> gone (notes.begin(), notes.end());
    const auto read = getClipNotesValue (song, clip);
    const auto next = withoutNotes (read, [&gone] (const NoteValues& note) { return gone.count (note.id) > 0; });

    if (next.isSameAs (read))
        return;

    // Holding other notes than its group's, it leaves the group (and the last one left of it leaves it too, as
    // eraseClip() leaves it): an alias group's clips hold one value.
    const Song::OutsideHistory outside (song);

    if (value->link != 0)
    {
        const auto aliases = getAliasClips (song, clip);
        setClipLink (song, clip, 0);

        if (aliases.size() == 2)
            setClipLink (song, aliases[1], 0);
    }

    setClipNotesValue (song, clip, next);
}

//==============================================================================
namespace
{
    // Whether a clip's value is a value of a lane: of its kind and on its channel, and a controller's of its number.
    bool isOfLane (const ControlValues& control, const ControlLane& lane)
    {
        return control.kind == lane.kind && (lane.kind != ControlKind::controller || control.number == lane.number)
               && control.channel == lane.channel;
    }

    // What of a clip's values plays, in song beats, as model/playback/internal/SongPlayer.h reads them: a value's beat is
    // its clip's content start and its own, and it plays from the clip's start up to its end.
    struct PlayedPart
    {
        double contentStart = 0.0, start = 0.0, end = 0.0;

        double getBeat (const ControlValues& control) const { return contentStart + control.beat; }
        bool plays (double beat) const { return beat >= start && beat < end; }
    };

    PlayedPart getPlayedPart (const ClipValue& clip)
    {
        const auto range = getClipRange (clip);
        return { getContentStart (clip).inBeats(), range.getStart().inBeats(), range.getEnd().inBeats() };
    }

    constexpr double nearBeats = 1.0e-9; // a range's ends take what is this close to them

    // A lane's values that play from one song beat to another, both included, in a clip's value: their places among its
    // values, in order; and the value of the lane's that holds at the range's start, the latest that plays before it (the
    // later of two at a beat).
    struct LaneValuesIn
    {
        std::vector<size_t> places;
        std::optional<int> before;
    };

    LaneValuesIn findLaneValues (const ClipNotes& clip, const ControlLane& lane, const PlayedPart& part, double from, double to)
    {
        LaneValuesIn found;
        auto beforeBeat = 0.0;

        forEachControl (clip,
                        [&] (size_t place, const ControlValues& control)
                        {
                            if (! isOfLane (control, lane))
                                return;

                            const auto beat = part.getBeat (control);

                            if (! part.plays (beat))
                                return;

                            if (beat >= from - nearBeats && beat <= to + nearBeats)
                            {
                                found.places.push_back (place);
                            }
                            else if (beat < from && (! found.before.has_value() || beat >= beforeBeat))
                            {
                                found.before = control.value;
                                beforeBeat = beat;
                            }
                        });

        return found;
    }

    // Removes the clip's values at these places (in order) and adds these, as one change of its group's value.
    void changeLaneValues (NotesEdit& edit, NotesEdit::Group& group, const std::vector<size_t>& removed,
                           const std::vector<ControlValues>& added)
    {
        if (removed.empty() && added.empty())
            return;

        auto next = group.after;

        if (! removed.empty())
        {
            auto gone = removed.begin();

            next = withoutControls (next,
                                    [&] (size_t place, const ControlValues&)
                                    {
                                        if (gone == removed.end() || *gone != place)
                                            return false;

                                        ++gone;
                                        return true;
                                    });
        }

        group.after = withControlsAdded (next, added);
        edit.commit();
    }

    // Makes the lane's values at these places in a clip's value the values wanted (in time order, in song beats): those
    // already there as wanted stay where they are, and only the others go or come.
    void putLaneValues (NotesEdit& edit, NotesEdit::Group& group, const std::vector<size_t>& places,
                        const std::vector<ControlPoint>& wanted, const ControlLane& lane, double contentStart)
    {
        // What is there, by its beat as the clip holds it.
        std::vector<std::pair<double, size_t>> there;

        for (size_t i = 0; i < places.size(); ++i)
            there.emplace_back (group.after.controls[places[i]].beat, i);

        std::sort (there.begin(), there.end());
        std::vector<bool> stays (places.size(), false);
        std::vector<ControlValues> added;

        for (const auto& each : wanted)
        {
            const auto control =
                makeControlValues ({ each.beat, lane.kind, lane.number, each.value, lane.channel }, BeatPosition::fromBeats (contentStart));
            auto found = std::lower_bound (there.begin(), there.end(), std::pair { control.beat - nearBeats, size_t {} });

            for (; found != there.end() && found->first <= control.beat + nearBeats; ++found)
                if (! stays[found->second] && group.after.controls[places[found->second]].value == control.value)
                    break;

            if (found != there.end() && found->first <= control.beat + nearBeats)
                stays[found->second] = true;
            else
                added.push_back (control);
        }

        std::vector<size_t> removed;

        for (size_t i = 0; i < places.size(); ++i)
            if (! stays[i])
                removed.push_back (places[i]);

        changeLaneValues (edit, group, removed, added);
    }
} // namespace

std::vector<ControlLane> getControlLanes (const Song& song, const std::vector<ClipID>& clips)
{
    std::set<ControlLane> lanes;

    for (const auto clip : clips)
    {
        const auto value = findMidiClip (song, clip);

        if (! value.has_value())
            continue;

        const auto part = getPlayedPart (*value);

        forEachControl (getClipNotesValue (song, clip),
                        [&] (size_t, const ControlValues& control)
                        {
                            if (part.plays (part.getBeat (control)))
                                lanes.insert (makeControlLane (control.kind, control.number, control.channel));
                        });
    }

    return { lanes.begin(), lanes.end() };
}

std::vector<ControlPoint> getControlValues (const Song& song, const std::vector<ClipID>& clips, const ControlLane& lane)
{
    std::vector<ControlPoint> points;

    for (const auto clip : clips)
    {
        const auto value = findMidiClip (song, clip);

        if (! value.has_value())
            continue;

        const auto part = getPlayedPart (*value);
        const auto first = points.size();

        forEachControl (getClipNotesValue (song, clip),
                        [&] (size_t, const ControlValues& control)
                        {
                            if (isOfLane (control, lane))
                                if (const auto beat = part.getBeat (control); part.plays (beat))
                                    points.push_back ({ clip, BeatPosition::fromBeats (beat),
                                                        juce::jlimit (0, lane.getHighestValue(), control.value),
                                                        juce::jlimit (0, 127, control.number) });
                        });

        std::stable_sort (points.begin() + (std::ptrdiff_t) first, points.end(),
                          [] (const ControlPoint& a, const ControlPoint& b) { return a.beat < b.beat; });
    }

    return points;
}

void setControlValues (Song& song, ClipID clip, const ControlLane& lane, BeatRange range, const std::vector<ControlPoint>& values)
{
    NotesEdit edit (song);
    const auto* clipValue = edit.findClip (clip);

    if (clipValue == nullptr || ! lane.canDraw())
        return;

    const auto part = getPlayedPart (*clipValue);
    const auto from = std::max (range.getStart().inBeats(), part.start), to = range.getEnd().inBeats();

    if (from >= part.end || to < from - nearBeats)
        return;

    // The values to put there, in time order: none outside the range or where the clip doesn't play them, and none closer
    // than the shortest gap after the one before, which takes its value instead.
    auto sorted = values;
    std::stable_sort (sorted.begin(), sorted.end(), [] (const ControlPoint& a, const ControlPoint& b) { return a.beat < b.beat; });
    std::vector<ControlPoint> kept;
    const auto gap = getShortestControlGap().inBeats();

    for (const auto& each : sorted)
    {
        const auto beat = each.beat.inBeats();

        if (beat < from - nearBeats || beat > to + nearBeats || ! part.plays (beat))
            continue;

        const auto value = juce::jlimit (0, lane.getHighestValue(), each.value);

        if (! kept.empty() && beat - kept.back().beat.inBeats() < gap - nearBeats)
            kept.back().value = value;
        else
            kept.push_back ({ clip, each.beat, value, 0 });
    }

    // A value that repeats the one that holds before it is left out. What is there already as it would be put stays as it
    // is, so that a click on the value there changes nothing, and a step of a drag changes only what it draws differently.
    auto& group = *edit.find (clipValue);
    const auto found = findLaneValues (group.after, lane, part, from, to);
    auto previous = found.before;
    std::vector<ControlPoint> wanted;

    for (const auto& each : kept)
    {
        if (previous == each.value)
            continue;

        previous = each.value;
        wanted.push_back (each);
    }

    putLaneValues (edit, group, found.places, wanted, lane, part.contentStart);
}

void deleteControlValues (Song& song, const std::vector<ClipID>& clips, const ControlLane& lane, BeatRange range)
{
    // Clip by clip, each with what it plays: one of a group named after another takes away what it plays of what is left.
    for (const auto clip : clips)
    {
        NotesEdit edit (song);
        const auto* clipValue = edit.findClip (clip);
        auto* group = edit.find (clipValue);

        if (group == nullptr)
            continue;

        const auto part = getPlayedPart (*clipValue);
        changeLaneValues (
            edit, *group,
            findLaneValues (group->after, lane, part, std::max (range.getStart().inBeats(), part.start), range.getEnd().inBeats()).places,
            {});
    }
}

void deleteAllControlValues (Song& song, const std::vector<ClipID>& clips, const ControlLane& lane)
{
    for (const auto clip : clips)
        if (const auto value = findMidiClip (song, clip))
            deleteControlValues (song, { clip }, lane, getClipRange (*value));
}

namespace
{
    // A note of the edit's group placed: at a start wanted (a song beat through the first clip it was named through), a pitch
    // and a length, the start kept where each clip it was named through plays it (keepPlayed()), each of those clips grown
    // to hold it (growToHold()). Of a note named through one clip, the start found as that clip says, and no other changed.
    void placeNoteIn (Song& song, NotesEdit& edit, Rooms& rooms, const std::vector<ClipID>& clips, NoteID id, BeatPosition wanted,
                      int pitch, BeatDuration length)
    {
        std::vector<const ClipValue*> named;

        for (const auto clip : clips)
            if (const auto* value = edit.findClip (clip))
                named.push_back (value);

        auto* group = named.empty() ? nullptr : edit.find (named.front());
        const auto* note = group != nullptr ? group->after.find (id) : nullptr;

        if (note == nullptr)
            return;

        const auto startNow = note->start; // among the clip's notes
        const auto first = keepPlayed (rooms, *named.front(), toSongBeat (*named.front(), startNow), wanted);
        auto start = toClipBeat (*named.front(), first);
        auto movedByAnother = false;

        for (size_t i = 1; i < named.size(); ++i)
        {
            const auto inThis = toSongBeat (*named[i], start);

            if (const auto kept = keepPlayed (rooms, *named[i], toSongBeat (*named[i], startNow), inThis); kept != inThis)
            {
                start = toClipBeat (*named[i], kept);
                movedByAnother = true;
            }
        }

        for (size_t i = 0; i < named.size(); ++i)
            growToHold (song, rooms, edit.getClips(), named[i],
                        (i == 0 && ! movedByAnother ? first : toSongBeat (*named[i], start)) + length);

        group->after = withNotesChanged (group->after, { id },
                                         [start, pitch, length] (NoteValues& each)
                                         {
                                             each.start = start;
                                             each.pitch = clampPitch (pitch);
                                             each.length = length;
                                         });
    }

    // A note of the edit's group made a length (at least getMinimumNoteLength()), each clip it was named through grown to hold
    // it.
    void setNoteLengthIn (Song& song, NotesEdit& edit, Rooms& rooms, const std::vector<ClipID>& clips, NoteID id, BeatDuration length)
    {
        auto* group = clips.empty() ? nullptr : edit.find (clips.front());
        const auto* note = group != nullptr ? group->after.find (id) : nullptr;

        if (note == nullptr)
            return;

        length = std::max (length, getMinimumNoteLength());

        for (const auto clip : clips)
            if (const auto* value = edit.findClip (clip))
                growToHold (song, rooms, edit.getClips(), value, toSongBeat (*value, note->start) + length);

        group->after = withNotesChanged (group->after, { id }, [length] (NoteValues& each) { each.length = length; });
    }

    // A note's values as it is now in the edit: none for a note that is gone.
    std::optional<NoteValues> findInEdit (NotesEdit& edit, const NoteRef& ref)
    {
        if (auto* group = edit.find (ref.clip))
            if (const auto* note = group->after.find (ref.note))
                return *note;

        return std::nullopt;
    }

    // Each note named, each once, changed by change (NoteValues&) in its group's value.
    template <typename Change>
    void changeEach (NotesEdit& edit, const std::vector<NoteRef>& notes, Change&& change)
    {
        for (const auto& ref : notes)
            if (auto* group = edit.find (ref.clip))
                group->after = withNotesChanged (group->after, { ref.note }, change);
    }
} // namespace

void moveNote (Song& song, const NoteRef& ref, BeatPosition start, int pitch)
{
    NotesEdit edit (song);
    Rooms rooms (song);

    if (const auto note = findInEdit (edit, ref))
        placeNoteIn (song, edit, rooms, { ref.clip }, ref.note, start, pitch, note->length);

    edit.commit();
}

void setNoteLength (Song& song, const NoteRef& ref, BeatDuration length)
{
    NotesEdit edit (song);
    Rooms rooms (song);
    setNoteLengthIn (song, edit, rooms, { ref.clip }, ref.note, length);
    edit.commit();
}

void placeNote (Song& song, const NoteRef& ref, BeatPosition start, int pitch, BeatDuration length)
{
    NotesEdit edit (song);
    Rooms rooms (song);
    placeNoteIn (song, edit, rooms, { ref.clip }, ref.note, start, pitch, std::max (length, getMinimumNoteLength()));
    edit.commit();
}

void placeNotes (Song& song, const std::vector<NoteData>& notes)
{
    // Each note once, where it was first named to go, kept where every clip it was named through plays it. One name given
    // twice goes where it was given last (kept where its clip plays it as counted from where it was before the call).
    NotesEdit edit (song);
    Rooms rooms (song);
    const auto named = nameEachOnce (song, getRefs (notes));
    std::map<NoteRef, const NoteData*> wanted;

    for (const auto& note : notes)
        wanted.insert_or_assign (note.getRef(), &note);

    for (const auto& note : named)
    {
        const auto& to = *wanted.at (note.ref);
        placeNoteIn (song, edit, rooms, note.clips, note.ref.note, to.start, to.pitch, std::max (to.length, getMinimumNoteLength()));
    }

    edit.commit();
}

void deleteNotes (Song& song, const std::vector<NoteRef>& notes)
{
    // Each by its ID, all found before any goes; a note's copies in its clip's aliases go with it.
    NotesEdit edit (song);
    std::map<NotesEdit::Group*, std::set<NoteID>> gone;

    for (const auto& ref : withoutAliasCopies (song, notes))
        if (auto* group = edit.find (ref.clip))
            gone[group].insert (ref.note);

    for (auto& [group, ids] : gone)
        group->after = withoutNotes (group->after, [&ids = ids] (const NoteValues& note) { return ids.count (note.id) > 0; });

    edit.commit();
}

void transposeNotes (Song& song, const std::vector<NoteRef>& notes, int semitones)
{
    const auto unique = getNoteData (song, withoutAliasCopies (song, notes));

    // As far as the highest or lowest note can go, so that chords keep their shape.
    for (const auto& note : unique)
        semitones = juce::jlimit (-note.pitch, 127 - note.pitch, semitones);

    if (semitones == 0)
        return;

    NotesEdit edit (song);
    changeEach (edit, getRefs (unique), [semitones] (NoteValues& note) { note.pitch += semitones; });
    edit.commit();
}

void nudgeNotes (Song& song, const std::vector<NoteRef>& notes, BeatDuration amount)
{
    const auto named = nameEachOnce (song, notes);
    Rooms rooms (song);

    // The note nearest its clip's start decides how far back they can go: none at all with one that starts before it. And
    // the one nearest where its clip stops playing it, how far on (keepPlayed()). Each note as it was named, through each clip
    // of its group it was named through: a note named through two clips stays where both play it, whichever comes first.
    MidiClips clips (song);

    for (const auto& note : getNoteData (song, notes))
        amount = keepPlayed (rooms, *clips.find (note.clip), note.start, note.start + amount) - note.start;

    NotesEdit edit (song);

    // Each note where it is in the song (where its clip's content starts, which growing a clip leaves as it is).
    for (const auto& note : named)
        if (const auto now = readNoteData (song, clips, note.ref))
            placeNoteIn (song, edit, rooms, note.clips, note.ref.note, now->start + amount, now->pitch, now->length);

    edit.commit();
}

void changeNoteLengths (Song& song, const std::vector<NoteRef>& notes, BeatDuration change)
{
    // Each note once, every clip of its group it was named through grown to hold it.
    NotesEdit edit (song);
    Rooms rooms (song);

    for (const auto& note : nameEachOnce (song, notes))
        if (const auto now = findInEdit (edit, note.ref))
            setNoteLengthIn (song, edit, rooms, note.clips, note.ref.note, now->length + change);

    edit.commit();
}

namespace
{
    bool isOn (const NoteValues& note, int flag)
    {
        return flag == 0 ? note.muted : flag == 1 ? note.slide : note.portamento;
    }

    bool hasFlag (const Song& song, const NoteRef& ref, int flag)
    {
        if (! isMidiClip (song, ref.clip))
            return false;

        const auto value = getClipNotesValue (song, ref.clip);
        const auto* note = value.find (ref.note);
        return note != nullptr && isOn (*note, flag);
    }

    void toggleNoteFlags (Song& song, const std::vector<NoteRef>& notes, int flag)
    {
        NotesEdit edit (song);

        toggleAll (
            withoutAliasCopies (song, notes), [&song, flag] (const NoteRef& ref) { return hasFlag (song, ref, flag); },
            [&edit, flag] (const NoteRef& ref, bool on)
            {
                changeEach (edit, { ref },
                            [flag, on] (NoteValues& note) { (flag == 0   ? note.muted
                                                             : flag == 1 ? note.slide
                                                                         : note.portamento) = on; });
            });

        edit.commit();
    }

    constexpr int mutedFlag = 0, slideFlag = 1, portamentoFlag = 2;
} // namespace

bool areAllNotesMuted (const Song& song, const std::vector<NoteRef>& notes)
{
    return areAllOn (notes, [&song] (const NoteRef& ref) { return hasFlag (song, ref, mutedFlag); });
}

void toggleNotesMuted (Song& song, const std::vector<NoteRef>& notes)
{
    toggleNoteFlags (song, notes, mutedFlag);
}

bool areAllNotesSlides (const Song& song, const std::vector<NoteRef>& notes)
{
    return areAllOn (notes, [&song] (const NoteRef& ref) { return hasFlag (song, ref, slideFlag); });
}

void toggleNotesSlide (Song& song, const std::vector<NoteRef>& notes)
{
    toggleNoteFlags (song, notes, slideFlag);
}

bool areAllNotesPortamento (const Song& song, const std::vector<NoteRef>& notes)
{
    return areAllOn (notes, [&song] (const NoteRef& ref) { return hasFlag (song, ref, portamentoFlag); });
}

void toggleNotesPortamento (Song& song, const std::vector<NoteRef>& notes)
{
    toggleNoteFlags (song, notes, portamentoFlag);
}

void setNoteVelocities (Song& song, const std::vector<NoteRef>& notes, int velocity)
{
    const auto clamped = juce::jlimit (1, 127, velocity);
    NotesEdit edit (song);
    changeEach (edit, withoutAliasCopies (song, notes), [clamped] (NoteValues& note) { note.velocity = clamped; });
    edit.commit();
}

void setNoteChannels (Song& song, const std::vector<NoteRef>& notes, int channel)
{
    const auto clamped = keepChannel (channel);
    NotesEdit edit (song);
    changeEach (edit, withoutAliasCopies (song, notes), [clamped] (NoteValues& note) { note.channel = clamped; });
    edit.commit();
}

void changeNoteVelocities (Song& song, const std::vector<NoteRef>& notes, int change)
{
    if (notes.empty())
        return;

    const UndoStep step (song, "Change Velocity");
    NotesEdit edit (song);
    changeEach (edit, withoutAliasCopies (song, notes),
                [change] (NoteValues& note) { note.velocity = juce::jlimit (1, 127, note.velocity + change); });
    edit.commit();
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
    NotesEdit edit (song);
    const auto* clipValue = edit.findClip (clip);
    auto* group = edit.find (clipValue);

    if (group == nullptr)
        return;

    const auto contentStart = getContentStart (*clipValue);
    std::vector<NoteValues> values;

    for (const auto& data : notes)
    {
        auto note = getValuesKept (data);
        note.id = NoteID::fromRaw (song.makeID());
        note.pitch = clampPitch (data.pitch);
        note.start = BeatPosition::fromBeats ((data.start - contentStart).inBeats());
        note.length = data.length;
        note.velocity = juce::jlimit (1, 127, data.velocity);
        values.push_back (note);
    }

    auto next = makeClipNotes (values);
    next.controls = group->after.controls;
    group->after = next;
    edit.commit();
}

std::vector<NoteRef> replaceNotes (Song& song, const std::vector<NoteRef>& old, const std::vector<NoteData>& replacements)
{
    // Each old note once, however many clips of its alias group name it: what comes of it is taken through one of its names,
    // the first named that any replacement carries (the first named, if none does), and what comes of it through its other
    // names is left out. The note is replaced once.
    const auto named = nameEachOnce (song, old);
    std::vector<NoteRef> unique;  // each old note by the name its replacements are taken through
    std::set<NoteRef> namedAgain; // an old note's other names
    {
        std::set<NoteRef> carried;

        for (const auto& data : replacements)
            carried.insert (data.getRef());

        for (const auto& note : named)
        {
            auto taken = note.ref;

            for (const auto clip : note.clips)
            {
                if (carried.count ({ clip, note.ref.note }) > 0)
                {
                    taken = { clip, note.ref.note };
                    break;
                }
            }

            unique.push_back (taken);

            for (const auto clip : note.clips)
                if (clip != taken.clip)
                    namedAgain.insert ({ clip, note.ref.note });
        }
    }

    // Where the old ones started: what comes of one that started before its clip did (a note a split or a trim cut) goes no
    // earlier, rather than to its clip's start.
    std::unordered_map<NoteRef, BeatPosition> oldStarts;

    for (const auto& note : getNoteData (song, unique))
        oldStarts[note.getRef()] = note.start;

    NotesEdit edit (song);
    std::map<NotesEdit::Group*, std::set<NoteID>> gone;

    for (const auto& ref : unique)
        if (auto* group = edit.find (ref.clip))
            gone[group].insert (ref.note);

    for (auto& [group, ids] : gone)
        group->after = withoutNotes (group->after, [&ids = ids] (const NoteValues& note) { return ids.count (note.id) > 0; });

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
        if (namedAgain.count (data.getRef()) > 0)
            continue;

        const auto* clip = edit.findClip (data.clip);
        const auto* group = edit.find (clip);
        const auto oldStart = oldStarts.find (data.getRef());
        const auto comesOfOne = oldStart != oldStarts.end();
        const auto replacesOne = comesOfOne && group != nullptr && group->after.find (data.id) == nullptr;

        if (! comesOfOne
            && (data.getEnd() <= (clip != nullptr ? clip->start : BeatPosition()) + tinyBeats || ! canPlayAt (rooms, clip, data.start)))
            continue;

        std::optional<BeatPosition> earliest, latest;

        if (comesOfOne && clip != nullptr)
            earliest = getEarliestStart (*clip, oldStart->second);

        if (replacesOne && clip != nullptr)
            if (const auto latestPlayed = findLatestStart (rooms, *clip))
                latest = std::max (oldStart->second, *latestPlayed);

        if (const auto note = addNoteTo (song, edit, rooms, data.clip, data.id, data.pitch, data.start, data.length, getValuesKept (data),
                                         earliest, latest);
            note.clip.isValid())
            added.push_back (note);
    }

    edit.commit();
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
