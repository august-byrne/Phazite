// SPDX-License-Identifier: AGPL-3.0-or-later
// The real-time audit (roadmap step 0.3). Each scenario builds a small project and plays it through Phazite's engine as
// the app plays it, on the tests' audio device (TestAudioDevice.h), which a separate thread drives as the audio thread.
// In Windows Debug builds it counts the heap allocations and frees made on that thread and shows where they came from
// (AudioThreadMonitor). Under the linux-asan and linux-tsan presets the same playback runs under AddressSanitizer and
// ThreadSanitizer.
//
// The scenarios whose steady playback must stay clean fail if it allocates or frees; those that read audio files only
// report what they find. To see the report:
//   build\windows-debug\phazite_tests_artefacts\Debug\phazite_tests.exe --test-suite=realtime-audit
// docs/realtime-audit.md lists the findings and what Phazite does about each.

#include "AudioThreadMonitor.h"
#include "SandboxedPlugins.h"
#include "TestAudioDevice.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Phaser.h"
#include "devices/reverb/Reverb.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/synth/Synth.h"
#include "devices/vocoder/Vocoder.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/devices/internal/Wavetables.h"
#include "model/engine/AudioFiles.h"
#include "model/engine/EngineContext.h"
#include "model/engine/Preview.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/engine/internal/Preview.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"

#include <chrono>
#include <thread>

using phazite::test::AudioThreadMonitor;
using phazite::test::TestSong;

namespace
{
constexpr double auditSampleRate = 48000.0;
constexpr int auditBlockSize = 256;
constexpr double warmUpSeconds = 0.25;   // the first blocks after starting playback, at least,
constexpr double quietSeconds = 0.25;    // and on until the audio thread has gone this long without allocating or freeing,
constexpr double maxWarmUpSeconds = 2.0; // but no longer
constexpr double steadySeconds = 1.0;    // then steady playback

// Runs the warm-up through processFor (seconds) and returns how long it took. What plays may first allocate some way into
// playback (what starts with a note, the first time one comes): steady playback starts once a quiet stretch shows that
// everything has started. Something that keeps allocating still shows in the steady second after the longest warm-up.
template <typename ProcessFor>
double warmUp (ProcessFor& processFor)
{
    processFor (warmUpSeconds);
    auto elapsed = warmUpSeconds, quietFor = 0.0;
    auto events = AudioThreadMonitor::getNumEvents();

    while (quietFor < quietSeconds && elapsed < maxWarmUpSeconds)
    {
        constexpr double step = 0.05;
        processFor (step);
        elapsed += step;

        const auto now = AudioThreadMonitor::getNumEvents();
        quietFor = now == events ? quietFor + step : 0.0;
        events = now;
    }

    return elapsed;
}

//==============================================================================
// A stereo sine wave file, two seconds long or as long as asked, so audio clips have something to play.
class SineFile
{
public:
    explicit SineFile (double seconds = 2.0)
    {
        juce::AudioBuffer<float> buffer (2, juce::roundToInt (auditSampleRate * seconds));

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto sample = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) auditSampleRate);
            buffer.setSample (0, i, sample);
            buffer.setSample (1, i, sample);
        }

        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (temp.getFile());
        const auto options = juce::AudioFormatWriterOptions {}.withSampleRate (auditSampleRate).withNumChannels (2).withBitsPerSample (24);

        if (auto writer = juce::WavAudioFormat().createWriterFor (stream, options))
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    juce::File getFile() const { return temp.getFile(); }

private:
    juce::TemporaryFile temp { ".wav" };
};

// The test song's song, which the model's functions change.
phazite::Song& getSong (const TestSong& song)
{
    auto* open = phazite::findSong (song.context);
    REQUIRE (open != nullptr);
    return *open;
}

void addSineClip (const TestSong& song, phazite::TrackID track, const SineFile& sine)
{
    phazite::test::addAudioClip (getSong (song), track, "Sine", sine.getFile(), 0.0, 2.0);
}

// A tone on a track for longer than a scenario plays: an audio clip of a sine wave, from the song's start.
void addTone (const TestSong& song, phazite::TrackID track)
{
    static const SineFile sine (8.0);
    phazite::test::addAudioClip (getSong (song), track, "Tone", sine.getFile(), 0.0, 8.0);
}

// A device as Phazite's engine plays it, while it plays the device's project: what its editor's displays read and its
// pads play (model/devices/DeviceHandle.h).
phazite::engine::DeviceRunner& getPlayed (const TestSong& song, phazite::DeviceID device)
{
    auto* session = phazite::findPlayingSession (getSong (song));
    REQUIRE (session != nullptr);
    auto* node = session->getPlayer().findDevice ((std::int64_t) device.getRaw());
    REQUIRE (node != nullptr);
    return node->getRunner();
}

// The ID of a parameter of one of the EQ's bands.
juce::String getBandParameter (int band, phazite::devices::Equalizer::Field field)
{
    using phazite::devices::Equalizer;
    const auto id = Equalizer::info.parameters[(size_t) Equalizer::getParameterIndex (band, field)].id;
    return juce::String (std::string (id));
}

//==============================================================================
struct Measurement
{
    AudioThreadMonitor::Counts warmUp, steady;
    juce::StringArray callSites;       // of every allocation and free measured
    juce::StringArray steadyCallSites; // of those after the warm-up
    int firstSteadyEvent = 0;
    double warmUpTaken = 0.0; // how long the warm-up took, in seconds (warmUp())
    float peakLevel = 0.0f;
};

// Plays the open project from its start through Phazite's engine as the app plays it, in real time on a separate "audio"
// thread that drives the tests' audio device (TestAudioDevice.h), whose callback is Phazite's
// (model/playback/internal/PlaybackHost.h), and measures that thread; a tone comes into the device's two inputs. What is
// given is done once the engine has the song, before it plays. Recording, it records a take instead, while another thread
// plays a note every twentieth of a second into a track live, as a MIDI driver would; stopping keeps the take.
Measurement playProjectAndMeasure (phazite::ProjectContext& context, const std::function<void()>& before = {}, bool recording = false,
                                   phazite::TrackID liveTrack = {})
{
    auto* openSong = phazite::findSong (context);
    REQUIRE (openSong != nullptr);
    auto& song = *openSong;
    auto& devices = phazite::test::getEngineContext().getAudioDeviceManager();
    auto* device = phazite::test::TestAudioDeviceType::open (devices, auditSampleRate, auditBlockSize, 2);
    REQUIRE (device != nullptr);
    phazite::test::dispatchPendingMessages(); // the song for the device's rate
    phazite::movePlayhead (song, phazite::BeatPosition());

    if (before)
        before();

    if (recording)
        phazite::startRecording (song);
    else
        phazite::playOrStop (song);

    phazite::test::dispatchPendingMessages();
    auto& engine = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    juce::AudioBuffer<float> tone (1, auditBlockSize);

    for (int i = 0; i < auditBlockSize; ++i)
        tone.setSample (0, i, 0.25f * std::sin (juce::MathConstants<float>::twoPi * (float) i * 4.0f / (float) auditBlockSize));

    std::atomic<bool> playingNotes { liveTrack.isValid() };
    std::thread notes (
        [&]
        {
            for (int note = 0; playingNotes.load(); ++note)
            {
                engine.playLive ((std::int64_t) liveTrack.getRaw(),
                                 { note % 2 == 0 ? phazite::devices::NoteEvent::Type::noteOn : phazite::devices::NoteEvent::Type::noteOff,
                                   0, 60, 0.8f });
                std::this_thread::sleep_for (std::chrono::milliseconds (50));
            }
        });

    // On one thread, so that the monitor counts every allocation (the workers play the same strips' code).
    const auto threads = engine.getNumThreads();
    engine.setNumThreads (1);

    Measurement m;
    juce::AudioBuffer<float> buffer (2, auditBlockSize); // allocated before the thread is marked as an audio thread
    const auto blockDuration =
        std::chrono::duration_cast<std::chrono::steady_clock::duration> (std::chrono::duration<double> (auditBlockSize / auditSampleRate));

    AudioThreadMonitor::reset();

    std::thread audioThread (
        [&]
        {
            const AudioThreadMonitor::ScopedAudioThread marker;

            const auto processFor = [&] (double seconds)
            {
                const auto numBlocks = juce::roundToInt (seconds * auditSampleRate / auditBlockSize);

                for (int i = 0; i < numBlocks; ++i)
                {
                    const auto blockEnd = std::chrono::steady_clock::now() + blockDuration;

                    for (int channel = 0; channel < 2; ++channel)
                        buffer.copyFrom (channel, 0, tone, 0, 0, auditBlockSize);

                    device->process (buffer);
                    m.peakLevel = std::max (m.peakLevel, buffer.getMagnitude (0, auditBlockSize));
                    std::this_thread::sleep_until (blockEnd);
                }
            };

            m.warmUpTaken = warmUp (processFor);
            m.warmUp = AudioThreadMonitor::getCounts();
            m.firstSteadyEvent = AudioThreadMonitor::getNumEvents();

            processFor (steadySeconds);
            const auto total = AudioThreadMonitor::getCounts();
            m.steady = { total.allocations - m.warmUp.allocations, total.frees - m.warmUp.frees };
        });
    audioThread.join();
    playingNotes = false;
    notes.join();
    engine.setNumThreads (threads);

    phazite::stopAndReturn (song);
    m.callSites = AudioThreadMonitor::describeCallSites();
    m.steadyCallSites = AudioThreadMonitor::describeCallSites (6, m.firstSteadyEvent);

    // So that later tests start without the device.
    phazite::test::TestAudioDeviceType::close (devices);
    return m;
}

// Plays a test song as playProjectAndMeasure() does, with what is to be done once Phazite's engine has it, before it plays,
// and after.
Measurement playAndMeasure (TestSong& song, const std::function<void()>& before = {}, const std::function<void()>& after = {})
{
    auto m = playProjectAndMeasure (song.context, before);

    if (after)
        after();

    return m;
}

void report (const juce::String& scenario, const Measurement& m)
{
    juce::String text;
    text << scenario << ": peak output " << juce::Decibels::toString (juce::Decibels::gainToDecibels (m.peakLevel)) << "\n";

    if (AudioThreadMonitor::isAvailable())
    {
        text << "  audio thread, warm-up of " << m.warmUpTaken << " s: " << m.warmUp.allocations << " allocations, " << m.warmUp.frees
             << " frees\n";
        text << "  audio thread, next " << steadySeconds << " s: " << m.steady.allocations << " allocations, " << m.steady.frees
             << " frees\n";

        for (const auto& site : m.callSites)
            text << "    " << site << "\n";

        if (! m.steadyCallSites.isEmpty())
        {
            text << "  of those, in the next " << steadySeconds << " s:\n";

            for (const auto& site : m.steadyCallSites)
                text << "    " << site << "\n";
        }
    }
    else
    {
        text << "  (allocations are only counted in Windows Debug builds)\n";
    }

    MESSAGE (text.toStdString());
}

// For playback paths that must stay clean: once playing, nothing is allocated or freed on the audio
// thread. (Only measured in Windows Debug builds.)
void checkSteadyPlaybackDoesNotAllocate (const Measurement& m)
{
    if (AudioThreadMonitor::isAvailable())
    {
        CHECK (m.steady.allocations == 0);
        CHECK (m.steady.frees == 0);
    }
}

// Chords a sixteenth long, one after another for two bars, so that the busiest blocks (one chord's ends with the next's
// starts) come from the first beat, within the warm-up.
void addChords (const TestSong& song, phazite::TrackID track)
{
    using phazite::test::at, phazite::test::lasting;
    auto& played = getSong (song);
    const auto clip = phazite::test::addMidiClip (played, track, { at (0.0), at (8.0) }, "Chords");
    REQUIRE (clip.isValid());

    for (int i = 0; i < 32; ++i)
        for (const auto note : { 48, 52, 55, 59 })
            phazite::test::addMidiNote (played, clip, note + (i % 2) * 5, at (i * 0.25), lasting (0.25), 100);
}

// The song kept as a version, as Save Version keeps it, and bounced, as Bounce renders it: the version's id.
juce::String keepAndBounce (TestSong& song, const juce::String& name)
{
    const auto idle = [&song]
    {
        const auto* history = phazite::getVersionHistory (song.context);
        return history != nullptr && ! history->isBusy();
    };

    REQUIRE (phazite::test::dispatchMessagesUntil (idle));
    phazite::setVersionDraft (song.context, name, {});
    std::optional<juce::Result> saved;
    REQUIRE (phazite::saveVersion (song.context, [&saved] (juce::Result r) { saved = r; }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&saved] { return saved.has_value(); }));
    REQUIRE (saved->wasOk());
    REQUIRE (phazite::test::dispatchMessagesUntil (idle));

    const auto version = phazite::getLastVersion (song.context)->id;
    phazite::bounceVersion (song.context, version, song.renders);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.renders.finished.isEmpty(); }, 60000));
    REQUIRE (song.renders.finished[0].endsWith (": ok"));
    return version;
}

// A cutoff swept by its automation, from one value to another and back on every beat, over two bars.
void sweepCutoff (const TestSong& song, phazite::DeviceID device, float from, float to)
{
    for (int beat = 0; beat < 8; ++beat)
    {
        phazite::addPoint (getSong (song), { device, "cutoff" }, { phazite::BeatPosition::fromBeats (beat), from, 0.0f });
        phazite::addPoint (getSong (song), { device, "cutoff" }, { phazite::BeatPosition::fromBeats (beat + 0.5), to, 0.0f });
    }
}
} // namespace

//==============================================================================
TEST_SUITE ("realtime-audit")
{
    // These read no files, so their steady playback is deterministic: it must not allocate.

    TEST_CASE ("Real-time audit: one empty track")
    {
        TestSong song;
        const auto m = playAndMeasure (song);
        report ("One empty track", m);
        CHECK (m.peakLevel == 0.0f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's own engine, its transport and its metronome")
    {
        TestSong song;
        song.addMidiClip (song.firstTrack, 0.0, 16.0);
        song.setSwitch ("transport.metronome", true);
        const auto m = playProjectAndMeasure (song.context);
        report ("Phazite's engine: the metronome", m);
        CHECK (m.peakLevel > 0.01f); // it clicks
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's own engine, chords on Phazite's synth through a track's EQ and fader into the master")
    {
        TestSong song;
        song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
        song.addEffect (song.firstTrack, phazite::devices::Equalizer::typeID);
        song.setPan (song.firstTrack, 0.3f);
        const auto clip = song.addMidiClip (song.firstTrack, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (clip, { 48 + (beat % 2) * 5, 52, 55, 59 }, beat, 0.75);

        const auto m = playProjectAndMeasure (song.context);
        report ("Phazite's engine: chords on the synth", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's own engine, audio clips read from their files as they play, faded, on two tracks")
    {
        TestSong song;
        const auto tone = phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 8.0, 220.0, 0.25f);
        const auto first = song.addAudioClip (song.firstTrack, tone, 0.0);
        phazite::fadeClipTo (song.context, first, true, phazite::BeatPosition::fromBeats (0.5));
        const auto second = song.addTrack ("Second");
        song.addAudioClip (second, tone, 1.0);
        const auto m = playProjectAndMeasure (song.context);
        report ("Phazite's engine: audio clips", m);
        CHECK (m.peakLevel > 0.01f); // they play
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's own engine, chords through a filter whose cutoff is automated, the volume and pan too")
    {
        using phazite::BeatPosition;
        TestSong song;
        const auto track = song.firstTrack;
        song.setInstrument (track, phazite::devices::Synth::typeID);
        const auto filter = song.addEffect (track, phazite::devices::FilterEffect::typeID);
        song.addAutomation ({ filter, "cutoff" }, { { BeatPosition(), 200.0f, 0.3f },
                                                    { BeatPosition::fromBeats (1.0), 8000.0f, -0.8f },
                                                    { BeatPosition::fromBeats (2.0), 500.0f, 1.0f },
                                                    { BeatPosition::fromBeats (3.0), 3000.0f } });
        song.addAutomation (song.getVolumeParameter (track), { { BeatPosition(), 0.5f, 0.4f }, { BeatPosition::fromBeats (2.0), 0.8f } });
        song.addAutomation (song.getPanParameter (track), { { BeatPosition(), -0.5f }, { BeatPosition::fromBeats (3.0), 0.5f } });
        const auto clip = song.addMidiClip (track, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (clip, { 60, 64, 67 }, beat, 0.75);

        const auto m = playProjectAndMeasure (song.context);
        report ("Phazite's engine: automation", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's own engine, chords through a filter that an LFO and a note follower move, the pan and volume "
               "by an envelope follower, steps and a macro")
    {
        using phazite::ModulatorType;
        TestSong song;
        const auto track = song.firstTrack;
        song.setInstrument (track, phazite::devices::Synth::typeID);
        const auto filter = song.addEffect (track, phazite::devices::FilterEffect::typeID);
        song.modulate (song.addModulator (track, ModulatorType::lfo), { filter, "cutoff" }, 0.4f);
        song.modulate (song.addModulator (track, ModulatorType::noteFollower), { filter, "cutoff" }, 0.2f);
        song.modulate (song.addModulator (track, ModulatorType::envelopeFollower), song.getPanParameter (track), 0.5f);
        song.modulate (song.addModulator (track, ModulatorType::steps), song.getVolumeParameter (track), 0.3f);
        song.modulate (song.addModulator (track, ModulatorType::random), song.getVolumeParameter (track), 0.1f);
        song.modulate (song.addModulator (track, ModulatorType::breakpoints), { filter, "resonance" }, 0.2f);
        const auto macro = song.addModulator (track, ModulatorType::macro);
        song.modulate (macro, song.getVolumeParameter (track), -0.2f);
        song.setMacroValue (macro, 0.5f);
        const auto clip = song.addMidiClip (track, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (clip, { 60 + beat % 3, 64, 67 }, beat, 0.75);

        const auto m = playProjectAndMeasure (song.context);
        report ("Phazite's engine: modulation", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's own engine, chords in a bus, sends before and after the fader to returns, a sidechain, "
               "a container and the loudness meter")
    {
        TestSong song;
        const auto keys = song.firstTrack;
        song.setInstrument (keys, phazite::devices::Synth::typeID);
        const auto filter = song.addEffect (keys, phazite::devices::FilterEffect::typeID);
        const auto saturator = song.addEffect (keys, phazite::devices::Saturator::typeID);
        REQUIRE (phazite::groupIntoContainer (song.context, { filter, saturator }).isValid());
        const auto chords = song.addMidiClip (keys, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (chords, { 60 + beat % 3, 64, 67 }, beat, 0.75);

        const auto kick = song.addTrack ("Kick");
        song.setInstrument (kick, phazite::devices::DrumSampler::typeID);
        const auto beats = song.addMidiClip (kick, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addNote (beats, 36, beat, 0.25);

        song.addBus ("Bus", { keys });
        song.addReturn (keys, phazite::devices::Reverb::typeID, -6.0f);
        song.addReturn (kick, phazite::devices::Delay::typeID, -10.0f);
        phazite::setPreFader (song.context, phazite::getSends (song.context, kick).front().id, true);
        REQUIRE (phazite::duckUnder (song.context, keys, kick).isValid());

        const auto m = playProjectAndMeasure (song.context);
        report ("Phazite's engine: routing", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's own engine recording a take: an armed track's input, heard, and notes played live")
    {
        TestSong song;
        song.setSwitch ("transport.countIn", false);
        song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
        phazite::routeLiveMidiInput (getSong (song)); // live MIDI plays the synth's track, which the take records
        const auto voice = song.addTrack ("Voice");
        phazite::setTrackInput (song.context, voice, "1");
        phazite::setArmed (song.context, voice, true);
        REQUIRE (phazite::isArmed (song.context, voice));

        const auto m = playProjectAndMeasure (song.context, {}, true, song.firstTrack);
        report ("Phazite's engine: recording", m);
        CHECK (m.peakLevel > 0.01f);                                           // the input is heard, and the notes
        CHECK (phazite::getClips (song.context, voice).size() == 1);           // the input's take
        CHECK (phazite::getClips (song.context, song.firstTrack).size() == 1); // the notes'
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's own engine, chords through a plug-in in its helper (the tests' stand-in) after Phazite's synth")
    {
        phazite::test::TestPlugin testPlugin;
        TestSong song;
        const auto track = song.firstTrack;
        song.setInstrument (track, phazite::devices::Synth::typeID);
        song.addEffect (track, "PhaziteTestEffect");
        const auto clip = song.addMidiClip (track, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (clip, { 60 + beat % 3, 64, 67 }, beat, 0.75);

        const auto m = playProjectAndMeasure (song.context);
        report ("Phazite's engine: a plug-in in its helper", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's own engine, chords on three plug-in synths in their helpers, handed their blocks early")
    {
        phazite::test::SandboxedPlugins sandboxed;
        TestSong song;

        for (int i = 0; i < 3; ++i)
        {
            const auto track = i == 0 ? song.firstTrack : song.addTrack ("Synth " + juce::String (i + 1));
            song.setInstrument (track, "PhaziteTestSynth");
            const auto clip = song.addMidiClip (track, 0.0, 16.0);

            for (int beat = 0; beat < 16; ++beat)
                song.addChord (clip, { 48 + 12 * i + beat % 3, 52 + 12 * i, 55 + 12 * i }, beat, 0.75);
        }

        const auto m = playProjectAndMeasure (song.context);
        report ("Phazite's engine: plug-in synths handed their blocks early", m);
        CHECK (m.peakLevel > 0.01f); // they play
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: volume automation and an LFO on pan")
    {
        using phazite::BeatPosition;
        TestSong song;
        const auto track = song.firstTrack;
        song.addAutomation (song.getVolumeParameter (track), { { BeatPosition(), 0.5f, 0.0f },
                                                               { BeatPosition::fromBeats (2.0), 0.8f, 0.0f },
                                                               { BeatPosition::fromBeats (4.0), 0.5f, 0.0f } });
        song.modulate (song.addModulator (track, phazite::ModulatorType::lfo), song.getPanParameter (track), 0.5f);

        const auto m = playAndMeasure (song);
        report ("Volume automation and an LFO on pan", m);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: a mix with a bus, a reverb return and the loudness meter")
    {
        // Built with the functions the mixer uses (roadmap step 1.4), with a tone in place of a recording.
        TestSong song;
        const auto tone = song.firstTrack;
        addTone (song, tone);
        song.addBus ("Bus", { tone });
        song.addReturn (tone, phazite::devices::Reverb::typeID);

        const auto m = playAndMeasure (song);
        report ("A tone through a bus, with a send to a reverb return, into the loudness meter", m);
        CHECK (m.peakLevel > 0.01f);
        const auto readings = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine().getLoudnessMeter().getReadings();
        CHECK (readings.momentary.value_or (-100.0f) > -40.0f); // it measured what played
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's EQ, every band on and one automated, with its analyzer listening")
    {
        using phazite::devices::Equalizer;
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);
        const auto eq = song.addEffect (track, Equalizer::typeID);

        for (int band = 0; band < Equalizer::numBands; ++band)
        {
            song.setParameter (eq, getBandParameter (band, Equalizer::on), 1.0f);
            song.setParameter (eq, getBandParameter (band, Equalizer::gain), 3.0f);
        }

        song.addAutomation ({ eq, "band3Gain" },
                            { { phazite::BeatPosition(), -12.0f, 0.0f }, { phazite::BeatPosition::fromBeats (2.0), 12.0f, 0.0f } });

        const auto tap = [&song, eq]() -> auto& { return dynamic_cast<Equalizer&> (getPlayed (song, eq).getProcessor()).getOutputTap(); };
        std::vector<float> tapped (32768);

        const auto m = playAndMeasure (
            song, [&tap] { tap().setListening (true); }, [&tap, &tapped] { CHECK (tap().pull (tapped.data(), (int) tapped.size()) > 0); });
        report ("Phazite's EQ, every band on and one automated, with its analyzer listening", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: every kind of modulator, and a macro, moving Phazite's EQ")
    {
        using phazite::devices::Equalizer;
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);
        const auto eq = song.addEffect (track, Equalizer::typeID);

        // Each moves a band's gain, as Phazite sets them up (model/modulation/internal/Modulation.h).
        int band = 0;

        for (const auto type : phazite::allModulatorTypes)
        {
            song.setParameter (eq, getBandParameter (band, Equalizer::on), 1.0f);
            song.modulate (song.addModulator (track, type), { eq, getBandParameter (band, Equalizer::gain) }, 0.5f);
            ++band;
        }

        size_t modulations = 0;

        for (int moved = 0; moved < band; ++moved)
            modulations += phazite::getModulations (getSong (song), { eq, getBandParameter (moved, Equalizer::gain) }).size();

        CHECK (modulations == std::size (phazite::allModulatorTypes));

        const auto m = playAndMeasure (song);
        report ("Every kind of modulator, and a macro, moving Phazite's EQ", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: lanes sweeping Phazite's EQ, a cycle a beat, and fading a track's volume")
    {
        using phazite::devices::Equalizer;
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);
        const auto eq = song.addEffect (track, Equalizer::typeID);
        song.setParameter (eq, getBandParameter (0, Equalizer::on), 1.0f);

        // Over all of the playback measured: the band's gain swept, a cycle a beat, and the volume faded.
        const auto range = phazite::BeatRange (phazite::BeatPosition(), phazite::BeatPosition::fromBeats (32.0));
        phazite::replacePoints (
            getSong (song), { eq, "band1Gain" }, range,
            phazite::makeAutomationShape (phazite::AutomationShape::sine, range, phazite::BeatDuration::fromBeats (1.0), -12.0f, 12.0f));
        phazite::replacePoints (getSong (song), song.getVolumeParameter (track), range,
                                { { range.getStart(), 0.8f }, { range.getEnd(), 0.4f } });

        const auto m = playAndMeasure (song);
        report ("Lanes sweeping Phazite's EQ, a cycle a beat, and fading a track's volume", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: a container of Phazite's EQ and compressor at half mix, a macro and the track's LFO turning them")
    {
        using phazite::devices::Equalizer;
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);
        const auto eq = song.addEffect (track, Equalizer::typeID);
        const auto compressor = song.addEffect (track, phazite::devices::Compressor::typeID);

        // Grouped as Phazite groups them (model/mixer/internal/Containers.h).
        const auto container = phazite::groupIntoContainer (song.context, { eq, compressor });
        REQUIRE (container.isValid());
        phazite::setMix (getSong (song), container, 0.5f);
        REQUIRE (phazite::getMix (getSong (song), container) == doctest::Approx (0.5f));

        // Its first macro turns a band of the EQ; the track's LFO moves the compressor's makeup gain.
        song.setParameter (eq, getBandParameter (0, Equalizer::on), 1.0f);
        const auto macros = phazite::getContainerMacros (getSong (song), container);
        REQUIRE (! macros.empty());
        song.modulate (macros[0], { eq, getBandParameter (0, Equalizer::gain) }, 0.5f);
        song.setMacroValue (macros[0], 0.7f);
        song.modulate (song.addModulator (track, phazite::ModulatorType::lfo), { compressor, "makeup" }, 0.5f);

        const auto m = playAndMeasure (song);
        report ("A container of Phazite's EQ and compressor at half mix, a macro and the track's LFO turning them", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's compressor, ducked by another track at 70 % mix, with its meters read")
    {
        using phazite::devices::Compressor;
        TestSong song;
        const auto track = song.firstTrack;
        const auto source = song.addTrack ("Source");
        addTone (song, track);
        addTone (song, source);
        phazite::setTrackMuted (getSong (song), source, true); // still heard by the sidechain

        const auto compressor = song.addEffect (track, Compressor::typeID);
        // Its curve turning down from -40 dB, so that the tone ducks it.
        song.setParameter (compressor, "point2X", phazite::devices::curves::axisForDecibels (-40.0f));
        song.setParameter (compressor, "point2Y", phazite::devices::curves::axisForDecibels (-40.0f));
        song.setParameter (compressor, "mix", 0.7f);
        phazite::setSidechainSource (getSong (song), compressor, source);
        REQUIRE (phazite::getSidechainSource (getSong (song), compressor) == source);

        const auto meter = [&song, compressor]() -> auto&
        { return dynamic_cast<Compressor&> (getPlayed (song, compressor).getProcessor()).getReductionMeter(); };
        const auto m = playAndMeasure (song, [&meter] { meter().take(); }, [&meter] { CHECK (meter().take() > 1.0f); });
        report ("Phazite's compressor, ducked by another track at 70 % mix, with its meters read", m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's vocoder at 40 bands, its carrier another track through the sidechain")
    {
        using phazite::devices::Vocoder;
        TestSong song;
        const auto voice = song.firstTrack;
        const auto carrier = song.addTrack ("Carrier");
        addTone (song, voice);
        addTone (song, carrier);
        phazite::setTrackMuted (getSong (song), carrier, true); // still heard by the sidechain

        const auto vocoder = song.addEffect (voice, Vocoder::typeID);
        song.setParameter (vocoder, "carrier", (float) Vocoder::Carrier::track);
        song.setParameter (vocoder, "bands", 5.0f);
        phazite::setSidechainSource (getSong (song), vocoder, carrier);
        REQUIRE (phazite::getSidechainSource (getSong (song), vocoder) == carrier);

        const auto m = playAndMeasure (song);
        report ("Phazite's vocoder at 40 bands, its carrier another track through the sidechain", m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's limiter on the master, pushed 12 dB, with its meters read")
    {
        using phazite::devices::Limiter;
        TestSong song;
        addTone (song, song.firstTrack);
        const auto limiter = song.addEffect ({}, Limiter::typeID); // the master's
        song.setParameter (limiter, "gain", 12.0f);

        const auto meter = [&song, limiter]() -> auto&
        { return dynamic_cast<Limiter&> (getPlayed (song, limiter).getProcessor()).getReductionMeter(); };
        const auto m = playAndMeasure (song, [&meter] { meter().take(); }, [&meter] { CHECK (meter().take() > 1.0f); });
        report ("Phazite's limiter on the master, pushed 12 dB, with its meters read", m);
        CHECK (m.peakLevel > 0.01f);
        CHECK (m.peakLevel < juce::Decibels::decibelsToGain (-1.0f) * 1.001f); // its ceiling
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: MIDI into Phazite's synth, notes overlapping, with noise and its LFO")
    {
        using phazite::devices::Synth;
        TestSong song;
        const auto track = song.firstTrack;
        const auto synth = song.setInstrument (track, Synth::typeID);
        song.setParameter (synth, "noiseLevel", 0.2f);
        song.setParameter (synth, "lfoDepth", 0.5f);
        song.setParameter (synth, "release", 400.0f);

        // More notes than voices, so some are stolen.
        using phazite::test::at, phazite::test::lasting;
        const auto clip = phazite::test::addMidiClip (getSong (song), track, { at (0.0), at (8.0) }, "Notes");
        REQUIRE (clip.isValid());

        for (int i = 0; i < 32; ++i)
            phazite::test::addMidiNote (getSong (song), clip, 36 + i, at (i * 0.125), lasting (1.0), 100);

        const auto m = playAndMeasure (song);
        report ("MIDI into Phazite's synth, notes overlapping, with noise and its LFO", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's synth with seven unison voices, spread wide")
    {
        using phazite::devices::Synth;
        TestSong song;
        const auto track = song.firstTrack;
        const auto synth = song.setInstrument (track, Synth::typeID);
        song.setParameter (synth, "unisonVoices", 6.0f); // seven
        song.setParameter (synth, "unisonDetune", 40.0f);
        addChords (song, track);

        const auto m = playAndMeasure (song);
        report ("Phazite's synth with seven unison voices, spread wide", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's wavetable synth, eight unison voices and warps, an envelope sweeping each note's table")
    {
        using phazite::devices::WavetableSynth;
        TestSong song;
        const auto track = song.firstTrack;
        const auto wavetable = song.setInstrument (track, WavetableSynth::typeID);
        phazite::setFactoryWavetable (getSong (song), wavetable, 1, phazite::devices::findFactoryWavetable ("Vowels"));

        const auto set = [&song, wavetable] (const char* id, float value) { song.setParameter (wavetable, id, value); };
        set ("unisonVoices", 7.0f); // eight
        set ("unisonPosition", 0.5f);
        set ("osc1Warp", (float) WavetableSynth::Warp::sync);
        set ("osc1WarpAmount", 0.4f);
        set ("osc2On", 1.0f);
        set ("osc2Warp", (float) WavetableSynth::Warp::fm);
        set ("osc2WarpAmount", 0.3f);
        set ("subLevel", 0.3f);
        set ("noiseLevel", 0.1f);
        set ("drive", 6.0f);
        set ("mod2Source", (float) WavetableSynth::ModSource::envelope3);
        set ("mod2Target", (float) WavetableSynth::ModTarget::osc1Position);
        set ("mod2Amount", 1.0f);
        set ("mod3Source", (float) WavetableSynth::ModSource::lfo1);
        set ("mod3Target", (float) WavetableSynth::ModTarget::osc2Position);
        set ("mod3Amount", 0.5f);

        // Chords a sixteenth long, as for the synth's unison.
        addChords (song, track);

        const auto m = playAndMeasure (song);
        report ("Phazite's wavetable synth, eight unison voices and warps, an envelope sweeping each note's table", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's filter, saturator, multiband, delay, phaser, flanger, stereo expander and limiter in a row, "
               "the filter swept")
    {
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);

        phazite::DeviceID filter;

        for (const auto* type :
             { phazite::devices::FilterEffect::typeID, phazite::devices::Saturator::typeID, phazite::devices::Multiband::typeID,
               phazite::devices::Delay::typeID, phazite::devices::Phaser::typeID, phazite::devices::Flanger::typeID,
               phazite::devices::StereoExpander::typeID, phazite::devices::Limiter::typeID })
        {
            const auto device = song.addEffect (track, type);

            if (! filter.isValid())
                filter = device;
        }

        // The filter swept by a lane, a cycle a beat, so its cutoff glides all the time; the others well into their work.
        sweepCutoff (song, filter, 200.0f, 8000.0f);

        const auto m = playAndMeasure (song);
        report ("Phazite's filter, saturator, multiband, delay, phaser, flanger, stereo expander and limiter in a row, the filter swept",
                m);
        CHECK (m.peakLevel > 0.001f);
        CHECK (m.peakLevel < juce::Decibels::decibelsToGain (-1.0f) * 1.001f); // the limiter's ceiling
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's effects with their editors' displays listening, what goes in and out of each tapped")
    {
        // Each device's scope (devices/core/DeviceScope.h) taking its sound in and out, and its peaks, as open editors have it.
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);

        std::vector<phazite::DeviceID> listening;

        for (const auto* type :
             { phazite::devices::Equalizer::typeID, phazite::devices::FilterEffect::typeID, phazite::devices::Saturator::typeID,
               phazite::devices::Compressor::typeID, phazite::devices::Multiband::typeID, phazite::devices::Delay::typeID,
               phazite::devices::Reverb::typeID, phazite::devices::Phaser::typeID, phazite::devices::Limiter::typeID })
            listening.push_back (song.addEffect (track, type));

        const auto m = playAndMeasure (song,
                                       [&song, &listening]
                                       {
                                           for (const auto device : listening)
                                               getPlayed (song, device).getScope().setListening (true);
                                       });
        report ("Phazite's effects with their editors' displays listening", m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: comb filters swept on Phazite's synth, chords a sixteenth apart, and on its filter, on a tone")
    {
        using phazite::devices::Synth;
        TestSong song;

        // The synth's Comb +, swept by its filter envelope at each note, in unison.
        const auto notes = song.firstTrack;
        const auto synth = song.setInstrument (notes, Synth::typeID);
        song.setParameter (synth, "filterType", (float) phazite::devices::dsp::FilterMode::combPlus);
        song.setParameter (synth, "resonance", 0.8f);
        song.setParameter (synth, "unisonVoices", 2.0f); // three
        addChords (song, notes);

        // The filter's Comb - on a tone, swept by a lane and ringing, on a track of its own.
        const auto tone = song.addTrack ("Tone");
        addTone (song, tone);
        const auto filter = song.addEffect (tone, phazite::devices::FilterEffect::typeID);
        song.setParameter (filter, "filterType", (float) phazite::devices::dsp::FilterMode::combMinus);
        song.setParameter (filter, "resonance", 0.9f);
        sweepCutoff (song, filter, 100.0f, 4000.0f);

        const auto m = playAndMeasure (song);
        report ("Comb filters swept on Phazite's synth, chords a sixteenth apart, and on its filter, on a tone", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's wavetable synth's comb, swept by an envelope at each note, chords a sixteenth apart")
    {
        using phazite::devices::WavetableSynth;
        TestSong song;
        const auto track = song.firstTrack;
        const auto wavetable = song.setInstrument (track, WavetableSynth::typeID);
        song.setParameter (wavetable, "filterType", (float) phazite::devices::dsp::FilterMode::combPlus);
        song.setParameter (wavetable, "resonance", 0.8f);
        song.setParameter (wavetable, "unisonVoices", 3.0f); // four
        song.setParameter (wavetable, "mod1Amount", 0.5f);   // its first slot: Envelope 2 opens the cutoff, here by four octaves
        addChords (song, track);

        const auto m = playAndMeasure (song);
        report ("Phazite's wavetable synth's comb, swept by an envelope at each note, chords a sixteenth apart", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: the synth's vowel swept by its filter envelope, chords a sixteenth apart, and the filter's phaser")
    {
        using phazite::devices::Synth;
        TestSong song;

        const auto notes = song.firstTrack;
        const auto synth = song.setInstrument (notes, Synth::typeID);
        song.setParameter (synth, "filterType", (float) phazite::devices::dsp::FilterMode::vowel);
        song.setParameter (synth, "resonance", 0.8f);
        addChords (song, notes);

        // The filter's phaser on a tone, swept by a lane, on a track of its own.
        const auto tone = song.addTrack ("Tone");
        addTone (song, tone);
        const auto filter = song.addEffect (tone, phazite::devices::FilterEffect::typeID);
        song.setParameter (filter, "filterType", (float) phazite::devices::dsp::FilterMode::phaser);
        song.setParameter (filter, "resonance", 0.9f);
        sweepCutoff (song, filter, 200.0f, 5000.0f);

        const auto m = playAndMeasure (song);
        report ("The synth's vowel swept by its filter envelope, chords a sixteenth apart, and the filter's phaser", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's wavetable synth's sample and hold, swept by an envelope at each note, chords a sixteenth apart")
    {
        using phazite::devices::WavetableSynth;
        TestSong song;
        const auto track = song.firstTrack;
        const auto wavetable = song.setInstrument (track, WavetableSynth::typeID);
        song.setParameter (wavetable, "filterType", (float) phazite::devices::dsp::FilterMode::sampleAndHold);
        song.setParameter (wavetable, "resonance", 0.8f);
        song.setParameter (wavetable, "unisonVoices", 3.0f); // four
        song.setParameter (wavetable, "mod1Amount", 0.5f);   // its first slot: Envelope 2 opens the cutoff, here by four octaves
        addChords (song, track);

        const auto m = playAndMeasure (song);
        report ("Phazite's wavetable synth's sample and hold, swept by an envelope at each note, chords a sixteenth apart", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: a beat on Phazite's drum sampler, its hats choking each other, a pad played from its editor")
    {
        using phazite::devices::DrumSampler;
        TestSong song;
        const auto track = song.firstTrack;
        const auto sampler = song.setInstrument (track, DrumSampler::typeID);

        // Kick and snare, closed hats on the eighths and an open hat the next one chokes.
        auto& played = getSong (song);
        const auto clip =
            phazite::test::addMidiClip (played, track, { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (8.0) }, "Beat");
        REQUIRE (clip.isValid());

        for (int eighth = 0; eighth < 16; ++eighth)
        {
            const auto at = phazite::BeatPosition::fromBeats (eighth * 0.5);
            const auto note = eighth % 4 == 3 ? 46 : 42;
            phazite::test::addMidiNote (played, clip, note, at, phazite::BeatDuration::fromBeats (0.25), 90);

            if (eighth % 4 == 0)
                phazite::test::addMidiNote (played, clip, eighth % 8 == 0 ? 36 : 38, at, phazite::BeatDuration::fromBeats (0.25), 110);
        }

        const auto m = playAndMeasure (song, [&song, sampler] { // the clap, as a click on its pad plays it
            dynamic_cast<DrumSampler&> (getPlayed (song, sampler).getProcessor()).audition (3, 1.0f);
        });
        report ("A beat on Phazite's drum sampler, its hats choking each other, a pad played from its editor", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's reverb on a return, with an 8-second decay")
    {
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);

        const auto reverbReturn = song.addReturn (track, phazite::devices::Reverb::typeID);
        const auto effects = phazite::getEffects (getSong (song), reverbReturn);
        REQUIRE (effects.size() == 1);
        song.setParameter (effects.front(), "decay", 8.0f);

        const auto m = playAndMeasure (song);
        report ("Phazite's reverb on a return, with an 8-second decay", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    // These read audio files, so they only report; but a frozen track plays a file Phazite made, and its steady playback
    // must not allocate either.

    TEST_CASE ("Real-time audit: a frozen track, its synth's notes played from its frozen file")
    {
        // As Freeze Track leaves a track (roadmap step 2.11): its file plays, and its synth doesn't.
        using phazite::devices::Synth;
        TestSong song;
        const auto track = song.firstTrack;
        song.setInstrument (track, Synth::typeID);
        using phazite::test::at, phazite::test::lasting;
        const auto clip = phazite::test::addMidiClip (getSong (song), track, { at (0.0), at (8.0) }, "Notes");
        REQUIRE (clip.isValid());

        for (int i = 0; i < 16; ++i)
            phazite::test::addMidiNote (getSong (song), clip, 60 + (i % 5), at (i * 0.5), lasting (0.4), 100);

        song.context.getSongSelection().selectOnly (track);
        song.run ("track.freeze");
        REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.renders.finished.isEmpty(); }, 60000));
        REQUIRE (song.renders.finished[0].endsWith (": ok"));
        REQUIRE (phazite::isFrozen (song.context, track));

        const auto m = playAndMeasure (song);
        report ("A frozen track: its synth's notes, played from its frozen file", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: a version's bounce heard in place of the song, both measured as they play")
    {
        // As Hear It plays it (docs/versions.md: Hearing a version): the bounce read from its file after the master, at the
        // song's beat, the song played and measured beside it, the louder turned down.
        TestSong song;
        addTone (song, song.firstTrack);
        const auto version = keepAndBounce (song, "Tone");
        song.setVolume (song.firstTrack, -6.0f);

        const auto m = playAndMeasure (song,
                                       [&song, &version]
                                       {
                                           REQUIRE (phazite::hearVersion (song.context, version));
                                           phazite::stopAndReturn (getSong (song)); // played from its start, as the others
                                       });
        report ("A version's bounce heard in place of the song", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: an audio clip")
    {
        SineFile sine;
        TestSong song;
        addSineClip (song, song.firstTrack, sine);

        const auto m = playAndMeasure (song);
        report ("An audio clip", m);
        CHECK (m.peakLevel > 0.01f);
    }

    TEST_CASE ("Real-time audit: a loop stretched to the song's tempo")
    {
        // As a loop dropped from the browser plays (roadmap step 1.5), stretched to the song's tempo.
        SineFile sine; // 2 seconds: a bar at 120 BPM
        phazite::test::TemporaryFolder temp;
        const auto loop = temp.folder.getChildFile ("Loop 120 BPM.wav");
        REQUIRE (sine.getFile().copyFileTo (loop));

        TestSong song;
        song.setTempo (100.0);
        auto& played = getSong (song);
        const auto clip = phazite::importFile (played, song.firstTrack, loop, phazite::BeatPosition());
        REQUIRE (phazite::isAudioState (phazite::findClipState (played, clip)));
        REQUIRE (phazite::getClipTempo (played, clip) == doctest::Approx (120.0));

        // Phazite renders the stretched loop to a file in the background, and its engine plays that, so the audio thread
        // only reads a file; until it is ready, the clip is silent.
        song.waitForClipRenders();
        REQUIRE (phazite::isRendered (played, clip));
        REQUIRE (phazite::getAudioFileInfo (phazite::getPlayedFile (played, phazite::findClipState (played, clip))).isValid());

        const auto m = playAndMeasure (song);
        report ("A loop at 120 BPM stretched to 100 BPM, from its render", m);
        CHECK (m.peakLevel > 0.01f);
    }

    TEST_CASE ("Real-time audit: an audio clip turned up, pitched and reversed")
    {
        // As the audio clip editor sets them (roadmap step 2.6). Phazite renders the file pitched and backwards, and its
        // engine plays the render, so the audio thread only reads a file.
        SineFile sine;
        TestSong song;
        auto& played = getSong (song);
        const auto clip = phazite::test::addAudioClip (played, song.firstTrack, "Sine", sine.getFile(), 0.0, 2.0);
        REQUIRE (clip.isValid());
        phazite::setClipGain (played, clip, 6.0f);
        phazite::setClipPitch (played, clip, 3.0f);
        phazite::setClipReversed (played, clip, true);
        song.waitForClipRenders();
        REQUIRE (phazite::isRendered (played, clip));
        REQUIRE (phazite::getAudioFileInfo (phazite::getPlayedFile (played, phazite::findClipState (played, clip))).isValid());

        const auto m = playAndMeasure (song);
        report ("An audio clip turned up 6 dB, pitched up 3 semitones and reversed, from its render", m);
        CHECK (m.peakLevel > 0.01f);
    }

    TEST_CASE ("Real-time audit: a loop previewed in time")
    {
        // The browser's preview (src/model/engine/Preview.h), played by Phazite's engine for previews beside the song's: Phazite
        // stretches the loop into memory as it starts, at the output's rate, and the previews' reading thread hands it to
        // the engine as it would a file (internal/PreviewLoop.h): steady playback reads no file. An empty song plays too.
        SineFile sine;
        phazite::test::TemporaryFolder temp;
        const auto loop = temp.folder.getChildFile ("Loop 120 BPM.wav");
        REQUIRE (sine.getFile().copyFileTo (loop));

        TestSong song;
        song.setTempo (100.0);
        const auto previewer = phazite::createPreviewer (phazite::test::getEngineContext());
        auto& previews = phazite::getPlaybackHost (phazite::test::getEngineContext()).getPreviewEngine();

        const auto m =
            playProjectAndMeasure (song.context,
                                   [&]
                                   {
                                       REQUIRE (previewer->play (loop, song.context));
                                       REQUIRE (previewer->isInTime());
                                       REQUIRE (phazite::test::runTimersUntil ([&previews] { return previews.isPlaying(); }, 60000));
                                   });
        previewer->stop();
        report ("A loop at 120 BPM previewed at 100 BPM, stretched by Phazite as it starts", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: two tracks in a folder bus")
    {
        SineFile sine;
        TestSong song;
        const auto first = song.firstTrack;
        const auto second = song.addTrack ("Second");
        addSineClip (song, first, sine);
        addSineClip (song, second, sine);

        const auto bus = song.addBus ("Bus", { first, second });
        song.addEffect (bus, phazite::devices::Compressor::typeID);

        const auto m = playAndMeasure (song);
        report ("Two tracks in a folder bus with a compressor", m);
        CHECK (m.peakLevel > 0.01f);
    }
}
