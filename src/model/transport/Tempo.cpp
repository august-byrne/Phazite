// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Tempo.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"

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

//==============================================================================
std::vector<TempoChange> getTempoChanges (const Song& song)
{
    std::vector<TempoChange> changes;

    for (const auto& entry : getEntries (findTempoMap (song), schema::TEMPO))
        changes.push_back ({ BeatPosition::fromBeats (entry[schema::mapBeat]), (double) entry.getProperty (schema::bpm, 120.0) });

    if (changes.empty())
        changes.push_back ({});

    return changes;
}

std::vector<TimeSignatureChange> getTimeSignatureChanges (const Song& song)
{
    std::vector<TimeSignatureChange> changes;

    for (const auto& entry : getEntries (findTempoMap (song), schema::TIMESIGNATURE))
        changes.push_back ({ BeatPosition::fromBeats (entry[schema::mapBeat]),
                             { (int) entry.getProperty (schema::numerator, 4), (int) entry.getProperty (schema::denominator, 4) } });

    if (changes.empty())
        changes.push_back ({});

    return changes;
}

Bars getBars (const Song& song)
{
    return Bars (getTimeSignatureChanges (song));
}

engine::TempoMap getTempoMap (const Song& song, double sampleRate)
{
    std::vector<engine::TempoMap::Tempo> tempos;
    std::vector<engine::TempoMap::TimeSignature> signatures;

    for (const auto& change : getTempoChanges (song))
        tempos.push_back ({ change.beat.inBeats(), change.bpm });

    for (const auto& change : getTimeSignatureChanges (song))
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
    auto map = findTempoMap (song);

    for (int i = map.getNumChildren(); --i >= 0;)
    {
        auto entry = map.getChild (i);
        const auto beat = (double) entry[schema::mapBeat];

        if (beat <= 0.0 || beat < range.getStart().inBeats())
            continue;

        if (beat < range.getEnd().inBeats())
            map.removeChild (i, &song.getUndoManager());
        else
            entry.setProperty (schema::mapBeat, beat - range.getLength().inBeats(), &song.getUndoManager());
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
}
} // namespace phazite
