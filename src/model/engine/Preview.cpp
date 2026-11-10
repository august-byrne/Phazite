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

    // A sound made in memory, read as a file's samples are (engine/audio/AudioFile.h).
    class MemoryReader final : public engine::SampleReader
    {
    public:
        MemoryReader (juce::AudioBuffer<float> madeSound, double rate)
            : sound (std::move (madeSound)),
              sampleRate (rate)
        {
        }

        int getNumChannels() const override { return sound.getNumChannels(); }
        double getSampleRate() const override { return sampleRate; }
        std::int64_t getLength() const override { return sound.getNumSamples(); }

        bool read (juce::AudioBuffer<float>& buffer, std::int64_t start, int numSamples) override
        {
            if (start < 0 || start + numSamples > sound.getNumSamples())
                return false;

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                buffer.copyFrom (channel, 0, sound, std::min (channel, sound.getNumChannels() - 1), (int) start, numSamples);

            return true;
        }

    private:
        juce::AudioBuffer<float> sound;
        double sampleRate;
    };
} // namespace

struct Previewer::Impl : private juce::Timer
{
    explicit Impl (PlaybackHost& h)
        : host (h),
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

    // The strip a sound plays on, into the master, made for the rate and block size the previews play at.
    engine::Plan::Strip makeSoundStrip() const
    {
        engine::Plan::Strip sound;
        sound.id = soundStrip;
        sound.state = std::make_shared<engine::StripState>();
        sound.state->prepare (previews.getSampleRate(), previews.getMaximumBlockSize());
        return sound;
    }

    // Hands the sound's strip to the previews' engine, from its start: into the master, at a tempo, over and over from its
    // start to lengthBeats while it repeats.
    void setPlan (engine::Plan::Strip sound, double bpm)
    {
        auto plan = std::make_unique<engine::Plan>();
        plan->maximumBlockSize = previews.getMaximumBlockSize(); // its sound's strip's and the master's
        plan->tempo = engine::TempoMap ({ { 0.0, bpm } }, { { 0.0, 4, 4 } }, previews.getSampleRate());

        if (repeating)
            plan->loop = engine::Plan::BeatRange { 0.0, lengthBeats };

        plan->strips.push_back (std::move (sound));
        engine::Plan::Strip toMaster;
        toMaster.state = getMaster();
        toMaster.inputs.push_back ({ 0 });
        plan->strips.push_back (std::move (toMaster));
        previews.setPlan (std::move (plan));
        previews.locate (0.0);
    }

    PlaybackHost& host;
    engine::Engine& previews;
    std::shared_ptr<engine::StripState> master;
    double masterRate = 0.0;
    int masterBlockSize = 0;
    std::shared_ptr<const StretchedLoop> loop; // a loop's sound, stretched to the project's tempo, which its plan reads
    juce::File file;                           // the file it plays, or none for a sound made in memory
    bool hasSound = false;                     // from play() or playSound() until stop()
    double lengthBeats = 0.0;                  // where the sound ends, in its plan's beats: a loop's length
    bool inTime = false;
    bool repeating = false;
    bool starting = false;   // a loop waiting for its start to be stretched
    bool synced = false;     // to the project, while it plays
    bool songPlayed = false; // whether the project played as it started: it stops once that changes
    float volume = decibelsToFaderPosition (-6.0f);
};

std::unique_ptr<Previewer> createPreviewer (EngineContext& context)
{
    return std::make_unique<Previewer> (std::make_unique<Previewer::Impl> (getPlaybackHost (context)));
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

    const auto* project = findSong (context);
    auto& previews = impl->previews;
    const auto rate = previews.getSampleRate();
    const auto blockSize = previews.getMaximumBlockSize();
    const auto bpm = project != nullptr ? getTempo (*project) : 120.0;

    // The sound on a strip of its own.
    auto sound = impl->makeSoundStrip();
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
            auto stretched = std::make_shared<StretchedLoop> (newFile, lengthBeats * 60.0 / bpm / fileSeconds, rate);

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
        region.state = std::make_shared<engine::RegionState> (*audio, rate);
        region.file = std::move (audio);
        sound.audio.push_back (std::move (region));
    }

    // Its plan: the sound into the master, at the project's tempo, a loop over and over unless repeat is false (a riser
    // made for some bars plays once, in time); a one-shot once.
    impl->repeating = inTime && repeat;
    impl->lengthBeats = lengthBeats;
    impl->setPlan (std::move (sound), bpm);

    impl->file = newFile;
    impl->hasSound = true;
    impl->inTime = inTime;
    impl->loop = std::move (loop);
    impl->synced = false;
    impl->songPlayed = project != nullptr && isSongPlaying (*project);

    if (impl->loop != nullptr)
        impl->startWhenReady();
    else
        impl->start();

    keepInTime (context);
    return true;
}

bool Previewer::playSound (juce::AudioBuffer<float> sound, double soundRate)
{
    stop();

    if (sound.getNumChannels() == 0 || sound.getNumSamples() == 0 || soundRate <= 0.0)
        return false;

    // Read whole before it plays (a part not read yet would play as silence), then at the previews' rate, once, at the tempo
    // of a sound that isn't in time; the transport leaves it be (keepInTime()).
    constexpr double bpm = 120.0;
    auto& previews = impl->previews;
    const auto seconds = sound.getNumSamples() / soundRate;
    auto audio =
        std::make_shared<engine::AudioFile> (std::make_unique<MemoryReader> (std::move (sound), soundRate), &previews.getBlocksPlayed());
    audio->readNow (0, audio->getLength());
    impl->host.getPreviewFileReader().add (audio);

    engine::AudioRegion region;
    region.length = (std::int64_t) std::llround (seconds * previews.getSampleRate());
    region.state = std::make_shared<engine::RegionState> (*audio, previews.getSampleRate());
    region.file = std::move (audio);
    auto strip = impl->makeSoundStrip();
    strip.audio.push_back (std::move (region));

    impl->repeating = false;
    impl->lengthBeats = seconds * bpm / 60.0;
    impl->setPlan (std::move (strip), bpm);
    impl->hasSound = true;
    impl->start();
    return true;
}

double Previewer::getSampleRate() const noexcept
{
    return impl->previews.getSampleRate();
}

void Previewer::keepInTime (const ProjectContext& context)
{
    const auto* project = findSong (context);

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
    impl->hasSound = false;
    impl->loop = nullptr;
    impl->inTime = false;
    impl->repeating = false;
    impl->synced = false;
}

bool Previewer::isPlaying() const
{
    if (impl->starting)
        return true;

    if (! impl->hasSound || ! impl->previews.isPlaying())
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
