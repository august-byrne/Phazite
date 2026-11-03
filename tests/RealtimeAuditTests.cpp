// SPDX-License-Identifier: AGPL-3.0-or-later
// The real-time audit (roadmap step 0.3). Each scenario builds a small project and plays it through
// Tracktion's hosted audio device (the way the engine runs inside a plug-in) on a separate thread that
// acts as the audio thread. In Windows Debug builds it counts the heap allocations and frees made on
// that thread and shows where they came from (AudioThreadMonitor). Under the linux-asan and linux-tsan
// presets the same playback runs under AddressSanitizer and ThreadSanitizer.
//
// Tracktion's own code breaks Phazite's real-time rules in places, so these tests report what they
// find rather than fail on it. To see the report:
//   build\windows-debug\phazite_tests_artefacts\Debug\phazite_tests.exe --test-suite=realtime-audit
// docs/realtime-audit.md lists the findings and what Phazite does about each.

#include "AudioThreadMonitor.h"
#include "SandboxedPlugins.h"
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
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/devices/internal/DrumSamplerPlugin.h"
#include "model/devices/internal/Wavetables.h"
#include "model/engine/Preview.h"
#include "model/engine/internal/LiveInput.h"
#include "model/engine/internal/LoudnessMeterPlugin.h"
#include "model/engine/internal/Preview.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/internal/AudioRecorder.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"

#include <chrono>
#include <thread>

using namespace tracktion::literals;
using phazite::test::AudioThreadMonitor;

namespace
{
constexpr double auditSampleRate = 48000.0;
constexpr int auditBlockSize = 256;
constexpr double warmUpSeconds = 0.25;   // the first blocks after starting playback, at least,
constexpr double quietSeconds = 0.25;    // and on until the audio thread has gone this long without allocating or freeing,
constexpr double maxWarmUpSeconds = 2.0; // but no longer
constexpr double steadySeconds = 1.0;    // then steady playback

// Runs the warm-up through processFor (seconds) and returns how long it took. Tracktion swaps MIDI buffers between its
// graph's nodes and pools them, so a buffer can first be used, and allocate, some way into playback: steady playback
// starts once each has been used, which a quiet stretch shows. Something that keeps allocating still shows in the
// steady second after the longest warm-up.
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

te::AudioTrack& getTrack (te::Edit& edit, int index)
{
    phazite::ensureTracks (edit, index + 1);
    return *te::getAudioTracks (edit)[index];
}

// Adds a device to a track by its type, after its other devices and before its volume and level meter.
te::Plugin& addPlugin (te::Track& track, const juce::String& type)
{
    auto plugin = track.edit.getPluginCache().createNewPlugin (type, {});
    phazite::insertDevice (track, plugin);
    return *plugin;
}

void addSineClip (te::AudioTrack& track, const SineFile& sine)
{
    phazite::test::addAudioClip (track, "Sine", sine.getFile(), { 0_tp, 2_tp });
}

// A tone on a track for longer than a scenario plays: an audio clip of a sine wave, from the song's start.
void addTone (te::AudioTrack& track)
{
    static const SineFile sine (8.0);
    phazite::test::addAudioClip (track, "Tone", sine.getFile(), { 0_tp, 8_tp });
}

// A device as Phazite's engine plays it, while it plays the device's project: what its editor's displays read and its
// pads play (model/devices/DeviceHandle.h).
phazite::engine::DeviceRunner& getPlayed (te::Plugin& device)
{
    auto* session = phazite::findPlayingSession (device.edit);
    REQUIRE (session != nullptr);
    auto* node = session->getPlayer().findDevice ((std::int64_t) device.itemID.getRawID());
    REQUIRE (node != nullptr);
    return node->getRunner();
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

// Plays an Edit that is no project's (a preview's) from the start through Tracktion's engine, in real time on a separate
// "audio" thread, through Tracktion's hosted audio device, and measures that thread.
Measurement playTracktionAndMeasure (te::Edit& edit)
{
    auto& deviceManager = edit.engine.getDeviceManager();
    auto& audioIO = deviceManager.getHostedAudioDeviceInterface();

    te::HostedAudioDeviceInterface::Parameters params;
    params.sampleRate = auditSampleRate;
    params.blockSize = auditBlockSize;
    params.inputChannels = 0;
    audioIO.initialise (params);
    audioIO.prepareToPlay (auditSampleRate, auditBlockSize);
    deviceManager.dispatchPendingUpdates();

    auto& transport = edit.getTransport();
    transport.ensureContextAllocated();
    transport.setPosition (0_tp);
    transport.play (false);

    Measurement m;

    // Allocated before the thread is marked as an audio thread, so they aren't counted.
    juce::AudioBuffer<float> buffer (std::max (params.inputChannels, params.outputChannels), auditBlockSize);
    juce::MidiBuffer midi;
    midi.ensureSize (4096);

    const auto blockDuration =
        std::chrono::duration_cast<std::chrono::steady_clock::duration> (std::chrono::duration<double> (auditBlockSize / auditSampleRate));

    AudioThreadMonitor::reset();

    std::thread audioThread (
        [&]
        {
            const AudioThreadMonitor::ScopedAudioThread marker;

            // In real time, so that Tracktion's background threads (file reading,
            // for example) keep up as they would with a real audio device.
            const auto processFor = [&] (double seconds)
            {
                const auto numBlocks = juce::roundToInt (seconds * auditSampleRate / auditBlockSize);

                for (int i = 0; i < numBlocks; ++i)
                {
                    const auto blockEnd = std::chrono::steady_clock::now() + blockDuration;
                    buffer.clear();
                    midi.clear();
                    audioIO.processBlock (buffer, midi);
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

    transport.stop (false, false);
    m.callSites = AudioThreadMonitor::describeCallSites();
    m.steadyCallSites = AudioThreadMonitor::describeCallSites (6, m.firstSteadyEvent);

    // Remove the hosted device again, as Tracktion's own tests do, so later tests start without one.
    deviceManager.closeDevices();
    deviceManager.removeHostedAudioDeviceInterface();
    deviceManager.deviceManager.closeAudioDevice();

    return m;
}

// Plays the open project from its start through Phazite's engine as the app plays it, in real time on a separate "audio"
// thread that drives Tracktion's hosted audio device, whose callbacks are Tracktion's, which plays nothing of the project,
// and Phazite's (model/playback/internal/PlaybackHost.h), and measures that thread; a tone comes into the device's two
// inputs. Recording, it records a take instead, while another thread plays a note every twentieth of a second into a
// track live, as a MIDI driver would; stopping keeps the take.
Measurement playProjectAndMeasure (phazite::ProjectContext& context, const std::function<void()>& before = {}, bool recording = false,
                                   phazite::TrackID liveTrack = {})
{
    auto& edit = *phazite::getEdit (context);
    auto& deviceManager = edit.engine.getDeviceManager();
    auto& audioIO = deviceManager.getHostedAudioDeviceInterface();

    te::HostedAudioDeviceInterface::Parameters params;
    params.sampleRate = auditSampleRate;
    params.blockSize = auditBlockSize;
    params.inputChannels = 2;
    audioIO.initialise (params);
    audioIO.prepareToPlay (auditSampleRate, auditBlockSize);
    deviceManager.dispatchPendingUpdates();
    phazite::test::dispatchPendingMessages(); // the song for the device's rate
    phazite::movePlayhead (edit, 0_tp);

    if (before)
        before();

    if (recording)
        phazite::startRecording (edit);
    else
        phazite::playOrStop (edit);

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

    // On one thread, so that the monitor counts every allocation (the workers play the same strips' code, T2 step 8).
    const auto threads = engine.getNumThreads();
    engine.setNumThreads (1);

    Measurement m;
    juce::AudioBuffer<float> buffer (2, auditBlockSize); // allocated before the thread is marked as an audio thread
    juce::MidiBuffer midi;
    midi.ensureSize (4096);
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

                    midi.clear();
                    audioIO.processBlock (buffer, midi);
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

    phazite::stopAndReturn (edit);
    m.callSites = AudioThreadMonitor::describeCallSites();
    m.steadyCallSites = AudioThreadMonitor::describeCallSites (6, m.firstSteadyEvent);

    // Remove the hosted device again, as Tracktion's own tests do, so later tests start without one.
    deviceManager.closeDevices();
    deviceManager.removeHostedAudioDeviceInterface();
    deviceManager.deviceManager.closeAudioDevice();
    return m;
}

// Plays an Edit's song as a project plays it (playProjectAndMeasure()): opened in a project context of its own, with
// what is to be done once Phazite's engine has it, before it plays, and after.
Measurement playAndMeasure (te::Edit& edit, const std::function<void()>& before = {}, const std::function<void()>& after = {})
{
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setEdit (context, &edit);
    auto m = playProjectAndMeasure (context, before);

    if (after)
        after();

    setEdit (context, nullptr);
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

// Sets one of the parameters of one of Phazite's devices.
void setDeviceValue (const te::Plugin::Ptr& plugin, const char* id, float value)
{
    auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
    REQUIRE (device != nullptr);
    phazite::setDeviceValue (*device->findParameter (id), value);
}

// Chords a sixteenth long, one after another for two bars, so that the busiest blocks (one chord's ends with the next's
// starts) come from the first beat, within the warm-up: Tracktion's MIDI arrays grow on the busiest block (finding 4).
// One track of them at most: with more, the arrays the graph pools for them can carry MIDI for the first time later on.
void addChords (te::AudioTrack& track)
{
    auto* clip = phazite::test::addMidiClip (track, { 0_bp, 8_bp }, "Chords");
    REQUIRE (clip != nullptr);

    for (int i = 0; i < 32; ++i)
        for (const auto note : { 48, 52, 55, 59 })
            phazite::test::addMidiNote (*clip, note + (i % 2) * 5, te::BeatPosition::fromBeats (i * 0.25),
                                        te::BeatDuration::fromBeats (0.25), 100);
}
} // namespace

//==============================================================================
TEST_SUITE ("realtime-audit")
{
    // These read no files, so their steady playback is deterministic: it must not allocate.

    TEST_CASE ("Real-time audit: one empty track")
    {
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        const auto m = playAndMeasure (*edit);
        report ("One empty track", m);
        CHECK (m.peakLevel == 0.0f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: the metronome, clicking louder on each bar's first beat")
    {
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        phazite::setMetronomeOn (*edit, true);
        const auto m = playAndMeasure (*edit);
        report ("The metronome", m);
        CHECK (m.peakLevel > 0.01f); // it clicks
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's own engine, its transport and its metronome")
    {
        phazite::test::TestSong song;
        song.addMidiClip (song.firstTrack, 0.0, 16.0);
        song.setSwitch ("transport.metronome", true);
        const auto m = playProjectAndMeasure (song.context);
        report ("Phazite's engine: the metronome", m);
        CHECK (m.peakLevel > 0.01f); // it clicks
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's own engine, chords on Phazite's synth through a track's EQ and fader into the master")
    {
        phazite::test::TestSong song;
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
        phazite::test::TestSong song;
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
        phazite::test::TestSong song;
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
        phazite::test::TestSong song;
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
        phazite::test::TestSong song;
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
        auto& engine = phazite::test::getEngine();

        if (phazite::getAllMidiInputs (engine) == nullptr)
        {
            engine.getDeviceManager().rescanMidiDeviceList();
            phazite::test::runTimersUntil ([&engine] { return phazite::getAllMidiInputs (engine) != nullptr; });
        }

        phazite::test::TestSong song;
        song.setSwitch ("transport.countIn", false);
        song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
        phazite::routeLiveMidiInput (*phazite::getEdit (song.context)); // live MIDI plays the synth's track, which the take records
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
        phazite::test::TestSong song;
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
        phazite::test::TestSong song;

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
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        auto* volume = track.getVolumePlugin();
        REQUIRE (volume != nullptr);

        for (const auto& [beat, value] : { std::pair (0.0, 0.5f), std::pair (2.0, 0.8f), std::pair (4.0, 0.5f) })
            phazite::addPoint (*volume->volParam, { phazite::BeatPosition::fromBeats (beat), value, 0.0f });

        const auto lfo = phazite::addModulator (track, phazite::ModulatorType::lfo);
        REQUIRE (lfo.has_value());
        phazite::setModulation (*lfo, *volume->panParam, 0.5f);

        const auto m = playAndMeasure (*edit);
        report ("Volume automation and an LFO on pan", m);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: a mix with a bus, a reverb return and the loudness meter")
    {
        // Built with the functions the mixer uses (roadmap step 1.4), with a tone in place of a recording.
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& tone = getTrack (*edit, 0);
        addTone (tone);

        auto* folder = phazite::insertFolder (*edit, &tone, nullptr); // a bus, as every new folder is
        REQUIRE (folder != nullptr);
        phazite::moveTrackIntoFolder (tone, *folder);
        REQUIRE (phazite::isBus (*folder));

        auto* reverbReturn = phazite::addReturnTrack (*edit, "Reverb", nullptr);
        REQUIRE (reverbReturn != nullptr);
        REQUIRE (phazite::insertDevice (*reverbReturn, edit->getPluginCache().createNewPlugin (phazite::devices::Reverb::typeID, {}))
                 != nullptr);
        REQUIRE (phazite::addSend (tone, phazite::getReturnBus (*reverbReturn), false) != nullptr);

        const auto m = playAndMeasure (*edit);
        report ("A tone through a bus, with a send to a reverb return, into the loudness meter", m);
        CHECK (m.peakLevel > 0.01f);
        const auto readings = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine().getLoudnessMeter().getReadings();
        CHECK (readings.momentary.value_or (-100.0f) > -40.0f); // it measured what played
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's EQ, every band on and one automated, with its analyzer listening")
    {
        using phazite::devices::Equalizer;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        addTone (track);

        const auto plugin = edit->getPluginCache().createNewPlugin (Equalizer::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::insertDevice (track, plugin) != nullptr);

        for (int band = 0; band < Equalizer::numBands; ++band)
        {
            phazite::setDeviceValue (*device->getAutomatableParameters()[Equalizer::getParameterIndex (band, Equalizer::on)], 1.0f);
            phazite::setDeviceValue (*device->getAutomatableParameters()[Equalizer::getParameterIndex (band, Equalizer::gain)], 3.0f);
        }

        auto* gain = device->findParameter ("band3Gain");
        phazite::addPoint (*gain, { phazite::BeatPosition(), -12.0f, 0.0f });
        phazite::addPoint (*gain, { phazite::BeatPosition::fromBeats (2.0), 12.0f, 0.0f });

        const auto tap = [&plugin]() -> auto& { return dynamic_cast<Equalizer&> (getPlayed (*plugin).getProcessor()).getOutputTap(); };
        std::vector<float> tapped (32768);

        const auto m = playAndMeasure (
            *edit, [&tap] { tap().setListening (true); }, [&tap, &tapped] { CHECK (tap().pull (tapped.data(), (int) tapped.size()) > 0); });
        report ("Phazite's EQ, every band on and one automated, with its analyzer listening", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: every kind of modulator, and a macro, moving Phazite's EQ")
    {
        using phazite::devices::Equalizer;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        addTone (track);

        const auto plugin = edit->getPluginCache().createNewPlugin (Equalizer::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::insertDevice (track, plugin) != nullptr);

        // Each moves a band's gain, as Phazite sets them up (model/modulation/internal/Modulation.h); the note follower
        // follows a note played into the track's MIDI.
        int band = 0;

        for (const auto type : phazite::allModulatorTypes)
        {
            phazite::setDeviceValue (*device->getAutomatableParameters()[Equalizer::getParameterIndex (band, Equalizer::on)], 1.0f);
            const auto modulator = phazite::addModulator (track, type);
            REQUIRE (modulator.has_value());
            phazite::setModulation (*modulator, *device->getAutomatableParameters()[Equalizer::getParameterIndex (band, Equalizer::gain)],
                                    0.5f);
            ++band;
        }

        // The EQ moves its bands itself, in every stretch of a block (model/devices/internal/DeviceModulation.h).
        phazite::updateDeviceModulation (*device);
        REQUIRE (device->getModulation() != nullptr);
        CHECK (device->getModulation()->plan.routes.size() == std::size (phazite::allModulatorTypes));

        const auto m = playAndMeasure (*edit);
        report ("Every kind of modulator, and a macro, moving Phazite's EQ", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: lanes sweeping Phazite's EQ, a cycle a beat, and fading a track's volume")
    {
        using phazite::devices::Equalizer;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        addTone (track);

        const auto plugin = edit->getPluginCache().createNewPlugin (Equalizer::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::insertDevice (track, plugin) != nullptr);
        phazite::setDeviceValue (*device->getAutomatableParameters()[Equalizer::getParameterIndex (0, Equalizer::on)], 1.0f);

        // Over all of the playback measured: the band's gain swept, a cycle a beat, and the volume faded.
        const auto song = phazite::BeatRange (phazite::BeatPosition(), phazite::BeatPosition::fromBeats (32.0));
        const auto gain = device->findParameter ("band1Gain");
        REQUIRE (gain != nullptr);
        phazite::replacePoints (
            *gain, song,
            phazite::makeAutomationShape (phazite::AutomationShape::sine, song, phazite::BeatDuration::fromBeats (1.0), -12.0f, 12.0f));
        phazite::replacePoints (*track.getVolumePlugin()->volParam, song, { { song.getStart(), 0.8f }, { song.getEnd(), 0.4f } });

        const auto m = playAndMeasure (*edit);
        report ("Lanes sweeping Phazite's EQ, a cycle a beat, and fading a track's volume", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: a container of Phazite's EQ and compressor at half mix, a macro and the track's LFO turning them")
    {
        using phazite::devices::Equalizer;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        addTone (track);

        const auto eq = edit->getPluginCache().createNewPlugin (Equalizer::typeID, {});
        const auto compressor = edit->getPluginCache().createNewPlugin (phazite::devices::Compressor::typeID, {});
        REQUIRE (phazite::insertDevice (track, eq) != nullptr);
        REQUIRE (phazite::insertDevice (track, compressor) != nullptr);

        // Grouped as Phazite groups them (model/mixer/internal/Containers.h).
        auto* container = phazite::groupIntoContainer ({ eq.get(), compressor.get() });
        REQUIRE (container != nullptr);
        phazite::setMix (*container, 0.5f);

        // Its first macro turns a band of the EQ; the track's LFO moves the compressor's makeup gain.
        phazite::setDeviceValue (*eq->getAutomatableParameters()[Equalizer::getParameterIndex (0, Equalizer::on)], 1.0f);
        const auto macro = phazite::getContainerMacros (*container)[0];
        phazite::setModulation ({ nullptr, macro }, *eq->getAutomatableParameters()[Equalizer::getParameterIndex (0, Equalizer::gain)],
                                0.5f);
        phazite::setDeviceValue (*macro, 0.7f);
        const auto lfo = phazite::addModulator (track, phazite::ModulatorType::lfo);
        REQUIRE (lfo.has_value());
        phazite::setModulation (*lfo, *compressor->getAutomatableParameterByID ("makeup"), 0.5f);

        const auto m = playAndMeasure (*edit);
        report ("A container of Phazite's EQ and compressor at half mix, a macro and the track's LFO turning them", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's compressor, ducked by another track at 70 % mix, with its meters read")
    {
        using phazite::devices::Compressor;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        auto& source = getTrack (*edit, 1);
        addTone (track);
        addTone (source);
        phazite::setTrackMuted (source, true); // still heard by the sidechain

        const auto plugin = edit->getPluginCache().createNewPlugin (Compressor::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::insertDevice (track, plugin) != nullptr);
        // Its curve turning down from -40 dB, so that the tone ducks it.
        phazite::setDeviceValue (*device->findParameter ("point2X"), phazite::devices::curves::axisForDecibels (-40.0f));
        phazite::setDeviceValue (*device->findParameter ("point2Y"), phazite::devices::curves::axisForDecibels (-40.0f));
        phazite::setDeviceValue (*device->findParameter ("mix"), 0.7f);
        phazite::setSidechainSource (*device, &source);
        REQUIRE (phazite::getSidechainSource (*device) == &source);

        const auto meter = [&plugin]() -> auto&
        { return dynamic_cast<Compressor&> (getPlayed (*plugin).getProcessor()).getReductionMeter(); };
        const auto m = playAndMeasure (*edit, [&meter] { meter().take(); }, [&meter] { CHECK (meter().take() > 1.0f); });
        report ("Phazite's compressor, ducked by another track at 70 % mix, with its meters read", m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's vocoder at 40 bands, its carrier another track through the sidechain")
    {
        using phazite::devices::Vocoder;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& voice = getTrack (*edit, 0);
        auto& carrier = getTrack (*edit, 1);
        addTone (voice);
        addTone (carrier);
        phazite::setTrackMuted (carrier, true); // still heard by the sidechain

        const auto plugin = edit->getPluginCache().createNewPlugin (Vocoder::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::insertDevice (voice, plugin) != nullptr);
        phazite::setDeviceValue (*device->findParameter ("carrier"), (float) Vocoder::Carrier::track);
        phazite::setDeviceValue (*device->findParameter ("bands"), 5.0f);
        phazite::setSidechainSource (*device, &carrier);
        REQUIRE (phazite::getSidechainSource (*device) == &carrier);

        const auto m = playAndMeasure (*edit);
        report ("Phazite's vocoder at 40 bands, its carrier another track through the sidechain", m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's limiter on the master, pushed 12 dB, with its meters read")
    {
        using phazite::devices::Limiter;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        addTone (getTrack (*edit, 0));

        const auto plugin = edit->getPluginCache().createNewPlugin (Limiter::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::insertMasterDevice (*edit, plugin) != nullptr);
        phazite::setDeviceValue (*device->findParameter ("gain"), 12.0f);

        const auto meter = [&plugin]() -> auto& { return dynamic_cast<Limiter&> (getPlayed (*plugin).getProcessor()).getReductionMeter(); };
        const auto m = playAndMeasure (*edit, [&meter] { meter().take(); }, [&meter] { CHECK (meter().take() > 1.0f); });
        report ("Phazite's limiter on the master, pushed 12 dB, with its meters read", m);
        CHECK (m.peakLevel > 0.01f);
        CHECK (m.peakLevel < juce::Decibels::decibelsToGain (-1.0f) * 1.001f); // its ceiling
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: MIDI into Phazite's synth, notes overlapping, with noise and its LFO")
    {
        using phazite::devices::Synth;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        const auto plugin = edit->getPluginCache().createNewPlugin (Synth::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::setInstrument (track, plugin) != nullptr);
        phazite::setDeviceValue (*device->findParameter ("noiseLevel"), 0.2f);
        phazite::setDeviceValue (*device->findParameter ("lfoDepth"), 0.5f);
        phazite::setDeviceValue (*device->findParameter ("release"), 400.0f);

        // More notes than voices, so some are stolen.
        auto* clip = phazite::test::addMidiClip (track, { 0_bp, 8_bp }, "Notes");
        REQUIRE (clip != nullptr);

        for (int i = 0; i < 32; ++i)
            phazite::test::addMidiNote (*clip, 36 + i, te::BeatPosition::fromBeats (i * 0.125), te::BeatDuration::fromBeats (1.0), 100);

        const auto m = playAndMeasure (*edit);
        report ("MIDI into Phazite's synth, notes overlapping, with noise and its LFO", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's synth with seven unison voices, spread wide")
    {
        using phazite::devices::Synth;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        const auto plugin = edit->getPluginCache().createNewPlugin (Synth::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::setInstrument (track, plugin) != nullptr);
        phazite::setDeviceValue (*device->findParameter ("unisonVoices"), 6.0f); // seven
        phazite::setDeviceValue (*device->findParameter ("unisonDetune"), 40.0f);

        // Chords a sixteenth long, one after another, so that the busiest blocks (one chord's ends with the next's starts)
        // come from the first beat, within the warm-up: Tracktion's MIDI arrays grow on the busiest block (finding 4).
        auto* clip = phazite::test::addMidiClip (track, { 0_bp, 8_bp }, "Chords");
        REQUIRE (clip != nullptr);

        for (int i = 0; i < 32; ++i)
            for (const auto note : { 48, 52, 55, 59 })
                phazite::test::addMidiNote (*clip, note + (i % 2) * 5, te::BeatPosition::fromBeats (i * 0.25),
                                            te::BeatDuration::fromBeats (0.25), 100);

        const auto m = playAndMeasure (*edit);
        report ("Phazite's synth with seven unison voices, spread wide", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's wavetable synth, eight unison voices and warps, an envelope sweeping each note's table")
    {
        using phazite::devices::WavetableSynth;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        const auto plugin = edit->getPluginCache().createNewPlugin (WavetableSynth::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::setInstrument (track, plugin) != nullptr);
        phazite::setFactoryWavetable (*plugin, 1, phazite::devices::findFactoryWavetable ("Vowels"));

        const auto set = [device] (const char* id, float value) { phazite::setDeviceValue (*device->findParameter (id), value); };
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
        auto* clip = phazite::test::addMidiClip (track, { 0_bp, 8_bp }, "Chords");
        REQUIRE (clip != nullptr);

        for (int i = 0; i < 32; ++i)
            for (const auto note : { 48, 52, 55, 59 })
                phazite::test::addMidiNote (*clip, note + (i % 2) * 5, te::BeatPosition::fromBeats (i * 0.25),
                                            te::BeatDuration::fromBeats (0.25), 100);

        const auto m = playAndMeasure (*edit);
        report ("Phazite's wavetable synth, eight unison voices and warps, an envelope sweeping each note's table", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's filter, saturator, multiband, delay, phaser, flanger, stereo expander and limiter in a row, "
               "the filter swept")
    {
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        addTone (track);

        phazite::DevicePlugin* filter = nullptr;

        for (const auto* type :
             { phazite::devices::FilterEffect::typeID, phazite::devices::Saturator::typeID, phazite::devices::Multiband::typeID,
               phazite::devices::Delay::typeID, phazite::devices::Phaser::typeID, phazite::devices::Flanger::typeID,
               phazite::devices::StereoExpander::typeID, phazite::devices::Limiter::typeID })
        {
            const auto plugin = edit->getPluginCache().createNewPlugin (type, {});
            auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
            REQUIRE (device != nullptr);
            REQUIRE (phazite::insertDevice (track, plugin) != nullptr);

            if (filter == nullptr)
                filter = device;
        }

        // The filter swept by a lane, a cycle a beat, so its cutoff glides all the time; the others well into their work.
        auto* cutoff = filter->findParameter ("cutoff");
        REQUIRE (cutoff != nullptr);

        for (int beat = 0; beat < 8; ++beat)
        {
            phazite::addPoint (*cutoff, { phazite::BeatPosition::fromBeats (beat), 200.0f, 0.0f });
            phazite::addPoint (*cutoff, { phazite::BeatPosition::fromBeats (beat + 0.5), 8000.0f, 0.0f });
        }

        const auto m = playAndMeasure (*edit);
        report ("Phazite's filter, saturator, multiband, delay, phaser, flanger, stereo expander and limiter in a row, the filter swept",
                m);
        CHECK (m.peakLevel > 0.001f);
        CHECK (m.peakLevel < juce::Decibels::decibelsToGain (-1.0f) * 1.001f); // the limiter's ceiling
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's effects with their editors' displays listening, what goes in and out of each tapped")
    {
        // Each device's scope (devices/core/DeviceScope.h) taking its sound in and out, and its peaks, as open editors have it.
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        addTone (track);

        std::vector<te::Plugin::Ptr> listening;

        for (const auto* type :
             { phazite::devices::Equalizer::typeID, phazite::devices::FilterEffect::typeID, phazite::devices::Saturator::typeID,
               phazite::devices::Compressor::typeID, phazite::devices::Multiband::typeID, phazite::devices::Delay::typeID,
               phazite::devices::Reverb::typeID, phazite::devices::Phaser::typeID, phazite::devices::Limiter::typeID })
        {
            const auto plugin = edit->getPluginCache().createNewPlugin (type, {});
            REQUIRE (phazite::insertDevice (track, plugin) != nullptr);
            listening.push_back (plugin);
        }

        const auto m = playAndMeasure (*edit,
                                       [&listening]
                                       {
                                           for (const auto& plugin : listening)
                                               getPlayed (*plugin).getScope().setListening (true);
                                       });
        report ("Phazite's effects with their editors' displays listening", m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: comb filters swept on Phazite's synth, chords a sixteenth apart, and on its filter, on a tone")
    {
        using phazite::devices::Synth;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        phazite::ensureTracks (*edit, 2);

        // The synth's Comb +, swept by its filter envelope at each note, in unison.
        auto& notes = getTrack (*edit, 0);
        const auto synth = edit->getPluginCache().createNewPlugin (Synth::typeID, {});
        REQUIRE (phazite::setInstrument (notes, synth) != nullptr);
        setDeviceValue (synth, "filterType", (float) phazite::devices::dsp::FilterMode::combPlus);
        setDeviceValue (synth, "resonance", 0.8f);
        setDeviceValue (synth, "unisonVoices", 2.0f); // three
        addChords (notes);

        // The filter's Comb - on a tone, swept by a lane and ringing. On a track of its own: after an instrument, a lane
        // would make Tracktion pass the notes on in pieces of blocks, moving their arrays around more (finding 4).
        auto& tone = getTrack (*edit, 1);
        addTone (tone);
        const auto filter = edit->getPluginCache().createNewPlugin (phazite::devices::FilterEffect::typeID, {});
        REQUIRE (phazite::insertDevice (tone, filter) != nullptr);
        setDeviceValue (filter, "filterType", (float) phazite::devices::dsp::FilterMode::combMinus);
        setDeviceValue (filter, "resonance", 0.9f);
        auto* cutoff = dynamic_cast<phazite::DevicePlugin*> (filter.get())->findParameter ("cutoff");

        for (int beat = 0; beat < 8; ++beat)
        {
            phazite::addPoint (*cutoff, { phazite::BeatPosition::fromBeats (beat), 100.0f, 0.0f });
            phazite::addPoint (*cutoff, { phazite::BeatPosition::fromBeats (beat + 0.5), 4000.0f, 0.0f });
        }

        const auto m = playAndMeasure (*edit);
        report ("Comb filters swept on Phazite's synth, chords a sixteenth apart, and on its filter, on a tone", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's wavetable synth's comb, swept by an envelope at each note, chords a sixteenth apart")
    {
        using phazite::devices::WavetableSynth;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        const auto wavetable = edit->getPluginCache().createNewPlugin (WavetableSynth::typeID, {});
        REQUIRE (phazite::setInstrument (track, wavetable) != nullptr);
        setDeviceValue (wavetable, "filterType", (float) phazite::devices::dsp::FilterMode::combPlus);
        setDeviceValue (wavetable, "resonance", 0.8f);
        setDeviceValue (wavetable, "unisonVoices", 3.0f); // four
        setDeviceValue (wavetable, "mod1Amount", 0.5f);   // its first slot: Envelope 2 opens the cutoff, here by four octaves
        addChords (track);

        const auto m = playAndMeasure (*edit);
        report ("Phazite's wavetable synth's comb, swept by an envelope at each note, chords a sixteenth apart", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: the synth's vowel swept by its filter envelope, chords a sixteenth apart, and the filter's phaser")
    {
        using phazite::devices::Synth;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        phazite::ensureTracks (*edit, 2);

        auto& notes = getTrack (*edit, 0);
        const auto synth = edit->getPluginCache().createNewPlugin (Synth::typeID, {});
        REQUIRE (phazite::setInstrument (notes, synth) != nullptr);
        setDeviceValue (synth, "filterType", (float) phazite::devices::dsp::FilterMode::vowel);
        setDeviceValue (synth, "resonance", 0.8f);
        addChords (notes);

        // The filter's phaser on a tone, swept by a lane, on a track of its own (finding 4).
        auto& tone = getTrack (*edit, 1);
        addTone (tone);
        const auto filter = edit->getPluginCache().createNewPlugin (phazite::devices::FilterEffect::typeID, {});
        REQUIRE (phazite::insertDevice (tone, filter) != nullptr);
        setDeviceValue (filter, "filterType", (float) phazite::devices::dsp::FilterMode::phaser);
        setDeviceValue (filter, "resonance", 0.9f);
        auto* cutoff = dynamic_cast<phazite::DevicePlugin*> (filter.get())->findParameter ("cutoff");

        for (int beat = 0; beat < 8; ++beat)
        {
            phazite::addPoint (*cutoff, { phazite::BeatPosition::fromBeats (beat), 200.0f, 0.0f });
            phazite::addPoint (*cutoff, { phazite::BeatPosition::fromBeats (beat + 0.5), 5000.0f, 0.0f });
        }

        const auto m = playAndMeasure (*edit);
        report ("The synth's vowel swept by its filter envelope, chords a sixteenth apart, and the filter's phaser", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's wavetable synth's sample and hold, swept by an envelope at each note, chords a sixteenth apart")
    {
        using phazite::devices::WavetableSynth;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        const auto wavetable = edit->getPluginCache().createNewPlugin (WavetableSynth::typeID, {});
        REQUIRE (phazite::setInstrument (track, wavetable) != nullptr);
        setDeviceValue (wavetable, "filterType", (float) phazite::devices::dsp::FilterMode::sampleAndHold);
        setDeviceValue (wavetable, "resonance", 0.8f);
        setDeviceValue (wavetable, "unisonVoices", 3.0f); // four
        setDeviceValue (wavetable, "mod1Amount", 0.5f);   // its first slot: Envelope 2 opens the cutoff, here by four octaves
        addChords (track);

        const auto m = playAndMeasure (*edit);
        report ("Phazite's wavetable synth's sample and hold, swept by an envelope at each note, chords a sixteenth apart", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: a beat on Phazite's drum sampler, its hats choking each other, a pad played from its editor")
    {
        using phazite::devices::DrumSampler;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        const auto plugin = edit->getPluginCache().createNewPlugin (DrumSampler::typeID, {});
        auto* device = dynamic_cast<phazite::DrumSamplerPlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::setInstrument (track, plugin) != nullptr);

        // Kick and snare, closed hats on the eighths and an open hat the next one chokes.
        auto* clip = phazite::test::addMidiClip (track, { 0_bp, 8_bp }, "Beat");
        REQUIRE (clip != nullptr);

        for (int eighth = 0; eighth < 16; ++eighth)
        {
            const auto at = te::BeatPosition::fromBeats (eighth * 0.5);
            const auto note = eighth % 4 == 3 ? 46 : 42;
            phazite::test::addMidiNote (*clip, note, at, te::BeatDuration::fromBeats (0.25), 90);

            if (eighth % 4 == 0)
                phazite::test::addMidiNote (*clip, eighth % 8 == 0 ? 36 : 38, at, te::BeatDuration::fromBeats (0.25), 110);
        }

        const auto m = playAndMeasure (*edit, [&plugin] { // the clap, as a click on its pad plays it
            dynamic_cast<DrumSampler&> (getPlayed (*plugin).getProcessor()).audition (3, 1.0f);
        });
        report ("A beat on Phazite's drum sampler, its hats choking each other, a pad played from its editor", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's reverb on a return, with an 8-second decay")
    {
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        addTone (track);

        const auto plugin = edit->getPluginCache().createNewPlugin (phazite::devices::Reverb::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::addReturnWithEffect (track, plugin) != nullptr);
        phazite::setDeviceValue (*device->findParameter ("decay"), 8.0f);

        const auto m = playAndMeasure (*edit);
        report ("Phazite's reverb on a return, with an 8-second decay", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    // These read audio files, so they only report; but a frozen track plays a file Phazite made, and its steady playback
    // must not allocate either.

    TEST_CASE ("Real-time audit: a frozen track, its synth's notes played from its frozen file")
    {
        // As Freeze Track leaves a track (roadmap step 2.11): its file plays, and its synth doesn't. Tracktion's own freeze,
        // which renders on this thread, leaves it playing the same way.
        using phazite::devices::Synth;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        REQUIRE (phazite::setInstrument (track, edit->getPluginCache().createNewPlugin (Synth::typeID, {})) != nullptr);
        auto* clip = phazite::test::addMidiClip (track, { 0_bp, 8_bp }, "Notes");
        REQUIRE (clip != nullptr);

        for (int i = 0; i < 16; ++i)
            phazite::test::addMidiNote (*clip, 60 + (i % 5), te::BeatPosition::fromBeats (i * 0.5), te::BeatDuration::fromBeats (0.4), 100);

        REQUIRE (track.setFrozen (true, te::Track::individualFreeze).wasOk());
        REQUIRE (track.isFrozen (te::Track::individualFreeze));

        const auto m = playAndMeasure (*edit);
        report ("A frozen track: its synth's notes, played from its frozen file", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: an audio clip")
    {
        SineFile sine;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        addSineClip (getTrack (*edit, 0), sine);

        const auto m = playAndMeasure (*edit);
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

        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        phazite::setTempo (*edit, 100.0);
        auto* clip = dynamic_cast<te::WaveAudioClip*> (phazite::importFile (getTrack (*edit, 0), loop, te::BeatPosition()));
        REQUIRE (clip != nullptr);
        REQUIRE (phazite::getClipTempo (*clip) == doctest::Approx (120.0));

        // Phazite renders the stretched loop to a file (in the background, for an open project), and Tracktion plays that,
        // so the audio thread only reads a file; until it is ready, the clip is silent.
        REQUIRE (phazite::renderMissingClipAudio (*edit));
        REQUIRE (clip->getAudioFile().isValid());

        const auto m = playAndMeasure (*edit);
        report ("A loop at 120 BPM stretched to 100 BPM, from its render", m);
        CHECK (m.peakLevel > 0.01f);
    }

    TEST_CASE ("Real-time audit: an audio clip turned up, pitched and reversed")
    {
        // As the audio clip editor sets them (roadmap step 2.6). Phazite renders the file pitched and backwards, and
        // Tracktion plays the render, so the audio thread only reads a file.
        SineFile sine;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());
        auto* clip = phazite::test::addAudioClip (getTrack (*edit, 0), "Sine", sine.getFile(), { 0_tp, 2_tp });
        REQUIRE (clip != nullptr);
        phazite::setClipGain (*clip, 6.0f);
        phazite::setClipPitch (*clip, 3.0f);
        phazite::setClipReversed (*clip, true);
        REQUIRE (phazite::renderMissingClipAudio (*edit));
        REQUIRE (clip->getAudioFile().isValid());

        const auto m = playAndMeasure (*edit);
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

        auto project = phazite::createSongEdit (phazite::test::getEngine());
        phazite::setTempo (*project, 100.0);
        phazite::ProjectContext context { phazite::test::getEngineContext() };
        setEdit (context, project.get());
        const auto previewer = phazite::createPreviewer (phazite::test::getEngineContext());
        auto& previews = phazite::getPlaybackHost (phazite::test::getEngineContext()).getPreviewEngine();

        const auto m =
            playProjectAndMeasure (context,
                                   [&]
                                   {
                                       REQUIRE (previewer->play (loop, context));
                                       REQUIRE (previewer->isInTime());
                                       REQUIRE (phazite::test::runTimersUntil ([&previews] { return previews.isPlaying(); }, 60000));
                                   });
        previewer->stop();
        setEdit (context, nullptr);
        report ("A loop at 120 BPM previewed at 100 BPM, stretched by Phazite as it starts", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }
    TEST_CASE ("Real-time audit: two tracks in a folder bus")
    {
        SineFile sine;
        auto edit = phazite::createSongEdit (phazite::test::getEngine());

        auto& first = getTrack (*edit, 0);
        auto& second = getTrack (*edit, 1);
        addSineClip (first, sine);
        addSineClip (second, sine);

        auto* folder = phazite::insertFolder (*edit, nullptr, nullptr);
        REQUIRE (folder != nullptr);
        phazite::moveTrackIntoFolder (first, *folder);
        phazite::moveTrackIntoFolder (second, *folder);
        addPlugin (*folder, phazite::devices::Compressor::typeID);

        const auto m = playAndMeasure (*edit);
        report ("Two tracks in a folder bus with a compressor", m);
        CHECK (m.peakLevel > 0.01f);
    }
}
