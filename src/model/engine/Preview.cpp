// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Preview.h"
#include "AudioFiles.h"
#include "devices/synth/Synth.h"
#include "internal/EngineContext.h"
#include "internal/Preview.h"
#include "internal/PreviewLoop.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/mixer/MixerModel.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>

namespace phazite
{
namespace
{
    constexpr std::int64_t soundStrip = 1; // the strip a preview's sound plays on, into the master (0)
    constexpr double beatsPerBar = 4.0;    // a MIDI file loops over whole bars of four beats

    // A MIDI file's notes, in beats from its start by its ticks per quarter note (not its tempo: it plays at the song's),
    // or none if it can't be read. A file timed in SMPTE frames plays its seconds as beats at 120 BPM.
    std::vector<engine::NoteSequence::Note> readMidiNotes (const juce::File& file)
    {
        std::vector<engine::NoteSequence::Note> notes;
        juce::FileInputStream stream (file);
        juce::MidiFile midi;

        if (! stream.openedOk() || ! midi.readFrom (stream))
            return notes;

        const auto ticks = midi.getTimeFormat();

        if (ticks <= 0)
            midi.convertTimestampTicksToSeconds();

        const auto toBeats = [ticks] (double time) { return ticks > 0 ? time / ticks : time * 2.0; };

        for (int track = 0; track < midi.getNumTracks(); ++track)
        {
            juce::MidiMessageSequence sequence (*midi.getTrack (track));
            sequence.updateMatchedPairs();

            for (int i = 0; i < sequence.getNumEvents(); ++i)
            {
                const auto* event = sequence.getEventPointer (i);

                if (! event->message.isNoteOn())
                    continue;

                const auto start = toBeats (event->message.getTimeStamp());
                const auto end = event->noteOffObject != nullptr ? toBeats (event->noteOffObject->message.getTimeStamp()) : start + 1.0;
                notes.push_back (
                    { start, std::max (end, start + 1.0 / 64.0), event->message.getNoteNumber(), event->message.getFloatVelocity() });
            }
        }

        std::stable_sort (notes.begin(), notes.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
        return notes;
    }
} // namespace

struct Previewer::Impl : private juce::Timer
{
    Impl (te::Engine& e, PlaybackHost& h)
        : tracktion (e),
          host (h),
          previews (h.getPreviewEngine())
    {
    }

    ~Impl() override { stopTimer(); }

    // Stops the sound: what it played rings out as a stopped song's does, and its plan stays until the next sound's.
    void release()
    {
        stopTimer();
        starting = false;
        previews.stop();
    }

    void start()
    {
        starting = false;
        previews.play();
        startTimer (250);
    }

    // A loop starts once its start is stretched (StretchedLoop::isReadyToPlay()), a moment after it is chosen; a sound
    // that plays once is released once it has played to its end.
    void timerCallback() override
    {
        if (starting)
        {
            if (loop == nullptr || loop->isReadyToPlay())
                start();
        }
        else if (! previews.isPlaying())
        {
            stopTimer();
        }
        else if (! repeating && previews.getPlayheadBeat() >= lengthBeats)
        {
            release();
        }
    }

    void startWhenReady()
    {
        starting = true;
        startTimer (5);
    }

    // The master of the sounds' plans, kept from one sound to the next (made anew for another rate or block size), whose
    // fader is the previews' volume.
    std::shared_ptr<engine::StripState> getMaster()
    {
        if (master == nullptr || ! juce::exactlyEqual (masterRate, previews.getSampleRate())
            || masterBlockSize != previews.getMaximumBlockSize())
        {
            masterRate = previews.getSampleRate();
            masterBlockSize = previews.getMaximumBlockSize();
            master = std::make_shared<engine::StripState>();
            master->prepare (masterRate, masterBlockSize);
        }

        master->getChannel().setVolume (faderPositionToDecibels (volume));
        return master;
    }

    te::Engine& tracktion; // its audio formats, and what it knows of loops' beats (until T6)
    PlaybackHost& host;
    engine::Engine& previews;
    std::shared_ptr<engine::StripState> master;
    double masterRate = 0.0;
    int masterBlockSize = 0;
    std::shared_ptr<const StretchedLoop> loop; // a loop's sound, stretched to the project's tempo, which its plan reads
    juce::File file;
    double lengthBeats = 0.0; // where the sound ends, in its plan's beats: a loop's length
    bool inTime = false;
    bool repeating = false;
    bool starting = false;   // a loop waiting for its start to be stretched
    bool synced = false;     // to the project, while it plays
    bool songPlayed = false; // whether the project played as it started: it stops once that changes
    float volume = decibelsToFaderPosition (-6.0f);
};

std::unique_ptr<Previewer> createPreviewer (EngineContext& context)
{
    return std::make_unique<Previewer> (std::make_unique<Previewer::Impl> (getEngine (context), getPlaybackHost (context)));
}

Previewer::Previewer (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

Previewer::~Previewer()
{
    stop();
    impl->previews.setPlan (nullptr); // freed by the previews' engine once the audio thread can't be playing it
}

bool Previewer::play (const juce::File& newFile, const ProjectContext& context, bool repeat)
{
    stop();

    if (! newFile.existsAsFile())
        return false;

    const auto* project = getEdit (context);
    auto& previews = impl->previews;
    const auto rate = previews.getSampleRate();
    const auto blockSize = previews.getMaximumBlockSize();
    const auto bpm = project != nullptr ? getTempo (getSong (*project)) : 120.0;

    // The sound on a strip of its own.
    engine::Plan::Strip sound;
    sound.id = soundStrip;
    sound.state = std::make_shared<engine::StripState>();
    sound.state->prepare (rate, blockSize);
    auto inTime = false;
    auto lengthBeats = 0.0;
    std::shared_ptr<const StretchedLoop> loop;

    if (newFile.hasFileExtension ("mid;midi"))
    {
        // A MIDI file plays on Phazite's synth, at its defaults, in time with the project, over whole bars.
        auto notes = readMidiNotes (newFile);
        const auto* synth = findOwnDevice (devices::Synth::typeID);

        if (notes.empty() || synth == nullptr)
            return false;

        auto lastEnd = 0.0;

        for (const auto& note : notes)
            lastEnd = std::max (lastEnd, note.end);

        lengthBeats = std::max (1.0, std::ceil (lastEnd / beatsPerBar - 1.0e-9)) * beatsPerBar;
        auto node = std::make_shared<engine::DeviceNode> (*synth, createOwnDevice (synth->typeID));
        node->getRunner().prepare (rate, blockSize);
        sound.devices.push_back ({ std::move (node) });
        sound.hasInstrument = true;
        sound.notes = std::make_shared<const engine::NoteSequence> (std::move (notes), std::vector<engine::NoteSequence::Control>());
        inTime = project != nullptr;
    }
    else
    {
        // An audio file, read afresh each time: a loop written again at the same path plays at its new length.
        auto reader = std::unique_ptr<juce::AudioFormatReader> (getAudioFormats().createReaderFor (newFile));

        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
            return false;

        // A loop that knows its beats (from its loop data, or from its name when it lasts whole bars) plays in time with the
        // project, stretched as a clip of it would be, by Rubber Band; at its own pitch. Anything else plays as it is.
        const auto fileSeconds = (double) reader->lengthInSamples / reader->sampleRate;
        const auto loopBeats = readAudioFileInfo (*reader, newFile.getFileNameWithoutExtension()).loopBeats;
        inTime = project != nullptr && loopBeats > 0.0;
        std::shared_ptr<engine::AudioFile> audio;
        engine::AudioRegion region;

        if (inTime)
        {
            lengthBeats = loopBeats;
            auto stretched = std::make_shared<StretchedLoop> (impl->tracktion, newFile, lengthBeats * 60.0 / bpm / fileSeconds, rate);

            if (! stretched->isValid())
                return false;

            region.length = stretched->getLength();
            audio = std::make_shared<engine::AudioFile> (makeLoopReader (stretched), &previews.getBlocksPlayed());
            loop = std::move (stretched);
        }
        else
        {
            lengthBeats = fileSeconds * bpm / 60.0;
            region.length = (std::int64_t) std::llround (fileSeconds * rate);
            audio = std::make_shared<engine::AudioFile> (makeSampleReader (std::move (reader)), &previews.getBlocksPlayed());
        }

        impl->host.getPreviewFileReader().add (audio);
        region.file = std::move (audio);
        region.state = std::make_shared<engine::RegionState>();
        sound.audio.push_back (std::move (region));
    }

    // Its plan: the sound into the master, at the project's tempo, a loop over and over unless repeat is false (a riser
    // made for some bars plays once, in time); a one-shot once.
    auto plan = std::make_unique<engine::Plan>();
    plan->tempo = engine::TempoMap ({ { 0.0, bpm } }, { { 0.0, 4, 4 } }, rate);
    impl->repeating = inTime && repeat;

    if (impl->repeating)
        plan->loop = engine::Plan::BeatRange { 0.0, lengthBeats };

    plan->strips.push_back (std::move (sound));
    engine::Plan::Strip master;
    master.state = impl->getMaster();
    master.inputs.push_back ({ 0 });
    plan->strips.push_back (std::move (master));
    previews.setPlan (std::move (plan));
    previews.locate (0.0);

    impl->file = newFile;
    impl->inTime = inTime;
    impl->loop = std::move (loop);
    impl->lengthBeats = lengthBeats;
    impl->synced = false;
    impl->songPlayed = project != nullptr && isSongPlaying (*project);

    if (impl->loop != nullptr)
        impl->startWhenReady();
    else
        impl->start();

    keepInTime (context);
    return true;
}

void Previewer::keepInTime (const ProjectContext& context)
{
    const auto* project = getEdit (context);

    if (impl->file == juce::File() || project == nullptr)
        return;

    // The song started or stopped stops the sound heard meanwhile: the transport plays the song, as FL Studio's Stop ends
    // a preview. A loop started while the song plays plays on in time with it.
    const auto songPlays = isSongPlaying (*project);

    if (songPlays != impl->songPlayed)
    {
        stop();
        return;
    }

    if (! impl->repeating || ! songPlays || impl->synced || impl->starting || impl->lengthBeats <= 0.0)
        return;

    // Once it plays: from where the song is in it, the loop counted from the song's start (its first bar), so that its
    // bars fall on the song's. Both engines play in the same blocks from then on.
    if (const auto* session = findSession (*project))
        impl->previews.locate (std::fmod (std::max (0.0, session->getPlayhead().inBeats()), impl->lengthBeats));

    impl->synced = true;
}

void Previewer::stop()
{
    impl->release();
    impl->file = juce::File();
    impl->loop = nullptr;
    impl->inTime = false;
    impl->repeating = false;
    impl->synced = false;
}

bool Previewer::isPlaying() const
{
    if (impl->starting)
        return true;

    if (impl->file == juce::File() || ! impl->previews.isPlaying())
        return false;

    // Anything that doesn't repeat has finished once the playhead passes its end.
    return impl->repeating || impl->previews.getPlayheadBeat() < impl->lengthBeats;
}

juce::File Previewer::getFile() const
{
    return impl->file;
}

bool Previewer::isInTime() const noexcept
{
    return impl->inTime;
}

void Previewer::setVolume (float faderPosition)
{
    impl->volume = juce::jlimit (0.0f, 1.0f, faderPosition);

    if (impl->master != nullptr)
        impl->master->getChannel().setVolume (faderPositionToDecibels (impl->volume));
}

float Previewer::getVolume() const noexcept
{
    return impl->volume;
}
} // namespace phazite
