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

#include <array>
#include <cmath>
#include <juce_audio_formats/juce_audio_formats.h>
#include <limits>

namespace phazite
{
namespace
{
    // What an input's buffer holds before the writing thread writes it, in seconds: room for the thread to fall far behind.
    constexpr double bufferSeconds = 8.0;

    // How often the thread writes what has come in, in milliseconds.
    constexpr int writeEveryMilliseconds = 20;

    constexpr double sixteenthInBeats = 0.25; // as Capture quantizes (Capture.h)
} // namespace

// An armed track's input: the engine's buffer of it, and the file it goes into.
struct Take::AudioTake
{
    TrackID track;
    juce::File file;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    engine::Recording::Input* input = nullptr; // the engine's recording's
    juce::int64 written = 0;

    // The writing thread, then the message thread at the end: what has come in, as far as so many frames in all.
    void write (juce::int64 upTo = std::numeric_limits<juce::int64>::max())
    {
        while (written < upTo && writer != nullptr)
        {
            const auto frames = input->read ((int) std::min<juce::int64> (upTo - written, 1 << 16),
                                             [this] (const float* const* channels, int numChannels, int count)
                                             {
                                                 writer->writeFromFloatArrays (channels, numChannels, count);
                                                 written += count;
                                             });

            if (frames <= 0)
                break;
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

    take->midiTrack = getLiveMidiTrack (song);

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

void Take::placeNotes (std::int64_t startSample, std::int64_t startClock, std::int64_t length)
{
    auto& song = session.getSong();

    if (const auto state = findTrackState (song, midiTrack); ! state.hasType (schema::TRACK) || isFolderState (state))
        return;

    const auto end = startSample + length - midiShift; // what was heard last, as if nothing looped

    // Each note played into the track, from where it was heard to where it ended, as if nothing looped; one still held
    // ends where the take does.
    struct Played
    {
        int pitch = 60, velocity = 100;
        std::int64_t start = 0, end = 0;
    };

    std::vector<Played> notes;
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

    for (const auto& played : session.getPlayedEvents())
    {
        const auto at = startSample + (played.clock - startClock) - midiShift;

        if (played.track != (std::int64_t) midiTrack.getRaw() || at >= end)
            continue;

        using Type = devices::NoteEvent::Type;
        const auto& event = played.event;
        const auto pitch = juce::jlimit (0, 127, event.number);

        if (event.type == Type::noteOn)
        {
            endNote (pitch, at);

            if (at >= startSample)
            {
                sounding[(size_t) pitch] = (int) notes.size();
                notes.push_back ({ pitch, juce::jlimit (1, 127, juce::roundToInt (event.value * 127.0f)), at, end });
            }
        }
        else if (event.type == Type::noteOff)
        {
            endNote (pitch, at);
        }
        else if (event.type == Type::allNotesOff)
        {
            for (int p = 0; p < 128; ++p)
                endNote (p, at);
        }
    }

    if (notes.empty())
        return;

    // Where each was in the song: folded into the looped range (one held over its end ending there), kept to the punched one.
    const auto toSample = [this] (BeatPosition beat) -> std::int64_t { return std::llround (tempo->toSamples (beat.inBeats())); };
    const auto toBeat = [this] (std::int64_t sample) { return BeatPosition::fromBeats (tempo->toBeats ((double) sample)); };

    auto from = startSample, to = end;

    if (loop.has_value())
    {
        const auto loopStart = toSample (loop->getStart()), loopEnd = toSample (loop->getEnd());

        if (loopEnd > loopStart)
        {
            for (auto& note : notes)
            {
                const auto noteLength = note.end - note.start;

                if (note.start >= loopEnd)
                    note.start = loopStart + (note.start - loopEnd) % (loopEnd - loopStart);

                note.end = std::min (note.start + noteLength, loopEnd);
            }

            from = loopStart;
            to = loopEnd;
        }
    }
    else if (punch.has_value())
    {
        from = std::max (from, toSample (punch->getStart()));
        to = std::min (to, toSample (punch->getEnd()));
        std::erase_if (notes, [from, to] (const Played& note) { return note.start < from || note.start >= to; });

        for (auto& note : notes)
            note.end = std::min (note.end, to);
    }

    if (notes.empty() || to <= from)
        return;

    // The clip they go in: with Overdub, the MIDI clip the first starts in; otherwise a new one over what was recorded, in
    // place of what was there.
    std::sort (notes.begin(), notes.end(), [] (const Played& a, const Played& b) { return a.start < b.start; });
    const auto range = BeatRange (toBeat (from), toBeat (to));
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
} // namespace phazite
