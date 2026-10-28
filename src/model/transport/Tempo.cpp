// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Tempo.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"

#include <cmath>

namespace phazite
{
namespace
{
    // Keeps the song's patterns at their speed while the tempo changes, when the song says so (doPatternsFollowTempo()).
    // Tracktion keeps a MIDI clip's notes on their beats, so this scales the beats of its notes, its length, its offset and
    // its loop by how much the tempo changed, which leaves them at their times. Each clip still starts on its beat.
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

            for (auto* track : te::getAudioTracks (edit))
                for (auto* clip : track->getClips())
                    if (auto* midi = dynamic_cast<te::MidiClip*> (clip))
                        keepSpeed (*midi, scale);
        }

    private:
        te::Edit& edit;
        const double tempoBefore;

        void keepSpeed (te::MidiClip& clip, double scale)
        {
            auto* um = &edit.getUndoManager();
            const auto scaled = [scale] (auto beats) { return std::remove_cvref_t<decltype (beats)>::fromBeats (beats.inBeats() * scale); };
            auto& sequence = clip.getSequence();

            for (auto* note : sequence.getNotes())
                note->setStartAndLength (scaled (note->getStartBeat()), scaled (note->getLengthBeats()), um);

            for (auto* event : sequence.getControllerEvents())
                event->setBeatPosition (scaled (event->getBeatPosition()), um);

            for (auto* event : sequence.getSysexEvents())
                event->setBeatPosition (scaled (event->getBeatPosition()), um);

            if (clip.isLooping())
                clip.setLoopRangeBeats ({ scaled (clip.getLoopStartBeats()), scaled (clip.getLoopLengthBeats()) });

            const auto& tempo = edit.tempoSequence;
            const auto start = clip.getPosition().getStart();
            clip.setPosition (
                { { start, tempo.toTime (clip.getStartBeat() + scaled (clip.getLengthInBeats())) },
                  te::TimeDuration::fromSeconds (scaled (clip.getOffsetInBeats()).inBeats() / tempo.getBeatsPerSecondAt (start)) });
        }
    };
} // namespace

double getTempo (const te::Edit& edit)
{
    return edit.tempoSequence.getTempo (0)->getBpm();
}

void setTempo (te::Edit& edit, double bpm)
{
    const auto rounded = std::round (juce::jlimit (minTempo, maxTempo, bpm) * 100.0) / 100.0;
    auto* tempo = edit.tempoSequence.getTempo (0);

    if (juce::approximatelyEqual (tempo->getBpm(), rounded))
        return;

    const UndoStep step (edit, "Change Tempo");
    const AutomationOnBeats keepAutomation (edit);    // as Tracktion keeps the clips
    const FollowingClipsOnBeats keepFollowing (edit); // stretched again for the new tempo
    const PatternsKeepTheirSpeed keepPatterns (edit); // unless they follow it
    tempo->setBpm (rounded);                          // Tracktion moves the clips with their beats
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
    const auto* timeSig = edit.tempoSequence.getTimeSig (0);
    return { timeSig->numerator.get(), timeSig->denominator.get() };
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
    auto* timeSig = edit.tempoSequence.getTimeSig (0);
    timeSig->numerator = timeSignature.numerator;
    timeSig->denominator = timeSignature.denominator;
}

void addTempoCommands (CommandRegistry& registry, te::SelectionManager& selection)
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
