// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Capture.h"
#include "internal/Playback.h"
#include "internal/Recording.h"
#include "internal/Tempo.h"
#include "model/arrangement/internal/Clips.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/LiveInput.h"
#include "model/notes/internal/Notes.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace phazite
{
namespace
{
    // One octave, so that a tempo and its double don't both fit.
    constexpr double slowestTempo = 80.0, fastestTempo = 160.0, tempoStep = 0.05;

    // The mean square distance of the notes from the grid, in sixteenths, above which no tempo fits them (notes
    // played at random lie about 0.083 from it).
    constexpr double worstFit = 0.03;

    constexpr double sixteenthInBeats = 0.25; // Tracktion's "1/4 beat", as Recording.h quantizes

    struct Fit
    {
        double error = 0.0;    // the mean square distance from the nearest sixteenth, in sixteenths
        double offBeats = 0.0; // how far the notes fall from the beats: 0 on a beat, 0.5 on an eighth, 1 on a sixteenth
    };

    Fit fitAt (const std::vector<double>& starts, double bpm)
    {
        const auto sixteenth = 15.0 / bpm; // seconds
        const auto first = starts.front();

        // Where the grid lies: the notes' places in sixteenths averaged round the circle, so that a first note played
        // a little early or late doesn't throw the others off.
        auto sinSum = 0.0, cosSum = 0.0;

        for (const auto start : starts)
        {
            const auto angle = juce::MathConstants<double>::twoPi * (start - first) / sixteenth;
            sinSum += std::sin (angle);
            cosSum += std::cos (angle);
        }

        const auto phase = std::atan2 (sinSum, cosSum) / juce::MathConstants<double>::twoPi;
        Fit fit;

        for (const auto start : starts)
        {
            const auto place = (start - first) / sixteenth - phase;
            const auto nearest = std::round (place);
            fit.error += (place - nearest) * (place - nearest);

            const auto inBeat = (static_cast<long long> (nearest) % 4 + 4) % 4;
            fit.offBeats += inBeat == 0 ? 0.0 : (inBeat == 2 ? 0.5 : 1.0);
        }

        fit.error /= (double) starts.size();
        fit.offBeats /= (double) starts.size();
        return fit;
    }

    // Mostly how well the notes fit; then, between tempos where they fit as well, more of them on beats, and a tempo
    // nearer 120.
    double scoreOf (const Fit& fit, double bpm)
    {
        const auto octavesFrom120 = std::log2 (bpm / 120.0);
        return fit.error + 0.02 * fit.offBeats + 0.01 * octavesFrom120 * octavesFrom120;
    }

    struct CapturedNote
    {
        int pitch = 60, velocity = 100;
        double start = 0.0, end = 0.0; // seconds: in the song while it plays, or as they were played
    };

    // The notes in the clips Tracktion's capture made, which go again.
    std::vector<CapturedNote> takeNotes (const juce::Array<te::Clip*>& clips)
    {
        std::vector<CapturedNote> notes;

        for (auto* clip : clips)
        {
            if (auto* midi = dynamic_cast<te::MidiClip*> (clip))
            {
                const auto& tempo = midi->edit.tempoSequence;

                for (auto* note : midi->getSequence().getNotes())
                {
                    const auto start = getNoteStart (*midi, *note);
                    notes.push_back ({ note->getNoteNumber(), note->getVelocity(), tempo.toTime (start).inSeconds(),
                                       tempo.toTime (start + note->getLengthBeats()).inSeconds() });
                }
            }
        }

        for (auto* clip : clips)
            clip->removeFromParent();

        std::sort (notes.begin(), notes.end(), [] (const CapturedNote& a, const CapturedNote& b) { return a.start < b.start; });
        return notes;
    }

    // The first bar line from one, in beats, after which a clip of some beats would overlap none of the track's clips.
    double findRoom (te::ClipTrack& track, double bar, double length, double beatsPerBar)
    {
        for (bool moved = true; moved;)
        {
            moved = false;

            for (auto* clip : track.getClips())
            {
                const auto end = clip->getEndBeat().inBeats();

                if (clip->getStartBeat().inBeats() < bar + length && end > bar)
                {
                    bar = std::ceil (end / beatsPerBar - 1.0e-9) * beatsPerBar;
                    moved = true;
                }
            }
        }

        return bar;
    }
} // namespace

bool canCapture (te::Edit& edit)
{
    return getLiveMidiTrack (edit) != nullptr && edit.getCurrentPlaybackContext() != nullptr;
}

te::MidiClip* capture (te::Edit& edit, te::SelectionManager* selection)
{
    auto* track = getLiveMidiTrack (edit);
    auto* context = edit.getCurrentPlaybackContext();
    const auto device = getAllMidiInputs (edit.engine);

    if (track == nullptr || context == nullptr || device == nullptr)
        return nullptr;

    const UndoStep step (edit, "Capture");
    const bool playing = edit.getTransport().isPlaying();
    const bool songWasEmpty = getSongEnd (edit) == te::TimePosition();

    // Tracktion's capture makes clips of what was played, which Phazite takes the notes out of, unquantized, to place
    // them itself.
    const auto quantizing = device->quantisation.getType (false);
    device->quantisation.setType ("(none)");
    juce::Array<te::Clip*> made;
    context->applyRetrospectiveRecord (&made, false);
    device->quantisation.setType (quantizing);

    auto notes = takeNotes (made);

    // While the song plays: only what was played since it started.
    if (playing)
        notes.erase (std::remove_if (notes.begin(), notes.end(), [] (const CapturedNote& n) { return n.start < 0.0; }), notes.end());

    if (notes.empty())
        return nullptr;

    // In beats: where they were played in the song, or from the first note, at the song's tempo (set from the notes
    // in a song without clips).
    auto& tempo = edit.tempoSequence;
    std::vector<std::pair<double, double>> beats; // start and end

    if (playing)
    {
        for (const auto& n : notes)
            beats.emplace_back (tempo.toBeats (te::TimePosition::fromSeconds (n.start)).inBeats(),
                                tempo.toBeats (te::TimePosition::fromSeconds (n.end)).inBeats());
    }
    else
    {
        if (songWasEmpty)
        {
            std::vector<double> starts;

            for (const auto& n : notes)
                starts.push_back (n.start);

            if (const auto detected = detectTempo (starts))
                setTempo (edit, *detected); // part of this step
        }

        const auto beatsPerSecond = getTempo (edit) / 60.0;

        for (const auto& n : notes)
            beats.emplace_back ((n.start - notes.front().start) * beatsPerSecond, (n.end - notes.front().start) * beatsPerSecond);
    }

    // Whole bars, from the bar the first note is in to the bar the last one ends in.
    const auto beatsPerBar = (double) getTimeSignature (edit).numerator;
    auto clipStart = std::floor (beats.front().first / beatsPerBar) * beatsPerBar;
    auto lastEnd = clipStart;

    for (const auto& [start, end] : beats)
        lastEnd = std::max (lastEnd, end);

    const auto length = std::max (beatsPerBar, std::ceil ((lastEnd - clipStart) / beatsPerBar - 1.0e-9) * beatsPerBar);

    // While stopped: at the bar the play start is in, or the first after the track's clips with room for it.
    if (! playing)
    {
        const auto playStartBar = std::floor (tempo.toBeats (getPlayStart (edit)).inBeats() / beatsPerBar) * beatsPerBar;
        const auto bar = findRoom (*track, playStartBar, length, beatsPerBar);

        for (auto& [start, end] : beats)
        {
            start += bar;
            end += bar;
        }

        clipStart = bar;
    }

    auto* clip =
        insertMidiClip (*track, { te::BeatPosition::fromBeats (clipStart), te::BeatPosition::fromBeats (clipStart + length) }, selection);

    if (clip == nullptr)
        return nullptr;

    const bool quantize = isRecordQuantizeOn (edit.engine);

    for (size_t i = 0; i < notes.size(); ++i)
    {
        auto [start, end] = beats[i];

        if (quantize)
        {
            const auto noteLength = end - start;
            start = std::round (start / sixteenthInBeats) * sixteenthInBeats;
            end = start + noteLength;
        }

        addNote (*clip, notes[i].pitch, te::BeatPosition::fromBeats (start),
                 te::BeatDuration::fromBeats (std::max (end - start, getMinimumNoteLength().inBeats())), notes[i].velocity);
    }

    return clip;
}

std::optional<double> detectTempo (const std::vector<double>& noteStartSeconds)
{
    if (noteStartSeconds.size() < 4 || noteStartSeconds.back() - noteStartSeconds.front() < 1.0)
        return {};

    auto best = slowestTempo, bestScore = std::numeric_limits<double>::max();
    Fit bestFit;

    for (int i = 0; slowestTempo + i * tempoStep < fastestTempo; ++i)
    {
        const auto bpm = slowestTempo + i * tempoStep;
        const auto fit = fitAt (noteStartSeconds, bpm);

        if (const auto score = scoreOf (fit, bpm); score < bestScore)
        {
            best = bpm;
            bestScore = score;
            bestFit = fit;
        }
    }

    if (bestFit.error > worstFit)
        return {};

    return std::round (best);
}

void addCaptureCommands (CommandRegistry& registry, te::SelectionManager& selection)
{
    registry.add (
        { .id = "transport.capture",
          .name = "Capture",
          .category = "Transport",
          .description = "Turns what you played in the last 30 seconds, recording or not, into a clip on the track live MIDI plays.",
          .synonyms = { "capture midi", "retrospective record", "keep what I played", "flashback", "recover notes" },
          .perform = withEdit (selection, [&selection] (te::Edit& edit) { capture (edit, &selection); }),
          .isEnabled = ifEdit (selection, canCapture) });
}
} // namespace phazite
