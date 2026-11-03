// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Tempo.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/mirror/internal/Mirror.h"

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
        explicit PatternsKeepTheirSpeed (te::Edit& e)
            : edit (e),
              tempoBefore (getTempo (getSong (e)))
        {
        }

        ~PatternsKeepTheirSpeed()
        {
            const auto scale = getTempo (getSong (edit)) / tempoBefore;

            if (doPatternsFollowTempo (getSong (edit)) || juce::approximatelyEqual (scale, 1.0))
                return;

            for (const auto& clipState : getClipStates (edit))
                if (! isAudioState (clipState))
                    keepSpeed (clipState, scale);
        }

    private:
        te::Edit& edit;
        const double tempoBefore;

        void keepSpeed (juce::ValueTree clipState, double scale)
        {
            auto* um = &getSong (edit).getUndoManager();

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

            // The sysex events Tracktion records with the notes, which only its clip holds.
            if (auto* clip = dynamic_cast<te::MidiClip*> (te::findClipForID (edit, toTracktion (getClipID (clipState)))))
                for (auto* event : clip->getSequence().getSysexEvents())
                    event->setBeatPosition (te::BeatPosition::fromBeats (event->getBeatPosition().inBeats() * scale), um);

            const auto range = getClipRange (clipState);
            setClipPlace (edit, clipState, range.getStart(), range.getLength() * scale, getClipOffset (clipState) * scale);
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

    song.reconcile (Song::Part::tempoMap);
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

    song.reconcile (Song::Part::tempoMap);
}

//==============================================================================
double getTempo (const Song& song)
{
    return getTempoChanges (song).front().bpm;
}

void setTempo (te::Edit& edit, double bpm)
{
    const auto rounded = std::round (juce::jlimit (minTempo, maxTempo, bpm) * 100.0) / 100.0;

    if (juce::approximatelyEqual (getTempo (getSong (edit)), rounded))
        return;

    const UndoStep step (edit, "Change Tempo");
    const PatternsKeepTheirSpeed keepPatterns (edit); // unless they follow it
    getFirstOfMap (getSong (edit), schema::TEMPO).setProperty (schema::bpm, rounded, &getSong (edit).getUndoManager());
    mirror::reconcileTempoMap (edit); // which places the clips and the automation again, a clip that follows it stretched again
}

TempoStretch getTempoStretch (te::Edit& edit)
{
    TempoStretch stretch;
    stretch.patternsFollow = doPatternsFollowTempo (getSong (edit));

    for (auto* track : te::getAudioTracks (edit))
    {
        for (auto* clip : track->getClips())
        {
            if (auto* audio = dynamic_cast<te::AudioClipBase*> (clip))
            {
                ++stretch.samples;

                if (getClipTempo (*audio) > 0.0)
                    ++stretch.samplesFollowing;
            }
            else if (dynamic_cast<te::MidiClip*> (clip) != nullptr)
            {
                ++stretch.patterns;
            }
        }
    }

    return stretch;
}

bool doAllSamplesFollowTempo (te::Edit& edit)
{
    const auto stretch = getTempoStretch (edit);
    return stretch.samples > 0 && stretch.samplesFollowing == stretch.samples;
}

void toggleSamplesFollowTempo (te::Edit& edit)
{
    juce::Array<te::Clip*> clips;

    for (auto* clip : getAudioClips (edit))
        clips.add (clip);

    toggleClipsFollowTempo (clips);
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
    song.reconcile (Song::Part::tempoMap);
}

void addTempoCommands (CommandRegistry& registry, Selection& selection)
{
    const auto hasEdit = [&selection] { return selection.getEdit() != nullptr; };
    const auto undoTarget = undoTargetOf (selection);

    registry.add ({ .id = "transport.samplesFollowTempo",
                    .name = "Samples Follow Tempo",
                    .category = "Transport",
                    .description = "Every sample in the song follows its tempo, stretched to stay in time when it changes, or keeps its "
                                   "own speed again if all of them did.",
                    .synonyms = { "stretch samples", "warp", "time stretch", "audio follows tempo", "sync to tempo" },
                    .perform = withEdit (selection, toggleSamplesFollowTempo),
                    .isEnabled = hasEdit,
                    .undoTarget = undoTarget,
                    .isTicked = ifEdit (selection, doAllSamplesFollowTempo) });

    registry.add ({ .id = "transport.patternsFollowTempo",
                    .name = "Patterns Follow Tempo",
                    .category = "Transport",
                    .description = "When the song's tempo changes, its patterns play faster or slower, their notes on their beats; off, "
                                   "they keep their speed.",
                    .synonyms = { "stretch patterns", "midi follows tempo", "notes follow tempo", "timebase" },
                    .perform = withSong (selection, togglePatternsFollowTempo),
                    .isEnabled = hasEdit,
                    .undoTarget = undoTarget,
                    .isTicked = ifSong (selection, doPatternsFollowTempo) });
}
} // namespace phazite
