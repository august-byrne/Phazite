// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Capture.h"
#include "internal/Playback.h"
#include "internal/Recording.h"
#include "internal/Take.h"
#include "internal/Tempo.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/Numbers.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/LiveInput.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace phazite
{
namespace
{
    // One octave, so that a tempo and its double don't both fit.
    constexpr double slowestTempo = 80.0, fastestTempo = 160.0, tempoStep = 0.05;

    // The mean square distance of the notes from the grid, in sixteenths, above which no tempo fits them (notes
    // played at random lie about 0.083 from it).
    constexpr double worstFit = 0.03;

    constexpr double sixteenthInBeats = 0.25; // as recordings are quantized (Recording.h, Take.h)

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

            const auto inBeat = (toWhole<long long> (nearest) % 4 + 4) % 4; // a device's rate of 0 makes it no number
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

    // A controller moved, the pitch bend or a key's pressure, in MIDI's terms (Take.h's PlayedControl), and when, as a
    // note's start.
    struct CapturedControl
    {
        ControlKind kind = ControlKind::controller;
        int number = 0, value = 0;
        double at = 0.0;
        bool held = false; // from before the first note: written where the clip starts
    };

    constexpr int sustainPedal = 64, bendMiddle = 8192;

    // Whether a control's value holds what it sets, to be carried to where a clip starts: the pedal down, the bend away
    // from its middle, the channel's pressure pressed.
    bool holds (const CapturedControl& control)
    {
        return (control.kind == ControlKind::pitchWheel && control.value != bendMiddle)
               || (control.kind == ControlKind::channelPressure && control.value > 0)
               || (control.kind == ControlKind::controller && control.number == sustainPedal && control.value >= 64);
    }

    // The notes played live into Phazite's engine in its last 30 seconds (PlaybackHost.h): while the song plays, those
    // played since it started, where they were heard in it (by the output's delay and the song's delay compensation, as a
    // take places them: Take.h); otherwise as they were played. One still held ends now. The controllers and the bend
    // played among them go with them, from the first note on: the pedal held down and the bend away from its middle as it
    // was played, there.
    std::vector<CapturedNote> takePlayedNotes (PlaybackSession& session, bool playing, std::vector<CapturedControl>& controls)
    {
        auto& engine = session.getEngine();
        const auto rate = engine.getSampleRate();
        const auto* plan = engine.getPlan();
        const auto shift = session.getOutputLatency() + (plan != nullptr ? engine::Engine::getOutputLatency (*plan) : 0);
        std::vector<CapturedNote> notes;
        std::vector<std::int64_t> startClocks; // each note's
        std::array<int, 128> sounding;
        sounding.fill (-1);
        std::array<int, Take::numControlKeys> lastValue; // each controller's (Take::getKey()), as played last
        lastValue.fill (-1);
        std::array<std::ptrdiff_t, Take::numControlKeys> lastIndex; // and where its last one kept is in controls
        lastIndex.fill (-1);

        const auto endNote = [&notes, &sounding] (int pitch, double at)
        {
            if (auto& index = sounding[(size_t) pitch]; index >= 0)
            {
                notes[(size_t) index].end = std::max (at, notes[(size_t) index].start);
                index = -1;
            }
        };

        for (const auto& played : session.getPlayedEvents())
        {
            if (playing && (played.clock < session.getPlayStartClock() || played.songSample < 0))
                continue;

            const auto at = playing ? (double) (played.songSample - shift) / rate : (double) played.clock / rate;
            const auto pitch = juce::jlimit (0, 127, played.event.number);
            using Type = devices::NoteEvent::Type;

            if (played.event.type == Type::noteOn)
            {
                endNote (pitch, at);
                sounding[(size_t) pitch] = (int) notes.size();
                notes.push_back ({ pitch, juce::jlimit (1, 127, juce::roundToInt (played.event.value * 127.0f)), at, at });
                startClocks.push_back (played.clock);
            }
            else if (played.event.type == Type::noteOff)
            {
                endNote (pitch, at);
            }
            else if (played.event.type == Type::allNotesOff)
            {
                for (int p = 0; p < 128; ++p)
                    endNote (p, at);
            }
            else if (const auto control = Take::toControl (played.event))
            {
                // A value that repeats the one before it changes nothing; and each controller keeps at most one a
                // millisecond (Take::controlSeconds), the latest at the time of the first, as a take does.
                const auto [kind, number, value] = *control;
                const auto key = Take::getKey (kind, number);

                if (std::exchange (lastValue[key], value) == value)
                    continue;

                if (const auto last = lastIndex[key]; last >= 0 && at - controls[(size_t) last].at < Take::controlSeconds)
                {
                    controls[(size_t) last].value = value;
                    continue;
                }

                lastIndex[key] = (std::ptrdiff_t) controls.size();
                controls.push_back ({ kind, number, value, at });
            }
        }

        // Those still held: as long as they have been.
        for (const auto index : sounding)
            if (index >= 0)
                notes[(size_t) index].end += (double) (engine.getClock() - startClocks[(size_t) index]) / rate;

        std::sort (notes.begin(), notes.end(), [] (const CapturedNote& a, const CapturedNote& b) { return a.start < b.start; });

        // The controls from the first note on; before it, the pedal held down, the bend away from its middle and the
        // channel's pressure held, at it.
        if (! notes.empty())
        {
            const auto first = notes.front().start;
            std::vector<CapturedControl> kept;
            std::array<bool, Take::numControlKeys> found {};

            for (auto control = controls.rbegin(); control != controls.rend(); ++control)
                if (control->at < first && ! std::exchange (found[Take::getKey (control->kind, control->number)], true) && holds (*control))
                    kept.push_back ({ control->kind, control->number, control->value, first, true });

            for (const auto& control : controls)
                if (control.at >= first)
                    kept.push_back (control);

            controls = std::move (kept);
        }

        return notes;
    }

    // The last bar line at or before a beat, and the first at or after it, as the song's time signatures lay its bars out,
    // in beats.
    double getBarStartAt (const Bars& bars, double beat)
    {
        const auto bar = bars.getBarAt (BeatPosition::fromBeats (beat));
        const auto start = bars.getBarStart (bar).inBeats();
        return start <= beat ? start : bars.getBarStart (bar - 1).inBeats();
    }

    double getBarLineFrom (const Bars& bars, double beat)
    {
        const auto bar = bars.getBarAt (BeatPosition::fromBeats (beat));
        const auto start = bars.getBarStart (bar).inBeats();
        return start >= beat ? start : bars.getBarStart (bar + 1).inBeats();
    }

    // The first bar line from one, in beats, after which a clip of some beats would overlap none of the track's clips.
    double findRoom (const Song& song, TrackID track, double bar, double length, const Bars& bars)
    {
        for (bool moved = true; moved;)
        {
            moved = false;

            for (const auto& clipState : getClipStates (findTrackState (song, track)))
            {
                const auto range = getClipRange (song, clipState);
                const auto end = range.getEnd().inBeats();

                if (range.getStart().inBeats() < bar + length && end > bar)
                {
                    bar = getBarLineFrom (bars, end);
                    moved = true;
                }
            }
        }

        return bar;
    }
} // namespace

bool canCapture (const Song& song)
{
    return getLiveMidiTrack (song).isValid() && findPlayingSession (song) != nullptr;
}

ClipID capture (Song& song, Selection* selection)
{
    const auto track = getLiveMidiTrack (song);
    auto* session = findPlayingSession (song);

    if (! track.isValid() || session == nullptr)
        return {};

    const UndoStep step (song, "Capture");
    const bool playing = isSongPlaying (song);
    const bool songWasEmpty = getSongEnd (song) <= BeatPosition();
    std::vector<CapturedControl> controls;
    const auto notes = takePlayedNotes (*session, playing, controls);

    if (notes.empty())
        return {};

    // In beats: where they were played in the song, or from the first note, at the song's tempo (set from the notes
    // in a song without clips).
    std::vector<std::pair<double, double>> beats; // start and end
    std::vector<double> controlBeats;

    if (playing)
    {
        for (const auto& n : notes)
            beats.emplace_back (toBeats (song, n.start).inBeats(), toBeats (song, n.end).inBeats());

        for (const auto& control : controls)
            controlBeats.push_back (toBeats (song, control.at).inBeats());
    }
    else
    {
        if (songWasEmpty)
        {
            std::vector<double> starts;

            for (const auto& n : notes)
                starts.push_back (n.start);

            if (const auto detected = detectTempo (starts))
                setTempo (song, *detected); // part of this step
        }

        const auto beatsPerSecond = getTempo (song) / 60.0;

        for (const auto& n : notes)
            beats.emplace_back ((n.start - notes.front().start) * beatsPerSecond, (n.end - notes.front().start) * beatsPerSecond);

        for (const auto& control : controls)
            controlBeats.push_back ((control.at - notes.front().start) * beatsPerSecond);
    }

    // Whole bars, from the bar the first note is in to the bar the last one ends in.
    const auto bars = getBars (song);
    auto clipStart = getBarStartAt (bars, beats.front().first);
    auto lastEnd = clipStart;

    for (const auto& [start, end] : beats)
        lastEnd = std::max (lastEnd, end);

    const auto length =
        std::max (bars.getBarLength (BeatPosition::fromBeats (clipStart)).inBeats(), getBarLineFrom (bars, lastEnd - 1.0e-9) - clipStart);

    // While stopped: at the bar the play start is in, or the first after the track's clips with room for it.
    if (! playing)
    {
        const auto playStartBar = getBarStartAt (bars, getPlayStartBeat (song).inBeats());
        const auto bar = findRoom (song, track, playStartBar, length, bars);

        for (auto& [start, end] : beats)
        {
            start += bar;
            end += bar;
        }

        for (auto& beat : controlBeats)
            beat += bar;

        clipStart = bar;
    }

    const auto clip =
        insertMidiClip (song, track, { BeatPosition::fromBeats (clipStart), BeatPosition::fromBeats (clipStart + length) }, selection);

    if (! clip.isValid())
        return {};

    const bool quantize = isRecordQuantizeOn (session->getAudioIO());

    for (size_t i = 0; i < notes.size(); ++i)
    {
        auto [start, end] = beats[i];

        if (quantize)
        {
            const auto noteLength = end - start;
            start = std::round (start / sixteenthInBeats) * sixteenthInBeats;
            end = start + noteLength;
        }

        addNote (song, clip, notes[i].pitch, BeatPosition::fromBeats (start),
                 BeatDuration::fromBeats (std::max (end - start, getMinimumNoteLength().inBeats())), notes[i].velocity);
    }

    // The controls where they were played, as a take keeps them (Take.h), not quantized; those held from before the first
    // note where the clip starts, before any note quantizing could move.
    // A pedal still down, a bend still away from its middle or the channel's pressure still held at the clip's end is let go
    // of just before it (a control at the end isn't played, and the song plays its clips' controls as one stream, so a
    // pedal left down would hold the next clip's notes).
    auto pedal = 0, bend = bendMiddle, pressure = 0;
    std::vector<ControlValue> values;

    for (size_t i = 0; i < controls.size(); ++i)
    {
        if (controlBeats[i] >= clipStart + length)
            continue;

        const auto& control = controls[i];
        values.push_back (
            { BeatPosition::fromBeats (control.held ? clipStart : controlBeats[i]), control.kind, control.number, control.value });

        if (control.kind == ControlKind::pitchWheel)
            bend = control.value;
        else if (control.kind == ControlKind::channelPressure)
            pressure = control.value;
        else if (control.kind == ControlKind::controller && control.number == sustainPedal)
            pedal = control.value;
    }

    constexpr auto releaseBeforeEnd = 1.0 / 960.0; // in beats: a tick, at 960 to the quarter note
    const auto end = BeatPosition::fromBeats (clipStart + length - releaseBeforeEnd);

    if (pedal >= 64)
        values.push_back ({ end, ControlKind::controller, sustainPedal, 0 });

    if (bend != bendMiddle)
        values.push_back ({ end, ControlKind::pitchWheel, 0, bendMiddle });

    if (pressure > 0)
        values.push_back ({ end, ControlKind::channelPressure, 0, 0 });

    addControls (song, clip, values); // at once: the clip Capture made, filled in one change
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

void addCaptureCommands (CommandRegistry& registry, Selection& selection)
{
    registry.add (
        { .id = "transport.capture",
          .name = "Capture",
          .category = "Transport",
          .description = "Turns what you played in the last 30 seconds, recording or not, into a clip on the track live MIDI plays.",
          .synonyms = { "capture midi", "retrospective record", "keep what I played", "flashback", "recover notes" },
          .perform = withSong (selection, [&selection] (Song& song) { capture (song, &selection); }),
          .isEnabled = ifSong (selection, canCapture) });
}
} // namespace phazite
