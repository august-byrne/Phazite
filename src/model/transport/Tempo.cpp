// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Tempo.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/automation/internal/Automation.h"
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
    juce::ValueTree findTempoMap (const te::Edit& edit)
    {
        return findPhaziteState (edit).getChildWithName (schema::TEMPOMAP);
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
    juce::ValueTree getFirstOfMap (te::Edit& edit, const juce::Identifier& type)
    {
        ensureTempoMap (edit);
        return getEntries (findTempoMap (edit), type).front();
    }

    // Keeps the song's patterns at their speed while the tempo changes, when the song says so (doPatternsFollowTempo()).
    // The song keeps a MIDI clip's notes on their beats, so this scales the beats of its notes, its length and its offset by
    // how much the tempo changed, which leaves them at their times. Each clip still starts on its beat.
    class PatternsKeepTheirSpeed
    {
    public:
        explicit PatternsKeepTheirSpeed (te::Edit& e)
            : edit (e),
              tempoBefore (getTempo (e))
        {
        }

        ~PatternsKeepTheirSpeed()
        {
            const auto scale = getTempo (edit) / tempoBefore;

            if (doPatternsFollowTempo (edit) || juce::approximatelyEqual (scale, 1.0))
                return;

            for (const auto& clipState : getClipStates (edit))
                keepSpeed (clipState, scale);
        }

    private:
        te::Edit& edit;
        const double tempoBefore;

        void keepSpeed (juce::ValueTree clipState, double scale)
        {
            auto* um = &edit.getUndoManager();

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
std::vector<TempoChange> getTempoChanges (const te::Edit& edit)
{
    std::vector<TempoChange> changes;

    for (const auto& entry : getEntries (findTempoMap (edit), schema::TEMPO))
        changes.push_back ({ BeatPosition::fromBeats (entry[schema::mapBeat]), (double) entry.getProperty (schema::bpm, 120.0) });

    if (changes.empty())
        changes.push_back ({});

    return changes;
}

std::vector<TimeSignatureChange> getTimeSignatureChanges (const te::Edit& edit)
{
    std::vector<TimeSignatureChange> changes;

    for (const auto& entry : getEntries (findTempoMap (edit), schema::TIMESIGNATURE))
        changes.push_back ({ BeatPosition::fromBeats (entry[schema::mapBeat]),
                             { (int) entry.getProperty (schema::numerator, 4), (int) entry.getProperty (schema::denominator, 4) } });

    if (changes.empty())
        changes.push_back ({});

    return changes;
}

TimeSignature getTimeSignatureAt (const te::Edit& edit, BeatPosition beat)
{
    const auto changes = getTimeSignatureChanges (edit);
    auto signature = changes.front().signature;

    for (const auto& change : changes)
        if (change.beat <= beat)
            signature = change.signature;

    return signature;
}

void ensureTempoMap (te::Edit& edit)
{
    auto phazite = getPhaziteState (edit);
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

void insertTempoMapTime (te::Edit& edit, BeatPosition at, BeatDuration length)
{
    for (auto entry : findTempoMap (edit))
        if (const auto beat = (double) entry[schema::mapBeat]; beat > 0.0 && beat >= at.inBeats())
            entry.setProperty (schema::mapBeat, beat + length.inBeats(), &edit.getUndoManager());

    mirror::reconcileTempoMap (edit);
}

void removeTempoMapTime (te::Edit& edit, BeatRange range)
{
    auto map = findTempoMap (edit);

    for (int i = map.getNumChildren(); --i >= 0;)
    {
        auto entry = map.getChild (i);
        const auto beat = (double) entry[schema::mapBeat];

        if (beat <= 0.0 || beat < range.getStart().inBeats())
            continue;

        if (beat < range.getEnd().inBeats())
            map.removeChild (i, &edit.getUndoManager());
        else
            entry.setProperty (schema::mapBeat, beat - range.getLength().inBeats(), &edit.getUndoManager());
    }

    mirror::reconcileTempoMap (edit);
}

//==============================================================================
double getTempo (const te::Edit& edit)
{
    return getTempoChanges (edit).front().bpm;
}

void setTempo (te::Edit& edit, double bpm)
{
    const auto rounded = std::round (juce::jlimit (minTempo, maxTempo, bpm) * 100.0) / 100.0;

    if (juce::approximatelyEqual (getTempo (edit), rounded))
        return;

    const UndoStep step (edit, "Change Tempo");
    const AutomationOnBeats keepAutomation (edit);    // as Tracktion keeps the clips
    const FollowingClipsOnBeats keepFollowing (edit); // stretched again for the new tempo
    const PatternsKeepTheirSpeed keepPatterns (edit); // unless they follow it
    getFirstOfMap (edit, schema::TEMPO).setProperty (schema::bpm, rounded, &edit.getUndoManager());
    mirror::reconcileTempoMap (edit); // Tracktion moves the clips with their beats
}

TempoStretch getTempoStretch (te::Edit& edit)
{
    TempoStretch stretch;
    stretch.patternsFollow = doPatternsFollowTempo (edit);

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

bool doPatternsFollowTempo (const te::Edit& edit)
{
    return ! (bool) findPhaziteState (edit).getProperty (schema::patternsKeepTheirSpeed, false);
}

void togglePatternsFollowTempo (te::Edit& edit)
{
    auto state = getPhaziteState (edit);

    if (doPatternsFollowTempo (edit))
        state.setProperty (schema::patternsKeepTheirSpeed, true, &edit.getUndoManager());
    else
        state.removeProperty (schema::patternsKeepTheirSpeed, &edit.getUndoManager());
}

TimeSignature getTimeSignature (const te::Edit& edit)
{
    return getTimeSignatureChanges (edit).front().signature;
}

bool isValid (TimeSignature timeSignature)
{
    const auto denominator = timeSignature.denominator;
    return timeSignature.numerator >= 1 && timeSignature.numerator <= 32
           && (denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16);
}

void setTimeSignature (te::Edit& edit, TimeSignature timeSignature)
{
    if (! isValid (timeSignature) || timeSignature == getTimeSignature (edit))
        return;

    const UndoStep step (edit, "Change Time Signature");
    const AutomationOnBeats keepAutomation (edit);
    auto first = getFirstOfMap (edit, schema::TIMESIGNATURE);
    first.setProperty (schema::numerator, timeSignature.numerator, &edit.getUndoManager());
    first.setProperty (schema::denominator, timeSignature.denominator, &edit.getUndoManager());
    mirror::reconcileTempoMap (edit);
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
                    .perform = withEdit (selection, togglePatternsFollowTempo),
                    .isEnabled = hasEdit,
                    .undoTarget = undoTarget,
                    .isTicked = ifEdit (selection, doPatternsFollowTempo) });
}
} // namespace phazite
