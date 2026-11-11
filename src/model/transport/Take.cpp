// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Take.h"
#include "internal/AudioRecorder.h"
#include "internal/Playback.h"
#include "internal/Recording.h"
#include "internal/Tempo.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/LiveInput.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <juce_audio_formats/juce_audio_formats.h>
#include <limits>
#include <mutex>

namespace phazite
{
namespace
{
    // What an input's buffer holds before the writing thread writes it, in seconds: room for the thread to fall far behind.
    constexpr double bufferSeconds = 8.0;

    // How often the thread writes what has come in, in milliseconds.
    constexpr int writeEveryMilliseconds = 20;

    constexpr double sixteenthInBeats = 0.25; // as Capture quantizes (Capture.h)

    // The waveform the playlist draws of an input while it records keeps the loudest sample of each stretch of so many
    // frames: 5 ms at 48 kHz, finer than a pixel at the zooms a recording is watched at.
    constexpr int peakFrames = 256;

    // Silence is written from a block of so many frames.
    constexpr int silenceFrames = 4096;

    // A count of frames in whole stretches of peakFrames, rounded down (towards the past, for frames before the first).
    std::int64_t toPeaks (std::int64_t frames)
    {
        return frames >= 0 ? frames / peakFrames : -((-frames + peakFrames - 1) / peakFrames);
    }

    // A place in the song as the user reads it: "bar 5", or "bar 5, beat 3" after its first beat, in whole beats; bars
    // count from 1, as the time line's do.
    juce::String describePlace (const Bars& bars, BeatPosition place)
    {
        const auto bar = bars.getBarAt (place);
        const auto beat = (int) std::floor ((place - bars.getBarStart (bar)).inBeats() + 1.0e-6);
        return "bar " + juce::String (bar + 1) + (beat > 0 ? ", beat " + juce::String (beat + 1) : juce::String());
    }

    // So long, as the user reads it: "2.0 s", or "350 ms" under a second.
    juce::String describeDuration (double seconds)
    {
        return seconds < 0.95 ? juce::String (juce::roundToInt (seconds * 1000.0)) + " ms" : juce::String (seconds, 1) + " s";
    }

    // "A", "A and B", "A, B and C".
    juce::String joinAsWords (const juce::StringArray& parts)
    {
        if (parts.size() < 2)
            return parts.joinIntoString ({});

        return parts.joinIntoString (", ", 0, parts.size() - 1) + " and " + parts[parts.size() - 1];
    }
} // namespace

// An armed track's input: the engine's buffer of it, and the file it goes into.
struct Take::AudioTake
{
    TrackID track;
    juce::File file;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    engine::Recording::Input* input = nullptr; // the engine's recording's

    // The frames in the file: first a lead of silence (Take::getLead()), once the take has started, then the take's, those
    // that came in and silence for those lost.
    std::int64_t lead = -1; // until it is written
    std::int64_t written = 0;

    // Each stretch of frames of the file that was lost, and written as silence: the writing thread's, then the message
    // thread's at the end.
    struct Lost
    {
        std::int64_t start = 0, length = 0;
    };

    std::vector<Lost> lost;
    std::vector<float> silence; // what silence is written from, once some is

    // What the take's clips keep of it, in the song's samples as if nothing looped, once it is placed (placeTake()).
    std::vector<juce::Range<std::int64_t>> kept;

    // The loudest sample of each peakFrames frames written, for the waveform the playlist draws while it records
    // (getLevels()): the writing thread adds to them, the message thread reads them, each under the lock (neither is the
    // audio thread); and the loudest of the frames written since the last.
    mutable std::mutex peaksLock;
    std::vector<float> peaks;
    float peak = 0.0f;
    int peakCount = 0;

    // The writing thread, then the message thread at the end, once the take has started: the lead of silence the file starts
    // with, once; then what has come in, as far as so many frames of the take in all, with silence where some were lost
    // (engine::Recording::Input::read()), so that what came after them stays where it was played.
    void write (std::int64_t leadFrames, std::int64_t upTo)
    {
        std::vector<float> found;

        if (lead < 0 && writer != nullptr)
        {
            lead = leadFrames;
            writeZeros (lead, found);
        }

        while (written - lead < upTo && writer != nullptr)
        {
            const auto frames = input->read (
                std::min<std::int64_t> (upTo - (written - lead), 1 << 16),
                [this, &found] (const float* const* channels, int numChannels, int count)
                {
                    writer->writeFromFloatArrays (channels, numChannels, count);
                    written += count;
                    addPeaks (channels, numChannels, count, found);
                },
                [this, &found] (std::int64_t count) { writeLost (count, found); });

            if (frames <= 0)
                break;
        }

        keepPeaks (found);
    }

    // The message thread, as the take ends: all of it, as far as so many frames of the take in all. Those lost at its end,
    // which the engine has noted no gap for (nothing came in after them), are silence too.
    void finish (std::int64_t leadFrames, std::int64_t length)
    {
        write (leadFrames, length);

        if (writer != nullptr && written - lead < length)
        {
            std::vector<float> found;
            writeLost (length - (written - lead), found);
            keepPeaks (found);
        }
    }

    void writeLost (std::int64_t count, std::vector<float>& found)
    {
        if (! lost.empty() && lost.back().start + lost.back().length == written)
            lost.back().length += count;
        else
            lost.push_back ({ written, count });

        writeZeros (count, found);
    }

    void writeZeros (std::int64_t count, std::vector<float>& found)
    {
        silence.resize ((size_t) silenceFrames, 0.0f);
        const float* channels[] = { silence.data(), silence.data() };
        const auto numChannels = (int) input->getChannels().size();

        for (auto left = count; left > 0;)
        {
            const auto part = (int) std::min<std::int64_t> (left, silenceFrames);
            writer->writeFromFloatArrays (channels, numChannels, part);
            addPeaks (channels, numChannels, part, found);
            written += part;
            left -= part;
        }
    }

    void addPeaks (const float* const* channels, int numChannels, int count, std::vector<float>& found)
    {
        for (int i = 0; i < count; ++i)
        {
            for (int channel = 0; channel < numChannels; ++channel)
                peak = std::max (peak, std::abs (channels[channel][i]));

            if (++peakCount == peakFrames)
            {
                found.push_back (peak);
                peak = 0.0f;
                peakCount = 0;
            }
        }
    }

    void keepPeaks (const std::vector<float>& found)
    {
        if (! found.empty())
        {
            const std::scoped_lock lock (peaksLock);
            peaks.insert (peaks.end(), found.begin(), found.end());
        }
    }
};

// The thread that writes what the inputs' buffers hold into their files, a few dozen times a second.
class Take::Writer : public juce::Thread
{
public:
    explicit Writer (Take& written)
        : juce::Thread ("Phazite take writer"),
          take (written)
    {
        startThread (juce::Thread::Priority::high);
    }

    // It finishes what it is writing however long the drive takes, never killed in the middle of a write (a time limit here
    // would end it with Windows' TerminateThread, which can leave the file broken and a lock of the heap's or the C runtime's
    // held); then the message thread writes the rest (Take::close()), as other DAWs finish their files at Stop.
    ~Writer() override { stopThread (-1); }

    void run() override
    {
        while (! threadShouldExit())
        {
            if (held.load())
            {
                waiting.store (true);
                wait (1);
                continue;
            }

            take.write (std::numeric_limits<std::int64_t>::max());
            wait (writeEveryMilliseconds);
        }
    }

    // Held, it writes nothing from its next turn until let go (Take::holdWriting()): holding it waits for that turn.
    void hold (bool shouldHold)
    {
        waiting.store (false);
        held.store (shouldHold);

        if (shouldHold)
        {
            notify();

            while (! waiting.load() && isThreadRunning())
                juce::Thread::sleep (1);
        }
    }

private:
    Take& take;
    std::atomic<bool> held { false }, waiting { false };
};

//==============================================================================
Take::Take (PlaybackSession& s)
    : session (s)
{
}

Take::~Take()
{
    close (false);
}

std::unique_ptr<Take> Take::start (PlaybackSession& session)
{
    auto& song = session.getSong();
    auto& engine = session.getEngine();
    const auto rate = engine.getSampleRate();
    const auto* plan = engine.getPlan();

    if (plan == nullptr)
        return nullptr;

    std::unique_ptr<Take> take (new Take (session));
    take->tempo = plan->tempo;

    // A punched take doesn't loop; the song's own loop doesn't loop a take.
    if (isPunching (song))
        take->punch = getLoopBeats (song);
    else if (isLoopOn (song) && ! doesLoopFollowSong (song))
        take->loop = getLoopBeats (song);

    // The loop in samples, as the engine plays it (Engine::process() rounds its ends to samples by the plan's tempo).
    if (take->loop.has_value())
        if (const TakeLoop range { take->toSample (take->loop->getStart()), take->toSample (take->loop->getEnd()) }; range.getLength() > 0)
            take->looped = range;

    take->midiTrack = session.recordsLiveTrack() ? getLiveMidiTrack (song) : TrackID();

    // Each armed track's input the audio device has, into a file of its own.
    std::vector<std::unique_ptr<engine::Recording::Input>> inputs;
    auto& recorder = session.getRecorder();

    for (const auto track : recorder.getArmedTracks())
    {
        auto channels = getInputChannels (findTrackState (song, track)[schema::trackInput].toString());

        if (channels.empty() || channels.back() >= session.getNumInputs())
            continue;

        auto audio = std::make_unique<AudioTake>();
        audio->track = track;
        audio->file = recorder.getTakeFile (track);
        audio->file.getParentDirectory().createDirectory();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (audio->file);

        if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
            continue;

        audio->writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels ((int) channels.size()).withBitsPerSample (24));

        if (audio->writer == nullptr)
        {
            audio->file.deleteFile();
            continue;
        }

        auto input = std::make_unique<engine::Recording::Input> (std::move (channels), (int) (bufferSeconds * rate));
        audio->input = input.get();
        inputs.push_back (std::move (input));
        take->audio.push_back (std::move (audio));
    }

    if (! take->midiTrack.isValid() && take->audio.empty())
        return nullptr;

    // How late what comes in is: a note by the output's delay and the song's delay compensation, as it was heard; an
    // input's sound by its own delay and the audio settings' correction too.
    const auto compensation = engine::Engine::getOutputLatency (*plan);
    const auto correction = session.getAudioIO().getRecordingCorrection(); // milliseconds
    take->midiShift = session.getOutputLatency() + compensation;
    take->audioShift = session.getInputLatency() + take->midiShift + std::llround (correction * rate / 1000.0);

    auto recording = std::make_unique<engine::Recording> (std::move (inputs));
    take->recording = recording.get();
    engine.setRecording (std::move (recording));
    session.keepPlayedFrom (engine.getClock());

    if (! take->audio.empty())
        take->writer = std::make_unique<Writer> (*take);

    return take;
}

void Take::write (std::int64_t upTo)
{
    if (recording == nullptr || ! recording->hasStarted())
        return;

    const auto lead = getLead (recording->getStartSample());

    for (auto& take : audio)
        take->write (lead, upTo);
}

std::int64_t Take::getLead (std::int64_t startSample) const
{
    const auto cameIn = startSample - audioShift; // where what came in first was played
    return looped.has_value() && cameIn > looped->start ? cameIn - looped->start : 0;
}

void Take::close (bool keep)
{
    if (finished)
        return;

    finished = true;
    const auto length = recording != nullptr ? recording->getLength() : 0;
    const auto lead = recording != nullptr && recording->hasStarted() ? getLead (recording->getStartSample()) : 0;
    writer = nullptr; // its thread ends, once it has written what it was writing

    // What is left, as far as what the take recorded (the engine may have written on since).
    for (auto& take : audio)
    {
        if (keep)
            take->finish (lead, length);

        take->writer = nullptr; // the file whole

        if (! keep)
            take->file.deleteFile();
    }

    session.getEngine().setRecording (nullptr);
    recording = nullptr;
    session.keepPlayedFrom ({});

    // What was armed or set to be heard meanwhile.
    session.getRecorder().update();
}

void Take::finish()
{
    if (finished || recording == nullptr)
        return;

    const auto started = recording->hasStarted();
    const auto startSample = recording->getStartSample(), startClock = recording->getStartClock();
    const auto length = recording->getLength();

    if (! started || length <= 0)
    {
        close (false);
        return;
    }

    // The notes, before the engine's notes played are let go of.
    placeNotes (startSample, startClock, length);
    close (true);

    for (auto& take : audio)
    {
        // Where in the song, as if nothing looped, the file's first frame was played: what came in first, after its lead of
        // silence.
        const auto lead = std::max<std::int64_t> (0, take->lead);
        TakePlace place;
        place.sampleRate = session.getEngine().getSampleRate();
        place.frames = std::min<std::int64_t> (take->written, lead + length);
        place.lead = lead;
        place.firstFrame = startSample - audioShift - lead;
        place.start = startSample;
        place.stop = startSample + length;
        place.loop = looped;

        if (punch.has_value())
            place.punch = juce::Range (toSample (punch->getStart()), toSample (punch->getEnd()));

        take->kept = placeTake (session.getSong(), take->track, take->file, place);
    }

    if (const auto lost = describeLost (startSample); lost.isNotEmpty() && session.onProblem != nullptr)
        session.onProblem ("Part of the recording was lost", lost);
}

juce::String Take::describeLost (std::int64_t startSample) const
{
    const auto& song = session.getSong();
    const auto bars = getBars (song);
    const auto rate = session.getEngine().getSampleRate();
    const auto toBeat = [this] (std::int64_t sample) { return BeatPosition::fromBeats (tempo->toBeats ((double) sample)); };
    juce::StringArray tracks;

    for (const auto& take : audio)
    {
        if (take->lost.empty() || take->kept.empty())
            continue;

        // Where in the song, as if nothing looped, the file's first frame was played; and whether recording went round a
        // looped range, which made the range's clip a take of each time round.
        const auto firstFrame = startSample - audioShift - std::max<std::int64_t> (0, take->lead);
        const auto wentRound = looped.has_value() && looped->getRound (take->kept.back().getEnd() - 1) > 0;
        juce::StringArray places;

        // Each stretch lost where a clip keeps it.
        for (const auto& stretch : take->lost)
        {
            for (const auto& kept : take->kept)
            {
                const auto from = std::max (firstFrame + stretch.start, kept.getStart());
                const auto end = std::min (firstFrame + stretch.start + stretch.length, kept.getEnd());

                if (end <= from)
                    continue;

                // Where it starts in the song, and round a looped range that recording went round, the take it is in.
                auto place = describePlace (bars, toBeat (looped.has_value() ? looped->fold (from) : from));

                if (wentRound && from >= looped->start)
                    place << " of take " << (looped->getRound (from) + 1);

                places.add (place + " (" + describeDuration ((double) (end - from) / rate) + ")");
            }
        }

        if (! places.isEmpty())
            tracks.add (getTrackName (song, take->track) + " at " + joinAsWords (places));
    }

    if (tracks.isEmpty())
        return {};

    return "The drive didn't keep up while recording, so Phazite kept the take in time with silence where sound was lost: "
           + tracks.joinIntoString ("; ") + ".";
}

//==============================================================================
std::int64_t Take::toSample (BeatPosition beat) const
{
    return (std::int64_t) std::llround (tempo->toSamples (beat.inBeats()));
}

int Take::getRoundHeard (std::int64_t heard) const
{
    return looped.has_value() ? looped->getRound (heard - 1) : 0;
}

std::vector<Take::PlayedNotes> Take::collectNotes (std::int64_t startSample, std::int64_t startClock, std::int64_t length)
{
    std::vector<Played> notes;
    const auto end = startSample + length - midiShift; // what was heard last, as if nothing looped

    // Each note played into the track, from where it was heard to where it ended, as if nothing looped; one still held
    // ends where the take does.
    std::array<int, 128> sounding;
    sounding.fill (-1);

    const auto endNote = [&notes, &sounding] (int pitch, std::int64_t at)
    {
        if (auto& index = sounding[(size_t) pitch]; index >= 0)
        {
            notes[(size_t) index].end = std::max (at, notes[(size_t) index].start);
            index = -1;
        }
    };

    for (const auto& event : session.getPlayedEvents())
    {
        const auto at = startSample + (event.clock - startClock) - midiShift;

        if (event.track != (std::int64_t) midiTrack.getRaw() || at >= end)
            continue;

        using Type = devices::NoteEvent::Type;
        const auto& note = event.event;
        const auto pitch = juce::jlimit (0, 127, note.number);

        if (note.type == Type::noteOn)
        {
            endNote (pitch, at);

            if (at >= startSample)
            {
                sounding[(size_t) pitch] = (int) notes.size();
                notes.push_back ({ pitch, juce::jlimit (1, 127, juce::roundToInt (note.value * 127.0f)), at, end });
            }
        }
        else if (note.type == Type::noteOff)
        {
            endNote (pitch, at);
        }
        else if (note.type == Type::allNotesOff)
        {
            for (int p = 0; p < 128; ++p)
                endNote (p, at);
        }
    }

    // Where each goes in the song, in the parts a clip of them goes over (collectNotes() in Take.h says which).
    const auto byStart = [] (const Played& a, const Played& b) { return a.start < b.start; };
    std::sort (notes.begin(), notes.end(), byStart);
    std::vector<PlayedNotes> parts;

    if (punch.has_value())
    {
        const auto from = std::max (startSample, toSample (punch->getStart()));
        const auto to = std::min (end, toSample (punch->getEnd()));
        std::erase_if (notes, [from, to] (const Played& note) { return note.start < from || note.start >= to; });

        for (auto& note : notes)
            note.end = std::min (note.end, to);

        parts.push_back ({ std::move (notes), from, to, false });
    }
    else if (getRoundHeard (end) > 0)
    {
        PlayedNotes before { {}, startSample, looped->start, false }, inRange { {}, looped->start, looped->end, true };

        for (auto note : notes)
        {
            if (note.start < looped->start)
            {
                note.end = std::min (note.end, looped->start);
                before.notes.push_back (note);
                continue;
            }

            const auto noteLength = note.end - note.start;
            note.start = looped->fold (note.start);
            note.end = std::min (note.start + noteLength, looped->end);
            inRange.notes.push_back (note);
        }

        std::sort (inRange.notes.begin(), inRange.notes.end(), byStart);

        if (before.to > before.from)
            parts.push_back (std::move (before));

        parts.push_back (std::move (inRange));
    }
    else
    {
        parts.push_back ({ std::move (notes), startSample, end, false });
    }

    return parts;
}

void Take::placeNotes (std::int64_t startSample, std::int64_t startClock, std::int64_t length)
{
    auto& song = session.getSong();

    if (const auto state = findTrackState (song, midiTrack); ! state.hasType (schema::TRACK) || isFolderState (state))
        return;

    const auto parts = collectNotes (startSample, startClock, length);
    const auto toBeat = [this] (std::int64_t sample) { return BeatPosition::fromBeats (tempo->toBeats ((double) sample)); };
    std::vector<BeatRange> ranges;

    for (const auto& part : parts)
        ranges.push_back (BeatRange (toBeat (part.from), toBeat (part.to)));

    // Each note where it starts, quantized to sixteenths when recordings are, in the part it starts in then: one played just
    // before a looped range, quantized to its start, goes into the range's clip. Quantized round a looped range, a note
    // played just before its end goes round to its start, where it would sound the next time round; at the end, it would make
    // the clip longer than the range and never play.
    struct Placed
    {
        int pitch = 60, velocity = 100;
        double start = 0.0, length = 0.0;
        size_t part = 0;
    };

    std::vector<Placed> placed;
    const auto quantize = isRecordQuantizeOn (session.getAudioIO());

    for (size_t p = 0; p < parts.size(); ++p)
    {
        if (ranges[p].isEmpty())
            continue;

        for (const auto& note : parts[p].notes)
        {
            auto start = toBeat (note.start).inBeats();
            const auto noteLength = toBeat (note.end).inBeats() - start;
            auto in = p;

            if (quantize)
            {
                start = std::round (start / sixteenthInBeats) * sixteenthInBeats;

                if (loop.has_value() && start >= loop->getEnd().inBeats() - 1.0e-9)
                    start -= loop->getLength().inBeats();

                if (in + 1 < parts.size() && start >= ranges[in + 1].getStart().inBeats() - 1.0e-9)
                    ++in;
            }

            placed.push_back ({ note.pitch, note.velocity, start, noteLength, in });
        }
    }

    // The clip each goes in: with Overdub, the MIDI clip it starts in, or one made for the gap it falls in, between the clips
    // around it within its part (all in the one that holds the first, the rest went outside it and never played); otherwise
    // a new one over each part that has notes, in place of what was there.
    const auto overdub = isOverdubOn (session.getAudioIO());
    std::vector<ClipID> recorded (parts.size());

    if (! overdub)
    {
        for (size_t p = 0; p < parts.size(); ++p)
        {
            if (std::none_of (placed.begin(), placed.end(), [p] (const Placed& note) { return note.part == p; }))
                continue;

            clearClipsIn (song, midiTrack, ranges[p]);
            recorded[p] = insertMidiClip (song, midiTrack, ranges[p], nullptr);
        }
    }

    const auto clipFor = [&] (size_t part, BeatPosition beat) -> ClipID
    {
        if (! overdub)
            return recorded[part];

        const auto clips = getClipStates (findTrackState (song, midiTrack));

        for (const auto& clipState : clips)
            if (isMidiState (clipState) && getClipRange (clipState).getStart() <= beat && getClipRange (clipState).getEnd() > beat)
                return getClipID (clipState);

        const auto& range = ranges[part];
        auto from = std::min (range.getStart(), beat), to = range.getEnd(); // quantized, a note can start before what was recorded

        for (const auto& clipState : clips)
        {
            if (const auto around = getClipRange (clipState); around.getEnd() <= beat)
                from = std::max (from, around.getEnd());
            else if (around.getStart() > beat)
                to = std::min (to, around.getStart());
        }

        return to > from ? insertMidiClip (song, midiTrack, { from, to }, nullptr) : ClipID();
    };

    for (const auto& note : placed)
        if (const auto clip = clipFor (note.part, BeatPosition::fromBeats (note.start)); clip.isValid())
            addNote (song, clip, note.pitch, BeatPosition::fromBeats (note.start),
                     BeatDuration::fromBeats (std::max (note.length, getMinimumNoteLength().inBeats())), note.velocity);
}

//==============================================================================
std::vector<RecordingInProgress> Take::getInProgress()
{
    std::vector<RecordingInProgress> shown;

    if (finished || recording == nullptr || ! recording->hasStarted() || ! tempo.has_value())
        return shown;

    const auto startSample = recording->getStartSample();
    const auto length = recording->getLength();
    const auto toBeat = [this] (std::int64_t sample) { return BeatPosition::fromBeats (tempo->toBeats ((double) sample)); };

    // Each armed track's input, where it was played: from where recording started to what has come in, which the input
    // brought late by audioShift; round a looped range, once recording has gone round it, what was played before the range,
    // whole, and this time round from the range's start; punched in, what falls in the range.
    const auto heard = startSample + length - audioShift; // as if nothing looped
    const auto heardRound = getRoundHeard (heard);
    auto from = startSample, to = heard;

    if (heardRound > 0)
    {
        if (startSample < looped->start)
            for (const auto& take : audio)
                shown.push_back ({ .track = take->track,
                                   .source = RecordSource::input,
                                   .range = BeatRange (toBeat (startSample), toBeat (looped->start)),
                                   .beforeLoop = true });

        from = looped->start;
        to = looped->fold (heard - 1) + 1;
    }

    if (punch.has_value())
    {
        from = std::max (from, toSample (punch->getStart()));
        to = std::min (to, toSample (punch->getEnd()));
    }

    if (to > from)
        for (const auto& take : audio)
            shown.push_back ({ .track = take->track,
                               .source = RecordSource::input,
                               .range = BeatRange (toBeat (from), toBeat (to)),
                               .take = heardRound + 1 });

    // The notes played into live MIDI's track, once there are any, over the parts they will be kept in: as far as recording
    // has got; round a looped range, once it has gone round, what was played before it, and all of the range.
    if (const auto state = findTrackState (session.getSong(), midiTrack); state.hasType (schema::TRACK) && ! isFolderState (state))
    {
        const auto parts = collectNotes (startSample, recording->getStartClock(), length);
        const auto now = getRoundHeard (startSample + length - midiShift);

        for (const auto& part : parts)
        {
            if (part.notes.empty() || part.to <= part.from)
                continue;

            RecordingInProgress midi { .track = midiTrack,
                                       .source = RecordSource::midi,
                                       .range = BeatRange (toBeat (part.from), toBeat (part.to)),
                                       .take = part.looped ? now + 1 : 1,
                                       .beforeLoop = now > 0 && ! part.looped };
            const auto start = midi.range.getStart().inBeats();

            for (const auto& note : part.notes)
                midi.notes.push_back ({ note.pitch, toBeat (note.start).inBeats() - start, toBeat (note.end).inBeats() - start });

            shown.push_back (std::move (midi));
        }
    }

    return shown;
}

bool Take::recordsOnto (TrackID track) const
{
    return ! finished && track.isValid()
           && (track == midiTrack || std::any_of (audio.begin(), audio.end(), [track] (const auto& take) { return take->track == track; }));
}

void Take::holdWriting (bool held)
{
    if (writer != nullptr)
        writer->hold (held);
}

std::vector<float> Take::getLevels (TrackID track, BeatRange span, int columns) const
{
    std::vector<float> levels ((size_t) std::max (0, columns), 0.0f);
    const auto found = std::find_if (audio.begin(), audio.end(), [track] (const auto& take) { return take->track == track; });

    if (columns <= 0 || found == audio.end() || finished || recording == nullptr || ! recording->hasStarted() || ! tempo.has_value())
        return levels;

    // The file's first frame was played at this sample of the song, as if nothing looped (its lead of silence's, if any). Once
    // recording has gone round a looped range, a sample of the range is so much later this time round than the first time
    // round, and one before the range was played the first time round.
    const auto firstFrame = recording->getStartSample() - audioShift - getLead (recording->getStartSample());
    const auto heardRound = getRoundHeard (recording->getStartSample() + recording->getLength() - audioShift);
    const auto shiftAt = [this, heardRound] (std::int64_t sample)
    { return heardRound > 0 && sample >= looped->start ? heardRound * looped->getLength() : (std::int64_t) 0; };

    const auto& take = **found;
    const std::scoped_lock lock (take.peaksLock);
    const auto count = (std::int64_t) take.peaks.size();
    const auto step = span.getLength().inBeats() / columns;

    for (int column = 0; column < columns; ++column)
    {
        // A column's frames, all of them as the time round its start was played in.
        const auto beat = span.getStart().inBeats() + step * column;
        const auto start = toSample (BeatPosition::fromBeats (beat)), end = toSample (BeatPosition::fromBeats (beat + step));
        const auto toFrame = shiftAt (start) - firstFrame;
        const auto first = toPeaks (start + toFrame);
        const auto last = std::max (first + 1, toPeaks (end + toFrame + peakFrames - 1));
        auto level = 0.0f;

        for (auto i = std::max<std::int64_t> (first, 0); i < std::min (last, count); ++i)
            level = std::max (level, take.peaks[(size_t) i]);

        levels[(size_t) column] = level;
    }

    return levels;
}
} // namespace phazite
