// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Tempo.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/transport/internal/Playback.h"

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    juce::ValueTree findTempoMap (const Song& song)
    {
        return song.getState().getChildWithName (schema::TEMPOMAP);
    }

    // A tempo map's tempos or time signatures (their type), in the song's order.
    std::vector<juce::ValueTree> getEntries (const juce::ValueTree& map, const juce::Identifier& type)
    {
        std::vector<juce::ValueTree> entries;

        for (const auto& entry : map)
            if (entry.hasType (type))
                entries.push_back (entry);

        std::stable_sort (entries.begin(), entries.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b)
                          { return (double) a[schema::mapBeat] < (double) b[schema::mapBeat]; });
        return entries;
    }

    // The song's tempo or time signature (the first of its type, at the song's start), to change: the tempo map gets
    // them first if it has none.
    juce::ValueTree getFirstOfMap (Song& song, const juce::Identifier& type)
    {
        ensureTempoMap (song);
        return getEntries (findTempoMap (song), type).front();
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

            for (const auto& clipState : getClipStates (song))
                if (! isAudioState (clipState))
                    keepSpeed (clipState, scale);
        }

    private:
        Song& song;
        const double tempoBefore;

        void keepSpeed (juce::ValueTree clipState, double scale)
        {
            auto* um = &song.getUndoManager();

            for (auto child : clipState)
            {
                if (child.hasType (schema::NOTE))
                {
                    child.setProperty (schema::noteStart, (double) child[schema::noteStart] * scale, um);
                    child.setProperty (schema::noteLength, (double) child[schema::noteLength] * scale, um);
                }
                else if (child.hasType (schema::CONTROL))
                {
                    child.setProperty (schema::controlBeat, (double) child[schema::controlBeat] * scale, um);
                }
            }

            const auto range = getClipRange (clipState);
            setClipPlace (song, clipState, range.getStart(), range.getLength() * scale, getClipOffset (clipState) * scale);
        }
    };
} // namespace

namespace
{
    // A song's state's tempo map: its tempos and time signatures, in order, a first at the song's start if there is none.
    std::vector<TempoChange> readTempoChanges (const juce::ValueTree& songState)
    {
        std::vector<TempoChange> changes;

        for (const auto& entry : getEntries (songState.getChildWithName (schema::TEMPOMAP), schema::TEMPO))
            changes.push_back ({ BeatPosition::fromBeats (entry[schema::mapBeat]), (double) entry.getProperty (schema::bpm, 120.0),
                                 juce::jlimit (-1.0f, 1.0f, (float) entry.getProperty (schema::tempoCurve, tempoJump)) });

        if (changes.empty())
            changes.push_back ({});

        return changes;
    }

    std::vector<TimeSignatureChange> readTimeSignatureChanges (const juce::ValueTree& songState)
    {
        std::vector<TimeSignatureChange> changes;

        for (const auto& entry : getEntries (songState.getChildWithName (schema::TEMPOMAP), schema::TIMESIGNATURE))
            changes.push_back ({ BeatPosition::fromBeats (entry[schema::mapBeat]),
                                 { (int) entry.getProperty (schema::numerator, 4), (int) entry.getProperty (schema::denominator, 4) } });

        if (changes.empty())
            changes.push_back ({});

        return changes;
    }
} // namespace

//==============================================================================
std::vector<TempoChange> getTempoChanges (const Song& song)
{
    return readTempoChanges (song.getState());
}

std::vector<TimeSignatureChange> getTimeSignatureChanges (const Song& song)
{
    return readTimeSignatureChanges (song.getState());
}

Bars getBars (const Song& song)
{
    return Bars (getTimeSignatureChanges (song));
}

engine::TempoMap getTempoMap (const Song& song, double sampleRate)
{
    return getTempoMap (song.getState(), sampleRate);
}

engine::TempoMap getTempoMap (const juce::ValueTree& songState, double sampleRate)
{
    std::vector<engine::TempoMap::Tempo> tempos;
    std::vector<engine::TempoMap::TimeSignature> signatures;

    for (const auto& change : readTempoChanges (songState))
        tempos.push_back ({ change.beat.inBeats(), change.bpm, change.curve });

    for (const auto& change : readTimeSignatureChanges (songState))
        signatures.push_back ({ change.beat.inBeats(), change.signature.numerator, change.signature.denominator });

    return engine::TempoMap (std::move (tempos), std::move (signatures), sampleRate);
}

double toSeconds (const Song& song, BeatPosition beat)
{
    return getTempoMap (song).toSamples (beat.inBeats());
}

BeatPosition toBeats (const Song& song, double seconds)
{
    return BeatPosition::fromBeats (getTempoMap (song).toBeats (seconds));
}

double getTempoAt (const Song& song, BeatPosition beat)
{
    return getTempoMap (song).getTempoAt (beat.inBeats());
}

void ensureTempoMap (Song& song)
{
    auto phazite = song.getState();
    auto map = phazite.getChildWithName (schema::TEMPOMAP);

    if (! map.isValid())
    {
        map = juce::ValueTree (schema::TEMPOMAP);
        phazite.appendChild (map, nullptr);
    }

    if (getEntries (map, schema::TEMPO).empty())
        map.appendChild (juce::ValueTree (schema::TEMPO, { { schema::mapBeat, 0.0 }, { schema::bpm, 120.0 } }), nullptr);

    if (getEntries (map, schema::TIMESIGNATURE).empty())
        map.appendChild (
            juce::ValueTree (schema::TIMESIGNATURE, { { schema::mapBeat, 0.0 }, { schema::numerator, 4 }, { schema::denominator, 4 } }),
            nullptr);
}

void insertTempoMapTime (Song& song, BeatPosition at, BeatDuration length)
{
    for (auto entry : findTempoMap (song))
        if (const auto beat = (double) entry[schema::mapBeat]; beat > 0.0 && beat >= at.inBeats())
            entry.setProperty (schema::mapBeat, beat + length.inBeats(), &song.getUndoManager());
}

void removeTempoMapTime (Song& song, BeatRange range)
{
    // Of each kind, tempos and time signatures, what played after the range plays from its start, as the chord lane's keys
    // do: the last change in the range moves to its start (unless one starts where it ends, which moves there too); the
    // others in it go, and those after it move back. One that lands on the song's start gives the first its values.
    auto map = findTempoMap (song);
    auto* undo = &song.getUndoManager();
    const auto start = range.getStart().inBeats(), end = range.getEnd().inBeats(), length = range.getLength().inBeats();

    for (const auto& type : { schema::TEMPO, schema::TIMESIGNATURE })
    {
        const auto entries = getEntries (map, type);
        juce::ValueTree last, first;
        auto oneAtEnd = false;

        for (const auto& entry : entries)
        {
            const auto beat = (double) entry[schema::mapBeat];

            if (beat <= 0.0)
                first = entry;
            else if (beat >= start && beat < end && (! last.isValid() || beat > (double) last[schema::mapBeat]))
                last = entry;
            else if (juce::approximatelyEqual (beat, end))
                oneAtEnd = true;
        }

        const auto moveToStart = [&] (juce::ValueTree entry)
        {
            if (start > 0.0 || ! first.isValid())
            {
                entry.setProperty (schema::mapBeat, start, undo);
                return;
            }

            for (int p = 0; p < entry.getNumProperties(); ++p)
                if (const auto name = entry.getPropertyName (p); name != schema::mapBeat)
                    first.setProperty (name, entry[name], undo);

            for (int p = first.getNumProperties(); --p >= 0;)
                if (const auto name = first.getPropertyName (p); name != schema::mapBeat && ! entry.hasProperty (name))
                    first.removeProperty (name, undo);

            map.removeChild (entry, undo);
        };

        for (auto entry : entries)
        {
            const auto beat = (double) entry[schema::mapBeat];

            if (beat <= 0.0 || beat < start)
                continue;

            if (beat >= end)
            {
                if (juce::approximatelyEqual (beat, end))
                    moveToStart (entry);
                else
                    entry.setProperty (schema::mapBeat, beat - length, undo);
            }
            else if (entry == last && ! oneAtEnd)
            {
                moveToStart (entry);
            }
            else
            {
                map.removeChild (entry, undo);
            }
        }
    }
}

//==============================================================================
double getTempo (const Song& song)
{
    return getTempoChanges (song).front().bpm;
}

void setTempo (Song& song, double bpm)
{
    const auto rounded = std::round (juce::jlimit (minTempo, maxTempo, bpm) * 100.0) / 100.0;

    if (juce::approximatelyEqual (getTempo (song), rounded))
        return;

    const UndoStep step (song, "Change Tempo");
    const PatternsKeepTheirSpeed keepPatterns (song); // unless they follow it
    getFirstOfMap (song, schema::TEMPO).setProperty (schema::bpm, rounded, &song.getUndoManager());
}

//==============================================================================
namespace
{
    double roundTempo (double bpm)
    {
        return std::round (juce::jlimit (minTempo, maxTempo, bpm) * 100.0) / 100.0;
    }

    // The song's tempos' elements, in getTempoChanges()'s order, the tempo map made first if it has none.
    std::vector<juce::ValueTree> getTempoEntries (Song& song)
    {
        ensureTempoMap (song);
        return getEntries (findTempoMap (song), schema::TEMPO);
    }

    // What isn't written is a step, as tempos start.
    void writeCurve (Song& song, juce::ValueTree entry, float curve)
    {
        if (const auto bounded = juce::jlimit (-1.0f, 1.0f, curve); juce::exactlyEqual (bounded, tempoJump))
            entry.removeProperty (schema::tempoCurve, &song.getUndoManager());
        else
            entry.setProperty (schema::tempoCurve, bounded, &song.getUndoManager());
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
    getTempoEntries (song)[(size_t) index].setProperty (schema::bpm, rounded, &song.getUndoManager());
}

bool hasTempoChanges (const Song& song)
{
    return getTempoChanges (song).size() > 1;
}

std::vector<double> getTempoValues (const Song& song, BeatRange range, int count)
{
    const auto map = getTempoMap (song);
    count = std::max (2, count);
    std::vector<double> values;
    values.reserve ((size_t) count);

    for (int i = 0; i < count; ++i)
        values.push_back (map.getTempoAt (range.getStart().inBeats() + range.getLength().inBeats() * i / (count - 1)));

    return values;
}

int addTempoChange (Song& song, BeatPosition beat, std::optional<double> bpm)
{
    const auto at = std::max (BeatPosition(), beat);
    const auto changes = getTempoChanges (song);
    const auto index = findTempoAt (changes, at);
    const auto value = roundTempo (bpm.value_or (getTempoAt (song, at)));
    const UndoStep step (song, "Add Tempo Change");
    auto entries = getTempoEntries (song);

    // A tempo already there takes the value.
    if (isSameBeat (changes[(size_t) index].beat, at))
    {
        entries[(size_t) index].setProperty (schema::bpm, value, &song.getUndoManager());
        return index;
    }

    // The line it is on goes on through it: a step stays a step, and a ramp ramps on from it.
    juce::ValueTree entry (schema::TEMPO, { { schema::mapBeat, at.inBeats() }, { schema::bpm, value } });
    findTempoMap (song).appendChild (entry, &song.getUndoManager());
    writeCurve (song, entry, changes[(size_t) index].curve);
    return findTempoAt (getTempoChanges (song), at);
}

void addTempoChangeAtPlayhead (Song& song)
{
    addTempoChange (song, getPlayheadBeat (song));
}

int moveTempoChange (Song& song, int index, BeatPosition beat, double bpm)
{
    auto entries = getTempoEntries (song);

    if (! juce::isPositiveAndBelow (index, (int) entries.size()))
        return -1;

    auto entry = entries[(size_t) index];
    const auto at = index == 0 ? BeatPosition() : std::max (BeatPosition(), beat);
    const auto rounded = roundTempo (bpm);

    if (isSameBeat (BeatPosition::fromBeats (entry[schema::mapBeat]), at)
        && juce::approximatelyEqual ((double) entry[schema::bpm], rounded))
        return index;

    const UndoStep step (song, "Move Tempo Change");
    entry.setProperty (schema::mapBeat, at.inBeats(), &song.getUndoManager());
    entry.setProperty (schema::bpm, rounded, &song.getUndoManager());

    const auto moved = getTempoEntries (song);
    const auto found = std::find (moved.begin(), moved.end(), entry);
    return found != moved.end() ? (int) std::distance (moved.begin(), found) : -1;
}

void setTempoCurve (Song& song, int index, float curve)
{
    auto entries = getTempoEntries (song);

    if (! juce::isPositiveAndBelow (index, (int) entries.size()))
        return;

    const auto bounded = juce::jlimit (-1.0f, 1.0f, curve);

    if (juce::exactlyEqual ((float) entries[(size_t) index].getProperty (schema::tempoCurve, tempoJump), bounded))
        return;

    const UndoStep step (song, "Change Tempo Ramp");
    writeCurve (song, entries[(size_t) index], bounded);
}

void deleteTempoChange (Song& song, int index)
{
    auto entries = getTempoEntries (song);

    if (index <= 0 || index >= (int) entries.size())
        return;

    const UndoStep step (song, "Delete Tempo Change");
    findTempoMap (song).removeChild (entries[(size_t) index], &song.getUndoManager());
}

void removeTempoChanges (Song& song)
{
    auto entries = getTempoEntries (song);

    if (entries.size() <= 1 && ! entries.front().hasProperty (schema::tempoCurve))
        return;

    const UndoStep step (song, "Remove Tempo Changes");
    auto map = findTempoMap (song);

    for (size_t i = 1; i < entries.size(); ++i)
        map.removeChild (entries[i], &song.getUndoManager());

    writeCurve (song, entries.front(), tempoJump);
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

    for (const auto& clipState : getClipStates (song))
    {
        if (isAudioState (clipState))
        {
            ++stretch.samples;

            if ((double) clipState.getProperty (schema::clipTempo, 0.0) > 0.0)
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
    return ! (bool) song.getState().getProperty (schema::patternsKeepTheirSpeed, false);
}

void togglePatternsFollowTempo (Song& song)
{
    auto state = song.getState();

    if (doPatternsFollowTempo (song))
        state.setProperty (schema::patternsKeepTheirSpeed, true, &song.getUndoManager());
    else
        state.removeProperty (schema::patternsKeepTheirSpeed, &song.getUndoManager());
}

TimeSignature getTimeSignature (const Song& song)
{
    return getTimeSignatureChanges (song).front().signature;
}

void setTimeSignature (Song& song, TimeSignature timeSignature)
{
    if (! isValid (timeSignature) || timeSignature == getTimeSignature (song))
        return;

    const UndoStep step (song, "Change Time Signature");
    auto first = getFirstOfMap (song, schema::TIMESIGNATURE);
    first.setProperty (schema::numerator, timeSignature.numerator, &song.getUndoManager());
    first.setProperty (schema::denominator, timeSignature.denominator, &song.getUndoManager());
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
} // namespace phazite
