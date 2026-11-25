// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Tempo.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongOwnValue.h"
#include "model/base/internal/SongText.h"
#include "model/base/internal/SongValues.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/internal/AreaClashes.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // A kind of the map's entries, each beat within its description's range (SongText.h: keepDescribed()), as the readers keep
    // one; the very entries where every one is so already. Kept by beat still: the bound keeps their order. Where the bound
    // puts one on a beat another of its kind is on (two past the song's last beat, both moved to it), they become one: the
    // later, which is the one that plays (engine/core/TempoMap.cpp: of two at a beat, the later counts), as the file holds one
    // of a kind at a beat. Two at one beat that the bound didn't move stay as they are (known bug 240).
    template <typename Entry>
    immer::flex_vector<Entry> keepBeatsDescribed (const immer::flex_vector<Entry>& entries)
    {
        const auto isKept = [] (const Entry& entry) { return entry.beat == keepDescribed (entry.beat, schema::mapBeat); };

        if (std::all_of (entries.begin(), entries.end(), isKept))
            return entries;

        std::vector<Entry> kept;
        auto atBeatMoved = false; // whether the bound moved one of those at the last kept entry's beat

        for (auto entry : entries)
        {
            const auto beat = keepDescribed (entry.beat, schema::mapBeat);
            const auto moved = ! (beat == entry.beat);
            const auto atSameBeat = ! kept.empty() && kept.back().beat == beat;
            entry.beat = beat;

            if (atSameBeat && (moved || atBeatMoved))
                while (! kept.empty() && kept.back().beat == beat)
                    kept.pop_back();

            atBeatMoved = moved || (atSameBeat && atBeatMoved);
            kept.push_back (entry);
        }

        auto value = immer::flex_vector<Entry>().transient();

        for (const auto& entry : kept)
            value.push_back (entry);

        return value.persistent();
    }

    // The song's tempo map made the next value: its area's, in the step in progress, or outside the history while the song
    // is written outside it (SongValues.h: setTempoMap(), which writes nothing for the map the song holds already). Every
    // edit below computes the next value from the one the song holds and sets it once, its beats kept within their range
    // (keepBeatsDescribed()).
    void setMap (Song& song, TempoMapValue next)
    {
        next.tempos = keepBeatsDescribed (next.tempos);
        next.signatures = keepBeatsDescribed (next.signatures);

        song.getValues()->setTempoMap (song, std::move (next));
    }

    // The map with one tempo (by its place in getTempoChanges()) changed in place: its bpm or its curve, never its beat.
    template <typename Change>
    TempoMapValue withTempoChanged (const TempoMapValue& map, size_t index, Change&& change)
    {
        auto next = map;
        auto tempo = map.tempos[index];
        change (tempo);
        next.tempos = map.tempos.set (index, tempo);
        return next;
    }

    // Keeps the song's patterns at their speed while the tempo changes, when the song says so (doPatternsFollowTempo()).
    // The song keeps a MIDI clip's notes on their beats, so this scales the beats of its notes, its length and its offset by
    // how much the tempo changed, which leaves them at their times. Each clip still starts on its beat.
    class PatternsKeepTheirSpeed
    {
    public:
        explicit PatternsKeepTheirSpeed (Song& s)
            : song (s),
              tempoBefore (getTempo (s))
        {
        }

        ~PatternsKeepTheirSpeed()
        {
            const auto scale = getTempo (song) / tempoBefore;

            if (doPatternsFollowTempo (song) || juce::approximatelyEqual (scale, 1.0))
                return;

            std::vector<ClipValue> clips;
            std::vector<ClipID> ids;

            for (const auto& clip : getClipValues (song))
            {
                if (clip.isMidi())
                {
                    clips.push_back (clip);
                    ids.push_back (clip.id);
                }
            }

            // Their notes, each alias group's once (Notes.h); then each clip's own length and offset.
            scaleClipContent (song, ids, scale);

            for (const auto& clip : clips)
                setClipPlace (song, clip.id, clip.start, getClipRange (clip).getLength() * scale, clip.offset * scale);
        }

    private:
        Song& song;
        const double tempoBefore;
    };
} // namespace

//==============================================================================
const TempoMapValue& getTempoMapValue (const Song& song)
{
    return song.getValues()->tempoMap.get();
}

std::vector<TempoChange> getTempoChanges (const Song& song)
{
    const auto& tempos = getTempoMapValue (song).tempos;
    std::vector<TempoChange> changes;
    changes.reserve (tempos.size());

    for (const auto& tempo : tempos)
        changes.push_back ({ tempo.beat, tempo.bpm, tempo.curve });

    return changes;
}

std::vector<TimeSignatureChange> getTimeSignatureChanges (const Song& song)
{
    const auto& signatures = getTempoMapValue (song).signatures;
    std::vector<TimeSignatureChange> changes;
    changes.reserve (signatures.size());

    for (const auto& signature : signatures)
        changes.push_back ({ signature.beat, { signature.numerator, signature.denominator } });

    return changes;
}

Bars getBars (const Song& song)
{
    return Bars (getTimeSignatureChanges (song));
}

namespace
{
    // The song's tempo map as the engine plays it at a sample rate, as the song keeps it for the value it holds now
    // (SongValues.h: PlayedTempoMap): made once for each change of the value, not at every call. None on another thread than
    // the message thread, where the map is made for the call.
    const engine::TempoMap* findPlayedTempoMap (const Song& song, double sampleRate)
    {
        const auto* values = song.getValues();

        if (! juce::MessageManager::existsAndIsCurrentThread())
            return nullptr;

        const auto& value = values->tempoMap.get();

        for (const auto& kept : values->playedTempoMaps)
            if (kept.has_value() && kept->madeOf.isSameAs (value) && juce::exactlyEqual (kept->sampleRate, sampleRate))
                return &kept->map;

        auto& slot = values->playedTempoMaps[values->nextPlayedTempoMap];
        values->nextPlayedTempoMap = (values->nextPlayedTempoMap + 1) % values->playedTempoMaps.size();
        slot = SongValues::PlayedTempoMap { value, sampleRate, getTempoMap (value, sampleRate) };
        return &slot->map;
    }

    // What a read of the engine's map gives, from the map kept, or one made for it.
    template <typename Read>
    auto readPlayed (const Song& song, double sampleRate, Read&& read)
    {
        if (const auto* kept = findPlayedTempoMap (song, sampleRate))
            return read (*kept);

        return read (getTempoMap (getTempoMapValue (song), sampleRate));
    }
} // namespace

engine::TempoMap getTempoMap (const Song& song, double sampleRate)
{
    return readPlayed (song, sampleRate, [] (const engine::TempoMap& map) { return map; });
}

engine::TempoMap getTempoMap (const TempoMapValue& value, double sampleRate)
{
    std::vector<engine::TempoMap::Tempo> tempos;
    std::vector<engine::TempoMap::TimeSignature> signatures;
    tempos.reserve (value.tempos.size());
    signatures.reserve (value.signatures.size());

    for (const auto& tempo : value.tempos)
        tempos.push_back ({ tempo.beat.inBeats(), tempo.bpm, tempo.curve });

    for (const auto& signature : value.signatures)
        signatures.push_back ({ signature.beat.inBeats(), signature.numerator, signature.denominator });

    return engine::TempoMap (std::move (tempos), std::move (signatures), sampleRate);
}

engine::TempoMap getTempoMap (const juce::ValueTree& songState, double sampleRate)
{
    return getTempoMap (readTempoMapValue (songState.getChildWithName (schema::TEMPOMAP)), sampleRate);
}

double toSeconds (const Song& song, BeatPosition beat)
{
    return readPlayed (song, 1.0, [beat] (const engine::TempoMap& map) { return map.toSamples (beat.inBeats()); });
}

BeatPosition toBeats (const Song& song, double seconds)
{
    return readPlayed (song, 1.0, [seconds] (const engine::TempoMap& map) { return BeatPosition::fromBeats (map.toBeats (seconds)); });
}

double getTempoAt (const Song& song, BeatPosition beat)
{
    return readPlayed (song, 1.0, [beat] (const engine::TempoMap& map) { return map.getTempoAt (beat.inBeats()); });
}

void ensureTempoMap (juce::ValueTree& song)
{
    auto map = song.getChildWithName (schema::TEMPOMAP);

    if (! map.isValid())
    {
        map = juce::ValueTree (schema::TEMPOMAP);
        song.appendChild (map, nullptr);
    }

    if (! map.getChildWithName (schema::TEMPO).isValid())
        map.appendChild (juce::ValueTree (schema::TEMPO, { { schema::mapBeat, 0.0 }, { schema::bpm, 120.0 } }), nullptr);

    if (! map.getChildWithName (schema::TIMESIGNATURE).isValid())
        map.appendChild (
            juce::ValueTree (schema::TIMESIGNATURE, { { schema::mapBeat, 0.0 }, { schema::numerator, 4 }, { schema::denominator, 4 } }),
            nullptr);
}

namespace
{
    // One kind of the map with the entries after a beat moved later by a length (insertTempoMapTime()): those at the song's
    // start stay. Their order stays: those that move all move by the same length, past every one that stays.
    template <typename Entry>
    immer::flex_vector<Entry> insertTime (const immer::flex_vector<Entry>& entries, BeatPosition at, BeatDuration length)
    {
        auto next = entries;

        for (size_t i = 0; i < entries.size(); ++i)
        {
            if (const auto beat = entries[i].beat.inBeats(); beat > 0.0 && beat >= at.inBeats())
            {
                auto moved = entries[i];
                moved.beat = BeatPosition::fromBeats (beat + length.inBeats());
                next = next.set (i, moved);
            }
        }

        return next;
    }

    // One kind of the map with a range taken out (removeTempoMapTime()), entry by entry in the kind's order: what played after
    // the range plays from its start; the last in the range moves to its start (unless one starts where it ends, which moves
    // there too); the others in it go, and those after it move back; one that lands on the song's start gives the first its
    // values (takeValues (first, from)). Then the kind in order by beat again, stably: two at one beat in the order they were
    // in (as the map's elements, in the kind's order, were read again by their beats).
    template <typename Entry, typename TakeValues>
    immer::flex_vector<Entry> removeTime (const immer::flex_vector<Entry>& entries, BeatRange range, TakeValues&& takeValues)
    {
        const auto start = range.getStart().inBeats(), end = range.getEnd().inBeats(), length = range.getLength().inBeats();
        std::vector<Entry> kept (entries.begin(), entries.end());
        std::vector<bool> gone (kept.size(), false);
        std::optional<size_t> last, first;
        auto oneAtEnd = false;

        for (size_t i = 0; i < kept.size(); ++i)
        {
            const auto beat = kept[i].beat.inBeats();

            if (beat <= 0.0)
                first = i;
            else if (beat >= start && beat < end && (! last.has_value() || beat > kept[*last].beat.inBeats()))
                last = i;
            else if (juce::approximatelyEqual (beat, end))
                oneAtEnd = true;
        }

        const auto moveToStart = [&] (size_t i)
        {
            if (start > 0.0 || ! first.has_value())
            {
                kept[i].beat = BeatPosition::fromBeats (start);
                return;
            }

            takeValues (kept[*first], kept[i]);
            gone[i] = true;
        };

        for (size_t i = 0; i < kept.size(); ++i)
        {
            const auto beat = kept[i].beat.inBeats();

            if (beat <= 0.0 || beat < start)
                continue;

            if (beat >= end)
            {
                if (juce::approximatelyEqual (beat, end))
                    moveToStart (i);
                else
                    kept[i].beat = BeatPosition::fromBeats (beat - length);
            }
            else if (last == i && ! oneAtEnd)
            {
                moveToStart (i);
            }
            else
            {
                gone[i] = true;
            }
        }

        std::vector<Entry> left;

        for (size_t i = 0; i < kept.size(); ++i)
            if (! gone[i])
                left.push_back (kept[i]);

        std::stable_sort (left.begin(), left.end(), [] (const Entry& a, const Entry& b) { return a.beat < b.beat; });
        return immer::flex_vector<Entry> (left.begin(), left.end());
    }
} // namespace

void insertTempoMapTime (Song& song, BeatPosition at, BeatDuration length)
{
    const auto map = getTempoMapValue (song);
    setMap (song, { insertTime (map.tempos, at, length), insertTime (map.signatures, at, length) });
}

void removeTempoMapTime (Song& song, BeatRange range)
{
    // Of each kind, tempos and time signatures, what played after the range plays from its start, as the chord lane's keys
    // do (removeTime()).
    const auto map = getTempoMapValue (song);
    setMap (song, { removeTime (map.tempos, range,
                                [] (TempoEntry& first, const TempoEntry& from)
                                {
                                    first.bpm = from.bpm;
                                    first.curve = from.curve;
                                }),
                    removeTime (map.signatures, range,
                                [] (SignatureEntry& first, const SignatureEntry& from)
                                {
                                    first.numerator = from.numerator;
                                    first.denominator = from.denominator;
                                }) });
}

//==============================================================================
double getTempo (const Song& song)
{
    return getTempoMapValue (song).tempos.front().bpm;
}

void setTempo (Song& song, double bpm)
{
    if (! std::isfinite (bpm))
        return; // no tempo at all

    const auto rounded = std::round (juce::jlimit (minTempo, maxTempo, bpm) * 100.0) / 100.0;

    if (juce::approximatelyEqual (getTempo (song), rounded))
        return;

    const UndoStep step (song, "Change Tempo");
    const PatternsKeepTheirSpeed keepPatterns (song); // unless they follow it
    setMap (song, withTempoChanged (getTempoMapValue (song), 0, [rounded] (TempoEntry& tempo) { tempo.bpm = rounded; }));
}

//==============================================================================
namespace
{
    double roundTempo (double bpm)
    {
        // What is no number at all: the slowest.
        return std::isfinite (bpm) ? std::round (juce::jlimit (minTempo, maxTempo, bpm) * 100.0) / 100.0 : minTempo;
    }

    // The place of the tempo in effect at a beat: the last at or before it.
    int findTempoAt (const std::vector<TempoChange>& changes, BeatPosition beat)
    {
        int found = 0;

        for (int i = 0; i < (int) changes.size(); ++i)
            if (changes[(size_t) i].beat <= beat)
                found = i;

        return found;
    }

    bool isSameBeat (BeatPosition a, BeatPosition b)
    {
        return std::abs ((a - b).inBeats()) < 1.0e-6;
    }
} // namespace

void setTempoAt (Song& song, BeatPosition beat, double bpm)
{
    const auto changes = getTempoChanges (song);

    if (changes.size() <= 1)
    {
        setTempo (song, bpm); // the song's one tempo, the patterns kept at their speed if the song says so
        return;
    }

    const auto index = findTempoAt (changes, beat);
    const auto rounded = roundTempo (bpm);

    if (juce::approximatelyEqual (changes[(size_t) index].bpm, rounded))
        return;

    const UndoStep step (song, "Change Tempo");
    setMap (song, withTempoChanged (getTempoMapValue (song), (size_t) index, [rounded] (TempoEntry& tempo) { tempo.bpm = rounded; }));
}

bool hasTempoChanges (const Song& song)
{
    return getTempoMapValue (song).tempos.size() > 1;
}

std::vector<double> getTempoValues (const Song& song, BeatRange range, int count)
{
    count = std::max (2, count);

    return readPlayed (song, 1.0,
                       [range, count] (const engine::TempoMap& map)
                       {
                           std::vector<double> values;
                           values.reserve ((size_t) count);

                           for (int i = 0; i < count; ++i)
                               values.push_back (
                                   map.getTempoAt (range.getStart().inBeats() + range.getLength().inBeats() * i / (count - 1)));

                           return values;
                       });
}

std::vector<double> getTempoValues (const Song& song, const std::vector<BeatPosition>& beats)
{
    return readPlayed (song, 1.0,
                       [&beats] (const engine::TempoMap& map)
                       {
                           std::vector<double> values;
                           values.reserve (beats.size());

                           for (const auto beat : beats)
                               values.push_back (map.getTempoAt (beat.inBeats()));

                           return values;
                       });
}

int addTempoChange (Song& song, BeatPosition beat, std::optional<double> bpm)
{
    const auto at = keepDescribed (beat, schema::mapBeat); // from 0 to the song's last beat
    const auto changes = getTempoChanges (song);
    const auto index = findTempoAt (changes, at);
    const auto value = roundTempo (bpm.value_or (getTempoAt (song, at)));
    const UndoStep step (song, "Add Tempo Change");
    const auto map = getTempoMapValue (song);

    // A tempo already there takes the value.
    if (isSameBeat (changes[(size_t) index].beat, at))
    {
        setMap (song, withTempoChanged (map, (size_t) index, [value] (TempoEntry& tempo) { tempo.bpm = value; }));
        return index;
    }

    // The line it is on goes on through it: a step stays a step, and a ramp ramps on from it. After any already at its beat,
    // as one appended to the map's elements was read.
    const TempoEntry added { at, value, juce::jlimit (-1.0f, 1.0f, changes[(size_t) index].curve) };
    const auto place = std::upper_bound (map.tempos.begin(), map.tempos.end(), added,
                                         [] (const TempoEntry& a, const TempoEntry& b) { return a.beat < b.beat; });
    auto next = map;
    next.tempos = map.tempos.insert ((size_t) std::distance (map.tempos.begin(), place), added);
    setMap (song, std::move (next));
    return findTempoAt (getTempoChanges (song), at);
}

void addTempoChangeAtPlayhead (Song& song)
{
    addTempoChange (song, getPlayheadBeat (song));
}

int moveTempoChange (Song& song, int index, BeatPosition beat, double bpm)
{
    const auto map = getTempoMapValue (song);

    if (! juce::isPositiveAndBelow (index, (int) map.tempos.size()))
        return -1;

    auto entry = map.tempos[(size_t) index];
    const auto at = index == 0 ? BeatPosition() : keepDescribed (beat, schema::mapBeat);
    const auto rounded = roundTempo (bpm);

    if (isSameBeat (entry.beat, at) && juce::approximatelyEqual (entry.bpm, rounded))
        return index;

    // Where its element would be read with its new beat (TempoMapValue.h: placeByBeat()): moved later, before any already at
    // its new beat; moved earlier, after them.
    const UndoStep step (song, "Move Tempo Change");
    entry.beat = at;
    entry.bpm = rounded;
    size_t place = 0;
    auto next = map;
    next.tempos = placeByBeat (map.tempos, (size_t) index, entry, place);
    setMap (song, std::move (next));
    return (int) place;
}

void setTempoCurve (Song& song, int index, float curve)
{
    const auto map = getTempoMapValue (song);

    if (! juce::isPositiveAndBelow (index, (int) map.tempos.size()))
        return;

    const auto bounded = juce::jlimit (-1.0f, 1.0f, curve);

    if (juce::exactlyEqual (map.tempos[(size_t) index].curve, bounded))
        return;

    const UndoStep step (song, "Change Tempo Ramp");
    setMap (song, withTempoChanged (map, (size_t) index, [bounded] (TempoEntry& tempo) { tempo.curve = bounded; }));
}

void deleteTempoChange (Song& song, int index)
{
    const auto map = getTempoMapValue (song);

    if (index <= 0 || index >= (int) map.tempos.size())
        return;

    const UndoStep step (song, "Delete Tempo Change");
    auto next = map;
    next.tempos = map.tempos.erase ((size_t) index);
    setMap (song, std::move (next));
}

void removeTempoChanges (Song& song)
{
    const auto map = getTempoMapValue (song);

    if (map.tempos.size() <= 1 && juce::exactlyEqual (map.tempos.front().curve, tempoJump))
        return;

    // The first alone, held all the way through.
    const UndoStep step (song, "Remove Tempo Changes");
    auto first = map.tempos.front();
    first.curve = tempoJump;
    auto next = map;
    next.tempos = immer::flex_vector<TempoEntry> { first };
    setMap (song, std::move (next));
}

void takeTempoChanges (Song& song, const std::vector<TempoChange>& tempos, BeatPosition from)
{
    for (const auto& tempo : tempos)
    {
        const auto beat = std::max (BeatPosition(), from + (tempo.beat - BeatPosition()));
        auto index = 0;

        if (isSameBeat (beat, BeatPosition()))
        {
            setTempo (song, tempo.bpm);
        }
        else
        {
            // A step to the tempo the song holds there already changes nothing, and goes in as nothing.
            const auto changes = getTempoChanges (song);
            const auto& held = changes[(size_t) findTempoAt (changes, beat)];

            if (tempo.curve >= tempoJump && held.curve >= tempoJump && juce::approximatelyEqual (held.bpm, roundTempo (tempo.bpm)))
                continue;

            index = addTempoChange (song, beat, tempo.bpm);
        }

        setTempoCurve (song, index, tempo.curve);
    }
}

TempoStretch getTempoStretch (const Song& song)
{
    TempoStretch stretch;
    stretch.patternsFollow = doPatternsFollowTempo (song);

    // Each clip's kind alone, and an audio clip's tempo: four times a second while the tempo's panel shows.
    for (const auto& clip : getClipPlacements (song))
    {
        if (clip.isAudio())
        {
            ++stretch.samples;

            if (getClipTempo (song, clip.id) > 0.0)
                ++stretch.samplesFollowing;
        }
        else
        {
            ++stretch.patterns;
        }
    }

    return stretch;
}

bool doAllSamplesFollowTempo (const Song& song)
{
    const auto stretch = getTempoStretch (song);
    return stretch.samples > 0 && stretch.samplesFollowing == stretch.samples;
}

void toggleSamplesFollowTempo (Song& song)
{
    toggleClipsFollowTempo (song, getAudioClips (song));
}

bool doPatternsFollowTempo (const Song& song)
{
    return ! getSongOwn (song).patternsKeepTheirSpeed;
}

void togglePatternsFollowTempo (Song& song)
{
    // In the command's step (written only while on: SongOwnValue.h, writeSongOwnInto()).
    auto own = getSongOwn (song);
    own.patternsKeepTheirSpeed = ! own.patternsKeepTheirSpeed;
    setSongOwn (song, std::move (own));
}

TimeSignature getTimeSignature (const Song& song)
{
    const auto& first = getTempoMapValue (song).signatures.front();
    return { first.numerator, first.denominator };
}

void setTimeSignature (Song& song, TimeSignature timeSignature)
{
    if (! isValid (timeSignature) || timeSignature == getTimeSignature (song))
        return;

    const UndoStep step (song, "Change Time Signature");
    const auto map = getTempoMapValue (song);
    auto first = map.signatures.front();
    first.numerator = timeSignature.numerator;
    first.denominator = timeSignature.denominator;
    auto next = map;
    next.signatures = map.signatures.set (0, first);
    setMap (song, std::move (next));
}

void setTimeSignatureChanges (Song& song, const std::vector<TimeSignatureChange>& changes)
{
    const auto isTaken = [] (const TimeSignatureChange& change)
    { return isValid (change.signature) && change.beat >= BeatPosition() && std::isfinite (change.beat.inBeats()); };

    if (changes.empty() || ! std::all_of (changes.begin(), changes.end(), isTaken))
        return;

    auto sorted = changes;
    std::stable_sort (sorted.begin(), sorted.end(),
                      [] (const TimeSignatureChange& a, const TimeSignatureChange& b) { return a.beat < b.beat; });

    if (sorted.front().beat != BeatPosition())
        return; // the first starts at the song's start

    const auto held = getTimeSignatureChanges (song);
    const auto isHeld = [] (const TimeSignatureChange& a, const TimeSignatureChange& b)
    { return a.beat == b.beat && a.signature == b.signature; };

    if (std::equal (sorted.begin(), sorted.end(), held.begin(), held.end(), isHeld))
        return;

    // The map's time signatures these, in their order.
    const UndoStep step (song, "Change Time Signatures");
    auto signatures = immer::flex_vector<SignatureEntry>().transient();

    for (const auto& change : sorted)
        signatures.push_back ({ change.beat, change.signature.numerator, change.signature.denominator });

    auto next = getTempoMapValue (song);
    next.signatures = signatures.persistent();
    setMap (song, std::move (next));
}

void addTempoCommands (CommandRegistry& registry, Selection& selection)
{
    const auto hasSong = [&selection] { return selection.getSong() != nullptr; };
    const auto undoTarget = undoTargetOf (selection);

    registry.add ({ .id = "transport.samplesFollowTempo",
                    .name = "Samples Follow Tempo",
                    .category = "Transport",
                    .description = "Every sample in the song follows its tempo, stretched to stay in time when it changes, or keeps its "
                                   "own speed again if all of them did.",
                    .synonyms = { "stretch samples", "warp", "time stretch", "audio follows tempo", "sync to tempo" },
                    .perform = withSong (selection, toggleSamplesFollowTempo),
                    .isEnabled = hasSong,
                    .undoTarget = undoTarget,
                    .isTicked = ifSong (selection, doAllSamplesFollowTempo) });

    registry.add ({ .id = "transport.patternsFollowTempo",
                    .name = "Patterns Follow Tempo",
                    .category = "Transport",
                    .description = "When the song's tempo changes, its patterns play faster or slower, their notes on their beats; off, "
                                   "they keep their speed.",
                    .synonyms = { "stretch patterns", "midi follows tempo", "notes follow tempo", "timebase" },
                    .perform = withSong (selection, togglePatternsFollowTempo),
                    .isEnabled = hasSong,
                    .undoTarget = undoTarget,
                    .isTicked = ifSong (selection, doPatternsFollowTempo) });

    registry.add ({ .id = "transport.addTempoChange",
                    .name = "Add Tempo Change",
                    .category = "Transport",
                    .description = "Adds a tempo change where the playhead is, at the tempo there, and shows the tempo lane over the "
                                   "tracks, where it can be dragged to another tempo and ramped.",
                    .synonyms = { "tempo map", "tempo automation", "tempo track", "insert tempo", "bpm change", "accelerando", "ritardando",
                                  "speed up", "slow down" },
                    .perform = withSong (selection, addTempoChangeAtPlayhead),
                    .isEnabled = hasSong,
                    .undoTarget = undoTarget });

    registry.add ({ .id = "transport.removeTempoChanges",
                    .name = "Remove Tempo Changes",
                    .category = "Transport",
                    .description = "Takes away the song's tempo changes, leaving its first tempo all the way through.",
                    .synonyms = { "clear tempo map", "one tempo", "steady tempo", "delete tempo changes", "flatten tempo" },
                    .perform = withSong (selection, removeTempoChanges),
                    .isEnabled = ifSong (selection, hasTempoChanges),
                    .undoTarget = undoTarget });
}

//==============================================================================
namespace
{
    // Whether a path in the song goes into its tempo map: its first step the song's TEMPOMAP, which the song holds as a value
    // beside its tree (model/base/internal/SongValues.h), its tree's element an empty place.
    bool isPathInTempoMap (const juce::StringArray& path)
    {
        return ! path.isEmpty() && path[0].startsWith (schema::TEMPOMAP.toString() + ":");
    }

    // A clash in the song's tempo map, chosen through its value (model/base/internal/TempoMapValue.h), as the clash's
    // property or element is chosen elsewhere in the song: an entry (the path's last step, "TEMPO:16.000000") given the
    // property's value chosen, or taken out where the side chosen held no entry there (a property of a tempo both lines
    // added, chosen as what was before: known bug 239's tempo map), or put back as that side holds it where an earlier
    // choice took it out; an entry one side added or took away made as the side chosen holds it (read as the song holds one:
    // TempoMapValue.h, withEntryAt()), or taken out. Refused
    // (false, nothing written) for a clash on the TEMPOMAP itself, which holds nothing of its own, for an element that
    // isn't an entry of the kind its step names, and where the map would not hold what was chosen as the song holds a map
    // (isHeldAsTheSongHoldsIt(): a kind left without an entry, a number out of its range), which a merge of two songs
    // Phazite holds never makes, but a commit's song may come from anyone (tests/FuzzTests.cpp chooses every clash of a
    // damaged merge). Through the tempo map's one setter, in the choice's step.
    bool chooseInTempoMap (Song& song, const MergeSides& merging, const MergeClash& clash, int value)
    {
        auto* values = song.getValues();

        if (clash.path.size() != 2)
            return false;

        const auto& step = clash.path[1];
        const auto& held = values->tempoMap.get();
        std::optional<TempoMapValue> next;

        if (clash.property.isValid())
        {
            // The side chosen's map (each side a whole song: the parting's, this line's before the merge, the other line's; one
            // without a TEMPOMAP reads as the map a song without one plays), where that song could be read, read as the song
            // holds a map, as everything that enters the tempo map's area is (TempoMapValue.h: readHeldTempoMap()).
            const juce::ValueTree* sides[] = { &merging.base, &merging.before, &merging.theirs };
            const auto& side = *sides[value];
            const auto canRead = side.hasType (schema::PHAZITE);
            const auto sideMap = readHeldTempoMap (side.getChildWithName (schema::TEMPOMAP));
            const juce::var* chosen[] = { &clash.base, &clash.ours, &clash.theirs };

            if (canRead && ! holdsEntryAt (sideMap, step))
            {
                next = withEntryAt (held, step, sideMap); // none there on that side: taken out
            }
            else
            {
                auto set = held;

                // None there in the song (an earlier choice took it out): the side's, as it holds it, first.
                if (! setTempoMapProperty (set, step, clash.property, *chosen[value]))
                {
                    const auto put = canRead ? withEntryAt (held, step, sideMap) : std::nullopt;

                    if (! put.has_value())
                        return false;

                    set = *put;

                    if (! setTempoMapProperty (set, step, clash.property, *chosen[value]))
                        return false;
                }

                next = set;
            }
        }
        else
        {
            const juce::ValueTree* elements[] = { &clash.baseElement, &clash.oursElement, &clash.theirsElement };
            next = withEntryAt (held, step, *elements[value]);
        }

        if (! next.has_value() || ! isHeldAsTheSongHoldsIt (*next))
            return false;

        values->setTempoMap (song, std::move (*next));
        return true;
    }
} // namespace

AreaChoice chooseTempoMapClash (Song& song, const MergeClash& clash, int value, const MergeSides& sides)
{
    if (! isPathInTempoMap (clash.path))
        return AreaChoice::notTheArea;

    return chooseInTempoMap (song, sides, clash, value) ? AreaChoice::chosen : AreaChoice::refused;
}
} // namespace phazite
