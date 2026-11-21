// SPDX-License-Identifier: AGPL-3.0-or-later
// A spike, not a part of Phazite: the hard parts of a song kept as immutable values (the immer library, Boost Software
// License 1.0), written for one area, the notes, and measured beside the song's own, for the question whether the song
// should become an immutable value (docs/design/decisions.md, "Engineering"). Built only where CMake is told where immer
// is (-DPHAZITE_IMMER_SPIKE_DIR=<its folder>), which no preset does: immer is no dependency of Phazite's.
//   phazite_tests.exe --test-suite=benchmark --no-skip --test-case="*hard parts*"
// ImmerSpikeBenchmarks.cpp measured the notes' storage alone. This one writes what stood between that and a model:
// - the song as a value: its tracks, their MIDI clips and the clips' notes, with its index (each clip's place by its ID)
//   and its alias groups as parts of the value;
// - the notes' edits by the model's own rules (Notes.cpp: a note kept where its clip plays it, a clip grown to hold a
//   note, an alias given what its clip gets), each a function from one song to the next;
// - a history of roots, with a gesture (a drag: one step however many changes, or given up);
// - what tells of a change: the two roots compared, which names the clips and tracks changed, as the song's listeners do
//   today, and the notes changed, which they don't;
// - a track's notes compiled for the engine from the value;
// - a copy for another thread (a save, a render), which is the root;
// - two lines merged by the versions' rules (SongMerge.h), for tracks, clips and notes.
// Each is checked against what the song itself comes to (the song's tree read as a value and compared), then timed in
// turns with it. The song is a project's, with everything that listens to it (its index, what plays it, its context).
// What it still leaves out: every other area (devices, automation, the mixer), tracks and clips added, moved and taken
// away, controllers' values, the UI reading from values, and a note named twice in an edit (withoutAliasCopies()).
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "engine/nodes/NoteSequence.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/NoteElements.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongMerge.h"

#include <immer/algorithm.hpp>
#include <immer/flex_vector.hpp>
#include <immer/flex_vector_transient.hpp>
#include <immer/map.hpp>
#include <immer/map_transient.hpp>
#include <immer/vector.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if JUCE_WINDOWS
 #include <malloc.h>
#endif

namespace
{
namespace schema = phazite::schema;
using ID = std::uint64_t;
using Sequence = phazite::engine::NoteSequence;

constexpr double tiny = 1.0e-6; // the model's own (Clips.cpp: tinyBeats)

double millisecondsOf (const std::function<void()>& f)
{
    const auto start = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
}

std::int64_t getHeapBytesInUse()
{
   #if JUCE_WINDOWS
    _HEAPINFO info;
    info._pentry = nullptr;
    std::int64_t used = 0;

    while (_heapwalk (&info) == _HEAPOK)
        if (info._useflag == _USEDENTRY)
            used += (std::int64_t) info._size;

    return used;
   #else
    return 0;
   #endif
}

template <typename Value>
bool isSame (const Value& a, const Value& b)
{
    if constexpr (std::is_floating_point_v<Value>)
        return juce::exactlyEqual (a, b);
    else
        return a == b;
}

//==============================================================================
// The song as a value.
struct ValueNote
{
    ID id = 0;
    int pitch = 60;
    double start = 0.0, length = 1.0; // among its clip's notes
    int velocity = 100;
    bool muted = false, slide = false, portamento = false;
    std::uint8_t channel = 1;

    bool operator== (const ValueNote&) const = default;
};

// A clip's notes: in the clip's order, and where each is by its ID.
struct ValueNotes
{
    immer::flex_vector<ValueNote> inOrder;
    immer::map<ID, std::uint32_t> places;

    const ValueNote* find (ID id) const
    {
        const auto* place = places.find (id);
        return place != nullptr ? &inOrder[*place] : nullptr;
    }

    bool isSharedWith (const ValueNotes& other) const { return inOrder.identity() == other.inOrder.identity(); }
    bool operator== (const ValueNotes& other) const { return inOrder == other.inOrder; }
};

template <typename Notes>
ValueNotes makeNotes (const Notes& notes)
{
    auto order = immer::flex_vector<ValueNote>().transient();
    auto places = immer::map<ID, std::uint32_t>().transient();

    for (const auto& note : notes)
    {
        places.set (note.id, (std::uint32_t) order.size());
        order.push_back (note);
    }

    return { order.persistent(), places.persistent() };
}

struct ValueClip
{
    ID id = 0;
    double start = 0.0, length = 4.0, offset = 0.0;
    bool muted = false;
    ID link = 0; // its alias group's, or none: the group's clips hold the same notes, which here are one value
    ValueNotes notes;

    double getEnd() const { return start + length; }
    double getContentStart() const { return start - offset; }
    bool operator== (const ValueClip&) const = default;
};

struct ValueTrack
{
    ID id = 0;
    immer::flex_vector<ValueClip> clips; // its MIDI clips, in the track's order

    bool operator== (const ValueTrack&) const = default;
};

struct ValuePlace
{
    std::uint32_t track = 0, clip = 0;
};

// The song: its tracks, and its index as a part of it (each clip's place and each track's by its ID, each alias group's
// clips), which a change of notes leaves as it is, so that the song before and the song after share it.
struct ValueSong
{
    immer::flex_vector<ValueTrack> tracks;
    immer::map<ID, ValuePlace> clipPlaces;
    immer::map<ID, std::uint32_t> trackPlaces;
    immer::map<ID, immer::vector<ID>> aliases;

    const ValueClip* findClip (ID id) const
    {
        const auto* place = clipPlaces.find (id);
        return place != nullptr ? &tracks[place->track].clips[place->clip] : nullptr;
    }

    const ValueTrack* findTrack (ID id) const
    {
        const auto* place = trackPlaces.find (id);
        return place != nullptr ? &tracks[*place] : nullptr;
    }
};

ValueSong withIndex (immer::flex_vector<ValueTrack> tracks)
{
    ValueSong song { std::move (tracks), {}, {}, {} };
    auto clipPlaces = song.clipPlaces.transient();
    auto trackPlaces = song.trackPlaces.transient();
    std::map<ID, std::vector<ID>> groups;
    std::uint32_t t = 0;

    for (const auto& track : song.tracks)
    {
        std::uint32_t c = 0;
        trackPlaces.set (track.id, t);

        for (const auto& clip : track.clips)
        {
            clipPlaces.set (clip.id, { t, c++ });

            if (clip.link != 0)
                groups[clip.link].push_back (clip.id);
        }

        ++t;
    }

    song.clipPlaces = clipPlaces.persistent();
    song.trackPlaces = trackPlaces.persistent();

    for (const auto& [link, clips] : groups)
        song.aliases = song.aliases.set (link, immer::vector<ID> (clips.begin(), clips.end()));

    return song;
}

//==============================================================================
// A song's tree read as a value: what a project's file would be read into, and what the checks compare with.
void readTracks (const juce::ValueTree& element, std::vector<ValueTrack>& tracks, std::unordered_map<ID, ValueNotes>& byLink)
{
    for (const auto& child : element)
    {
        if (child.hasType (schema::TRACK))
        {
            std::vector<ValueClip> clips;

            for (const auto& clip : child)
            {
                if (! clip.hasType (schema::CLIP) || clip[schema::clipKind].toString() != schema::clipIsMidi)
                    continue;

                const auto link = (ID) (juce::int64) clip[schema::clipLink];
                const auto shared = byLink.find (link);
                ValueNotes notes;

                if (link != 0 && shared != byLink.end())
                {
                    notes = shared->second;
                }
                else
                {
                    std::vector<ValueNote> read;
                    phazite::forEachNote (
                        clip,
                        [&read] (const phazite::NoteElement& note)
                        {
                            read.push_back ({ note.getID().getRaw(), note.getPitch(), note.getStart().inBeats(), note.getLength().inBeats(),
                                              note.getVelocity(), note.has (phazite::NoteFlag::muted), note.has (phazite::NoteFlag::slide),
                                              note.has (phazite::NoteFlag::portamento), (std::uint8_t) note.getChannel() });
                        });
                    notes = makeNotes (read);

                    if (link != 0)
                        byLink[link] = notes;
                }

                clips.push_back ({ phazite::getClipID (clip).getRaw(), (double) clip[schema::clipStart], (double) clip[schema::clipLength],
                                   (double) clip.getProperty (schema::clipOffset, 0.0), (bool) clip[schema::clipMute], link, notes });
            }

            tracks.push_back ({ (ID) (juce::int64) child[schema::trackID], immer::flex_vector<ValueClip> (clips.begin(), clips.end()) });
        }

        if (child.hasType (schema::TRACK) || child.hasType (schema::TRACKS))
            readTracks (child, tracks, byLink);
    }
}

ValueSong toValue (const juce::ValueTree& songState)
{
    std::vector<ValueTrack> tracks;
    std::unordered_map<ID, ValueNotes> byLink;
    readTracks (songState, tracks, byLink);
    return withIndex (immer::flex_vector<ValueTrack> (tracks.begin(), tracks.end()));
}

// Where two songs differ, the first place found, or nothing: with the notes of each clip in any order, for a merge, which
// puts what it adds where it likes.
juce::String describeDifference (const ValueSong& a, const ValueSong& b, bool anyNoteOrder)
{
    if (a.tracks.size() != b.tracks.size())
        return "tracks: " + juce::String ((int) a.tracks.size()) + " and " + juce::String ((int) b.tracks.size());

    for (size_t t = 0; t < a.tracks.size(); ++t)
    {
        const auto& ta = a.tracks[t];
        const auto& tb = b.tracks[t];
        const auto track = "track " + juce::String ((juce::int64) ta.id);

        if (ta.id != tb.id || ta.clips.size() != tb.clips.size())
            return track + ": another track, or other clips (" + juce::String ((int) ta.clips.size()) + " and "
                   + juce::String ((int) tb.clips.size()) + ")";

        for (size_t c = 0; c < ta.clips.size(); ++c)
        {
            const auto& ca = ta.clips[c];
            const auto& cb = tb.clips[c];
            const auto clip = track + ", clip " + juce::String ((juce::int64) ca.id);

            if (ca.id != cb.id || ! isSame (ca.start, cb.start) || ! isSame (ca.length, cb.length) || ! isSame (ca.offset, cb.offset)
                || ca.muted != cb.muted || ca.link != cb.link)
                return clip + ": its own values (length " + juce::String (ca.length) + " and " + juce::String (cb.length) + ")";

            std::vector<ValueNote> na (ca.notes.inOrder.begin(), ca.notes.inOrder.end()),
                nb (cb.notes.inOrder.begin(), cb.notes.inOrder.end());

            if (anyNoteOrder)
                for (auto* notes : { &na, &nb })
                    std::sort (notes->begin(), notes->end(), [] (const ValueNote& x, const ValueNote& y) { return x.id < y.id; });

            if (na.size() != nb.size())
                return clip + ": " + juce::String ((int) na.size()) + " notes and " + juce::String ((int) nb.size());

            for (size_t n = 0; n < na.size(); ++n)
                if (! (na[n] == nb[n]))
                    return clip + ", note " + juce::String ((int) n) + ": " + juce::String ((juce::int64) na[n].id) + " pitch "
                           + juce::String (na[n].pitch) + " start " + juce::String (na[n].start, 6) + " and "
                           + juce::String ((juce::int64) nb[n].id) + " pitch " + juce::String (nb[n].pitch) + " start "
                           + juce::String (nb[n].start, 6);
        }
    }

    return {};
}

//==============================================================================
// The rules of an edit of notes, as Notes.cpp has them, on values.

// Where a clip can grow to without covering another on its track (findRoomEnd()).
std::optional<double> findRoomEnd (const ValueTrack& track, const ValueClip& clip)
{
    std::optional<double> roomEnd;

    for (const auto& other : track.clips)
    {
        if (other.id == clip.id || other.getEnd() <= clip.getEnd() + tiny)
            continue;

        const auto covered = std::max (other.start, clip.getEnd());
        roomEnd = roomEnd.has_value() ? std::min (*roomEnd, covered) : covered;
    }

    return roomEnd;
}

// Where an edit puts a note's start, kept where its clip plays the note (keepPlayed()).
double keepPlayed (const ValueClip& clip, const std::optional<double>& roomEnd, double startNow, double wanted)
{
    if (roomEnd.has_value())
        wanted = std::min (wanted, std::max (startNow, std::max (*roomEnd, clip.getEnd()) - phazite::getMinimumNoteLength().inBeats()));

    return std::max (wanted, std::min (startNow, clip.start));
}

// The clip's length once it holds a note that ends at a song beat (growToHold()): to the end of that beat's bar, of four
// beats here, and no further than its room.
double getLengthToHold (const ValueClip& clip, const std::optional<double>& roomEnd, double end)
{
    if (end <= clip.getEnd() + tiny)
        return clip.length;

    auto newEnd = (std::floor ((end - tiny) / 4.0) + 1.0) * 4.0;

    if (roomEnd.has_value())
        newEnd = std::min (newEnd, *roomEnd);

    return newEnd > clip.getEnd() + tiny ? newEnd - clip.start : clip.length;
}

template <typename Change>
ValueSong withClip (ValueSong song, ID clip, Change&& change)
{
    if (const auto* place = song.clipPlaces.find (clip))
    {
        auto tracks = std::move (song.tracks);
        song.tracks = std::move (tracks).update (place->track,
                                                 [&] (ValueTrack track)
                                                 {
                                                     auto clips = std::move (track.clips);
                                                     track.clips = std::move (clips).update (place->clip,
                                                                                             [&] (ValueClip changed)
                                                                                             {
                                                                                                 change (changed);
                                                                                                 return changed;
                                                                                             });
                                                     return track;
                                                 });
    }

    return song;
}

// A clip given other notes and a length, and each of its aliases the notes: one value, which they share.
ValueSong withNotes (ValueSong song, ID clip, const ValueNotes& notes, double length)
{
    const auto* found = song.findClip (clip);

    if (found == nullptr)
        return song;

    const auto link = found->link;
    song = withClip (std::move (song), clip,
                     [&] (ValueClip& changed)
                     {
                         changed.notes = notes;
                         changed.length = length;
                     });

    if (link != 0)
        if (const auto* group = song.aliases.find (link))
            for (const auto other : immer::vector<ID> (*group))
                if (other != clip)
                    song = withClip (std::move (song), other, [&] (ValueClip& changed) { changed.notes = notes; });

    return song;
}

// The notes named, by their clips, in the order named: where each is among the refs.
std::vector<std::pair<ID, std::vector<size_t>>> groupByClip (const std::vector<phazite::NoteRef>& refs)
{
    std::vector<std::pair<ID, std::vector<size_t>>> groups;
    std::unordered_map<ID, size_t> known;

    for (size_t i = 0; i < refs.size(); ++i)
    {
        const auto clip = refs[i].clip.getRaw();
        const auto [found, isNew] = known.try_emplace (clip, groups.size());

        if (isNew)
            groups.push_back ({ clip, {} });

        groups[found->second].second.push_back (i);
    }

    return groups;
}

// What a change of a clip's notes knows of the clip, and tells of it: the song beat the clip must reach.
struct ClipEdit
{
    const ValueClip& clip;
    std::optional<double> roomEnd;
    double end = 0.0;
};

// The notes named changed, each clip's in one batch: change (the clip's edit, the note, its place among the refs).
template <typename Change>
ValueSong changeNotes (const ValueSong& song, const std::vector<phazite::NoteRef>& refs, Change&& change)
{
    auto next = song;

    for (const auto& [clipID, places] : groupByClip (refs))
    {
        const auto* place = next.clipPlaces.find (clipID);

        if (place == nullptr)
            continue;

        const auto track = next.tracks[place->track];
        const auto clip = track.clips[place->clip];
        ClipEdit edit { clip, findRoomEnd (track, clip), clip.getEnd() };
        auto notes = clip.notes.inOrder.transient();

        for (const auto i : places)
            if (const auto* at = clip.notes.places.find (refs[i].note.getRaw()))
                notes.update (*at,
                              [&] (ValueNote note)
                              {
                                  change (edit, note, i);
                                  return note;
                              });

        next =
            withNotes (std::move (next), clipID, { notes.persistent(), clip.notes.places }, getLengthToHold (clip, edit.roomEnd, edit.end));
    }

    return next;
}

// Where a note is to go, in the song (placeNotes()'s NoteData).
struct Placement
{
    double start = 0.0;
    int pitch = 60;
    double length = 1.0;
};

ValueSong placeNotes (const ValueSong& song, const std::vector<phazite::NoteRef>& refs, const std::vector<Placement>& wanted)
{
    return changeNotes (song, refs,
                        [&wanted] (ClipEdit& edit, ValueNote& note, size_t i)
                        {
                            const auto contentStart = edit.clip.getContentStart();
                            const auto start = keepPlayed (edit.clip, edit.roomEnd, contentStart + note.start, wanted[i].start);
                            note.length = std::max (wanted[i].length, phazite::getMinimumNoteLength().inBeats());
                            edit.end = std::max (edit.end, start + note.length);
                            note.start = start - contentStart;
                            note.pitch = juce::jlimit (0, 127, wanted[i].pitch);
                        });
}

ValueSong transposeNotes (const ValueSong& song, const std::vector<phazite::NoteRef>& refs, int semitones)
{
    // As far as the highest or lowest note can go, so that chords keep their shape.
    for (const auto& ref : refs)
        if (const auto* clip = song.findClip (ref.clip.getRaw()))
            if (const auto* note = clip->notes.find (ref.note.getRaw()))
                semitones = juce::jlimit (-note->pitch, 127 - note->pitch, semitones);

    if (semitones == 0)
        return song;

    return changeNotes (song, refs, [semitones] (ClipEdit&, ValueNote& note, size_t) { note.pitch += semitones; });
}

ValueSong nudgeNotes (const ValueSong& song, const std::vector<phazite::NoteRef>& refs, double amount)
{
    std::unordered_map<ID, std::optional<double>> rooms;

    for (const auto& ref : refs)
    {
        const auto* place = song.clipPlaces.find (ref.clip.getRaw());

        if (place == nullptr)
            continue;

        const auto& track = song.tracks[place->track];
        const auto& clip = track.clips[place->clip];

        if (const auto* note = clip.notes.find (ref.note.getRaw()))
        {
            const auto room = rooms.try_emplace (clip.id, std::nullopt);

            if (room.second)
                room.first->second = findRoomEnd (track, clip);

            const auto start = clip.getContentStart() + note->start;
            amount = keepPlayed (clip, room.first->second, start, start + amount) - start;
        }
    }

    return changeNotes (song, refs,
                        [amount] (ClipEdit& edit, ValueNote& note, size_t)
                        {
                            const auto contentStart = edit.clip.getContentStart();
                            const auto startNow = contentStart + note.start;
                            const auto start = keepPlayed (edit.clip, edit.roomEnd, startNow, startNow + amount);
                            edit.end = std::max (edit.end, start + note.length);
                            note.start = start - contentStart;
                        });
}

// The notes named taken out of their clips: each clip's notes made anew without them, since the places of those after
// them change.
ValueSong deleteNotes (const ValueSong& song, const std::vector<phazite::NoteRef>& refs)
{
    auto next = song;

    for (const auto& [clipID, places] : groupByClip (refs))
    {
        const auto* found = next.findClip (clipID);

        if (found == nullptr)
            continue;

        const auto clip = *found;
        std::unordered_set<ID> gone;

        for (const auto i : places)
            gone.insert (refs[i].note.getRaw());

        auto order = immer::flex_vector<ValueNote>().transient();
        auto where = immer::map<ID, std::uint32_t>().transient();

        immer::for_each (clip.notes.inOrder,
                         [&] (const ValueNote& note)
                         {
                             if (! gone.contains (note.id))
                             {
                                 where.set (note.id, (std::uint32_t) order.size());
                                 order.push_back (note);
                             }
                         });

        next = withNotes (std::move (next), clipID, { order.persistent(), where.persistent() }, clip.length);
    }

    return next;
}

// A note added at a song beat (addNote()), with the ID given.
ValueSong addNote (const ValueSong& song, ID clipID, ID id, int pitch, double start, double length, int velocity)
{
    const auto* place = song.clipPlaces.find (clipID);

    if (place == nullptr)
        return song;

    const auto track = song.tracks[place->track];
    const auto clip = track.clips[place->clip];
    const auto roomEnd = findRoomEnd (track, clip);
    start = std::max (start, clip.start);
    length = std::max (length, phazite::getMinimumNoteLength().inBeats());
    ValueNote note { .id = id,
                     .pitch = juce::jlimit (0, 127, pitch),
                     .start = start - clip.getContentStart(),
                     .length = length,
                     .velocity = juce::jlimit (1, 127, velocity) };
    const ValueNotes notes { clip.notes.inOrder.push_back (note), clip.notes.places.set (id, (std::uint32_t) clip.notes.inOrder.size()) };
    return withNotes (song, clipID, notes, getLengthToHold (clip, roomEnd, start + length));
}

//==============================================================================
// What changed between two songs, by comparing them: what is shared between them is the same without a look inside.
struct ValueChanges
{
    bool tracks = false;           // tracks added, taken away or moved: the spike looks no further
    std::vector<ID> clips {};      // the clips added, changed (their notes included) or removed
    std::vector<ID> clipTracks {}; // the tracks they are on: those whose notes are compiled again
};

bool isSameClip (const ValueClip& a, const ValueClip& b)
{
    return a.id == b.id && isSame (a.start, b.start) && isSame (a.length, b.length) && isSame (a.offset, b.offset) && a.muted == b.muted
           && a.link == b.link && a.notes.isSharedWith (b.notes);
}

void findClipChanges (const ValueTrack& before, const ValueTrack& after, ValueChanges& changes)
{
    const auto count = changes.clips.size();
    auto sameClips = before.clips.size() == after.clips.size();

    if (sameClips)
    {
        auto other = after.clips.begin();

        for (const auto& clip : before.clips)
        {
            const auto& now = *other++;

            if (clip.id != now.id)
            {
                sameClips = false;
                break;
            }

            if (! isSameClip (clip, now))
                changes.clips.push_back (clip.id);
        }
    }

    if (! sameClips)
    {
        changes.clips.resize (count);
        std::unordered_map<ID, const ValueClip*> was;

        for (const auto& clip : before.clips)
            was[clip.id] = &clip;

        for (const auto& clip : after.clips)
        {
            const auto found = was.find (clip.id);

            if (found == was.end() || ! isSameClip (*found->second, clip))
                changes.clips.push_back (clip.id);

            if (found != was.end())
                was.erase (found);
        }

        for (const auto& gone : was)
            changes.clips.push_back (gone.first);
    }

    if (changes.clips.size() > count)
        changes.clipTracks.push_back (after.id);
}

ValueChanges findChanges (const ValueSong& before, const ValueSong& after)
{
    ValueChanges changes;

    if (before.tracks.identity() == after.tracks.identity())
        return changes;

    if (before.tracks.size() != after.tracks.size())
    {
        changes.tracks = true;
        return changes;
    }

    auto other = after.tracks.begin();

    for (const auto& track : before.tracks)
    {
        const auto& now = *other++;

        if (track.id != now.id)
            changes.tracks = true;
        else if (track.clips.identity() != now.clips.identity())
            findClipChanges (track, now, changes);
    }

    return changes;
}

// The notes of a clip that changed, were added or went, between two of its values: what the song's listeners don't tell
// today. The same notes at the same places are compared a block at a time, a block the two share being the same.
std::vector<ID> findChangedNotes (const ValueNotes& before, const ValueNotes& after)
{
    std::vector<ID> changed;

    if (before.isSharedWith (after))
        return changed;

    if (before.places.identity() == after.places.identity())
    {
        using Block = std::pair<const ValueNote*, const ValueNote*>;
        std::vector<Block> was, is;
        immer::for_each_chunk (before.inOrder, [&was] (const ValueNote* first, const ValueNote* last) { was.push_back ({ first, last }); });
        immer::for_each_chunk (after.inOrder, [&is] (const ValueNote* first, const ValueNote* last) { is.push_back ({ first, last }); });
        auto alike = was.size() == is.size();

        for (size_t i = 0; alike && i < was.size(); ++i)
            alike = was[i].second - was[i].first == is[i].second - is[i].first;

        if (alike)
        {
            for (size_t i = 0; i < was.size(); ++i)
                if (was[i].first != is[i].first)
                    for (auto a = was[i].first, b = is[i].first; a != was[i].second; ++a, ++b)
                        if (! (*a == *b))
                            changed.push_back (b->id);

            return changed;
        }
    }

    immer::for_each (after.inOrder,
                     [&] (const ValueNote& note)
                     {
                         if (const auto* was = before.find (note.id); was == nullptr || ! (*was == note))
                             changed.push_back (note.id);
                     });
    immer::for_each (before.inOrder,
                     [&] (const ValueNote& note)
                     {
                         if (after.places.find (note.id) == nullptr)
                             changed.push_back (note.id);
                     });
    return changed;
}

//==============================================================================
// The song's history: the roots before, by their steps' names. Each move tells what changed by comparing the root left
// with the root come to. A gesture is the root at its start: its changes are steps of none, and it ends as one step, or
// with that root put back.
class ValueHistory
{
public:
    explicit ValueHistory (ValueSong start)
        : now (std::move (start))
    {
    }

    const ValueSong& get() const noexcept { return now; }

    ValueChanges change (const juce::String& name, ValueSong next)
    {
        if (! gesture.has_value())
        {
            done.push_back ({ name, now });
            ahead.clear();
        }

        return moveTo (std::move (next));
    }

    void beginGesture (const juce::String& name) { gesture = Step { name, now }; }

    void commitGesture()
    {
        if (gesture->song.tracks.identity() != now.tracks.identity())
        {
            done.push_back (std::move (*gesture));
            ahead.clear();
        }

        gesture.reset();
    }

    ValueChanges rollBack()
    {
        auto start = std::move (gesture->song);
        gesture.reset();
        return moveTo (std::move (start));
    }

    ValueChanges undo()
    {
        auto step = std::move (done.back());
        done.pop_back();
        ahead.push_back ({ step.name, now });
        return moveTo (std::move (step.song));
    }

    ValueChanges redo()
    {
        auto step = std::move (ahead.back());
        ahead.pop_back();
        done.push_back ({ step.name, now });
        return moveTo (std::move (step.song));
    }

private:
    struct Step
    {
        juce::String name;
        ValueSong song; // done: the song before the step; ahead: the song after it
    };

    ValueChanges moveTo (ValueSong next)
    {
        auto changes = findChanges (now, next);
        now = std::move (next);
        return changes;
    }

    ValueSong now;
    std::vector<Step> done, ahead;
    std::optional<Step> gesture;
};

//==============================================================================
// What a track's MIDI clips play (SongPlayer.cpp: compileNotes(), its notes; this song has no controllers' values): from
// the song's tree, as it is compiled today, and from the value.
std::shared_ptr<const Sequence> finish (std::vector<Sequence::Note> notes)
{
    if (notes.empty())
        return nullptr;

    return std::make_shared<const Sequence> (std::move (notes), std::vector<Sequence::Control>());
}

std::vector<Sequence::Note> gatherNotes (const juce::ValueTree& trackState)
{
    std::vector<Sequence::Note> notes;

    for (const auto& clip : phazite::getClipStates (trackState))
    {
        if (clip[schema::clipKind].toString() != schema::clipIsMidi || (bool) clip[schema::clipMute])
            continue;

        const auto start = (double) clip[schema::clipStart];
        const auto end = start + (double) clip[schema::clipLength];
        const auto contentStart = start - (double) clip.getProperty (schema::clipOffset, 0.0);

        phazite::forEachNote (clip,
                              [&] (const phazite::NoteElement& note)
                              {
                                  if (note.has (phazite::NoteFlag::muted))
                                      return;

                                  const auto from = contentStart + note.getStart().inBeats();
                                  const auto to = from + note.getLength().inBeats();

                                  if (const auto playedFrom = std::max (from, start), playedTo = std::min (to, end); playedTo > playedFrom)
                                      notes.push_back ({ playedFrom, playedTo, note.getPitch(), (float) note.getVelocity() / 127.0f,
                                                         note.has (phazite::NoteFlag::slide), note.has (phazite::NoteFlag::portamento),
                                                         (std::uint8_t) (juce::jlimit (1, 16, note.getChannel()) - 1) });
                              });
    }

    return notes;
}

std::vector<Sequence::Note> gatherNotes (const ValueTrack& track)
{
    std::vector<Sequence::Note> notes;

    for (const auto& clip : track.clips)
    {
        if (clip.muted)
            continue;

        const auto start = clip.start, end = clip.getEnd(), contentStart = clip.getContentStart();

        immer::for_each (clip.notes.inOrder,
                         [&] (const ValueNote& note)
                         {
                             if (note.muted)
                                 return;

                             const auto from = contentStart + note.start;
                             const auto to = from + note.length;

                             if (const auto playedFrom = std::max (from, start), playedTo = std::min (to, end); playedTo > playedFrom)
                                 notes.push_back ({ playedFrom, playedTo, note.pitch, (float) note.velocity / 127.0f, note.slide,
                                                    note.portamento, (std::uint8_t) (juce::jlimit (1, 16, (int) note.channel) - 1) });
                         });
    }

    return notes;
}

bool isSameSequence (const Sequence* a, const Sequence* b)
{
    if (a == nullptr || b == nullptr)
        return a == b;

    const auto same = [] (const Sequence::Event& x, const Sequence::Event& y)
    {
        return juce::exactlyEqual (x.beat, y.beat) && x.type == y.type && x.number == y.number && juce::exactlyEqual (x.value, y.value)
               && x.channel == y.channel;
    };

    return std::equal (a->getEvents().begin(), a->getEvents().end(), b->getEvents().begin(), b->getEvents().end(), same);
}

//==============================================================================
// Two lines of a song merged, by the versions' rules (SongMerge.h), for what this song holds: a value changed on one side
// takes that side's, on both to different values is a clash and keeps this side's; a note or a clip added on one side is
// added; taken away on one side and unchanged on the other, it goes; taken away on one and changed on the other, it stays.
// What a side left as it was is known without a look inside where it still shares the parting's value: the other side's
// is taken whole.
struct ValueMerge
{
    ValueSong song;
    int clashes = 0, stayed = 0;
};

template <typename Element, typename... Fields>
void mergeFields (Element& ours, const Element& base, const Element& theirs, int& clashes, Fields Element::*... fields)
{
    const auto mergeField = [&clashes] (auto& mine, const auto& was, const auto& other)
    {
        if (isSame (other, was))
            return;

        if (isSame (mine, was))
            mine = other;
        else if (! isSame (mine, other))
            ++clashes;
    };

    (mergeField (ours.*fields, base.*fields, theirs.*fields), ...);
}

ValueNotes mergeNotes (const ValueNotes& base, const ValueNotes& ours, const ValueNotes& theirs, ValueMerge& merge,
                       const std::function<ID()>& makeID)
{
    if (theirs == base)
        return ours;

    if (ours == base)
        return theirs;

    std::vector<ValueNote> merged;
    merged.reserve (ours.inOrder.size() + 16);

    immer::for_each (ours.inOrder,
                     [&] (const ValueNote& note)
                     {
                         const auto* was = base.find (note.id);

                         if (was == nullptr)
                             return (void) merged.push_back (note); // added here

                         if (const auto* other = theirs.find (note.id))
                         {
                             auto both = note;
                             mergeFields (both, *was, *other, merge.clashes, &ValueNote::pitch, &ValueNote::start, &ValueNote::length,
                                          &ValueNote::velocity, &ValueNote::muted, &ValueNote::slide, &ValueNote::portamento,
                                          &ValueNote::channel);
                             merged.push_back (both);
                         }
                         else if (! (note == *was)) // taken away there, changed here: it stays
                         {
                             merged.push_back (note);
                             ++merge.stayed;
                         }
                     });

    immer::for_each (theirs.inOrder,
                     [&] (ValueNote note)
                     {
                         const auto* was = base.find (note.id);

                         if (was == nullptr) // added there: with a new ID where this side made a note with its own
                         {
                             if (ours.places.find (note.id) != nullptr)
                                 note.id = makeID();

                             merged.push_back (note);
                         }
                         else if (ours.places.find (note.id) == nullptr && ! (note == *was)) // taken away here, changed there
                         {
                             merged.push_back (note);
                             ++merge.stayed;
                         }
                     });

    return makeNotes (merged);
}

ValueClip mergeClips (const ValueClip& base, const ValueClip& ours, const ValueClip& theirs, ValueMerge& merge,
                      const std::function<ID()>& makeID)
{
    auto merged = ours;
    mergeFields (merged, base, theirs, merge.clashes, &ValueClip::start, &ValueClip::length, &ValueClip::offset, &ValueClip::muted,
                 &ValueClip::link);
    merged.notes = mergeNotes (base.notes, ours.notes, theirs.notes, merge, makeID);
    return merged;
}

ValueTrack mergeTracks (const ValueTrack& base, const ValueTrack& ours, const ValueTrack& theirs, ValueMerge& merge,
                        const std::function<ID()>& makeID)
{
    std::unordered_map<ID, const ValueClip*> was, other, mine;

    for (const auto& clip : base.clips)
        was[clip.id] = &clip;

    for (const auto& clip : theirs.clips)
        other[clip.id] = &clip;

    std::vector<ValueClip> merged;

    for (const auto& clip : ours.clips)
    {
        mine[clip.id] = &clip;
        const auto before = was.find (clip.id);
        const auto there = other.find (clip.id);

        if (before == was.end())
        {
            merged.push_back (clip); // added here
        }
        else if (there != other.end())
        {
            merged.push_back (mergeClips (*before->second, clip, *there->second, merge, makeID));
        }
        else if (! (clip == *before->second)) // taken away there, changed here: it stays
        {
            merged.push_back (clip);
            ++merge.stayed;
        }
    }

    for (const auto& clip : theirs.clips)
    {
        const auto before = was.find (clip.id);

        if (before == was.end())
        {
            merged.push_back (clip); // added there
        }
        else if (! mine.contains (clip.id) && ! (clip == *before->second)) // taken away here, changed there
        {
            merged.push_back (clip);
            ++merge.stayed;
        }
    }

    return { ours.id, immer::flex_vector<ValueClip> (merged.begin(), merged.end()) };
}

// The three hold the same tracks in the same order: the spike merges no track added, moved or taken away.
ValueMerge mergeSongs (const ValueSong& base, const ValueSong& ours, const ValueSong& theirs, const std::function<ID()>& makeID)
{
    ValueMerge merge { ours };

    if (theirs.tracks == base.tracks)
        return merge;

    if (ours.tracks == base.tracks)
    {
        merge.song = theirs;
        return merge;
    }

    auto tracks = ours.tracks.transient();
    auto b = base.tracks.begin();
    auto t = theirs.tracks.begin();
    size_t place = 0;

    for (const auto& mine : ours.tracks)
    {
        const auto& was = *b++;
        const auto& other = *t++;

        if (! (other.clips == was.clips))
            tracks.set (place, mine.clips == was.clips ? other : mergeTracks (was, mine, other, merge, makeID));

        ++place;
    }

    merge.song = withIndex (tracks.persistent());
    return merge;
}

//==============================================================================
struct Compared
{
    juce::String what;
    std::vector<double> song {}, value {};
};

juce::String describeTimes (std::vector<double> times)
{
    if (times.empty())
        return "-";

    std::sort (times.begin(), times.end());
    const auto median = times[times.size() / 2];
    const auto places = median < 0.01 ? 5 : median < 1.0 ? 4 : 2;
    return juce::String (median, places) + " ms (" + juce::String (times.front(), places) + ")";
}

double medianOf (std::vector<double> times)
{
    std::sort (times.begin(), times.end());
    return times[times.size() / 2];
}

juce::String makeTable (const juce::String& title, const std::vector<Compared*>& rows)
{
    juce::String text;
    text << "\n| " << title << " | The song today: median (least) | Immutable values: median (least) | Times faster |\n|---|---|---|---|\n";

    for (const auto* row : rows)
    {
        text << "| " << row->what << " | " << describeTimes (row->song) << " | " << describeTimes (row->value) << " | ";

        if (! row->song.empty() && ! row->value.empty() && medianOf (row->value) > 0.0)
            text << juce::String (medianOf (row->song) / medianOf (row->value), 1);
        else
            text << "-";

        text << " |\n";
    }

    return text;
}
} // namespace

TEST_CASE ("Benchmark: the hard parts of a song of immutable values beside the song's own: a drag, notes deleted and added, aliases, "
           "what tells of a change, the notes compiled, a copy for another thread, a merge"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    using phazite::test::at;
    using phazite::test::dispatchPendingMessages;
    using phazite::test::lasting;

    constexpr int count = 5000;
    constexpr size_t dragged = 500;
    const auto startAt = [] (size_t i) { return (double) i * 0.25 + 0.03; };
    const auto pitchAt = [] (size_t i) { return 54 + (int) ((i * 7) % 12); };

    //==========================================================================
    // A project's song of 101 tracks: the first with a clip of 5,000 notes and a clip after it; 99 of four clips of 64 notes
    // each; the last with a clip of 1,000 notes and seven aliases of it. 38,344 notes as the song counts them.
    phazite::test::TestSong s;
    auto& song = *phazite::findSong (s.context);
    std::vector<phazite::TrackID> tracks { s.firstTrack };

    while (tracks.size() < 101)
        tracks.push_back (s.addTrack ("Track " + juce::String ((int) tracks.size() + 1)));

    const auto fill = [&song] (phazite::ClipID clip, double clipStart, size_t notes, const auto& startOf, const auto& pitchOf)
    {
        std::vector<phazite::NoteData> made;

        for (size_t i = 0; i < notes; ++i)
            made.push_back ({ .clip = clip, .pitch = pitchOf (i), .start = at (clipStart + startOf (i)), .length = lasting (0.2) });

        phazite::setClipNotes (song, clip, made);
    };

    const auto bigClip = phazite::insertMidiClip (song, tracks[0], { at (0.0), at (count * 0.25 + 30.0) }, nullptr);
    fill (bigClip, 0.0, (size_t) count, startAt, pitchAt);
    phazite::insertMidiClip (song, tracks[0], { at (1400.0), at (1416.0) }, nullptr);
    std::vector<phazite::ClipID> smallClips;

    for (size_t t = 1; t < 100; ++t)
    {
        for (int c = 0; c < 4; ++c)
        {
            const auto clip = phazite::insertMidiClip (song, tracks[t], { at (c * 16.0), at (c * 16.0 + 16.0) }, nullptr);
            fill (clip, c * 16.0, 64, [] (size_t i) { return (double) i * 0.25; }, [] (size_t i) { return 48 + (int) ((i * 7) % 24); });
            smallClips.push_back (clip);
        }
    }

    const auto aliasedClip = phazite::insertMidiClip (song, tracks[100], { at (0.0), at (256.0) }, nullptr);
    fill (aliasedClip, 0.0, 1000, [] (size_t i) { return (double) i * 0.25; }, pitchAt);

    for (int copy = 1; copy <= 7; ++copy)
        s.copyClip (aliasedClip, copy * 256.0, true);

    dispatchPendingMessages();

    const auto refs = getRefs (phazite::getClipNotes (song, bigClip));
    REQUIRE (refs.size() == (size_t) count);

    // The notes in another order than the clip holds them, as a selection's are.
    std::vector<size_t> order ((size_t) count);

    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;

    juce::Random random (20261121);

    for (size_t i = order.size(); i > 1; --i)
        std::swap (order[i - 1], order[(size_t) random.nextInt ((int) i)]);

    const auto someOf = [&] (size_t from, size_t to)
    {
        std::vector<phazite::NoteRef> some;

        for (auto i = from; i < to; ++i)
            some.push_back (refs[order[i]]);

        return some;
    };

    const auto selected = someOf (0, dragged);

    ValueHistory value (toValue (song.getState()));
    REQUIRE (value.get().tracks.size() == tracks.size());
    REQUIRE (value.get().findClip (bigClip.getRaw())->notes.inOrder.size() == (size_t) count);

    const auto check = [&] (const char* after)
    {
        const auto difference = describeDifference (toValue (song.getState()), value.get(), false);
        CHECK_MESSAGE (difference.isEmpty(), after << ": " << difference);
    };

    const auto bigTrack = [&] { return phazite::findTrackState (song, tracks[0]); };
    juce::String report;

    //==========================================================================
    // A drag of 500 notes: 20 steps of a quarter beat, each note's pitch a semitone up at every other step, as one undo
    // step; then undone, done again, and a drag of five steps given up.
    Compared dragStep { "A step of a drag of 500 of a clip's 5,000 notes, with what tells of it (the song: its listeners; values: the "
                        "roots compared)" };
    Compared afterStep { "What follows on the message loop (the song: its views told and its new plan; values: the track's notes "
                         "compiled again)" };
    Compared gather { "The track's 5,000 notes read out for the engine" };
    Compared compile { "The track's notes compiled (read out, and the engine's sequence made of them)" };
    Compared dragEnd { "The drag ended: 20 steps kept as one undo step" };
    Compared dragUndo { "That drag undone" };
    Compared dragRedo { "... and done again" };
    Compared dragGivenUp { "A drag of 5 steps given up (Escape)" };

    const auto makeStep = [&] (const std::vector<phazite::NoteData>& from, int step, std::vector<phazite::NoteData>& moved,
                               std::vector<Placement>& placements)
    {
        moved = from;
        placements.clear();

        for (auto& note : moved)
        {
            note.start = note.start + lasting (0.25 * step);
            note.pitch += step % 2;
            placements.push_back ({ note.start.inBeats(), note.pitch, note.length.inBeats() });
        }
    };

    for (int drag = 0; drag < 4; ++drag)
    {
        const auto givenUp = drag == 3;
        const auto from = phazite::getNoteData (song, selected);
        REQUIRE (from.size() == dragged);
        std::vector<phazite::NoteData> moved;
        std::vector<Placement> placements;
        auto gesture = std::make_unique<phazite::Gesture> (song, "Move Notes");
        value.beginGesture ("Move Notes");

        for (int step = 1; step <= (givenUp ? 5 : 20); ++step)
        {
            makeStep (from, step, moved, placements);
            ValueChanges changes;
            const auto ofSong = millisecondsOf ([&] { phazite::placeNotes (song, moved); });
            const auto afterSong = millisecondsOf ([] { dispatchPendingMessages(); });
            const auto ofValue =
                millisecondsOf ([&] { changes = value.change ("Move Notes", placeNotes (value.get(), selected, placements)); });
            REQUIRE (changes.clips == std::vector<ID> { bigClip.getRaw() });
            REQUIRE (changes.clipTracks == std::vector<ID> { tracks[0].getRaw() });
            std::shared_ptr<const Sequence> fromSong, fromValue;
            std::vector<Sequence::Note> read;
            const auto afterValue = millisecondsOf (
                [&]
                {
                    for (const auto track : changes.clipTracks)
                        fromValue = finish (gatherNotes (*value.get().findTrack (track)));
                });

            if (! givenUp)
            {
                dragStep.song.push_back (ofSong);
                dragStep.value.push_back (ofValue);
                afterStep.song.push_back (afterSong);
                afterStep.value.push_back (afterValue);
                compile.value.push_back (afterValue);
                compile.song.push_back (millisecondsOf ([&] { fromSong = finish (gatherNotes (bigTrack())); }));
                gather.song.push_back (millisecondsOf ([&] { read = gatherNotes (bigTrack()); }));
                gather.value.push_back (millisecondsOf ([&] { read = gatherNotes (*value.get().findTrack (tracks[0].getRaw())); }));
                CHECK (isSameSequence (fromSong.get(), fromValue.get()));
            }
        }

        if (givenUp)
        {
            dragGivenUp.song.push_back (millisecondsOf ([&] { gesture->rollBack(); }));
            dragGivenUp.value.push_back (millisecondsOf ([&] { value.rollBack(); }));
            gesture.reset();
            dispatchPendingMessages();
            check ("a drag given up");
            break;
        }

        dragEnd.song.push_back (millisecondsOf ([&] { gesture->commit(); }));
        dragEnd.value.push_back (millisecondsOf ([&] { value.commitGesture(); }));
        gesture.reset();
        dispatchPendingMessages();
        check ("a drag");

        dragUndo.song.push_back (millisecondsOf ([&] { phazite::undo (song); }));
        dragUndo.value.push_back (millisecondsOf ([&] { value.undo(); }));
        dispatchPendingMessages();
        check ("a drag undone");

        dragRedo.song.push_back (millisecondsOf ([&] { phazite::redo (song); }));
        dragRedo.value.push_back (millisecondsOf ([&] { value.redo(); }));
        dispatchPendingMessages();
        check ("a drag done again");
    }

    report << makeTable ("A drag, 101 tracks", { &dragStep, &afterStep, &gather, &compile, &dragEnd, &dragUndo, &dragRedo, &dragGivenUp });

    //==========================================================================
    // Notes deleted, added and transposed, each as one undo step, and the step undone.
    Compared deleted { "500 of the clip's 5,000 notes deleted" };
    Compared deletedUndo { "... undone" };
    Compared addedSmall { "A note added to a clip of 64" };
    Compared addedBig { "A note added to the clip of 5,000" };
    Compared addedUndo { "... undone" };
    Compared transposed { "The clip's 5,000 notes transposed" };
    Compared transposedUndo { "... undone" };
    Compared nudgedAliases { "500 of 1,000 notes nudged in a clip with seven aliases" };
    Compared nudgedAliasesUndo { "... undone" };

    const auto undoBoth = [&] (Compared& row, const char* what)
    {
        dispatchPendingMessages();
        row.song.push_back (millisecondsOf ([&] { phazite::undo (song); }));
        row.value.push_back (millisecondsOf ([&] { value.undo(); }));
        dispatchPendingMessages();
        check (what);
    };

    const auto aliasedRefs = getRefs (phazite::getClipNotes (song, aliasedClip));
    REQUIRE (aliasedRefs.size() == 1000);
    std::vector<phazite::NoteRef> aliasedSelected;

    for (size_t i = 0; i < aliasedRefs.size(); i += 2)
        aliasedSelected.push_back (aliasedRefs[i]);

    // A small edit made and undone on both: it lets go of what Redo held, so that the row after it times its own edit and
    // not the letting go of the step undone before it. That is timed once, after the heaviest for the values: the clip made
    // anew without 500 of its notes.
    Compared redoLetGo { "The next edit after that Undo (a note added to a clip of 64): what Redo held is let go" };

    const auto letGoOfRedo = [&] (Compared* row)
    {
        phazite::NoteRef added;
        const auto ofSong = millisecondsOf (
            [&]
            {
                const phazite::UndoStep step (song, "Add Note");
                added = phazite::addNote (song, smallClips[300], 60, at (2.1), lasting (0.5), 100);
            });
        const auto ofValue = millisecondsOf (
            [&] { value.change ("Add Note", addNote (value.get(), smallClips[300].getRaw(), added.note.getRaw(), 60, 2.1, 0.5, 100)); });

        if (row != nullptr)
        {
            row->song.push_back (ofSong);
            row->value.push_back (ofValue);
        }

        dispatchPendingMessages();
        phazite::undo (song);
        value.undo();
        dispatchPendingMessages();
    };

    for (int pass = 0; pass < 9; ++pass)
    {
        letGoOfRedo (nullptr);
        deleted.song.push_back (millisecondsOf (
            [&]
            {
                const phazite::UndoStep step (song, "Delete Notes");
                phazite::deleteNotes (song, selected);
            }));
        deleted.value.push_back (millisecondsOf ([&] { value.change ("Delete Notes", deleteNotes (value.get(), selected)); }));
        check ("notes deleted");
        undoBoth (deletedUndo, "notes deleted, undone");
        letGoOfRedo (&redoLetGo);

        for (const auto big : { false, true })
        {
            const auto clip = big ? bigClip : smallClips[200];
            const auto start = big ? 700.1 : 3.1;
            phazite::NoteRef added;
            (big ? addedBig : addedSmall)
                .song.push_back (millisecondsOf (
                    [&]
                    {
                        const phazite::UndoStep step (song, "Add Note");
                        added = phazite::addNote (song, clip, 72, at (start), lasting (0.5), 100);
                    }));
            REQUIRE (added.note.isValid());
            (big ? addedBig : addedSmall)
                .value.push_back (millisecondsOf (
                    [&] { value.change ("Add Note", addNote (value.get(), clip.getRaw(), added.note.getRaw(), 72, start, 0.5, 100)); }));
            check ("a note added");
            undoBoth (addedUndo, "a note added, undone");
        }

        transposed.song.push_back (millisecondsOf (
            [&]
            {
                const phazite::UndoStep step (song, "Transpose Notes");
                phazite::transposeNotes (song, refs, 1);
            }));
        transposed.value.push_back (millisecondsOf ([&] { value.change ("Transpose Notes", transposeNotes (value.get(), refs, 1)); }));
        check ("notes transposed");
        undoBoth (transposedUndo, "notes transposed, undone");
        letGoOfRedo (nullptr);

        nudgedAliases.song.push_back (millisecondsOf (
            [&]
            {
                const phazite::UndoStep step (song, "Nudge Notes");
                phazite::nudgeNotes (song, aliasedSelected, lasting (0.25));
            }));
        nudgedAliases.value.push_back (
            millisecondsOf ([&] { value.change ("Nudge Notes", nudgeNotes (value.get(), aliasedSelected, 0.25)); }));
        check ("notes nudged in a clip with aliases");
        undoBoth (nudgedAliasesUndo, "notes nudged in a clip with aliases, undone");
    }

    report << makeTable ("Edits as one undo step each, with what tells of them",
                         { &deleted, &deletedUndo, &redoLetGo, &addedSmall, &addedBig, &addedUndo, &transposed, &transposedUndo,
                           &nudgedAliases, &nudgedAliasesUndo });

    //==========================================================================
    // What tells of a change, alone, and a copy of the song for another thread.
    {
        const auto before = value.get();
        std::vector<Placement> placements;

        for (const auto& note : phazite::getNoteData (song, selected))
            placements.push_back ({ note.start.inBeats() + 0.25, note.pitch, note.length.inBeats() });

        const auto oneMoved = placeNotes (before, { selected.front() }, { placements.front() });
        const auto manyMoved = placeNotes (before, selected, placements);
        constexpr int times = 1000;
        size_t found = 0;

        const auto each = [] (const std::function<void()>& work)
        {
            return millisecondsOf (
                       [&]
                       {
                           for (int i = 0; i < times; ++i)
                               work();
                       })
                   / times;
        };

        const auto oneFound = each ([&] { found += findChanges (before, oneMoved).clips.size(); });
        const auto manyFound = each ([&] { found += findChanges (before, manyMoved).clips.size(); });
        CHECK (found == 2 * (size_t) times);
        std::vector<ID> notes;
        const auto notesFound = each (
            [&] { notes = findChangedNotes (before.findClip (bigClip.getRaw())->notes, manyMoved.findClip (bigClip.getRaw())->notes); });
        CHECK (notes.size() == dragged);

        report << "\n| Immutable values alone: what changed found by comparing two songs of 101 tracks | Each |\n|---|---|\n"
               << "| The clip changed, after one note moved | " << juce::String (oneFound * 1.0e6, 0) << " ns |\n"
               << "| The clip changed, after 500 notes moved | " << juce::String (manyFound * 1.0e6, 0) << " ns |\n"
               << "| Which of the clip's 5,000 notes changed, after 500 moved (the song's listeners don't tell) | "
               << juce::String (notesFound * 1.0e3, 1) << " us |\n";

        Compared copied { "A copy of the whole song for another thread (a save, an autosave, a render): 38,344 notes" };
        juce::ValueTree treeCopy;
        ValueSong valueCopy;

        for (int pass = 0; pass < 9; ++pass)
        {
            copied.song.push_back (millisecondsOf ([&] { treeCopy = song.getState().createCopy(); }));
            copied.value.push_back (each ([&] { valueCopy = value.get(); }));
        }

        report << makeTable ("A copy", { &copied });
    }

    //==========================================================================
    // What a step kept for Undo holds: 40 drags of 20 steps each, there and back by turns, each kept as a step; then 40
    // steps that each delete 50 of the clip's notes. The heap's bytes in use before and after, for each step. immer keeps
    // some 2,000 of the blocks it lets go for its next ones, which the heap counts as in use: emptied first (blocks taken
    // and held meanwhile), so that what fills it again counts against the values, by at most a tenth of what 40 steps hold.
    {
        constexpr int kept = 40;
        const auto from = phazite::getNoteData (song, selected);
        std::vector<std::vector<phazite::NoteData>> moved (21);
        std::vector<std::vector<Placement>> placements (21);

        for (int step = 0; step <= 20; ++step)
            makeStep (from, step, moved[(size_t) step], placements[(size_t) step]);

        // A drag there goes through steps 1 to 20, a drag back through 19 to 0.
        const auto stepOf = [] (int drag, int step) { return (size_t) (drag % 2 == 0 ? step : 20 - step); };
        const auto kilobytes = [] (std::int64_t bytes) { return juce::String ((double) bytes / kept / 1024.0, 1) + " KB"; };
        std::vector<ValueNote> many (20000);

        for (size_t i = 0; i < many.size(); ++i)
            many[i].id = i + 1;

        const auto held = makeNotes (many);
        const auto start = getHeapBytesInUse();

        for (int drag = 0; drag < kept; ++drag)
        {
            phazite::Gesture gesture (song, "Move Notes");

            for (int step = 1; step <= 20; ++step)
                phazite::placeNotes (song, moved[stepOf (drag, step)]);

            gesture.commit();
        }

        const auto afterSong = getHeapBytesInUse();

        for (int drag = 0; drag < kept; ++drag)
        {
            value.beginGesture ("Move Notes");

            for (int step = 1; step <= 20; ++step)
                value.change ("Move Notes", placeNotes (value.get(), selected, placements[stepOf (drag, step)]));

            value.commitGesture();
        }

        const auto afterValue = getHeapBytesInUse();
        dispatchPendingMessages();
        check ("drags, for what they keep");

        std::vector<std::vector<phazite::NoteRef>> gone;

        for (size_t i = 0; i < (size_t) kept; ++i)
            gone.push_back (someOf (2000 + 50 * i, 2050 + 50 * i));

        const auto beforeDelete = getHeapBytesInUse();

        for (const auto& notes : gone)
        {
            const phazite::UndoStep step (song, "Delete Notes");
            phazite::deleteNotes (song, notes);
        }

        const auto deletedSong = getHeapBytesInUse();

        for (const auto& notes : gone)
            value.change ("Delete Notes", deleteNotes (value.get(), notes));

        const auto deletedValue = getHeapBytesInUse();
        CHECK (held.inOrder.size() == many.size());

        report << "\n| What a step kept for Undo holds, each of 40 | The song (its undo actions) | Immutable values (the root before) "
                  "|\n|---|---|---|\n"
               << "| A drag of 500 notes in 20 steps, kept as one step | " << kilobytes (afterSong - start) << " | "
               << kilobytes (afterValue - afterSong) << " |\n"
               << "| 50 of the clip's 5,000 notes deleted | " << kilobytes (deletedSong - beforeDelete) << " | "
               << kilobytes (deletedValue - deletedSong) << " |\n";

        dispatchPendingMessages();
        check ("notes deleted, for what the steps keep");

        for (int step = 0; step < 2 * kept; ++step)
        {
            phazite::undo (song);
            value.undo();
        }

        dispatchPendingMessages();
        check ("what the steps kept, undone");
    }

    //==========================================================================
    // Two lines merged. From where they part (the song as it is), this side nudges 500 of the big clip's notes, deletes 25,
    // adds 10 and transposes a small clip's; the other side transposes 500 of the big clip's (50 of them among this side's
    // 500, 5 among those this side deleted), nudges 25 that this side nudged too (the clashes), deletes 25 (10 that this side
    // deleted too, 5 that it nudged), adds 10, nudges the same small clip's notes and another's.
    {
        const auto base = song.getState().createCopy();
        const auto baseValue = value.get();
        auto ours = phazite::makeSong (base.createCopy());
        auto theirs = phazite::makeSong (base.createCopy());
        REQUIRE (ours != nullptr);
        REQUIRE (theirs != nullptr);
        REQUIRE (describeDifference (toValue (ours->getState()), baseValue, false).isEmpty());

        for (int i = 0; i < 1000; ++i)
            ours->makeID(); // so that the notes the two sides add have IDs of their own

        const auto smallRefs = [&] (size_t clip) { return getRefs (phazite::getClipNotes (song, smallClips[clip])); };
        auto oursValue = baseValue, theirsValue = baseValue;

        const auto addTo = [&] (phazite::Song& side, ValueSong& sideValue, double start)
        {
            for (int i = 0; i < 10; ++i)
            {
                const auto added = phazite::addNote (side, bigClip, 60 + i, at (start + i), lasting (0.5), 90);
                REQUIRE (added.note.isValid());
                sideValue = addNote (sideValue, bigClip.getRaw(), added.note.getRaw(), 60 + i, start + i, 0.5, 90);
            }
        };

        // This side's.
        phazite::nudgeNotes (*ours, someOf (0, 500), lasting (0.5));
        oursValue = nudgeNotes (oursValue, someOf (0, 500), 0.5);
        auto oursDeleted = someOf (1000, 1020);

        for (const auto& ref : someOf (900, 905))
            oursDeleted.push_back (ref);

        phazite::deleteNotes (*ours, oursDeleted);
        oursValue = deleteNotes (oursValue, oursDeleted);
        addTo (*ours, oursValue, 100.1);
        phazite::transposeNotes (*ours, smallRefs (24), 2);
        oursValue = transposeNotes (oursValue, smallRefs (24), 2);

        // The other side's.
        phazite::transposeNotes (*theirs, someOf (450, 950), 3);
        theirsValue = transposeNotes (theirsValue, someOf (450, 950), 3);
        phazite::nudgeNotes (*theirs, someOf (0, 25), lasting (1.0));
        theirsValue = nudgeNotes (theirsValue, someOf (0, 25), 1.0);
        auto theirsDeleted = someOf (1010, 1030);

        for (const auto& ref : someOf (100, 105))
            theirsDeleted.push_back (ref);

        phazite::deleteNotes (*theirs, theirsDeleted);
        theirsValue = deleteNotes (theirsValue, theirsDeleted);
        addTo (*theirs, theirsValue, 300.1);
        phazite::nudgeNotes (*theirs, smallRefs (24), lasting (0.25));
        theirsValue = nudgeNotes (theirsValue, smallRefs (24), 0.25);
        phazite::nudgeNotes (*theirs, smallRefs (40), lasting (0.25));
        theirsValue = nudgeNotes (theirsValue, smallRefs (40), 0.25);

        CHECK_MESSAGE (describeDifference (toValue (ours->getState()), oursValue, false).isEmpty(),
                       describeDifference (toValue (ours->getState()), oursValue, false));
        CHECK_MESSAGE (describeDifference (toValue (theirs->getState()), theirsValue, false).isEmpty(),
                       describeDifference (toValue (theirs->getState()), theirsValue, false));

        // The same three as a file would give them: nothing shared between them.
        const auto baseRead = toValue (base), oursRead = toValue (ours->getState()), theirsRead = toValue (theirs->getState());
        const std::function<ID()> makeID = [&song] { return song.makeID(); };

        Compared lines { "The other side's changes found (the song: SongDiff's findChanges(); values: no step of their own)" };
        Compared merging { "The three merged, the values sharing what no side changed" };
        Compared mergingRead { "The three merged, the values read from files: nothing shared" };
        phazite::MergeResult merged;
        ValueMerge mergedValue, mergedRead;

        for (int pass = 0; pass < 5; ++pass)
        {
            std::vector<phazite::SongChange> changes;
            lines.song.push_back (millisecondsOf ([&] { changes = phazite::findChanges (base, theirs->getState()); }));
            const auto ofSong =
                millisecondsOf ([&] { merged = phazite::mergeSongs (base, ours->getState(), theirs->getState(), changes, makeID); });
            merging.song.push_back (ofSong);
            mergingRead.song.push_back (ofSong);
            merging.value.push_back (millisecondsOf ([&] { mergedValue = mergeSongs (baseValue, oursValue, theirsValue, makeID); }));
            mergingRead.value.push_back (millisecondsOf ([&] { mergedRead = mergeSongs (baseRead, oursRead, theirsRead, makeID); }));
        }

        const auto mergedBySong = toValue (merged.song);
        const auto difference = describeDifference (mergedBySong, mergedValue.song, true);
        CHECK_MESSAGE (difference.isEmpty(), "merged: " << difference);
        const auto differenceRead = describeDifference (mergedBySong, mergedRead.song, true);
        CHECK_MESSAGE (differenceRead.isEmpty(), "merged, read: " << differenceRead);
        CHECK ((int) merged.clashes.size() == mergedValue.clashes);
        CHECK (mergedRead.clashes == mergedValue.clashes);

        report << makeTable ("Two lines merged (the tracks, clips and notes of 101 tracks)", { &lines, &merging, &mergingRead });
        report << "\nThe merge: " << (int) merged.clashes.size() << " clashes by the song's merge, " << mergedValue.clashes
               << " by the values'; " << merged.notes.size() << " notes for the user by the song's, " << mergedValue.stayed
               << " items that stayed by the values'; the big clip holds "
               << (int) mergedBySong.findClip (bigClip.getRaw())->notes.inOrder.size() << " notes by the song's and "
               << (int) mergedValue.song.findClip (bigClip.getRaw())->notes.inOrder.size() << " by the values'.\n";
    }

    MESSAGE (report);
}
