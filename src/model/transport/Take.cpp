// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Take.h"
#include "internal/AudioRecorder.h"
#include "internal/Playback.h"
#include "internal/Recording.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/LiveInput.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"

#include <algorithm>
#include <array>
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

    // A count of frames in whole stretches of peakFrames, rounded down (towards the past, for frames before the first).
    std::int64_t toPeaks (std::int64_t frames)
    {
        return frames >= 0 ? frames / peakFrames : -((-frames + peakFrames - 1) / peakFrames);
    }
} // namespace

// An armed track's input: the engine's buffer of it, and the file it goes into.
struct Take::AudioTake
{
    TrackID track;
    juce::File file;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    engine::Recording::Input* input = nullptr; // the engine's recording's
    juce::int64 written = 0;

    // The loudest sample of each peakFrames frames written, for the waveform the playlist draws while it records
    // (getLevels()): the writing thread adds to them, the message thread reads them, each under the lock (neither is the
    // audio thread); and the loudest of the frames written since the last.
    mutable std::mutex peaksLock;
    std::vector<float> peaks;
    float peak = 0.0f;
    int peakCount = 0;

    // The writing thread, then the message thread at the end: what has come in, as far as so many frames in all.
    void write (juce::int64 upTo = std::numeric_limits<juce::int64>::max())
    {
        std::vector<float> found;

        while (written < upTo && writer != nullptr)
        {
            const auto frames = input->read ((int) std::min<juce::int64> (upTo - written, 1 << 16),
                                             [this, &found] (const float* const* channels, int numChannels, int count)
                                             {
                                                 writer->writeFromFloatArrays (channels, numChannels, count);
                                                 written += count;
                                                 addPeaks (channels, numChannels, count, found);
                                             });

            if (frames <= 0)
                break;
        }

        if (! found.empty())
        {
            const std::scoped_lock lock (peaksLock);
            peaks.insert (peaks.end(), found.begin(), found.end());
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
};

// The thread that writes what the inputs' buffers hold into their files, a few dozen times a second.
class Take::Writer : public juce::Thread
{
public:
    explicit Writer (std::vector<AudioTake*> written)
        : juce::Thread ("Phazite take writer"),
          takes (std::move (written))
    {
        startThread (juce::Thread::Priority::high);
    }

    ~Writer() override { stopThread (2000); }

    void run() override
    {
        while (! threadShouldExit())
        {
            for (auto* take : takes)
                take->write();

            wait (writeEveryMilliseconds);
        }
    }

private:
    const std::vector<AudioTake*> takes;
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

    std::vector<AudioTake*> written;

    for (auto& audio : take->audio)
        written.push_back (audio.get());

    if (! written.empty())
        take->writer = std::make_unique<Writer> (std::move (written));

    return take;
}

void Take::close (bool keep)
{
    if (finished)
        return;

    finished = true;
    const auto length = recording != nullptr ? recording->getLength() : 0;
    writer = nullptr; // its thread ends

    // What is left, as far as what the take recorded (the engine may have written on since).
    for (auto& take : audio)
    {
        if (keep)
            take->write (length);

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

    const auto rate = session.getEngine().getSampleRate();
    const auto toSeconds = [this, rate] (BeatPosition beat) { return tempo->toSamples (beat.inBeats()) / rate; };

    for (auto& take : audio)
    {
        // Where in the song, as if nothing looped, the file's first frame was played.
        TakePlace place;
        place.sampleRate = rate;
        place.frames = std::min<juce::int64> (take->written, length);
        place.firstFrame = (double) (startSample - audioShift) / rate;
        place.start = (double) startSample / rate;
        place.stop = (double) (startSample + length) / rate;

        if (punch.has_value())
            place.punch = juce::Range (toSeconds (punch->getStart()), toSeconds (punch->getEnd()));

        if (loop.has_value())
        {
            place.loop = juce::Range (toSeconds (loop->getStart()), toSeconds (loop->getEnd()));
            place.firstLoopStart = place.loop->getStart();
        }

        placeTake (session.getSong(), take->track, take->file, place);
    }
}

//==============================================================================
std::optional<std::pair<std::int64_t, std::int64_t>> Take::getLoopSamples() const
{
    if (! loop.has_value() || ! tempo.has_value())
        return std::nullopt;

    const auto start = (std::int64_t) std::llround (tempo->toSamples (loop->getStart().inBeats()));
    const auto end = (std::int64_t) std::llround (tempo->toSamples (loop->getEnd().inBeats()));

    if (end <= start)
        return std::nullopt;

    return std::pair { start, end };
}

std::pair<std::int64_t, int> Take::fold (std::int64_t sample) const
{
    // The first time round reaches the range's end from wherever recording started (playback past its end starts again at
    // its start); each time round after it is as long as the range.
    const auto range = getLoopSamples();

    if (! range.has_value() || sample < range->second)
        return { sample, 0 };

    const auto length = range->second - range->first;
    const auto pass = (sample - range->first) / length;
    return { sample - pass * length, (int) pass };
}

Take::PlayedNotes Take::collectNotes (std::int64_t startSample, std::int64_t startClock, std::int64_t length)
{
    PlayedNotes played;
    auto& notes = played.notes;
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

    // Where each was in the song: folded into the looped range (one held over its end ending there), kept to the punched one.
    const auto toSample = [this] (BeatPosition beat) { return (std::int64_t) std::llround (tempo->toSamples (beat.inBeats())); };
    played.from = startSample;
    played.to = end;

    if (const auto range = getLoopSamples())
    {
        const auto loopStart = range->first, loopEnd = range->second;

        for (auto& note : notes)
        {
            const auto noteLength = note.end - note.start;

            if (note.start >= loopEnd)
                note.start = loopStart + (note.start - loopEnd) % (loopEnd - loopStart);

            note.end = std::min (note.start + noteLength, loopEnd);
        }

        played.from = loopStart;
        played.to = loopEnd;
    }
    else if (punch.has_value())
    {
        const auto from = std::max (played.from, toSample (punch->getStart()));
        const auto to = std::min (played.to, toSample (punch->getEnd()));
        std::erase_if (notes, [from, to] (const Played& note) { return note.start < from || note.start >= to; });

        for (auto& note : notes)
            note.end = std::min (note.end, to);

        played.from = from;
        played.to = to;
    }

    std::sort (notes.begin(), notes.end(), [] (const Played& a, const Played& b) { return a.start < b.start; });
    return played;
}

void Take::placeNotes (std::int64_t startSample, std::int64_t startClock, std::int64_t length)
{
    auto& song = session.getSong();

    if (const auto state = findTrackState (song, midiTrack); ! state.hasType (schema::TRACK) || isFolderState (state))
        return;

    const auto played = collectNotes (startSample, startClock, length);
    const auto& notes = played.notes;

    if (notes.empty() || played.to <= played.from)
        return;

    // The clip they go in: with Overdub, the MIDI clip the first starts in; otherwise a new one over what was recorded, in
    // place of what was there.
    const auto toBeat = [this] (std::int64_t sample) { return BeatPosition::fromBeats (tempo->toBeats ((double) sample)); };
    const auto range = BeatRange (toBeat (played.from), toBeat (played.to));
    const auto firstStart = toBeat (notes.front().start);
    ClipID clip;

    if (isOverdubOn (session.getAudioIO()))
    {
        for (const auto& clipState : getClipStates (findTrackState (song, midiTrack)))
            if (! clip.isValid() && isMidiState (clipState) && getClipRange (clipState).getStart() <= firstStart
                && getClipRange (clipState).getEnd() > firstStart)
                clip = getClipID (clipState);
    }
    else
    {
        clearClipsIn (song, midiTrack, range);
    }

    if (! clip.isValid())
        clip = insertMidiClip (song, midiTrack, range, nullptr);

    if (! clip.isValid())
        return;

    const auto quantize = isRecordQuantizeOn (session.getAudioIO());

    for (const auto& note : notes)
    {
        auto start = toBeat (note.start).inBeats();
        const auto noteLength = toBeat (note.end).inBeats() - start;

        if (quantize)
            start = std::round (start / sixteenthInBeats) * sixteenthInBeats;

        addNote (song, clip, note.pitch, BeatPosition::fromBeats (start),
                 BeatDuration::fromBeats (std::max (noteLength, getMinimumNoteLength().inBeats())), note.velocity);
    }
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
    const auto toSample = [this] (BeatPosition beat) { return (std::int64_t) std::llround (tempo->toSamples (beat.inBeats())); };

    // Each armed track's input, where it was played: from where recording started (round a looped range, where this time
    // round began) to what has come in, which the input brought late by audioShift; punched in, what falls in the range.
    const auto heard = fold (startSample + length - audioShift);
    auto from = heard.second > 0 ? getLoopSamples()->first : startSample;
    auto to = heard.first;

    if (punch.has_value())
    {
        from = std::max (from, toSample (punch->getStart()));
        to = std::min (to, toSample (punch->getEnd()));
    }

    if (to > from)
        for (const auto& take : audio)
            shown.push_back ({ take->track, RecordSource::input, BeatRange (toBeat (from), toBeat (to)), heard.second + 1, {} });

    // The notes played into live MIDI's track, once there are any, over the range they will be kept in (round a looped
    // range, as far as it has played the first time round, then all of it).
    if (const auto state = findTrackState (session.getSong(), midiTrack); state.hasType (schema::TRACK) && ! isFolderState (state))
    {
        const auto played = collectNotes (startSample, recording->getStartClock(), length);
        const auto now = fold (startSample + length - midiShift);
        const auto end = now.second > 0 ? played.to : std::min (played.to, now.first);

        if (! played.notes.empty() && end > played.from)
        {
            RecordingInProgress midi { midiTrack, RecordSource::midi, BeatRange (toBeat (played.from), toBeat (end)), now.second + 1, {} };
            const auto start = midi.range.getStart().inBeats();

            for (const auto& note : played.notes)
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

std::vector<float> Take::getLevels (TrackID track, BeatRange span, int columns) const
{
    std::vector<float> levels ((size_t) std::max (0, columns), 0.0f);
    const auto found = std::find_if (audio.begin(), audio.end(), [track] (const auto& take) { return take->track == track; });

    if (columns <= 0 || found == audio.end() || finished || recording == nullptr || ! recording->hasStarted() || ! tempo.has_value())
        return levels;

    // The file's first frame was played at this sample of the song, as if nothing looped; a sample of the time round a looped
    // range it is on now is so much later than the same sample the first time round.
    const auto firstFrame = recording->getStartSample() - audioShift;
    const auto pass = fold (recording->getStartSample() + recording->getLength() - audioShift).second;
    const auto range = getLoopSamples();
    const auto shift = pass > 0 && range.has_value() ? pass * (range->second - range->first) : (std::int64_t) 0;
    const auto frameAt = [this, shift, firstFrame] (double beat)
    { return (std::int64_t) std::llround (tempo->toSamples (beat)) + shift - firstFrame; };

    const auto& take = **found;
    const std::scoped_lock lock (take.peaksLock);
    const auto count = (std::int64_t) take.peaks.size();
    const auto step = span.getLength().inBeats() / columns;

    for (int column = 0; column < columns; ++column)
    {
        const auto beat = span.getStart().inBeats() + step * column;
        const auto first = toPeaks (frameAt (beat));
        const auto last = std::max (first + 1, toPeaks (frameAt (beat + step) + peakFrames - 1));
        auto level = 0.0f;

        for (auto i = std::max<std::int64_t> (first, 0); i < std::min (last, count); ++i)
            level = std::max (level, take.peaks[(size_t) i]);

        levels[(size_t) column] = level;
    }

    return levels;
}
} // namespace phazite
