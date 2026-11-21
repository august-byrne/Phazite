// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Take.h"
#include "internal/AudioRecorder.h"
#include "internal/Playback.h"
#include "internal/Recording.h"
#include "internal/Tempo.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/Numbers.h"
#include "model/base/internal/Schema.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/LiveInput.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <juce_audio_formats/juce_audio_formats.h>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

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

    // MIDI's controller numbers, and the pitch bend's middle.
    constexpr int sustainPedal = 64, bendMiddle = 8192;
} // namespace

std::optional<ControlValue> Take::toControl (const devices::NoteEvent& event)
{
    using Type = devices::NoteEvent::Type;
    const auto toByte = [] (float value) { return juce::jlimit (0, 127, juce::roundToInt (value * 127.0f)); };
    const auto channel = (event.channel & 15) + 1;

    switch (event.type)
    {
        case Type::controller:
            return ControlValue { {}, ControlKind::controller, juce::jlimit (0, 127, event.number), toByte (event.value), channel };
        case Type::pitchBend:
            return ControlValue { {},
                                  ControlKind::pitchWheel,
                                  0,
                                  juce::jlimit (0, 16383, juce::roundToInt (event.value * (float) bendMiddle) + bendMiddle),
                                  channel };
        case Type::channelPressure:
            return ControlValue { {}, ControlKind::channelPressure, 0, toByte (event.value), channel };
        case Type::aftertouch:
            return ControlValue { {}, ControlKind::aftertouch, juce::jlimit (0, 127, event.number), toByte (event.value), channel };
        case Type::programChange:
            return ControlValue { {}, ControlKind::programChange, 0, juce::jlimit (0, 127, event.number), channel };
        case Type::noteOn:
        case Type::noteOff:
        case Type::allNotesOff:
        case Type::portamentoOn:
        case Type::slide:
            break;
    }

    return std::nullopt;
}

std::size_t Take::getKey (ControlKind kind, int number, int channel) noexcept
{
    const auto first = (std::size_t) (juce::jlimit (1, 16, channel) - 1) * controlKeysPerChannel;

    switch (kind)
    {
        case ControlKind::controller:
            return first + (std::size_t) juce::jlimit (0, 127, number);
        case ControlKind::pitchWheel:
            return first + 128;
        case ControlKind::channelPressure:
            return first + 129;
        case ControlKind::aftertouch:
            return first + 130 + (std::size_t) juce::jlimit (0, 127, number);
        case ControlKind::programChange:
            return first + 258;
    }

    return first;
}

bool Take::carriesOn (const PlayedControl& carried, const PlayedControl& release, const PlayedNotes& endedPart) noexcept
{
    if (carried.kind != release.kind || carried.number != release.number || carried.channel != release.channel)
        return false;

    const auto& end = endedPart.ends[(size_t) (juce::jlimit (1, 16, release.channel) - 1)];

    switch (release.kind)
    {
        case ControlKind::pitchWheel:
            return carried.value == end.bend;
        case ControlKind::channelPressure:
            return carried.value == end.pressure;
        case ControlKind::controller: // the pedal
        case ControlKind::aftertouch:
        case ControlKind::programChange: // never let go of
            break;
    }

    return carried.value >= 64;
}

std::optional<juce::String> describeLittleSpace (std::int64_t bytesFree, double bytesPerSecond)
{
    if (bytesPerSecond <= 0.0 || (double) bytesFree >= littleSpaceSeconds * bytesPerSecond)
        return std::nullopt;

    const auto seconds = std::max (0.0, (double) bytesFree / bytesPerSecond);
    const auto minutes = (int) std::floor (seconds / 60.0);
    const auto room = minutes >= 2   ? "about " + juce::String (minutes) + " minutes"
                      : minutes == 1 ? juce::String ("about a minute")
                                     : "less than a minute";

    return "The drive the project records onto has room for " + room
           + " of recording. Phazite stops recording, keeping what it wrote, if the drive fills up.";
}

// The file an input is written into, written straight through to the drive (not buffered), so that a write that fails is
// known as it fails; and, for the tests, a drive that fills up (Take::fillDrive()): a write that would make the file longer
// fails, one over what it holds already doesn't.
class Take::FileStream final : public juce::OutputStream
{
public:
    FileStream (const juce::File& file, const std::atomic<bool>& full)
        : stream (file, 0),
          driveFull (full)
    {
    }

    bool openedOk() const { return stream.openedOk(); }

    void flush() override { stream.flush(); }
    bool setPosition (juce::int64 position) override { return stream.setPosition (position); }
    juce::int64 getPosition() override { return stream.getPosition(); }

    bool write (const void* data, size_t numBytes) override
    {
        if (driveFull.load() && stream.getPosition() + (juce::int64) numBytes > longest)
            return false;

        const auto written = stream.write (data, numBytes);
        longest = std::max (longest, stream.getPosition());
        return written;
    }

private:
    juce::FileOutputStream stream;
    const std::atomic<bool>& driveFull;
    juce::int64 longest = 0; // how long the file is
};

// An armed track's input: the engine's buffer of it, and the file it goes into.
struct Take::AudioTake
{
    TrackID track;
    juce::File file;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    engine::Recording::Input* input = nullptr; // the engine's recording's

    // The take's: once a write of any of its files fails, nothing more is written into any (the drive is full, or gone).
    std::atomic<bool>* failed = nullptr;

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

    // Whether its file is open and written on: not once a write of the take's has failed.
    bool canWrite() const { return writer != nullptr && ! failed->load(); }

    // A write into the file: one that fails is noted for the take, and writes nothing more (the file's frames are those
    // written before it, which its header says as it is closed).
    bool writeFrames (const float* const* channels, int numChannels, int count)
    {
        if (! canWrite())
            return false;

        if (! writer->writeFromFloatArrays (channels, numChannels, count))
        {
            failed->store (true);
            return false;
        }

        written += count;
        return true;
    }

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

        while (written - lead < upTo && canWrite())
        {
            const auto frames = input->read (
                std::min<std::int64_t> (upTo - (written - lead), 1 << 16),
                [this, &found] (const float* const* channels, int numChannels, int count)
                {
                    if (writeFrames (channels, numChannels, count))
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

        if (canWrite() && written - lead < length)
        {
            std::vector<float> found;
            writeLost (length - (written - lead), found);
            keepPeaks (found);
        }
    }

    void writeLost (std::int64_t count, std::vector<float>& found)
    {
        if (! canWrite())
            return;

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

            if (! writeFrames (channels, numChannels, part))
                return;

            addPeaks (channels, numChannels, part, found);
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

std::unique_ptr<Take> Take::start (PlaybackSession& session, Take* goesOnFrom)
{
    auto& song = session.getSong();
    auto& engine = session.getEngine();
    const auto rate = engine.getSampleRate();
    const auto* plan = engine.getPlan();

    // The rate and the tempo map from one source: a plan for another rate (the device started again, and the song isn't
    // given for its new rate yet) would place what is recorded by the wrong clock.
    if (plan == nullptr || ! juce::exactlyEqual (plan->tempo.getSampleRate(), rate))
        return nullptr;

    std::unique_ptr<Take> take (new Take (session));
    take->rate = rate;
    take->tempo = plan->tempo;
    take->afterJump = goesOnFrom != nullptr;
    take->deviceStarts = session.getHost().getDeviceStarts();
    const auto inputsOf = goesOnFrom != nullptr ? std::optional (goesOnFrom->getInputTracks()) : std::nullopt;

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

    // The controllers and the bend as they are now on live MIDI's track: the take before's state (after a jump), then what
    // was played since that is still kept.
    if (goesOnFrom != nullptr)
        take->heldAtStart = goesOnFrom->heldAtStart;
    else
        take->heldAtStart.fill (-1);

    for (const auto& event : session.getPlayedEvents())
        if (event.track == (std::int64_t) take->midiTrack.getRaw())
            if (const auto control = toControl (event.event))
                take->heldAtStart[getKey (control->kind, control->number, control->channel)] = control->value;

    // Each armed track's input the audio device has, into a file of its own.
    std::vector<std::unique_ptr<engine::Recording::Input>> inputs;
    auto& recorder = session.getRecorder();
    juce::StringArray notMade;      // the tracks whose files couldn't be made
    std::int64_t bytesPerFrame = 0; // of all the files: 24 bits a channel

    for (const auto track : recorder.getArmedTracks())
    {
        if (inputsOf.has_value() && std::find (inputsOf->begin(), inputsOf->end(), track) == inputsOf->end())
            continue; // armed since the take before started: its input isn't open yet

        auto channels = getInputChannels (findTrackState (song, track)[schema::trackInput].toString());

        if (channels.empty() || channels.back() >= session.getNumInputs())
            continue;

        auto audio = std::make_unique<AudioTake>();
        audio->track = track;
        audio->failed = &take->writeFailed;
        audio->file = recorder.getTakeFile (track);
        audio->file.getParentDirectory().createDirectory();
        auto stream = std::make_unique<FileStream> (audio->file, take->driveFull);

        if (! stream->openedOk())
        {
            notMade.add (getTrackName (song, track));
            continue;
        }

        std::unique_ptr<juce::OutputStream> output = std::move (stream);
        audio->writer = juce::WavAudioFormat().createWriterFor (
            output, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels ((int) channels.size()).withBitsPerSample (24));

        if (audio->writer == nullptr)
        {
            notMade.add (getTrackName (song, track));
            audio->file.deleteFile();
            continue;
        }

        bytesPerFrame += 3 * (std::int64_t) channels.size();
        auto input = std::make_unique<engine::Recording::Input> (std::move (channels), toWhole<int> (bufferSeconds * rate));
        audio->input = input.get();
        inputs.push_back (std::move (input));
        take->audio.push_back (std::move (audio));
    }

    // What the user should know: an input that won't be recorded (from where the playhead was put, for a take that goes on
    // after a jump), and before playing, a drive with little room left.
    if (session.onProblem != nullptr)
    {
        if (! notMade.isEmpty())
            session.onProblem ("Phazite can't record an input",
                               "Phazite couldn't make a file in the project's Audio folder for " + joinAsWords (notMade)
                                   + (goesOnFrom != nullptr ? ", so it isn't recorded from where the playhead was moved, and what was "
                                                              "recorded before is kept."
                                                            : ", so it isn't recorded.")
                                   + " Check that the drive is there and has room.");

        if (goesOnFrom == nullptr && ! take->audio.empty())
            if (const auto little = describeLittleSpace (take->audio.front()->file.getBytesFreeOnVolume(), (double) bytesPerFrame * rate))
                session.onProblem ("Little room left to record", *little);
    }

    if (! take->midiTrack.isValid() && take->audio.empty())
        return nullptr;

    // How late what comes in is: a note by the output's delay and the song's delay compensation, as it was heard; an
    // input's sound by its own delay and the audio settings' correction too.
    const auto compensation = engine::Engine::getOutputLatency (*plan);
    const auto correction = session.getAudioIO().getRecordingCorrection(); // milliseconds
    take->midiShift = session.getOutputLatency() + compensation;
    take->audioShift = session.getInputLatency() + take->midiShift + roundToWhole<std::int64_t> (correction * rate / 1000.0);

    // Given to the engine at once. One that goes on after a jump starts where the playhead lands, the jump the message
    // thread asks for next, once this is given (only it asks for locates); a block that still plays the take before hands
    // it on to it. It keeps the notes played once the one before is finished, which needs those played since it started.
    auto recording = std::make_unique<engine::Recording> (std::move (inputs));
    take->recording = recording.get();
    take->keepFrom = engine.getClock() - (goesOnFrom != nullptr ? 0 : roundToWhole<std::int64_t> (earlyNoteSeconds * rate));

    if (goesOnFrom != nullptr)
    {
        recording->startAfterLocate (engine.getLocatesTaken(), engine.getLocatesAsked() + 1);

        if (goesOnFrom->recording != nullptr)
            goesOnFrom->recording->continueInto (*recording);
    }

    engine.setRecording (std::move (recording));

    if (goesOnFrom == nullptr)
        take->keepPlayed();

    if (! take->audio.empty())
        take->writer = std::make_unique<Writer> (*take);

    return take;
}

void Take::keepPlayed()
{
    session.keepPlayedFrom (keepFrom);
}

void Take::followOn (const Take& before)
{
    seamBefore = before.seamAtEnd;
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

    // The engine records nothing more, and the notes played go after 30 seconds again; unless a take that goes on from this
    // one after a jump has the engine already (start()), which keeps them.
    const auto stillRecorded = session.getEngine().getRecording() == recording;
    recording = nullptr;

    if (! stillRecorded)
        return;

    session.getEngine().setRecording (nullptr);
    session.keepPlayedFrom ({});

    // What was armed or set to be heard meanwhile.
    session.getRecorder().update();
}

void Take::finish()
{
    end();
    place();
}

void Take::end()
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
    ended = Ended { startSample, length, collectNotes (startSample, startClock, length) };
    close (true);
}

void Take::place()
{
    if (! ended.has_value() || std::exchange (clipsPlaced, true))
        return;

    const auto startSample = ended->startSample, length = ended->length;
    placeNotes (ended->parts);

    for (auto& take : audio)
    {
        // Where in the song, as if nothing looped, the file's first frame was played: what came in first, after its lead of
        // silence.
        const auto lead = std::max<std::int64_t> (0, take->lead);
        TakePlace place;
        place.sampleRate = rate;
        place.frames = std::min<std::int64_t> (take->written, lead + length);
        place.lead = lead;
        place.firstFrame = startSample - audioShift - lead;
        place.start = startSample;
        place.stop = startSample + length;
        place.loop = looped;
        place.tempo = tempo; // the map its notes are placed by too

        if (punch.has_value())
            place.punch = juce::Range (toSample (punch->getStart()), toSample (punch->getEnd()));

        take->kept = placeTake (session.getSong(), take->track, take->file, place);
    }

    if (const auto lost = describeLost (startSample); lost.isNotEmpty() && session.onProblem != nullptr)
        session.onProblem ("Part of the recording was lost", lost);

    if (const auto failed = describeFailedWrite (startSample); failed.isNotEmpty() && session.onProblem != nullptr)
        session.onProblem ("Recording stopped", failed);
}

juce::String Take::describeFailedWrite (std::int64_t startSample) const
{
    if (! writeFailed.load())
        return {};

    // Where the shortest file ends, in the song, as if nothing looped: the take's audio stops there (what came in first was
    // played audioShift before it came in).
    auto shortest = std::numeric_limits<std::int64_t>::max();

    for (const auto& take : audio)
        shortest = std::min (shortest, std::max<std::int64_t> (0, take->written - std::max<std::int64_t> (0, take->lead)));

    const auto end = shortest == std::numeric_limits<std::int64_t>::max() ? startSample : startSample - audioShift + shortest;
    return "Phazite couldn't write the recording to the drive (it may be full, or gone), so it stopped recording and kept what "
           "it wrote, to "
           + describeSample (looped.has_value() ? looped->fold (end) : end) + ". Make room on the drive, or save the project "
           + "elsewhere, before recording again.";
}

juce::String Take::describeSample (std::int64_t sample) const
{
    return describePlace (getBars (session.getSong()), BeatPosition::fromBeats (tempo->toBeats ((double) sample)));
}

juce::String Take::describeLost (std::int64_t startSample) const
{
    const auto& song = session.getSong();
    const auto bars = getBars (song);
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
    std::vector<PlayedControl> controls;               // every one played, before the take's start too
    const auto end = startSample + length - midiShift; // what was heard last, as if nothing looped
    const auto early = afterJump ? (std::int64_t) 0 : roundToWhole<std::int64_t> (earlyNoteSeconds * rate);
    std::vector<int> lastValue (numControlKeys, -1);            // each controller's (by getKey()), as played last
    std::vector<std::ptrdiff_t> lastIndex (numControlKeys, -1); // and where its last one kept is in controls
    const auto millisecond = roundToWhole<std::int64_t> (controlSeconds * rate);

    // Each note played into the track, from where it was heard to where it ended, as if nothing looped; one still held
    // ends where the take does. One played a moment before the take started (a downbeat played early after the count-in,
    // or in the first samples the output's latency covers) starts where it did, as Ableton Live, Logic Pro and Cubase keep
    // a note played just before the count-in ends. A note's end is its start's on its channel and pitch.
    std::vector<int> sounding (16 * 128, -1); // by channel x 128 + pitch

    const auto endNote = [&notes, &sounding] (size_t key, std::int64_t at)
    {
        if (auto& index = sounding[key]; index >= 0)
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
        const auto noteKey = (size_t) (note.channel & 15) * 128 + (size_t) pitch;

        if (note.type == Type::noteOn)
        {
            endNote (noteKey, at);

            if (at >= startSample - early)
            {
                sounding[noteKey] = (int) notes.size();
                notes.push_back ({ pitch, juce::jlimit (1, 127, juce::roundToInt (note.value * 127.0f)), std::max (at, startSample), end,
                                   (note.channel & 15) + 1 });
            }
        }
        else if (note.type == Type::noteOff)
        {
            endNote (noteKey, at);
        }
        else if (note.type == Type::allNotesOff)
        {
            for (size_t k = 0; k < sounding.size(); ++k)
                endNote (k, at);
        }
        else if (const auto played = toControl (note))
        {
            // A value that repeats the one before it changes nothing.
            const PlayedControl control { played->kind, played->number, played->value, at, played->channel };
            const auto key = getKey (control);

            if (std::exchange (lastValue[key], control.value) == control.value)
                continue;

            // At most one a millisecond for each controller (a wheel can send many more): the latest value at the time of
            // the first.
            if (const auto last = lastIndex[key]; last >= 0 && control.at - controls[(size_t) last].at < millisecond)
            {
                controls[(size_t) last].value = control.value;
                continue;
            }

            lastIndex[key] = (std::ptrdiff_t) controls.size();
            controls.push_back (control);
        }
    }

    // The controls from one sample to another: those moved in it, as they were played, folded into the looped range if
    // asked; those whose values hold as it starts (carried in: the pedal held down, the bend away from its middle, the
    // channel's pressure held, and any controller moved a moment before, as the notes played then are kept); and the
    // pedal, the bend and the channel's pressure let go of at its end, where they are still down, away from the middle or
    // pressed then; each channel's on that channel. A note's pressure goes with its note, which ends; a program change is
    // never let go of.
    const auto controlsIn = [this, &controls, early] (std::int64_t from, std::int64_t to, const TakeLoop* fold, PlayedNotes& part)
    {
        std::vector<bool> found (numControlKeys, false); // each controller's last value before the part

        const auto holds = [] (const PlayedControl& control)
        {
            return (control.kind == ControlKind::pitchWheel && control.value != bendMiddle)
                   || (control.kind == ControlKind::channelPressure && control.value > 0)
                   || (control.kind == ControlKind::controller && control.number == sustainPedal && control.value >= 64);
        };

        for (auto control = controls.rbegin(); control != controls.rend(); ++control)
        {
            if (control->at >= from || found[getKey (*control)])
                continue;

            found[getKey (*control)] = true;

            if (holds (*control) || control->at >= from - early)
                part.carried.push_back ({ control->kind, control->number, control->value, from, control->channel });
        }

        // The pedal, the bend and the pressure as the take started, on each channel, where nothing played since is kept to
        // say (what was played before it goes after 30 seconds: PlaybackHost::getPlayedEvents()).
        for (int channel = 1; channel <= 16; ++channel)
            for (const auto& [kind, number] : { std::pair (ControlKind::controller, sustainPedal), std::pair (ControlKind::pitchWheel, 0),
                                                std::pair (ControlKind::channelPressure, 0) })
                if (const auto key = getKey (kind, number, channel); ! found[key] && heldAtStart[key] >= 0)
                    if (const PlayedControl held { kind, number, heldAtStart[key], from, channel }; holds (held))
                        part.carried.push_back (held);

        for (auto control : controls)
        {
            if (control.at < from || control.at >= to)
                continue;

            if (fold != nullptr)
                control.at = fold->fold (control.at);

            part.controls.push_back (control);
        }

        std::stable_sort (part.controls.begin(), part.controls.end(),
                          [] (const PlayedControl& a, const PlayedControl& b) { return a.at < b.at; });

        for (const auto* list : { &part.carried, &part.controls })
        {
            for (const auto& control : *list)
            {
                auto& at = part.ends[(size_t) (juce::jlimit (1, 16, control.channel) - 1)];

                if (control.kind == ControlKind::pitchWheel)
                    at.bend = control.value;
                else if (control.kind == ControlKind::channelPressure)
                    at.pressure = control.value;
                else if (control.kind == ControlKind::controller && control.number == sustainPedal)
                    at.pedal = control.value;
            }
        }

        for (int channel = 1; channel <= 16; ++channel)
        {
            const auto& at = part.ends[(size_t) channel - 1];

            if (at.pedal >= 64)
                part.released.push_back ({ ControlKind::controller, sustainPedal, 0, part.to, channel });

            if (at.bend != bendMiddle)
                part.released.push_back ({ ControlKind::pitchWheel, 0, bendMiddle, part.to, channel });

            if (at.pressure > 0)
                part.released.push_back ({ ControlKind::channelPressure, 0, 0, part.to, channel });
        }
    };

    // Where each goes in the song, in the parts a clip of them goes over (collectNotes() in Take.h says which).
    const auto byStart = [] (const Played& a, const Played& b) { return a.start < b.start; };
    std::sort (notes.begin(), notes.end(), byStart);
    std::vector<PlayedNotes> parts;

    if (punch.has_value())
    {
        const auto from = std::max (startSample, toSample (punch->getStart()));
        const auto to = std::min (end, toSample (punch->getEnd()));
        std::erase_if (notes, [from, to, early] (const Played& note) { return note.start < from - early || note.start >= to; });

        for (auto& note : notes)
        {
            note.start = std::max (note.start, from);
            note.end = std::min (std::max (note.end, note.start), to);
        }

        parts.push_back ({ std::move (notes), from, to, false });
        controlsIn (from, to, nullptr, parts.back());
    }
    else if (getRoundHeard (end) > 0)
    {
        PlayedNotes before { {}, startSample, looped->start, false }, inRange { {}, looped->start, looped->end, true };
        controlsIn (startSample, looped->start, nullptr, before);
        controlsIn (looped->start, end, &*looped, inRange);

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
        controlsIn (startSample, end, nullptr, parts.back());
    }

    // One performance cut into parts (the part before a looped range, and the range's): where the next begins at this one's
    // end and carries on the pedal held, the bend or the pressure as it is, nothing lets go of it between them, which would
    // release what it held for a tick.
    for (size_t p = 0; p + 1 < parts.size(); ++p)
    {
        if (parts[p].to != parts[p + 1].from)
            continue;

        const auto& next = parts[p + 1].carried;
        std::erase_if (parts[p].released,
                       [&next, &part = parts[p]] (const PlayedControl& release)
                       {
                           return std::any_of (next.begin(), next.end(),
                                               [&] (const PlayedControl& carried) { return carriesOn (carried, release, part); });
                       });
    }

    return parts;
}

void Take::placeNotes (const std::vector<PlayedNotes>& parts)
{
    auto& song = session.getSong();

    if (const auto state = findTrackState (song, midiTrack); ! state.hasType (schema::TRACK) || isFolderState (state))
        return;

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
        int channel = 1;
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

            placed.push_back ({ note.pitch, note.velocity, start, noteLength, in, note.channel });
        }
    }

    // What was played in a part, which alone makes a clip for it: notes, or controls moved within it (Ableton Live records
    // controllers alone too). What a part carries in, or lets go of at its end, makes none. Only notes clear what was there
    // without Overdub: controllers moved without notes go into the clips there, as Overdub puts them (a bend wheel bumped
    // while a vocal records leaves the instrument's clips as they were), and into a clip of their own only where the track
    // has none across the part.
    const auto hasNotes = [&] (size_t p)
    { return std::any_of (placed.begin(), placed.end(), [p] (const Placed& note) { return note.part == p; }); };

    const auto wasPlayedIn = [&] (size_t p) { return ! ranges[p].isEmpty() && (! parts[p].controls.empty() || hasNotes (p)); };

    const auto overdub = isOverdubOn (session.getAudioIO());
    std::vector<ClipID> recorded (parts.size());
    std::set<std::uint64_t> made; // the clips the take made, whose ends it may let go of the pedal at

    if (! overdub)
    {
        for (size_t p = 0; p < parts.size(); ++p)
        {
            if (ranges[p].isEmpty() || ! hasNotes (p))
                continue;

            clearClipsIn (song, midiTrack, ranges[p]);
            recorded[p] = insertMidiClip (song, midiTrack, ranges[p], nullptr);
            made.insert (recorded[p].getRaw());
        }
    }

    // The clip each goes in: without Overdub, the part's own; with it, or for controllers alone, the MIDI clip it falls in,
    // or for a note one made for the gap it falls in, between the clips around it within its part (all in the one that holds
    // the first, the rest went outside it and never played); for a controller alone, one over the part where the track has
    // no clip across it, and none otherwise.
    const auto clipFor = [&] (size_t part, BeatPosition beat, bool note) -> ClipID
    {
        if (recorded[part].isValid())
            return recorded[part];

        const auto clips = getClipStates (findTrackState (song, midiTrack));

        for (const auto& clipState : clips)
            if (isMidiState (clipState) && getClipRange (clipState).getStart() <= beat && getClipRange (clipState).getEnd() > beat)
                return getClipID (clipState);

        const auto& range = ranges[part];

        if (! note)
        {
            if (std::any_of (clips.begin(), clips.end(),
                             [&range] (const auto& clipState) { return getClipRange (clipState).overlaps (range); }))
                return {};

            recorded[part] = insertMidiClip (song, midiTrack, range, nullptr);
            made.insert (recorded[part].getRaw());
            return recorded[part];
        }

        auto from = std::min (range.getStart(), beat), to = range.getEnd(); // quantized, a note can start before what was recorded

        for (const auto& clipState : clips)
        {
            if (const auto around = getClipRange (clipState); around.getEnd() <= beat)
                from = std::max (from, around.getEnd());
            else if (around.getStart() > beat)
                to = std::min (to, around.getStart());
        }

        if (to <= from)
            return {};

        const auto gap = insertMidiClip (song, midiTrack, { from, to }, nullptr);
        made.insert (gap.getRaw());
        return gap;
    };

    // Each part's first and last of what was played, and the clips they went into.
    struct Reach
    {
        std::optional<std::pair<BeatPosition, ClipID>> first, last;

        void add (BeatPosition beat, ClipID clip)
        {
            if (! first.has_value() || beat < first->first)
                first = std::pair (beat, clip);

            if (! last.has_value() || beat >= last->first)
                last = std::pair (beat, clip);
        }
    };

    std::vector<Reach> reach (parts.size());

    for (const auto& note : placed)
    {
        const auto beat = BeatPosition::fromBeats (note.start);

        if (const auto clip = clipFor (note.part, beat, true); clip.isValid())
        {
            addNote (song, clip, note.pitch, beat, BeatDuration::fromBeats (std::max (note.length, getMinimumNoteLength().inBeats())),
                     note.velocity, note.channel);
            reach[note.part].add (beat, clip);
        }
    }

    // A clip's own value of a controller (or the bend, or the pressure) at a beat, as it was before the take: its last one
    // there, or none.
    const auto valueBefore = [&song] (ClipID clip, const PlayedControl& control, BeatPosition beat) -> std::optional<int>
    {
        const auto clipState = findClipState (song, clip);
        const auto contentStart = getContentStart (clipState);
        std::optional<std::pair<double, int>> last;

        for (const auto& child : clipState)
        {
            if (! child.hasType (schema::CONTROL) || findControlKind (child) != control.kind
                || (hasNumber (control.kind) && (int) child[schema::controlNumber] != control.number)
                || readChannel (child, schema::controlChannel) != control.channel)
                continue;

            const auto at = contentStart.inBeats() + (double) child[schema::controlBeat];

            if (at <= beat.inBeats() + 1.0e-9 && (! last.has_value() || at >= last->first))
                last = std::pair (at, (int) child[schema::controlValue]);
        }

        return last.has_value() ? std::optional (last->second) : std::nullopt;
    };

    // The controls beside them, where they were heard, not quantized: those moved in a part in the clip each falls in, as a
    // note would be; then what the part carried in at the start of the clip that holds what was played first in it (before
    // that, and not before the part); and what it lets go of just before the end of what holds what was played last (not
    // after the part), so that a control at the clip's end, which isn't played, never leaves a pedal down: in a clip the take
    // made; in one that was there and goes on, the value that clip had there before, so that what follows in it plays as it
    // did.
    struct Write
    {
        ClipID clip;
        BeatPosition beat;
        PlayedControl control;
        bool moved = false; // moved in the take: what Overdub takes the place of, from the first to the last
    };

    std::vector<Write> writes;
    constexpr auto releaseBeforeEnd = 1.0 / 960.0; // in beats: a tick, at 960 to the quarter note

    for (size_t p = 0; p < parts.size(); ++p)
    {
        if (! wasPlayedIn (p))
            continue;

        for (const auto& control : parts[p].controls)
        {
            if (const auto clip = clipFor (p, toBeat (control.at), false); clip.isValid())
            {
                writes.push_back ({ clip, toBeat (control.at), control, true });
                reach[p].add (toBeat (control.at), clip);
            }
        }

        if (const auto& first = reach[p].first; first.has_value())
        {
            const auto clipStart = getClipRange (findClipState (song, first->second)).getStart();
            const auto beat = std::min (first->first, std::max (ranges[p].getStart(), clipStart));

            for (const auto& control : parts[p].carried)
                writes.push_back ({ first->second, beat, control });

            // After a jump that landed where the take before ended, what that one let go of at its end and this one carries
            // on from there is held on between them, as between the parts of one take (known bug 175: the pedal let go for a
            // tick at the seam). In this take's undo step.
            if (const auto* before = seamBefore != nullptr && seamBefore->has_value() ? &**seamBefore : nullptr;
                p == 0 && before != nullptr && before->at == parts[p].from && beat == ranges[p].getStart())
                for (size_t r = 0; r < before->end.released.size(); ++r)
                    if (std::any_of (parts[p].carried.begin(), parts[p].carried.end(),
                                     [before, &release = before->end.released[r]] (const PlayedControl& carried)
                                     { return carriesOn (carried, release, before->end); }))
                        removeControl (song, before->clip, before->writes[r]);
        }

        if (const auto& last = reach[p].last; last.has_value())
        {
            const auto clipEnd = getClipRange (findClipState (song, last->second)).getEnd();
            const auto beat =
                std::max (last->first, BeatPosition::fromBeats (std::min (ranges[p].getEnd(), clipEnd).inBeats() - releaseBeforeEnd));
            Seam seam { parts[p].to, last->second, {}, {} };

            for (auto control : parts[p].released)
            {
                if (! made.contains (last->second.getRaw()))
                    control.value = valueBefore (last->second, control, beat).value_or (control.value);

                writes.push_back ({ last->second, beat, control });
                seam.end.released.push_back (control);
                seam.writes.push_back ({ beat, control.kind, control.number, control.value, control.channel });
            }

            // What the take after a jump may carry on (followOn()).
            if (p + 1 == parts.size())
            {
                seam.end.ends = parts[p].ends;
                *seamAtEnd = std::move (seam);
            }
        }
    }

    // A controller recorded into a clip that was there takes the place of its earlier values in it on its channel, from its
    // first move in the take to its last, as Ableton Live overdubs a clip's envelope; the clip's other controllers, those on
    // other channels and its notes stay.
    {
        std::map<std::tuple<std::uint64_t, ControlKind, int, int>, std::pair<BeatPosition, BeatPosition>> spans;

        for (const auto& write : writes)
        {
            if (! write.moved || made.contains (write.clip.getRaw()))
                continue;

            const auto key = std::tuple (write.clip.getRaw(), write.control.kind, write.control.number, write.control.channel);

            if (auto found = spans.find (key); found == spans.end())
                spans.emplace (key, std::pair (write.beat, write.beat));
            else
                found->second = { std::min (found->second.first, write.beat), std::max (found->second.second, write.beat) };
        }

        for (const auto& [key, span] : spans)
            removeControls (song, ClipID::fromRaw (std::get<0> (key)), std::get<1> (key), std::get<2> (key), std::get<3> (key), span.first,
                            span.second);
    }

    // Each clip's at once, in order (a bend sweep holds thousands).
    std::map<std::uint64_t, std::vector<ControlValue>> byClip;

    for (const auto& write : writes)
        byClip[write.clip.getRaw()].push_back (
            { write.beat, write.control.kind, write.control.number, write.control.value, write.control.channel });

    for (const auto& [clip, values] : byClip)
        addControls (song, ClipID::fromRaw (clip), values);
}

//==============================================================================
std::optional<std::pair<std::int64_t, std::int64_t>> Take::getRecorded() const
{
    // While it records, as far as the engine has got; once it has ended, what it kept, until its clips take its place.
    if (! tempo.has_value())
        return std::nullopt;

    if (! finished && recording != nullptr && recording->hasStarted())
        return std::pair (recording->getStartSample(), recording->getLength());

    if (ended.has_value() && ! clipsPlaced)
        return std::pair (ended->startSample, ended->length);

    return std::nullopt;
}

std::vector<RecordingInProgress> Take::getInputsInProgress() const
{
    std::vector<RecordingInProgress> shown;
    const auto recorded = getRecorded();

    if (! recorded.has_value())
        return shown;

    const auto [startSample, length] = *recorded;
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

    return shown;
}

std::vector<BeatRange> Take::getInputRanges (TrackID track) const
{
    std::vector<BeatRange> ranges;

    for (const auto& shown : getInputsInProgress())
        if (shown.track == track)
            ranges.push_back (shown.range);

    return ranges;
}

std::vector<RecordingInProgress> Take::getInProgress()
{
    auto shown = getInputsInProgress();
    const auto recorded = getRecorded();

    if (! recorded.has_value())
        return shown;

    const auto [startSample, length] = *recorded;
    const auto toBeat = [this] (std::int64_t sample) { return BeatPosition::fromBeats (tempo->toBeats ((double) sample)); };

    // The notes played into live MIDI's track, once there are any, over the parts they will be kept in: as far as recording
    // has got; round a looped range, once it has gone round, what was played before it, and all of the range.
    if (const auto state = findTrackState (session.getSong(), midiTrack); state.hasType (schema::TRACK) && ! isFolderState (state))
    {
        // (Once it has ended, the engine's notes played may be gone: the notes it kept.)
        const auto parts = ended.has_value() ? ended->parts : collectNotes (startSample, recording->getStartClock(), length);
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

std::vector<TrackID> Take::getInputTracks() const
{
    std::vector<TrackID> tracks;

    for (const auto& take : audio)
        tracks.push_back (take->track);

    return tracks;
}

bool Take::recordsOnto (TrackID track) const
{
    return ! finished && track.isValid()
           && (track == midiTrack || std::any_of (audio.begin(), audio.end(), [track] (const auto& take) { return take->track == track; }));
}

bool Take::showsOn (TrackID track) const
{
    return getRecorded().has_value() && track.isValid()
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

    const auto recorded = getRecorded();

    if (columns <= 0 || found == audio.end() || ! recorded.has_value())
        return levels;

    // The file's first frame was played at this sample of the song, as if nothing looped (its lead of silence's, if any). Once
    // recording has gone round a looped range, a sample of the range is so much later this time round than the first time
    // round, and one before the range was played the first time round.
    const auto [startSample, length] = *recorded;
    const auto firstFrame = startSample - audioShift - getLead (startSample);
    const auto heardRound = getRoundHeard (startSample + length - audioShift);
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
