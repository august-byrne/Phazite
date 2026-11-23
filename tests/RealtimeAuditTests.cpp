// SPDX-License-Identifier: AGPL-3.0-or-later
// The real-time audit (roadmap step 0.3). Each scenario builds a small project and plays it through Phazite's engine as
// the app plays it, on the tests' audio device (TestAudioDevice.h), which a separate thread drives as the audio thread.
// In Windows Debug and Linux builds it counts the heap allocations and frees made on that thread and on the engine's
// workers, and shows where they came from (AudioThreadMonitor; without stacks under the sanitizers). Under the linux-asan
// and linux-tsan presets the same playback runs under AddressSanitizer and ThreadSanitizer.
//
// The scenarios whose steady playback must stay clean fail if it allocates or frees; those that read audio files only
// report what they find. To see the report:
//   build\windows-debug\phazite_tests_artefacts\Debug\phazite_tests.exe --test-suite=realtime-audit
// docs/realtime-audit.md lists the findings and what Phazite does about each.

#include "AudioThreadMonitor.h"
#include "SandboxedPlugins.h"
#include "TestAudioDevice.h"
#include "TestClips.h"
#include "TestDeviceValues.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/age/Age.h"
#include "devices/core/AudioThreads.h"
#include "devices/core/DeviceInfo.h"
#include "devices/core/Processor.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/fm/FmSynth.h"
#include "devices/motion/Motion.h"
#include "devices/reverb/Reverb.h"
#include "devices/sampler/FactorySamples.h"
#include "devices/sampler/Sampler.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/Stereo.h"
#include "devices/synth/Synth.h"
#include "devices/vocoder/Vocoder.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "engine/core/Recording.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/devices/internal/SamplerSound.h"
#include "model/devices/internal/Wavetables.h"
#include "model/engine/AudioFiles.h"
#include "model/engine/EngineContext.h"
#include "model/engine/Preview.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/engine/internal/Preview.h"
#include "model/midi/MidiLinkModel.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/notes/NotesModel.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"

#include <array>
#include <chrono>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

using phazite::test::AudioThreadMonitor;
using phazite::test::TestSong;

namespace
{
constexpr double auditSampleRate = 48000.0;
constexpr int auditBlockSize = 256;
constexpr int steadyBlocks = 188; // a second at 48 kHz: what plays after the warm-up, which must allocate nothing

// The blocks from the start of playback that may allocate or free: none, unless a scenario says which and why. Every
// block after them is measured, whatever comes in it (a note's start, an edit, a loop's wrap): an allocation there fails
// a guarded scenario, with where it came from.
constexpr int noWarmUp = 0;

// An edit made on the message thread while the song plays, as the user makes one: before the block it is due at, the
// audio thread waits for it to be made and for the message thread to deliver what it posted (the song's new plan, handed
// to the engine), so that every edit is played, at the same block every time, however fast the machine is.
struct Edit
{
    int atBlock = 0;
    const char* what = "";
    std::function<void()> make;
};

//==============================================================================
// A stereo sine wave file, two seconds long or as long as asked, so audio clips have something to play; at the engine's rate,
// or at another.
class SineFile
{
public:
    explicit SineFile (double seconds = 2.0, double fileRate = auditSampleRate)
    {
        juce::AudioBuffer<float> buffer (2, juce::roundToInt (fileRate * seconds));

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto sample = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) fileRate);
            buffer.setSample (0, i, sample);
            buffer.setSample (1, i, sample);
        }

        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (temp.getFile());
        const auto options = juce::AudioFormatWriterOptions {}.withSampleRate (fileRate).withNumChannels (2).withBitsPerSample (24);

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
    int warmUpBlocks = noWarmUp; // the blocks that may allocate, from the first
    int blocksPlayed = 0;
    int firstBusyBlock = -1, lastBusyBlock = -1; // the first and last blocks that allocated or freed, or -1
    float peakLevel = 0.0f;
    int assertions = 0;                      // JUCE assertions that fired on the audio thread, which fail the test
    AudioThreadMonitor::Counts ofAssertions; // what JUCE allocated for their messages after the warm-up, left out of steady
    float lastLevel = 0.0f;                  // the peak of the last 16 blocks
};

// How many threads the engine plays a block on, the audio thread one of them: the others are its workers
// (engine/core/Workers.h), whose allocations the monitor counts as the audio thread's.
constexpr int severalThreads = 4;

// How a scenario plays: what is done once the engine has the song, before it plays; whether it records a take instead,
// while another thread plays a note every twentieth of a second into a track live, as a MIDI driver would; the blocks
// that may allocate as it starts (noWarmUp, or a number with its reason where the scenario gives it); the edits made
// while it plays; and on how many threads.
struct Playing
{
    std::function<void()> before = {};
    bool recording = false;
    phazite::TrackID liveTrack = {};
    int warmUpBlocks = noWarmUp;
    std::vector<Edit> edits = {};
    int threads = severalThreads;
};

// Plays the open project from its start through Phazite's engine as the app plays it, in real time on a separate "audio"
// thread that drives the tests' audio device (TestAudioDevice.h), whose callback is Phazite's
// (model/playback/internal/PlaybackHost.h), and measures that thread; a tone comes into the device's two inputs. It plays
// the warm-up's blocks, then a second, or on until a block after the last edit's. Recording, stopping keeps the take.
Measurement playProjectAndMeasure (phazite::ProjectContext& context, const Playing& playing)
{
    const auto& before = playing.before;
    const auto recording = playing.recording;
    const auto liveTrack = playing.liveTrack;
    const auto& edits = playing.edits;
    auto lastEditBlock = -1;

    for (const auto& edit : edits)
    {
        REQUIRE (edit.atBlock > lastEditBlock); // in their order, one at each block at most
        lastEditBlock = edit.atBlock;
    }

    auto* openSong = phazite::findSong (context);
    REQUIRE (openSong != nullptr);
    auto& song = *openSong;
    auto& devices = phazite::test::getEngineContext().getAudioDeviceManager();
    auto* device = phazite::test::TestAudioDeviceType::open (devices, auditSampleRate, auditBlockSize, 2);
    REQUIRE (device != nullptr);
    phazite::test::dispatchPendingMessages(); // the song for the device's rate
    phazite::movePlayhead (song, phazite::BeatPosition());

    // JUCE's AudioDeviceManager sizes a buffer of its own in the first block it plays with a device's block size, once in
    // the program's life (juce_AudioDeviceManager.cpp, audioDeviceIOCallbackInt(): tempBuffer.setSize()): the app's first
    // audio block allocates that once, which Phazite can't change (docs/realtime-audit.md). A block played here, the song
    // stopped, gives it that before anything is measured, so that every scenario measures its own blocks alike, whichever
    // runs first.
    {
        juce::AudioBuffer<float> stopped (2, auditBlockSize);
        stopped.clear();
        device->process (stopped);
    }

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

    // On the threads asked for: the monitor counts what the audio thread allocates and what the engine's workers do.
    const auto threads = engine.getNumThreads();
    engine.setNumThreads (playing.threads);

    Measurement m;
    m.warmUpBlocks = playing.warmUpBlocks;

    // A second after the warm-up, and at least what follows the last edit for a while (a device removed is let go of by the
    // message thread after the engine has played on without it).
    const auto totalBlocks = std::max (playing.warmUpBlocks + steadyBlocks, lastEditBlock + 64);
    std::vector<AudioThreadMonitor::Counts> countsAfter ((size_t) totalBlocks); // made before the thread is an audio thread
    juce::AudioBuffer<float> buffer (2, auditBlockSize);
    const auto blockDuration =
        std::chrono::duration_cast<std::chrono::steady_clock::duration> (std::chrono::duration<double> (auditBlockSize / auditSampleRate));

    // The edits asked for by the audio thread, and made by the message thread; and whether the audio thread is done, or
    // must stop early (a check on the message thread failed).
    std::atomic<int> editsDue { 0 }, editsMade { 0 };
    std::atomic<bool> finished { false }, abandoned { false };

    const auto assertionsBefore = phazite::test::getNumAudioThreadAssertions();
    AudioThreadMonitor::reset();

    std::thread audioThread (
        [&]
        {
            const AudioThreadMonitor::ScopedAudioThread marker;
            size_t nextEdit = 0;

            for (int block = 0; block < totalBlocks && ! abandoned.load(); ++block)
            {
                // The edits due now, made on the message thread while this one waits (outside the device's call, which
                // is what is measured).
                for (; nextEdit < edits.size() && edits[nextEdit].atBlock == block; ++nextEdit)
                {
                    editsDue = (int) nextEdit + 1;

                    while (editsMade.load() <= (int) nextEdit && ! abandoned.load())
                        std::this_thread::sleep_for (std::chrono::milliseconds (1));
                }

                const auto blockEnd = std::chrono::steady_clock::now() + blockDuration;

                for (int channel = 0; channel < 2; ++channel)
                    buffer.copyFrom (channel, 0, tone, 0, 0, auditBlockSize);

                device->process (buffer);
                countsAfter[(size_t) block] = AudioThreadMonitor::getCounts();
                m.blocksPlayed = block + 1;
                m.peakLevel = std::max (m.peakLevel, buffer.getMagnitude (0, auditBlockSize));

                if (block >= totalBlocks - 16)
                    m.lastLevel = std::max (m.lastLevel, buffer.getMagnitude (0, auditBlockSize));

                std::this_thread::sleep_until (blockEnd);
            }

            finished = true;
        });

    // Meanwhile the message thread does its usual work as the song plays (the session's timer, what threads post to it), and
    // makes each edit when it is due. A check that fails in an edit stops the audio thread before it goes on.
    try
    {
        while (! finished.load())
        {
            if (const auto due = editsDue.load(); due > editsMade.load())
            {
                const auto& edit = edits[(size_t) due - 1];
                INFO ("making the edit at block " << edit.atBlock << ": " << edit.what);
                edit.make();
                phazite::test::dispatchPendingMessages(); // the song's new plan, handed to the engine
                editsMade = due;
            }
            else
            {
                phazite::test::dispatchPendingMessages();
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
        }
    }
    catch (...)
    {
        abandoned = true;
        audioThread.join();
        playingNotes = false;
        notes.join();
        engine.setNumThreads (threads);
        phazite::test::TestAudioDeviceType::close (devices);
        throw;
    }

    audioThread.join();
    playingNotes = false;
    notes.join();
    engine.setNumThreads (threads);
    REQUIRE (m.blocksPlayed == totalBlocks);

    // Where the audio thread allocated or freed, block by block.
    AudioThreadMonitor::Counts previous;

    for (int block = 0; block < totalBlocks; ++block)
    {
        const auto& now = countsAfter[(size_t) block];

        if (now.allocations != previous.allocations || now.frees != previous.frees)
        {
            if (m.firstBusyBlock < 0)
                m.firstBusyBlock = block;

            m.lastBusyBlock = block;
        }

        if (block == m.warmUpBlocks - 1)
        {
            m.warmUp = now;
            m.firstSteadyEvent = now.allocations + now.frees;
        }

        previous = now;
    }

    const auto total = AudioThreadMonitor::getCounts();
    m.steady = { total.allocations - m.warmUp.allocations, total.frees - m.warmUp.frees };

    // A JUCE assertion on the audio thread is a failure of the test, which the assertion net reports by its place
    // (TestMain.cpp); what JUCE allocated for its message there (logAssertion()) isn't playback's, and is left out (known
    // bug 153).
    m.assertions = phazite::test::getNumAudioThreadAssertions() - assertionsBefore;

    if (m.assertions > 0)
    {
        m.ofAssertions = AudioThreadMonitor::ofAssertions (m.firstSteadyEvent);
        m.steady.allocations -= m.ofAssertions.allocations;
        m.steady.frees -= m.ofAssertions.frees;
    }

    phazite::stopAndReturn (song);
    m.callSites = AudioThreadMonitor::describeCallSites();
    m.steadyCallSites = AudioThreadMonitor::describeCallSites (6, m.firstSteadyEvent);

    // So that later tests start without the device.
    phazite::test::TestAudioDeviceType::close (devices);
    return m;
}

// As above, with only what is to be done before it plays, or a take recorded with notes played live into a track.
Measurement playProjectAndMeasure (phazite::ProjectContext& context, const std::function<void()>& before = {}, bool recording = false,
                                   phazite::TrackID liveTrack = {})
{
    return playProjectAndMeasure (context, Playing { .before = before, .recording = recording, .liveTrack = liveTrack });
}

// As above, on the audio thread alone.
Measurement playProjectAndMeasureOnOneThread (phazite::ProjectContext& context)
{
    return playProjectAndMeasure (context, Playing { .threads = 1 });
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

    if (m.assertions > 0)
        text << "  an assertion fired on the audio thread (" << m.assertions << " in all; the test fails with each one's place):"
             << " what JUCE allocated for their messages after the warm-up (" << m.ofAssertions.allocations << " allocations, "
             << m.ofAssertions.frees << " frees) is left out below\n";

    if (AudioThreadMonitor::isAvailable())
    {
        text << "  audio thread, " << m.blocksPlayed << " blocks of " << auditBlockSize << " samples: ";

        if (m.firstBusyBlock < 0)
            text << "none allocated or freed\n";
        else
            text << "the first to allocate or free was block " << m.firstBusyBlock << ", the last block " << m.lastBusyBlock << "\n";

        text << "  in the warm-up's " << m.warmUpBlocks << " blocks: " << m.warmUp.allocations << " allocations, " << m.warmUp.frees
             << " frees\n";
        text << "  after it: " << m.steady.allocations << " allocations, " << m.steady.frees << " frees\n";

        for (const auto& site : m.callSites)
            text << "    " << site << "\n";

        if (! m.steadyCallSites.isEmpty())
        {
            text << "  of those, after the warm-up:\n";

            for (const auto& site : m.steadyCallSites)
                text << "    " << site << "\n";
        }
    }
    else
    {
        text << "  (allocations are counted only in Windows Debug and Linux builds: AudioThreadMonitor.h)\n";
    }

    MESSAGE (text.toStdString());
}

// For playback paths that must stay clean: after the warm-up's blocks (none, unless the scenario gave some, with why),
// nothing is allocated or freed on the audio thread, whatever comes. (Measured where AudioThreadMonitor can: Windows Debug
// and Linux builds.)
void checkSteadyPlaybackDoesNotAllocate (const Measurement& m)
{
    if (AudioThreadMonitor::isAvailable())
    {
        INFO ("blocks that allocated or freed: from " << m.firstBusyBlock << " to " << m.lastBusyBlock << ", the warm-up's "
                                                      << m.warmUpBlocks << " allowed; where:\n"
                                                      << m.steadyCallSites.joinIntoString ("\n").toStdString());
        CHECK (m.steady.allocations == 0);
        CHECK (m.steady.frees == 0);

        // A warm-up allowed that nothing needs any more goes: the scenario then guards every block.
        if (m.warmUpBlocks > 0)
            CHECK (m.firstBusyBlock >= 0);
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

    REQUIRE (phazite::test::dispatchMessagesUntil (idle, phazite::test::versionHistoryTimeout));
    phazite::setVersionDraft (song.context, name, {});
    std::optional<juce::Result> saved;
    REQUIRE (phazite::saveVersion (song.context, [&saved] (juce::Result r) { saved = r; }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&saved] { return saved.has_value(); }));
    REQUIRE (saved->wasOk());
    REQUIRE (phazite::test::dispatchMessagesUntil (idle, phazite::test::versionHistoryTimeout));

    const auto version = phazite::getLastVersion (song.context)->id;
    phazite::bounceVersion (song.context, version, song.renders);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.renders.finished.isEmpty(); }));
    REQUIRE (song.renders.finished[0].endsWith (": ok"));
    return version;
}

// A parameter swept by its automation, from one value to another (in its own units, each where its knob would be for it)
// and back on every beat, over two bars.
void sweepParameter (const TestSong& song, const phazite::ParameterRef& parameter, float from, float to)
{
    const auto range = phazite::getControlTravel (getSong (song), parameter);
    REQUIRE (range.has_value());

    for (int beat = 0; beat < 8; ++beat)
    {
        phazite::addPoint (getSong (song), parameter,
                           { phazite::BeatPosition::fromBeats (beat), phazite::toControlPosition (*range, from), 0.0f });
        phazite::addPoint (getSong (song), parameter,
                           { phazite::BeatPosition::fromBeats (beat + 0.5), phazite::toControlPosition (*range, to), 0.0f });
    }
}

void sweepCutoff (const TestSong& song, phazite::DeviceID device, float from, float to)
{
    sweepParameter (song, { device, "cutoff" }, from, to);
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

    TEST_CASE ("Real-time audit: Phazite's own engine on the audio thread alone, chords on two tracks of Phazite's synth into a bus")
    {
        // The other scenarios play on several threads, the engine's workers taking tasks beside the audio thread; this one
        // on the audio thread alone, as a machine with one core plays, and as the engine plays a song of one strip.
        TestSong song;
        const auto second = song.addTrack ("Second");

        for (const auto& track : { song.firstTrack, second })
        {
            song.setInstrument (track, phazite::devices::Synth::typeID);
            const auto clip = song.addMidiClip (track, 0.0, 16.0);

            for (int beat = 0; beat < 16; ++beat)
                song.addChord (clip, { 48 + (beat % 2) * 5, 52, 55, 59 }, beat, 0.75);
        }

        song.addBus ("Bus", { song.firstTrack, second });
        const auto m = playProjectAndMeasureOnOneThread (song.context);
        report ("Phazite's engine on the audio thread alone", m);
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

    TEST_CASE ("Real-time audit: Phazite's own engine, chords and the metronome through tempo changes that ramp, bend, hold and jump")
    {
        // As the tempo lane makes them (roadmap step 4.8): the engine works each block's beats out of the song's tempo map, a
        // ramp's exactly and a bent one's in straight pieces. A change every half beat, so that steady playback goes through
        // each kind of line.
        TestSong song;
        song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
        const auto clip = song.addMidiClip (song.firstTrack, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (clip, { 60 + beat % 3, 64, 67 }, beat, 0.75);

        song.setSwitch ("transport.metronome", true);
        constexpr float curves[] { phazite::tempoRamp, 0.5f, -0.5f, phazite::tempoJump, -1.0f };
        std::vector<phazite::TempoChange> changes;

        for (int i = 0; i < 32; ++i)
            changes.push_back ({ phazite::BeatPosition::fromBeats (0.5 * i), i % 2 == 0 ? 100.0 : 150.0, curves[i % 5] });

        phazite::takeTempoChanges (getSong (song), changes, phazite::BeatPosition());
        REQUIRE (phazite::getTempoChanges (getSong (song)).size() == changes.size());
        const auto m = playProjectAndMeasure (song.context);
        report ("Phazite's engine: tempo changes", m);
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

    TEST_CASE (
        "Real-time audit: Phazite's own engine, a bass ducking under a kick through a linear-phase EQ, its sound waiting for its key")
    {
        // The kick's EQ in Linear phase comes 4,096 samples late at 48 kHz, so that the bass's compressor, keyed by it, holds
        // what comes into it back as long (engine::Plan::Device::inputDelay): the wait played while allocations are counted.
        TestSong song;
        const auto kick = song.firstTrack;
        song.setInstrument (kick, phazite::devices::DrumSampler::typeID);
        const auto beats = song.addMidiClip (kick, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addNote (beats, 36, beat, 0.25);

        const auto eq = song.addEffect (kick, phazite::devices::Equalizer::typeID);
        song.setParameter (eq, "phase", 1.0f);
        const auto bass = song.addTrack ("Bass");
        song.setInstrument (bass, phazite::devices::Synth::typeID);
        song.addNote (song.addMidiClip (bass, 0.0, 16.0), 36, 0.0, 16.0);
        REQUIRE (phazite::duckUnder (song.context, bass, kick).isValid());

        const auto m = playProjectAndMeasure (song.context);
        report ("Phazite's engine: a bass waiting for its key through a linear-phase EQ", m);
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

    TEST_CASE ("Real-time audit: the monitor counts what Phazite's own audio threads allocate, and what other threads do not")
    {
        // The engine's workers are such threads (engine/core/Workers.h): a ProAudioTask lives on each while it runs.
        if (! AudioThreadMonitor::isAvailable())
            return;

        const auto allocateOnThread = [] (bool asAudioThread)
        {
            std::thread thread (
                [asAudioThread]
                {
                    std::optional<phazite::devices::ProAudioTask> task;

                    if (asAudioThread)
                        task.emplace();

                    std::vector<int> made (100, 1);
                    juce::ignoreUnused (made);
                });
            thread.join();
        };

        AudioThreadMonitor::reset();
        allocateOnThread (false);
        CHECK (AudioThreadMonitor::getCounts().allocations == 0);
        allocateOnThread (true);
        CHECK (AudioThreadMonitor::getCounts().allocations >= 1);
        CHECK (AudioThreadMonitor::getCounts().frees >= 1);
    }

    TEST_CASE ("Real-time audit: a take's input that falls a whole buffer behind notes what it loses without allocating, past its room to")
    {
        // Known bug 12's fix (engine/core/Recording.h): while the writing thread stalls, the audio thread notes where frames
        // were lost, for that thread to write silence there. A buffer of 1000 frames in blocks of 10, which a reader keeps
        // full by taking a block's worth at a time from what came in first: each block lost is a gap, noted once the next
        // has room, until there is no room to note more (31 gaps), when what comes in is lost into the gap open. Frame n
        // is n + 1, so that silence shows.
        phazite::engine::Recording::Input input ({ 0 }, 1000);
        constexpr int blockFrames = 10;
        std::array<float, blockFrames> block {};
        auto next = 1.0f;
        std::vector<float> taken;
        taken.reserve (4000);

        const auto write = [&input, &block, &next]
        {
            for (auto& sample : block)
                sample = next++;

            const float* channels[] = { block.data() };
            const AudioThreadMonitor::ScopedAudioThread audioThread;
            input.write (channels, 1, 0, blockFrames);
        };

        const auto take = [&input, &taken] (std::int64_t most)
        {
            input.read (
                most,
                [&taken] (const float* const* channels, int, int count) { taken.insert (taken.end(), channels[0], channels[0] + count); },
                [&taken] (std::int64_t count) { taken.insert (taken.end(), (size_t) count, 0.0f); });
        };

        AudioThreadMonitor::reset();

        for (int i = 0; i < 100; ++i)
            write(); // 99 blocks fit, and the last is lost

        for (int i = 0; i < 60; ++i)
        {
            take (blockFrames);
            write(); // has room, after the gap before it is noted, until there is no room to note it
            write(); // lost
        }

        take (std::numeric_limits<std::int64_t>::max());

        if (AudioThreadMonitor::isAvailable())
        {
            const auto counts = AudioThreadMonitor::getCounts();
            CHECK (counts.allocations == 0);
            CHECK (counts.frees == 0);
        }

        // What came in before the first gap, then 31 gaps noted, each with the block that came in after it; what came in
        // later was lost into the gap still open, which the take's end makes silence (model/transport/internal/Take.h).
        // Everything is where it came in.
        REQUIRE (taken.size() == (size_t) (990 + 31 * 2 * blockFrames));

        for (size_t i = 0; i < taken.size(); ++i)
        {
            CAPTURE (i);
            const auto lost = i >= 990 && (i - 990) % (size_t) (2 * blockFrames) < (size_t) blockFrames;
            CHECK (taken[i] == (lost ? 0.0f : (float) (i + 1)));
        }
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

    TEST_CASE ("Real-time audit: Phazite's own engine, chords on three plug-in synths in their helpers, handed their blocks early, "
               "on several MIDI channels with bends and program changes")
    {
        phazite::test::SandboxedPlugins sandboxed;
        TestSong song;
        using phazite::BeatPosition;

        for (int i = 0; i < 3; ++i)
        {
            const auto track = i == 0 ? song.firstTrack : song.addTrack ("Synth " + juce::String (i + 1));
            song.setInstrument (track, "PhaziteTestSynth");
            const auto clip = song.addMidiClip (track, 0.0, 16.0);

            for (int beat = 0; beat < 16; ++beat)
                song.addChord (clip, { 48 + 12 * i + beat % 3, 52 + 12 * i, 55 + 12 * i }, beat, 0.75);

            // A line on channel 2 and on channel 10 (each a part of a plug-in with several), each bent on its channel, and a
            // program change on each at the start and half way.
            auto& played = getSong (song);

            for (int beat = 0; beat < 16; ++beat)
            {
                for (const auto channel : { 2, 10 })
                {
                    phazite::addNote (played, clip, 60 + 12 * i + channel, BeatPosition::fromBeats (beat + 0.5),
                                      phazite::BeatDuration::fromBeats (0.25), 90, channel);
                    phazite::addControl (played, clip, BeatPosition::fromBeats (beat + 0.25), phazite::ControlKind::pitchWheel, 0,
                                         beat % 2 == 0 ? 10000 : 8192, channel);
                }
            }

            for (const auto beat : { 0.0, 8.0 })
                for (const auto channel : { 2, 10 })
                    phazite::addControl (played, clip, BeatPosition::fromBeats (beat), phazite::ControlKind::programChange, 0,
                                         (int) beat / 8, channel);
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

    TEST_CASE ("Real-time audit: Phazite's EQ with dynamic bands on the middle, the side and the left, a band's placement and "
               "another's Dynamic automated")
    {
        using phazite::devices::Equalizer;
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);
        const auto eq = song.addEffect (track, Equalizer::typeID);

        // Bands 3, 4 and 5 dynamic, each on a placement of its own, their Thresholds low enough that the tone moves them.
        for (const auto& [band, placement] : { std::pair { 2, Equalizer::Placement::mid }, std::pair { 3, Equalizer::Placement::side },
                                               std::pair { 4, Equalizer::Placement::left } })
        {
            song.setParameter (eq, getBandParameter (band, Equalizer::placement), (float) placement);
            song.setParameter (eq, getBandParameter (band, Equalizer::dynamic), -6.0f);
            song.setParameter (eq, getBandParameter (band, Equalizer::threshold), -60.0f);
        }

        // Band 6, raised, going through every placement, a beat on each, crossfading as it goes; band 3's Dynamic swept.
        song.setParameter (eq, getBandParameter (5, Equalizer::gain), 6.0f);
        song.addAutomation ({ eq, getBandParameter (5, Equalizer::placement) }, { { phazite::BeatPosition(), 0.0f, 0.0f },
                                                                                  { phazite::BeatPosition::fromBeats (1.0), 1.0f, 0.0f },
                                                                                  { phazite::BeatPosition::fromBeats (2.0), 2.0f, 0.0f },
                                                                                  { phazite::BeatPosition::fromBeats (3.0), 3.0f, 0.0f },
                                                                                  { phazite::BeatPosition::fromBeats (4.0), 4.0f, 0.0f } });
        song.addAutomation ({ eq, getBandParameter (2, Equalizer::dynamic) },
                            { { phazite::BeatPosition(), -12.0f, 0.0f }, { phazite::BeatPosition::fromBeats (3.0), 12.0f, 0.0f } });

        const auto m = playAndMeasure (song);
        report ("Phazite's EQ with dynamic bands on the middle, the side and the left, a band's placement and another's Dynamic automated",
                m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's EQ in Linear phase, a band's Frequency and Gain automated, a band dynamic and one on Mid")
    {
        using phazite::devices::Equalizer;
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);
        const auto eq = song.addEffect (track, Equalizer::typeID);
        song.setParameter (eq, "phase", 1.0f);

        // Band 3 dynamic; band 5 raised on the middle; band 4's frequency and gain swept, so that its kernels are designed
        // again at every line of their grid, glide, and are designed again while they glide.
        song.setParameter (eq, getBandParameter (2, Equalizer::dynamic), -6.0f);
        song.setParameter (eq, getBandParameter (2, Equalizer::threshold), -60.0f);
        song.setParameter (eq, getBandParameter (4, Equalizer::placement), (float) Equalizer::Placement::mid);
        song.setParameter (eq, getBandParameter (4, Equalizer::gain), 4.0f);
        song.addAutomation ({ eq, getBandParameter (3, Equalizer::frequency) },
                            { { phazite::BeatPosition(), 200.0f, 0.0f }, { phazite::BeatPosition::fromBeats (4.0), 4000.0f, 0.0f } });
        song.addAutomation ({ eq, getBandParameter (3, Equalizer::gain) },
                            { { phazite::BeatPosition(), -12.0f, 0.0f }, { phazite::BeatPosition::fromBeats (4.0), 12.0f, 0.0f } });

        const auto m = playAndMeasure (song);
        report ("Phazite's EQ in Linear phase, a band's Frequency and Gain automated, a band dynamic and one on Mid", m);
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

    TEST_CASE ("Real-time audit: containers' chains: three bands, their crossover modulated, layers muted, and two synths split by keys")
    {
        TestSong song;
        auto& context = song.context;
        const auto track = song.firstTrack;
        addTone (song, track);

        // A container of effects split into three bands, an effect in each, the second band's crossover moved by the track's
        // LFO and the third chain's volume by a macro (model/mixer/ContainerModel.h).
        const auto bands = phazite::addContainer (context, track);
        REQUIRE (phazite::addChain (context, bands).isValid());
        REQUIRE (phazite::addChain (context, bands).isValid());
        phazite::setContainerSplit (context, bands, phazite::ContainerSplit::bands);
        const auto chains = phazite::getChains (context, bands);
        REQUIRE (chains.size() == 3);
        phazite::addEffectToChain (context, chains[0].id, song.findDeviceType (phazite::devices::Compressor::typeID));
        phazite::addEffectToChain (context, chains[1].id, song.findDeviceType (phazite::devices::Equalizer::typeID));
        phazite::addEffectToChain (context, chains[2].id, song.findDeviceType (phazite::devices::Reverb::typeID));
        song.modulate (song.addModulator (track, phazite::ModulatorType::lfo), { chains[1].id, "crossover" }, 0.3f);
        const auto macro = phazite::getContainerMacros (context, bands)[0].id;
        song.modulate (macro, { chains[2].id, "volume" }, -0.3f);
        song.setMacroValue (macro, 0.5f);

        // After it, a container of two layers, the first muted, the second a delay.
        const auto layers = phazite::addContainer (context, track);
        const auto delayed = phazite::addChain (context, layers);
        phazite::addEffectToChain (context, delayed, song.findDeviceType (phazite::devices::Delay::typeID));
        phazite::toggleChainMute (context, phazite::getChains (context, layers)[0].id);

        // On another track, an instrument container of two synths split by keys, a chord across both.
        const auto keys = song.addTrack ("Keys");
        song.setInstrument (keys, phazite::devices::Synth::typeID);
        const auto instruments = phazite::addInstrumentContainer (context, keys);
        const auto high = phazite::addChain (context, instruments);
        phazite::chooseChainInstrument (context, high, song.findDeviceType (phazite::devices::Synth::typeID));
        phazite::setChainKeys (context, phazite::getChains (context, instruments)[0].id, { 0, 59 });
        phazite::setChainKeys (context, high, { 60, 127 });
        song.addChord (song.addMidiClip (keys, 0.0, 16.0), { 48, 55, 64, 67 }, 0.0, 16.0);

        const auto m = playAndMeasure (song);
        report ("Containers' chains: three bands, their crossover modulated, layers muted, and two synths split by keys", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: edited while it plays: a container's chain's volume, pan and crossover and its mix, set at once from "
               "what the song's values tell, and undone")
    {
        // What the devices' listener plays at once (SongPlayer::takeDevice(), from the values before and after the song's
        // values tell: SongValues.h, devicesChanged()), with no plan: a chain's fader and crossover, a container's mix.
        TestSong song;
        auto& context = song.context;
        const auto track = song.firstTrack;
        addTone (song, track);
        const auto bands = phazite::addContainer (context, track);
        REQUIRE (phazite::addChain (context, bands).isValid());
        phazite::setContainerSplit (context, bands, phazite::ContainerSplit::bands);
        const auto chains = phazite::getChains (context, bands);
        REQUIRE (chains.size() == 2);
        phazite::addEffectToChain (context, chains[1].id, song.findDeviceType (phazite::devices::Equalizer::typeID));
        auto& played = getSong (song);

        const auto inStep = [&played] (const char* name, const std::function<void()>& change)
        {
            const phazite::UndoStep step (played, name);
            change();
        };

        const auto m = playProjectAndMeasure (
            context,
            Playing { .edits = {
                          { 10, "the second chain's volume turned down",
                            [&] { inStep ("Volume", [&] { phazite::setChainVolume (played, chains[1].id, -9.0f); }); } },
                          { 20, "its pan moved", [&] { inStep ("Pan", [&] { phazite::setChainPan (played, chains[1].id, -0.5f); }); } },
                          { 30, "its crossover moved",
                            [&] { inStep ("Crossover", [&] { phazite::setChainCrossover (played, chains[1].id, 1200.0f); }); } },
                          { 40, "the container's mix", [&] { inStep ("Mix", [&] { phazite::setMix (played, bands, 0.6f); }); } },
                          { 60, "undone", [&] { phazite::undo (played); } },
                          { 70, "undone again", [&] { phazite::undo (played); } },
                      } });
        report ("Edited while it plays: a chain's volume, pan and crossover and a container's mix", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's compressor, ducked by another track at 70 % mix, with its meters and its trail read")
    {
        using phazite::devices::Compressor;
        TestSong song;
        const auto track = song.firstTrack;
        const auto source = song.addTrack ("Source");
        addTone (song, track);
        addTone (song, source);
        phazite::setTrackMuted (getSong (song), source, true); // still heard by the sidechain

        const auto compressor = song.addEffect (track, Compressor::typeID);
        // Ducking from -40 dB, so that the tone ducks it (its key chosen makes it duck).
        song.setParameter (compressor, "threshold", -40.0f);
        song.setParameter (compressor, "mix", 0.7f);
        phazite::setSidechainSource (getSong (song), compressor, source);
        REQUIRE (phazite::getSidechainSource (getSong (song), compressor) == source);
        REQUIRE (phazite::hearsSidechain (getSong (song), compressor));

        const auto played = [&song, compressor]() -> auto&
        { return dynamic_cast<Compressor&> (getPlayed (song, compressor).getProcessor()); };
        std::vector<phazite::devices::LevelReading> readings ((size_t) phazite::devices::LevelTrail::capacity);
        const auto m = playAndMeasure (
            song,
            [&played]
            {
                played().getReductionMeter().take();
                played().getTrail().setListening (true); // the history's trail, its readings mixed as its runner mixes them
            },
            [&played, &readings]
            {
                CHECK (played().getReductionMeter().take() > 1.0f);
                CHECK (played().getTrail().pull (readings.data(), (int) readings.size()) > 0);
            });
        report ("Phazite's compressor, ducked by another track at 70 % mix, with its meters and its trail read", m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's compressor ducking under a kick track, held and let go along a curve, Listen on")
    {
        // Duck mode under a drum sampler's kick on every beat, muted (its sidechain still hears it): each kick grips, holds and
        // lets go, its release bent, the history's meters read; and Listen asked for as it starts (it plays the key for half a
        // second, then the sound again).
        using phazite::devices::Compressor;
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);
        const auto kick = song.addTrack ("Kick");
        song.setInstrument (kick, phazite::devices::DrumSampler::typeID);
        const auto beats = song.addMidiClip (kick, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addNote (beats, 36, beat, 0.25);

        phazite::setTrackMuted (getSong (song), kick, true);
        const auto compressor = phazite::duckUnder (song.context, track, kick);
        REQUIRE (compressor.isValid());
        REQUIRE (phazite::hearsSidechain (getSong (song), compressor));
        song.setParameter (compressor, "threshold", -10.0f); // the kick's tail falls under it within the beat, and it lets go
        song.setParameter (compressor, "hold", 60.0f);
        song.setParameter (compressor, "releaseCurve", 0.5f);
        song.setParameter (compressor, "depth", 18.0f);

        const auto played = [&song, compressor]() -> auto&
        { return dynamic_cast<Compressor&> (getPlayed (song, compressor).getProcessor()); };
        const auto m = playAndMeasure (
            song,
            [&played]
            {
                played().getReductionMeter().take();
                played().getLevelMeter().take();
                played().getTrail().setListening (true); // its trail fills and then drops what it can't hold
                played().listenToKey();
            },
            [&played] { CHECK (played().getReductionMeter().take() > 1.0f); });
        report ("Phazite's compressor ducking under a kick track, held and let go along a curve, Listen on", m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's compressor keyed by another track, its mode, look-ahead and hold moved by lanes")
    {
        // Compressing keyed by a muted track (set its own way before its key is chosen, so that it stays in Compress), made
        // with a 5 ms look-ahead; lanes switch it through Compress, Duck and Transient and back, and sweep its look-ahead
        // within its latency and its hold, a cycle a beat.
        using phazite::devices::Compressor;
        TestSong song;
        const auto track = song.firstTrack;
        const auto source = song.addTrack ("Source");
        addTone (song, track);
        addTone (song, source);
        phazite::setTrackMuted (getSong (song), source, true);

        const auto compressor = song.addEffect (track, Compressor::typeID);
        song.setParameter (compressor, "lookahead", 5.0f);
        song.setParameter (compressor, "punch", 0.8f);
        song.setParameter (compressor, "sustain", -0.5f);
        phazite::setSidechainSource (getSong (song), compressor, source);
        REQUIRE (phazite::hearsSidechain (getSong (song), compressor));

        const auto range = phazite::BeatRange (phazite::BeatPosition(), phazite::BeatPosition::fromBeats (32.0));
        const auto at = [] (double beats) { return phazite::BeatPosition::fromBeats (beats); };
        phazite::replacePoints (getSong (song), { compressor, "mode" }, range,
                                { { at (0.0), 0.0f }, { at (4.0), 2.0f }, { at (8.0), 0.0f }, { at (16.0), 2.0f }, { at (32.0), 0.0f } });
        phazite::replacePoints (
            getSong (song), { compressor, "lookahead" }, range,
            phazite::makeAutomationShape (phazite::AutomationShape::sine, range, phazite::BeatDuration::fromBeats (1.0), 0.0f, 10.0f));
        phazite::replacePoints (
            getSong (song), { compressor, "levelHold" }, range,
            phazite::makeAutomationShape (phazite::AutomationShape::sine, range, phazite::BeatDuration::fromBeats (1.0), 0.0f, 200.0f));

        const auto m = playAndMeasure (song);
        report ("Phazite's compressor keyed by another track, its mode, look-ahead and hold moved by lanes", m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's multiband, its low band and master keyed by another track through the sidechain, Listen on")
    {
        using phazite::devices::Multiband;
        TestSong song;
        const auto track = song.firstTrack;
        const auto source = song.addTrack ("Source");
        addTone (song, track);
        addTone (song, source);
        phazite::setTrackMuted (getSong (song), source, true); // still heard by the sidechain

        // The low band and the master limiting at -40 dB, each hearing the key through its cuts, so that the tone ducks them.
        const auto multiband = song.addEffect (track, Multiband::typeID);

        for (const auto* band : { "low", "master" })
        {
            const auto prefix = juce::String (band);
            const auto place = phazite::devices::curves::axisForDecibels (-40.0f);
            song.setParameter (multiband, prefix + "Point2X", place);
            song.setParameter (multiband, prefix + "Point2Y", place);
            song.setParameter (multiband, prefix + "Point3Y", place);
            song.setParameter (multiband, prefix + "Key", 1.0f);
        }

        song.setParameter (multiband, "keyLowCut", 60.0f);
        song.setParameter (multiband, "keyHighCut", 2000.0f);
        phazite::setSidechainSource (getSong (song), multiband, source);
        REQUIRE (phazite::getSidechainSource (getSong (song), multiband) == source);

        // Listen asked for as it starts (it plays the key for half a second, then the sound again), its meters read.
        const auto played = [&song, multiband]() -> auto& { return dynamic_cast<Multiband&> (getPlayed (song, multiband).getProcessor()); };
        const auto m = playAndMeasure (
            song,
            [&played]
            {
                played().getReductionMeter (Multiband::master).take();
                played().listenToKey();
            },
            [&played] { CHECK (played().getReductionMeter (Multiband::master).take() > 1.0f); });
        report ("Phazite's multiband, its low band and master keyed by another track through the sidechain, Listen on", m);
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
        song.setParameter (synth, "mod2Source", (float) Synth::ModSource::lfo1);
        song.setParameter (synth, "mod2Target", (float) Synth::ModTarget::cutoff);
        song.setParameter (synth, "mod2Amount", 0.5f);
        song.setParameter (synth, "env1Release", 400.0f);

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

    TEST_CASE ("Real-time audit: Phazite's synth, eight unison voices, stacked and drifting, and warps, an envelope "
               "sweeping each note's table, LFOs fading in, legato glides, Drift and a slot moving another's amount, automated, "
               "its Quality and its filter's Character switched")
    {
        using phazite::devices::Synth;
        TestSong song;
        const auto track = song.firstTrack;
        const auto wavetable = song.setInstrument (track, Synth::typeID);
        phazite::setFactoryWavetable (getSong (song), wavetable, 1, phazite::devices::findFactoryWavetable ("Vowels"));

        const auto set = [&song, wavetable] (const char* id, float value) { song.setParameter (wavetable, id, value); };
        set ("unisonVoices", 7.0f); // eight
        set ("unisonPosition", 0.5f);
        set ("unisonStack", (float) Synth::Stack::twoOctaves);
        set ("unisonDrift", 25.0f);
        set ("osc1Warp", (float) Synth::Warp::sync);
        set ("osc1WarpAmount", 0.4f);
        set ("osc2On", 1.0f);
        set ("osc2Warp", (float) Synth::Warp::fm);
        set ("osc2WarpAmount", 0.3f);
        set ("subLevel", 0.3f);
        set ("noiseLevel", 0.1f);
        set ("drive", 6.0f);
        set ("mod2Source", (float) Synth::ModSource::envelope3);
        set ("mod2Target", (float) Synth::ModTarget::osc1Position);
        set ("mod2Amount", 1.0f);
        set ("mod3Source", (float) Synth::ModSource::lfo1);
        set ("mod3Target", (float) Synth::ModTarget::osc2Position);
        set ("mod3Amount", 0.5f);

        // Each LFO's fade, legato glides, Drift on the cutoff, and LFO 2 moving slot 3's
        // amount; the fade and that slot's amount automated.
        set ("lfo1FadeIn", 120.0f);
        set ("lfo2Retrigger", 0.0f);
        set ("glide", 40.0f);
        set ("glideMode", (float) Synth::GlideMode::legato);
        set ("mod4Source", (float) Synth::ModSource::drift);
        set ("mod4Target", (float) Synth::ModTarget::cutoff);
        set ("mod4Amount", 0.05f);
        set ("mod5Source", (float) Synth::ModSource::lfo2);
        set ("mod5Target", (float) Synth::ModTarget::slot3Amount);
        sweepParameter (song, { wavetable, "lfo2FadeIn" }, 0.0f, 300.0f);
        sweepParameter (song, { wavetable, "mod5Amount" }, -1.0f, 1.0f);

        // Its Quality switched by a lane, 4x, 2x and 1x in turn, a beat each, while chords sound.
        const phazite::ParameterRef quality { wavetable, "quality" };
        const auto range = phazite::getControlTravel (getSong (song), quality);
        REQUIRE (range.has_value());

        for (int beat = 0; beat < 8; ++beat)
        {
            const auto position = phazite::toControlPosition (*range, (float) (2 - beat % 3));
            phazite::addPoint (getSong (song), quality, { phazite::BeatPosition::fromBeats (beat), position, 0.0f });
            phazite::addPoint (getSong (song), quality, { phazite::BeatPosition::fromBeats (beat + 0.9), position, 0.0f });
        }

        // Its filter Driven near singing, and its Character switched by a lane, Clean and Driven in turn, two beats each, a
        // half beat off the Quality's changes, so that a crossfade plays while chords sound.
        set ("filterCharacter", (float) Synth::FilterCharacter::driven);
        set ("resonance", 0.85f);
        const phazite::ParameterRef character { wavetable, "filterCharacter" };
        const auto characters = phazite::getControlTravel (getSong (song), character);
        REQUIRE (characters.has_value());

        for (int beat = 0; beat < 8; beat += 2)
        {
            const auto position = phazite::toControlPosition (*characters, (float) ((beat / 2 + 1) % 2));
            phazite::addPoint (getSong (song), character, { phazite::BeatPosition::fromBeats (beat + 0.5), position, 0.0f });
            phazite::addPoint (getSong (song), character, { phazite::BeatPosition::fromBeats (beat + 2.4), position, 0.0f });
        }

        // Chords a sixteenth long, as for the synth's unison.
        addChords (song, track);

        const auto m = playAndMeasure (song);
        report ("Phazite's synth, eight unison voices, stacked and drifting, and warps, an envelope sweeping each note's "
                "table, LFOs fading in, legato glides, Drift and a slot moving another's amount, automated, its Quality "
                "and its filter's Character switched",
                m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's filter, saturator, multiband, delay, Motion in each mode, Stereo and limiter in a row, "
               "the filter swept and a Motion's mode automated")
    {
        using phazite::devices::Motion;
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);

        phazite::DeviceID filter, stereo;
        std::vector<phazite::DeviceID> motions;

        for (const auto* type : { phazite::devices::FilterEffect::typeID, phazite::devices::Saturator::typeID,
                                  phazite::devices::Multiband::typeID, phazite::devices::Delay::typeID, Motion::typeID, Motion::typeID,
                                  Motion::typeID, phazite::devices::Stereo::typeID, phazite::devices::Limiter::typeID })
        {
            const auto device = song.addEffect (track, type);

            if (! filter.isValid())
                filter = device;

            if (std::string_view (type) == Motion::typeID)
                motions.push_back (device);

            if (std::string_view (type) == phazite::devices::Stereo::typeID)
                stereo = device;
        }

        REQUIRE (motions.size() == 3);
        const auto phaser = motions[0], flanger = motions[1], chorus = motions[2];

        // The filter swept by a lane, a cycle a beat, so its cutoff glides all the time; the others well into their work: a
        // Motion as a phaser and one as a flanger, with Safe Bass on (their split, and the sound their runners mix back in
        // turned with it, at a quarter of their mix: half the flanger's and the phaser's own), and one as a chorus with eight
        // copies a side, each in its part of the sweep, with their shimmer, its feedback, High Cut and Safe Bass on, its mode
        // moved by a lane from the chorus to the flanger, the phaser and back, a beat each, so that its modes fade into
        // each other all the time.
        sweepCutoff (song, filter, 200.0f, 8000.0f);
        song.setParameter (phaser, "mode", (float) Motion::Mode::phaser);
        song.setParameter (flanger, "mode", (float) Motion::Mode::flanger);

        for (const auto device : { phaser, flanger })
        {
            song.setParameter (device, "safeBass", 120.0f);
            song.setParameter (device, "mix", 0.25f);
        }

        for (const auto& [id, value] : { std::pair<const char*, float> { "voices", 7.0f },
                                         { "shimmer", 1.0f },
                                         { "voicePhase", 0.5f },
                                         { "spread", 0.6f },
                                         { "feedback", 0.4f },
                                         { "highCut", 6000.0f },
                                         { "safeBass", 150.0f } })
            song.setParameter (chorus, id, value);

        const phazite::ParameterRef mode { chorus, "mode" };
        const auto range = phazite::getControlTravel (getSong (song), mode);
        REQUIRE (range.has_value());

        for (int beat = 0; beat < 8; ++beat)
        {
            const auto position = phazite::toControlPosition (*range, (float) (beat % 3));
            phazite::addPoint (getSong (song), mode, { phazite::BeatPosition::fromBeats (beat), position, 0.0f });
            phazite::addPoint (getSong (song), mode, { phazite::BeatPosition::fromBeats (beat + 0.9), position, 0.0f });
        }

        // Stereo with its parts at work: spread, its bass in mono, balanced to one side, its sides swapped, its DC filter on.
        for (const auto& [id, value] : { std::pair<const char*, float> { "spread", 0.5f },
                                         { "balance", 0.3f },
                                         { "swap", 1.0f },
                                         { "bassMono", 1.0f },
                                         { "dcFilter", 1.0f } })
            song.setParameter (stereo, id, value);

        const auto m = playAndMeasure (song);
        report ("Phazite's filter, saturator, multiband, delay, Motion in each mode, Stereo and limiter in a row, the filter swept "
                "and a Motion's mode automated",
                m);
        CHECK (m.peakLevel > 0.001f);
        CHECK (m.peakLevel < juce::Decibels::decibelsToGain (-1.0f) * 1.001f); // the limiter's ceiling
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's saturator on the synth's chords, its Band's edges swept and its Quality automated, with "
               "its editor listening and Listen on")
    {
        using phazite::devices::Saturator;
        TestSong song;
        const auto track = song.firstTrack;
        song.setInstrument (track, phazite::devices::Synth::typeID);
        addChords (song, track);
        const auto saturator = song.addEffect (track, Saturator::typeID);

        // Its band of highs driven hard into a lopsided tube, lifted, at half its mix; where the band starts swept by a lane, a
        // cycle a beat, so that its crossovers glide all the time; and its Quality moved by a lane from 8x to 4x and 2x and
        // back, a beat each, so that its ways at each rate fade into each other all the time.
        for (const auto& [id, value] : { std::pair<const char*, float> { "band", 1.0f },
                                         { "bandHigh", 12000.0f },
                                         { "drive", 30.0f },
                                         { "shape", (float) Saturator::Shape::tube },
                                         { "colour", 0.5f },
                                         { "output", 6.0f },
                                         { "quality", 2.0f },
                                         { "mix", 0.5f } })
            song.setParameter (saturator, id, value);

        sweepParameter (song, { saturator, "bandLow" }, 1000.0f, 8000.0f);
        const phazite::ParameterRef quality { saturator, "quality" };
        const auto range = phazite::getControlTravel (getSong (song), quality);
        REQUIRE (range.has_value());

        for (int beat = 0; beat < 8; ++beat)
        {
            const auto position = phazite::toControlPosition (*range, (float) (2 - beat % 3));
            phazite::addPoint (getSong (song), quality, { phazite::BeatPosition::fromBeats (beat), position, 0.0f });
            phazite::addPoint (getSong (song), quality, { phazite::BeatPosition::fromBeats (beat + 0.9), position, 0.0f });
        }

        // As its open editor has it: its scope tapped, and Listen asked for once, so that it plays the band alone for the first
        // half second and then fades back to the sound.
        const auto played = [&song, saturator]() -> auto& { return dynamic_cast<Saturator&> (getPlayed (song, saturator).getProcessor()); };
        std::vector<float> left (32768), right (32768);

        const auto m = playAndMeasure (
            song,
            [&]
            {
                getPlayed (song, saturator).getScope().setListening (true);
                played().listenToBand();
            },
            [&]
            {
                auto* tap = getPlayed (song, saturator).getScope().getOutput();
                REQUIRE (tap != nullptr);
                CHECK (tap->pull (left.data(), right.data(), (int) left.size()) > 0);
            });
        report ("Phazite's saturator on the synth's chords, its Band's edges swept and its Quality automated by lanes, its editor's "
                "scope listening and Listen on",
                m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's filter on the synth's chords, driven, its cutoff swept and its Character and Quality "
               "automated")
    {
        using phazite::devices::FilterEffect;
        TestSong song;
        const auto track = song.firstTrack;
        song.setInstrument (track, phazite::devices::Synth::typeID);
        addChords (song, track);
        const auto filter = song.addEffect (track, FilterEffect::typeID);

        // A low-pass near singing, driven, at half its mix; its cutoff swept by a lane, a cycle a beat; its Character switched
        // every two beats and its Quality moved from 8x to 4x and 2x and back, a beat each, so that its ways fade into each
        // other all the time.
        for (const auto& [id, value] :
             { std::pair<const char*, float> { "resonance", 0.97f }, { "drive", 18.0f }, { "quality", 2.0f }, { "mix", 0.5f } })
            song.setParameter (filter, id, value);

        sweepParameter (song, { filter, "cutoff" }, 200.0f, 4000.0f);

        for (const auto& [id, value] : { std::pair<const char*, int> { "character", 2 }, { "quality", 1 } })
        {
            const phazite::ParameterRef parameter { filter, id };
            const auto range = phazite::getControlTravel (getSong (song), parameter);
            REQUIRE (range.has_value());

            for (int beat = 0; beat < 8; ++beat)
            {
                const auto choice = value == 2 ? (float) ((beat / 2) % 2) : (float) (2 - beat % 3);
                const auto position = phazite::toControlPosition (*range, choice);
                phazite::addPoint (getSong (song), parameter, { phazite::BeatPosition::fromBeats (beat), position, 0.0f });
                phazite::addPoint (getSong (song), parameter, { phazite::BeatPosition::fromBeats (beat + 0.9), position, 0.0f });
            }
        }

        const auto m = playAndMeasure (song, [&song, filter] { getPlayed (song, filter).getScope().setListening (true); });
        report ("Phazite's filter on the synth's chords, driven near singing, its cutoff swept and its Character and Quality "
                "automated by lanes, its editor's display listening",
                m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's Age on the synth's chords, everything on, its medium and each part's switch automated, "
               "its band swept, with its editor listening")
    {
        using phazite::devices::Age;
        TestSong song;
        const auto track = song.firstTrack;
        song.setInstrument (track, phazite::devices::Synth::typeID);
        addChords (song, track);
        const auto age = song.addEffect (track, Age::typeID);

        // A deep wow and flutter, the drive, the lows in mono, hiss and dense crackle, dropouts and a head bump; its high cut
        // swept by a lane, a cycle a beat, so that its band glides all the time.
        for (const auto& [id, value] : { std::pair<const char*, float> { "wow", 20.0f },
                                         { "flutter", 8.0f },
                                         { "distortion", 0.8f },
                                         { "headBump", 4.0f },
                                         { "bassMono", 200.0f },
                                         { "hiss", -50.0f },
                                         { "crackle", -18.0f },
                                         { "density", 300.0f },
                                         { "dropouts", 1.0f } })
            song.setParameter (age, id, value);

        sweepParameter (song, { age, "highCut" }, 2000.0f, 16000.0f);

        // Its medium moved by a lane from the record to the tape and back, and each part switched off and on, each on beats of
        // its own, so that the media and the parts fade in and out all along.
        const auto steps = [&song, age] (const char* id, int every, float first = 1.0f)
        {
            const phazite::ParameterRef parameter { age, id };
            const auto range = phazite::getControlTravel (getSong (song), parameter);
            REQUIRE (range.has_value());

            for (int beat = 0; beat < 8; ++beat)
            {
                const auto position = phazite::toControlPosition (*range, (beat / every) % 2 == 0 ? first : 1.0f - first);
                phazite::addPoint (getSong (song), parameter, { phazite::BeatPosition::fromBeats (beat), position, 0.0f });
                phazite::addPoint (getSong (song), parameter, { phazite::BeatPosition::fromBeats (beat + 0.9), position, 0.0f });
            }
        };

        steps ("medium", 2, 0.0f); // the record first
        steps ("wobble", 1);
        steps ("noise", 3);
        steps ("drive", 1);
        steps ("stereo", 3);
        steps ("band", 2);

        // As its open editor has it: its scope and its pitch tapped, its clicks and dropouts counted.
        const auto played = [&song, age]() -> auto& { return dynamic_cast<Age&> (getPlayed (song, age).getProcessor()); };
        std::vector<float> readings (4096);

        const auto m = playAndMeasure (
            song,
            [&]
            {
                getPlayed (song, age).getScope().setListening (true);
                played().getPitchTap().setListening (true);
                played().getSurfaceNoise().takeClicks();
                played().getTapeWear().takeDropouts();
            },
            [&]
            {
                CHECK (played().getPitchTap().pull (readings.data(), (int) readings.size()) > 0);
                CHECK (played().getSurfaceNoise().takeClicks() > 10); // the record crackled
                played().getTapeWear().takeDropouts();
            });
        report ("Phazite's Age on the synth's chords, everything on, its medium and each part's switch automated, its high cut swept "
                "by a lane, its editor's displays listening",
                m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's delay on the synth's chords, everything on, its times, Freeze, Character and Transition "
               "automated")
    {
        using phazite::devices::Delay;
        TestSong song;
        const auto track = song.firstTrack;
        song.setInstrument (track, phazite::devices::Synth::typeID);
        addChords (song, track);
        const auto delay = song.addEffect (track, Delay::typeID);

        // Each side its own time, bouncing, a feedback over 100 %, the modulation and the duck at work; both times swept by
        // lanes, a cycle a beat, so that they glide or fade all the time.
        for (const auto& [id, value] : { std::pair<const char*, float> { "link", 0.0f },
                                         { "pingPong", 1.0f },
                                         { "feedback", 1.05f },
                                         { "modDepth", 0.6f },
                                         { "modRate", 3.0f },
                                         { "duck", 0.6f } })
            song.setParameter (delay, id, value);

        sweepParameter (song, { delay, "time" }, 80.0f, 400.0f);
        sweepParameter (song, { delay, "timeRight" }, 120.0f, 600.0f);

        // Sync, the Freeze, the Character and the Transition switched by lanes, each on beats of its own, so that the times
        // move between note values and milliseconds and the switches crossfade all along.
        const auto steps = [&song, delay] (const char* id, int every)
        {
            const phazite::ParameterRef parameter { delay, id };
            const auto range = phazite::getControlTravel (getSong (song), parameter);
            REQUIRE (range.has_value());

            for (int beat = 0; beat < 8; ++beat)
            {
                const auto position = phazite::toControlPosition (*range, (beat / every) % 2 == 0 ? 0.0f : 1.0f);
                phazite::addPoint (getSong (song), parameter, { phazite::BeatPosition::fromBeats (beat), position, 0.0f });
                phazite::addPoint (getSong (song), parameter, { phazite::BeatPosition::fromBeats (beat + 0.9), position, 0.0f });
            }
        };

        steps ("sync", 4);
        steps ("freeze", 3);
        steps ("character", 2);
        steps ("transition", 1);

        const auto m = playAndMeasure (song, [&song, delay] { getPlayed (song, delay).getScope().setListening (true); });
        report ("Phazite's delay on the synth's chords, everything on, its times swept by lanes, Sync, Freeze, Character and "
                "Transition automated, its editor's display listening",
                m);
        CHECK (m.peakLevel > 0.001f);
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
               phazite::devices::Reverb::typeID, phazite::devices::Motion::typeID, phazite::devices::Limiter::typeID })
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

    TEST_CASE ("Real-time audit: Phazite's synth's comb, swept by an envelope at each note, chords a sixteenth apart")
    {
        using phazite::devices::Synth;
        TestSong song;
        const auto track = song.firstTrack;
        const auto wavetable = song.setInstrument (track, Synth::typeID);
        song.setParameter (wavetable, "filterType", (float) phazite::devices::dsp::FilterMode::combPlus);
        song.setParameter (wavetable, "resonance", 0.8f);
        song.setParameter (wavetable, "unisonVoices", 3.0f); // four
        song.setParameter (wavetable, "mod1Amount", 0.5f);   // its first slot: Envelope 2 opens the cutoff, here by four octaves
        addChords (song, track);

        const auto m = playAndMeasure (song);
        report ("Phazite's synth's comb, swept by an envelope at each note, chords a sixteenth apart", m);
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

    TEST_CASE ("Real-time audit: Phazite's synth's sample and hold, swept by an envelope at each note, chords a sixteenth apart")
    {
        using phazite::devices::Synth;
        TestSong song;
        const auto track = song.firstTrack;
        const auto wavetable = song.setInstrument (track, Synth::typeID);
        song.setParameter (wavetable, "filterType", (float) phazite::devices::dsp::FilterMode::sampleAndHold);
        song.setParameter (wavetable, "resonance", 0.8f);
        song.setParameter (wavetable, "unisonVoices", 3.0f); // four
        song.setParameter (wavetable, "mod1Amount", 0.5f);   // its first slot: Envelope 2 opens the cutoff, here by four octaves
        addChords (song, track);

        const auto m = playAndMeasure (song);
        report ("Phazite's synth's sample and hold, swept by an envelope at each note, chords a sixteenth apart", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: a beat on Phazite's drum sampler, its hats choking each other, its snare in two layers in turn, its "
               "kick on a track of its own, a pad played from its editor")
    {
        using phazite::devices::DrumSampler;
        TestSong song;
        const auto track = song.firstTrack;
        const auto sampler = song.setInstrument (track, DrumSampler::typeID);

        // The snare's second layer a clap, the two in turn; and the kick on a track of its own (step 4.12).
        {
            auto& model = getSong (song);
            phazite::setDeviceSound (model, sampler, phazite::schema::getPadSound (38, 1), "909 Clap");
            phazite::setValueSetByHand (model, phazite::ParameterRef { sampler, "pad38LayerMode" },
                                        (float) DrumSampler::LayerMode::roundRobin);
            REQUIRE (phazite::givePadItsOwnTrack (model, sampler, 36).isValid());
        }

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
            dynamic_cast<DrumSampler&> (getPlayed (song, sampler).getProcessor()).audition (39, 1.0f);
        });
        report ("A beat on Phazite's drum sampler, its hats choking each other, its snare in two layers in turn, its kick on a "
                "track of its own, a pad played from its editor",
                m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's sampler, a crash chopped into reversed slices that choke each other, an 808 gliding "
               "in mono through its filter, a loop crossfading under more notes than voices, a seamless sound plucked from "
               "random starts with times and a filter that follow the key and the velocity, a slice played from its editor")
    {
        using phazite::devices::Sampler;
        using phazite::test::at, phazite::test::lasting;
        TestSong song;
        auto& played = getSong (song);

        // Sixteen equal slices of a crash, reversed, one a sixteenth, each cutting off the one before.
        const auto chops = song.firstTrack;
        const auto slicer = song.setInstrument (chops, Sampler::typeID);
        phazite::setFactorySample (played, slicer, phazite::devices::findFactorySample ("808 Cymbal"));
        song.setParameter (slicer, "mode", (float) Sampler::Mode::slice);
        song.setParameter (slicer, "sliceBy", (float) Sampler::SliceBy::equal);
        song.setParameter (slicer, "sliceCount", 16.0f);
        song.setParameter (slicer, "reverse", 1.0f);
        const auto chopped = phazite::test::addMidiClip (played, chops, { at (0.0), at (8.0) }, "Chops");
        REQUIRE (chopped.isValid());

        for (int i = 0; i < 32; ++i)
            phazite::test::addMidiNote (played, chopped, Sampler::firstSliceNote + (i * 5) % 16, at (i * 0.25), lasting (0.25), 100);

        // The 808 it starts with, in mono, gliding between overlapping notes through its filter and its envelope, the LFO
        // on its pitch.
        const auto bass = song.addTrack ("808");
        const auto glider = song.setInstrument (bass, Sampler::typeID);
        song.setParameter (glider, "voiceMode", 1.0f);
        song.setParameter (glider, "glide", 80.0f);
        song.setParameter (glider, "filterOn", 1.0f);
        song.setParameter (glider, "filterEnvelopeOn", 1.0f);
        song.setParameter (glider, "lfoOn", 1.0f);
        const auto line = phazite::test::addMidiClip (played, bass, { at (0.0), at (8.0) }, "Line");
        REQUIRE (line.isValid());

        for (int i = 0; i < 16; ++i)
            phazite::test::addMidiNote (played, line, 36 + (i * 7) % 12, at (i * 0.5), lasting (0.75), 110);

        // A loop crossfading at its seam, more notes held than voices, so that some are taken for others.
        const auto pad = song.addTrack ("Loop");
        const auto looper = song.setInstrument (pad, Sampler::typeID);
        phazite::setFactorySample (played, looper, phazite::devices::findFactorySample ("808 Mid Tom"));
        song.setParameter (looper, "loopOn", 1.0f);
        song.setParameter (looper, "loopStart", 0.2f);
        song.setParameter (looper, "loopEnd", 0.6f);
        song.setParameter (looper, "loopCrossfade", 0.5f);
        song.setParameter (looper, "release", 400.0f);
        const auto held = phazite::test::addMidiClip (played, pad, { at (0.0), at (8.0) }, "Held");
        REQUIRE (held.isValid());

        for (int i = 0; i < 20; ++i)
            phazite::test::addMidiNote (played, held, 60 + i, at (i * 0.125), lasting (4.0), 90);

        // The sound made from its spectrum, one seamless loop, plucked in sixteenths up and down the keys, each note from a
        // random start (read round the loop's end from its octaves), its times by its key and its filter by its velocity.
        const auto arp = song.addTrack ("Wide");
        const auto plucker = song.setInstrument (arp, Sampler::typeID);
        phazite::setFactorySample (played, plucker, phazite::devices::findFactorySample ("Wide Saw"));
        song.setParameter (plucker, "startRandom", 1.0f);
        song.setParameter (plucker, "keyTime", 1.0f);
        song.setParameter (plucker, "filterOn", 1.0f);
        song.setParameter (plucker, "filterVelocity", 0.5f);
        song.setParameter (plucker, "decay", 250.0f);
        song.setParameter (plucker, "sustain", 0.0f);
        const auto plucked = phazite::test::addMidiClip (played, arp, { at (0.0), at (8.0) }, "Plucks");
        REQUIRE (plucked.isValid());

        for (int i = 0; i < 32; ++i)
            phazite::test::addMidiNote (played, plucked, 45 + (i * 7) % 48, at (i * 0.25), lasting (0.25), 40 + (i * 29) % 88);

        const auto m = playAndMeasure (song, [&song, slicer] { // a slice, as a press on the drawing plays it
            dynamic_cast<Sampler&> (getPlayed (song, slicer).getProcessor()).audition (Sampler::firstSliceNote + 3, 1.0f);
        });
        report ("Phazite's sampler, a crash chopped into reversed slices that choke each other, an 808 gliding in mono through "
                "its filter, a loop crossfading under more notes than voices, a seamless sound plucked from random starts with "
                "times and a filter that follow the key and the velocity, a slice played from its editor",
                m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's FM synth, a stack at full feedback, chords a sixteenth apart, its LFO on Timbre, a "
               "pitch envelope, and four operators side by side, one fixed, one fed back toward a square, spread, gliding in mono")
    {
        using phazite::devices::FmSynth;
        TestSong song;

        // The stack, every operator deep, its feedback at full, the LFO moving every modulator.
        const auto keys = song.firstTrack;
        const auto stack = song.setInstrument (keys, FmSynth::typeID);
        song.setParameter (stack, "algorithm", 0.0f);
        song.setParameter (stack, "op4Feedback", 1.0f);
        song.setParameter (stack, "timbre", 1.5f);
        song.setParameter (stack, "op2Wave", (float) FmSynth::Wave::saw);
        song.setParameter (stack, "pitchAmount", 12.0f);
        song.setParameter (stack, "filterOn", 1.0f);
        song.setParameter (stack, "filterEnvelope", 30.0f); // its filter opened by its envelope on every note, and by velocity
        song.setParameter (stack, "filterDecay", 120.0f);
        song.setParameter (stack, "filterVelocity", 0.5f);
        song.setParameter (stack, "lfoOn", 1.0f);
        song.setParameter (stack, "lfoTarget", (float) FmSynth::LfoTarget::timbre);
        song.setParameter (stack, "phase", (float) FmSynth::Phase::free);
        addChords (song, keys);

        // Four side by side, one at a fixed frequency, one fed back toward a square, each note spread, in mono, gliding.
        const auto bass = song.addTrack ("FM Bass");
        const auto organ = song.setInstrument (bass, FmSynth::typeID);
        song.setParameter (organ, "algorithm", 7.0f);
        song.setParameter (organ, "op3Fixed", 1.0f);
        song.setParameter (organ, "op3Frequency", 2000.0f);
        song.setParameter (organ, "op1Feedback", -1.0f);
        song.setParameter (organ, "op2Phase", 90.0f); // each note's operators from their own places in their waves
        song.setParameter (organ, "op4Phase", 180.0f);
        song.setParameter (organ, "spread", 12.0f);
        song.setParameter (organ, "voiceMode", 1.0f);
        song.setParameter (organ, "glide", 60.0f);
        using phazite::test::at, phazite::test::lasting;
        const auto line = phazite::test::addMidiClip (getSong (song), bass, { at (0.0), at (8.0) }, "Line");
        REQUIRE (line.isValid());

        for (int i = 0; i < 16; ++i)
            phazite::test::addMidiNote (getSong (song), line, 36 + (i * 5) % 12, at (i * 0.5), lasting (0.75), 110);

        const auto m = playAndMeasure (song);
        report ("Phazite's FM synth, a stack at full feedback, chords a sixteenth apart, its LFO on Timbre, a pitch envelope, and "
                "four operators side by side, one fixed, one fed back toward a square, spread, gliding in mono",
                m);
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

    TEST_CASE ("Real-time audit: Phazite's reverb as a plate on a return, large and fully diffused, with an 8-second decay")
    {
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);

        const auto reverbReturn = song.addReturn (track, phazite::devices::Reverb::typeID);
        const auto effects = phazite::getEffects (getSong (song), reverbReturn);
        REQUIRE (effects.size() == 1);
        song.setParameter (effects.front(), "mode", (float) phazite::devices::Reverb::plate);
        song.setParameter (effects.front(), "decay", 8.0f);
        song.setParameter (effects.front(), "size", 1.0f);
        song.setParameter (effects.front(), "diffusion", 1.0f);
        song.setParameter (effects.front(), "highCut", 3000.0f);
        song.setParameter (effects.front(), "highCutSlope", 0.0f);

        const auto m = playAndMeasure (song);
        report ("Phazite's reverb as a plate on a return, large and fully diffused, with an 8-second decay", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's reverb on a return as a large hall, its mode moved by a lane through the room, the plate "
               "and the hall")
    {
        TestSong song;
        const auto track = song.firstTrack;
        addTone (song, track);

        const auto reverbReturn = song.addReturn (track, phazite::devices::Reverb::typeID);
        const auto effects = phazite::getEffects (getSong (song), reverbReturn);
        REQUIRE (effects.size() == 1);
        song.setParameter (effects.front(), "mode", (float) phazite::devices::Reverb::hall);
        song.setParameter (effects.front(), "decay", 8.0f);
        song.setParameter (effects.front(), "size", 1.0f);
        song.setParameter (effects.front(), "diffusion", 1.0f);

        // A beat each in the room, the plate and the hall, and round again, so that its modes fade into each other all the time.
        const phazite::ParameterRef mode { effects.front(), "mode" };
        const auto range = phazite::getControlTravel (getSong (song), mode);
        REQUIRE (range.has_value());

        for (int beat = 0; beat < 8; ++beat)
        {
            const auto position = phazite::toControlPosition (*range, (float) (beat % 3));
            phazite::addPoint (getSong (song), mode, { phazite::BeatPosition::fromBeats (beat), position, 0.0f });
            phazite::addPoint (getSong (song), mode, { phazite::BeatPosition::fromBeats (beat + 0.9), position, 0.0f });
        }

        const auto m = playAndMeasure (song);
        report ("Phazite's reverb on a return as a large hall, its mode moved by a lane through the room, the plate and the hall", m);
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
        REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.renders.finished.isEmpty(); }));
        REQUIRE (song.renders.finished[0].endsWith (": ok"));
        REQUIRE (phazite::isFrozen (song.context, track));

        const auto m = playAndMeasure (song);
        report ("A frozen track: its synth's notes, played from its frozen file", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: a version's bounce heard in place of the song, both measured as they play")
    {
        // As Hear It plays it (docs/design/versions.md: Hearing a version): the bounce read from its file after the master, at the
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

    TEST_CASE ("Real-time audit: audio clips of a file at another rate than the engine's, resampled as they play")
    {
        // A 44.1 kHz file on the 48 kHz engine, through r8brain-free-src (src/engine/audio/Resampler.h), on two tracks.
        SineFile sine (2.0, 44100.0);
        TestSong song;
        addSineClip (song, song.firstTrack, sine);
        addSineClip (song, song.addTrack ("Second"), sine);

        const auto m = playAndMeasure (song);
        report ("Audio clips of a 44.1 kHz file, resampled", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
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
        REQUIRE (phazite::isAudioClip (played, clip));
        REQUIRE (phazite::getClipTempo (played, clip) == doctest::Approx (120.0));

        // Phazite renders the stretched loop to a file in the background, and its engine plays that, so the audio thread
        // only reads a file; until it is ready, the clip is silent.
        song.waitForClipRenders();
        REQUIRE (phazite::isRendered (played, clip));
        REQUIRE (phazite::getAudioFileInfo (phazite::getPlayedFile (played, clip)).isValid());

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
        REQUIRE (phazite::getAudioFileInfo (phazite::getPlayedFile (played, clip)).isValid());

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

        const auto m = playProjectAndMeasure (song.context,
                                              [&]
                                              {
                                                  REQUIRE (previewer->play (loop, song.context));
                                                  REQUIRE (previewer->isInTime());
                                                  REQUIRE (phazite::test::runTimersUntil ([&previews] { return previews.isPlaying(); }));
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

    //==============================================================================
    // Edited while it plays, as the user edits: each edit made on the message thread between two blocks (Edit), the song's
    // new plan handed to the engine, and every block after the first measured. A block is 256 samples at 48 kHz: a beat at
    // 120 BPM is about 94 blocks.

    TEST_CASE ("Real-time audit: edited while it plays: a parameter and a fader turned, a note added, a clip moved, undo and redo")
    {
        TestSong song;
        const auto keys = song.firstTrack;
        song.setInstrument (keys, phazite::devices::Synth::typeID);
        const auto filter = song.addEffect (keys, phazite::devices::FilterEffect::typeID);
        const auto chords = song.addMidiClip (keys, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (chords, { 60 + beat % 3, 64, 67 }, beat, 0.75);

        const auto bass = song.addTrack ("Bass");
        song.setInstrument (bass, phazite::devices::Synth::typeID);
        const auto line = song.addMidiClip (bass, 8.0, 4.0);
        song.addNote (line, 36, 8.0, 2.0);
        song.addNote (line, 43, 10.0, 2.0);
        auto& played = getSong (song);

        const auto m = playProjectAndMeasure (
            song.context, Playing { .edits = {
                                        { 10, "the filter's cutoff turned by hand", [&] { song.setParameter (filter, "cutoff", 900.0f); } },
                                        { 20, "the bass track's fader moved", [&] { song.setVolume (bass, -9.0f); } },
                                        { 30, "a note added just ahead of the playhead", [&] { song.addNote (chords, 72, 0.5, 0.25); } },
                                        { 50, "the bass clip moved under the playhead",
                                          [&] { phazite::moveClip (song.context, line, phazite::BeatPosition::fromBeats (0.5), bass); } },
                                        { 70, "undone", [&] { phazite::undo (played); } },
                                        { 80, "redone", [&] { phazite::redo (played); } },
                                        { 90, "undone twice",
                                          [&]
                                          {
                                              phazite::undo (played);
                                              phazite::undo (played);
                                          } },
                                        { 110, "the cutoff turned again", [&] { song.setParameter (filter, "cutoff", 4000.0f); } },
                                    } });
        report ("Edited while it plays: values, notes, a clip, undo and redo", m);
        CHECK (m.peakLevel > 0.01f);                                                   // it plays
        CHECK (phazite::getNotes (song.context, std::vector { chords }).size() == 48); // the note undone
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: edited while it plays: notes moved, deleted, 200 transposed at once, one edited through an alias "
               "on another track, a controller's value added, and a note edit undone and redone")
    {
        // Each edit hands the engine a new plan of the notes, and the old one goes on the message thread: the audio thread
        // plays on through each without allocating or freeing. A block is 256 samples at 48 kHz, so the playhead is at about
        // a hundredth of a beat a block: the edits are made under it and just ahead of it.
        using phazite::test::at, phazite::test::lasting;
        TestSong song;
        auto& played = getSong (song);
        const auto keys = song.firstTrack;
        song.setInstrument (keys, phazite::devices::Synth::typeID);
        const auto chords = song.addMidiClip (keys, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (chords, { 60 + beat % 3, 64, 67 }, beat * 0.5, 0.4);

        // A pad of 200 short notes, all through the first beats.
        const auto pad = song.addTrack ("Pad");
        song.setInstrument (pad, phazite::devices::Synth::typeID);
        const auto many = song.addMidiClip (pad, 0.0, 16.0);

        for (int i = 0; i < 200; ++i)
            song.addNote (many, 48 + i % 24, i * 0.02, 0.1, 60);

        // A bass line, and an alias of it that plays on another track.
        const auto bass = song.addTrack ("Bass");
        song.setInstrument (bass, phazite::devices::Synth::typeID);
        const auto line = song.addMidiClip (bass, 0.0, 8.0);

        for (int beat = 0; beat < 8; ++beat)
            song.addNote (line, 36 + beat % 4, beat * 0.5, 0.25);

        const auto lead = song.addTrack ("Lead");
        song.setInstrument (lead, phazite::devices::Synth::typeID);
        const auto alias = song.copyClip (line, 8.0, true);
        phazite::moveClip (song.context, alias, at (0.0), lead);
        REQUIRE (phazite::getClipTrack (played, alias) == lead);

        const auto chordNotes = phazite::getClipNotes (played, chords);
        const auto padNotes = phazite::getClipNotes (played, many);
        const auto aliasNotes = phazite::getClipNotes (played, alias);
        REQUIRE (chordNotes.size() == 48);
        REQUIRE (padNotes.size() == 200);
        REQUIRE (aliasNotes.size() == 8);
        std::vector<phazite::NoteRef> allPad;

        for (const auto& note : padNotes)
            allPad.push_back (note.getRef());

        const auto inStep = [&played] (const char* name, const std::function<void()>& edit)
        {
            const phazite::UndoStep step (played, name);
            edit();
        };

        const auto m = playProjectAndMeasure (
            song.context,
            Playing { .edits = {
                          { 10, "a chord's note moved to just ahead of the playhead",
                            [&] { inStep ("Move Note", [&] { phazite::moveNote (played, chordNotes[6].getRef(), at (0.3), 72); }); } },
                          { 25, "a note deleted just ahead of the playhead",
                            [&] { inStep ("Delete Note", [&] { phazite::deleteNotes (played, { chordNotes[3].getRef() }); }); } },
                          { 40, "the pad's 200 notes transposed in one step",
                            [&] { inStep ("Transpose", [&] { phazite::transposeNotes (played, allPad, 1); }); } },
                          { 55, "a controller's value added ahead of the playhead",
                            [&]
                            {
                                inStep ("Add Controller Value",
                                        [&] { phazite::addControl (played, chords, at (1.0), phazite::ControlKind::controller, 1, 100); });
                            } },
                          { 70, "a bass note moved through its alias on the lead track, under the playhead",
                            [&] { inStep ("Move Note", [&] { phazite::moveNote (played, aliasNotes[1].getRef(), at (0.75), 43); }); } },
                          { 85, "the note edit undone", [&] { phazite::undo (played); } },
                          { 100, "and redone", [&] { phazite::redo (played); } },
                          { 115, "a note's velocity set through the alias, ahead of the playhead", [&]
                            { inStep ("Set Velocity", [&] { phazite::setNoteVelocities (played, { aliasNotes[3].getRef() }, 30); }); } },
                      } });
        report ("Edited while it plays: notes moved, deleted, transposed, through an alias, a controller, undo and redo", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        CHECK (phazite::getClipNotes (played, chords).size() == 47);
        CHECK (phazite::getClipNotes (played, many)[0].pitch == padNotes[0].pitch + 1);
        CHECK (phazite::test::describeClipNotes (played, line) == phazite::test::describeClipNotes (played, alias));
        CHECK (phazite::getClipNotes (played, line)[1].pitch == 43);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: edited while it plays: a parameter turned by the MIDI controller linked to it")
    {
        TestSong song;
        const auto keys = song.firstTrack;
        song.setInstrument (keys, phazite::devices::Synth::typeID);
        const auto filter = song.addEffect (keys, phazite::devices::FilterEffect::typeID);
        const auto chords = song.addMidiClip (keys, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (chords, { 60 + beat % 3, 64, 67 }, beat, 0.75);

        // Linked as a user links it: MIDI Learn, then the knob moved, as an input's thread hands it over.
        auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
        const auto cutoff = phazite::getParameter (song.context, { filter, "cutoff" });
        phazite::startMidiLearn (cutoff);
        audio.takeMidi (juce::MidiMessage::controllerEvent (1, 74, 0));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&cutoff] { return phazite::getMidiLink (cutoff).has_value(); }));

        const auto turn = [&audio] (int value) { audio.takeMidi (juce::MidiMessage::controllerEvent (1, 74, value)); };
        auto& played = getSong (song);

        const auto m = playProjectAndMeasure (
            song.context, Playing { .edits = {
                                        { 10, "the knob turned", [&] { turn (20); } },
                                        { 20, "turned on", [&] { turn (70); } },
                                        { 30, "and on", [&] { turn (110); } },
                                        { 40, "the knob rested: its burst kept", [&] { phazite::findMidiLinker (played)->endBurst(); } },
                                        { 60, "undone", [&] { phazite::undo (played); } },
                                        { 70, "turned again", [&] { turn (40); } },
                                    } });
        report ("Edited while it plays: a parameter turned by a MIDI controller", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        CHECK (cutoff.getValue() == doctest::Approx (*phazite::getValueAtControllerPosition (played, { filter, "cutoff" }, 40)));
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: edited while it plays: effects added, removed, undone and redone, and the instrument replaced")
    {
        TestSong song;
        const auto keys = song.firstTrack;
        song.setInstrument (keys, phazite::devices::Synth::typeID);
        const auto chords = song.addMidiClip (keys, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (chords, { 60 + beat % 3, 64, 67 }, beat, 0.75);

        auto& played = getSong (song);
        phazite::DeviceID eq;

        const auto m = playProjectAndMeasure (
            song.context,
            Playing { .edits = {
                          { 10, "an EQ added after the synth", [&] { eq = song.addEffect (keys, phazite::devices::Equalizer::typeID); } },
                          { 30, "a compressor added after it", [&] { song.addEffect (keys, phazite::devices::Compressor::typeID); } },
                          { 50, "the EQ removed", [&] { phazite::removeDevice (song.context, eq); } },
                          { 70, "undone: the EQ back", [&] { phazite::undo (played); } },
                          { 90, "redone: the EQ removed again", [&] { phazite::redo (played); } },
                          { 110, "the instrument replaced by the FM synth",
                            [&] { song.setInstrument (keys, phazite::devices::FmSynth::typeID); } },
                          { 140, "undone: the synth back", [&] { phazite::undo (played); } },
                      } });
        report ("Edited while it plays: effects and the instrument", m);
        CHECK (m.peakLevel > 0.01f);                                  // it plays
        CHECK (phazite::getEffects (song.context, keys).size() == 1); // the compressor
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: edited while it plays: the EQ of a kick that keys a bass's compressor switched to Linear phase and back")
    {
        // Each switch makes the EQ anew with its new latency, and the bass's compressor waits for its key as long, or no
        // longer: a new plan that realigns the song, its wait read from what came in before.
        TestSong song;
        const auto kick = song.firstTrack;
        song.setInstrument (kick, phazite::devices::DrumSampler::typeID);
        const auto beats = song.addMidiClip (kick, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addNote (beats, 36, beat, 0.25);

        const auto eq = song.addEffect (kick, phazite::devices::Equalizer::typeID);
        const auto bass = song.addTrack ("Bass");
        song.setInstrument (bass, phazite::devices::Synth::typeID);
        song.addNote (song.addMidiClip (bass, 0.0, 16.0), 36, 0.0, 16.0);
        REQUIRE (phazite::duckUnder (song.context, bass, kick).isValid());

        const auto m = playProjectAndMeasure (
            song.context, Playing { .edits = {
                                        { 10, "the kick's EQ to Linear phase", [&] { song.setParameter (eq, "phase", 1.0f); } },
                                        { 60, "back to Minimum phase", [&] { song.setParameter (eq, "phase", 0.0f); } },
                                        { 110, "to Linear phase again", [&] { song.setParameter (eq, "phase", 1.0f); } },
                                    } });
        report ("Edited while it plays: a key's EQ switched to Linear phase and back", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: an old version opened live in the song's place while it plays, and back to Now, as A/B does")
    {
        // docs/design/versions.md, A version open: the song made the version's in place, which what plays follows as it follows an
        // edit, and back. The version's song read once before it plays (the first open reads it on the history's thread), so
        // that each open is made at its block.
        TestSong song;
        const auto keys = song.firstTrack;
        song.setInstrument (keys, phazite::devices::Synth::typeID);
        const auto chords = song.addMidiClip (keys, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (chords, { 60 + beat % 3, 64, 67 }, beat, 0.75);

        REQUIRE (phazite::test::waitForVersionHistory (song.context));
        phazite::setVersionDraft (song.context, "Keys", {});
        std::optional<juce::Result> saved;
        REQUIRE (phazite::saveVersion (song.context, [&saved] (juce::Result r) { saved = r; }));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&saved] { return saved.has_value(); }));
        REQUIRE (phazite::test::waitForVersionHistory (song.context));
        const auto version = phazite::getLastVersion (song.context)->id;

        // Now: quieter, with a bass the version hasn't.
        song.setVolume (keys, -6.0f);
        const auto bass = song.addTrack ("Bass");
        song.setInstrument (bass, phazite::devices::Synth::typeID);
        song.addNote (song.addMidiClip (bass, 0.0, 16.0), 36, 0.0, 16.0);
        const auto now = phazite::test::copyWholeSong (*phazite::findSong (song.context));

        REQUIRE (phazite::test::finished (song.context, [&] { return phazite::openVersion (song.context, version); }));
        phazite::backToNow (song.context);

        const auto m = playProjectAndMeasure (
            song.context, Playing { .edits = {
                                        { 10, "the version opened", [&] { REQUIRE (phazite::openVersion (song.context, version)); } },
                                        { 40, "back to Now", [&] { phazite::backToNow (song.context); } },
                                        { 70, "A/B: the version again", [&] { phazite::toggleCompare (song.context); } },
                                        { 100, "A/B: Now again", [&] { phazite::toggleCompare (song.context); } },
                                    } });
        report ("A version opened and closed while it plays", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        CHECK (phazite::getOpenVersion (song.context).isEmpty());
        CHECK (phazite::test::copyWholeSong (*phazite::findSong (song.context)).isEquivalentTo (now));
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: edited while it plays: a short loop wrapping, moved, turned off, and the playhead moved")
    {
        using phazite::BeatPosition;
        TestSong song;
        const auto keys = song.firstTrack;
        song.setInstrument (keys, phazite::devices::Synth::typeID);
        const auto chords = song.addMidiClip (keys, 0.0, 16.0);

        for (int i = 0; i < 64; ++i)
            song.addChord (chords, { 60 + i % 3, 64, 67 }, i * 0.25, 0.2);

        // And an audio clip of a file at 44.1 kHz, which the engine resamples, its spare resampling made ready for the loop's
        // start each time round and taking the first's place there (known bug 85).
        song.addAudioClip (song.addTrack ("Pad"), phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Pad.wav"), 10.0), 0.0);

        auto& played = getSong (song);

        // And controllers, the bend and the keys' pressure in the chords, which each time round, each jump and the start
        // chase (Engine.h).
        for (const auto& [beat, kind, number, value] :
             { std::tuple (0.05, phazite::ControlKind::controller, 1, 90), std::tuple (0.1, phazite::ControlKind::pitchWheel, 0, 10000),
               std::tuple (0.15, phazite::ControlKind::channelPressure, 0, 70), std::tuple (0.15, phazite::ControlKind::aftertouch, 64, 60),
               std::tuple (2.1, phazite::ControlKind::controller, 64, 127), std::tuple (5.0, phazite::ControlKind::controller, 1, 20) })
            phazite::addControl (played, chords, BeatPosition::fromBeats (beat), kind, number, value);

        const auto m = playProjectAndMeasure (
            song.context,
            Playing {
                .edits = {
                    { 10, "a quarter-beat loop around the playhead, wrapping every 23 blocks",
                      [&] { phazite::loopBeats (played, { BeatPosition(), BeatPosition::fromBeats (0.25) }); } },
                    { 80, "the loop moved later",
                      [&] { phazite::loopBeats (played, { BeatPosition::fromBeats (2.0), BeatPosition::fromBeats (2.5) }); } },
                    { 150, "looping turned off", [&] { phazite::toggleLoop (played); } },
                    { 170, "the playhead moved while it plays", [&] { phazite::movePlayhead (played, BeatPosition::fromBeats (6.0)); } },
                } });
        report ("Edited while it plays: the loop and the playhead", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        CHECK_FALSE (phazite::isLoopOn (played));
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: edited while it plays: a track muted and soloed out in the middle of its notes and heard again, and a "
               "device with latency added and removed, the mix faded across the delays it changes")
    {
        // A muted track plays on, its channel silencing it (known bug 78), so that its held chord, and its reverb's tail, are
        // heard at once as it is heard again. A limiter (its look-ahead a latency) on one track changes the delays that line
        // the others up with it: the engine keeps the plan before playing while what goes out fades out, then is silent while
        // the new delays fill, and fades in (known bug 167).
        TestSong song;
        const auto keys = song.firstTrack;
        song.setInstrument (keys, phazite::devices::Synth::typeID);
        song.addEffect (keys, phazite::devices::Reverb::typeID);
        const auto chords = song.addMidiClip (keys, 0.0, 16.0);

        for (int bar = 0; bar < 4; ++bar)
            song.addChord (chords, { 60 + bar % 3, 64, 67 }, bar * 4.0, 3.5);

        const auto bass = song.addTrack ("Bass");
        song.setInstrument (bass, phazite::devices::Synth::typeID);
        const auto line = song.addMidiClip (bass, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addNote (line, 36 + beat % 2 * 7, beat, 0.9);

        auto& played = getSong (song);
        phazite::DeviceID limiter, latency, send;

        // For the everyday edits: a return, a bus of a track of its own, and a third track to delete.
        const auto reverb = phazite::addReturnTrack (played, "Reverb", nullptr);
        const auto member = song.addTrack ("Member");
        const auto bus = song.addBus ("Bus", { member });
        const auto spare = song.addTrack ("Spare");
        song.setInstrument (spare, phazite::devices::Synth::typeID);
        song.addNote (song.addMidiClip (spare, 0.0, 16.0), 48, 0.0, 16.0);

        const auto m = playProjectAndMeasure (
            song.context,
            Playing {
                .edits = {
                    { 20, "the chords' track muted as a chord sounds", [&] { phazite::toggleMute (song.context, keys); } },
                    { 40, "heard again in the middle of the chord", [&] { phazite::toggleMute (song.context, keys); } },
                    { 60, "the bass soloed, the chords silenced", [&] { phazite::toggleSolo (song.context, bass); } },
                    { 80, "the solo ended", [&] { phazite::toggleSolo (song.context, bass); } },
                    { 100, "a limiter added to the bass, the chords delayed to line up with it",
                      [&] { limiter = song.addEffect (bass, phazite::devices::Limiter::typeID); } },
                    { 140, "the limiter removed, the delays as they were", [&] { phazite::removeDevice (song.context, limiter); } },
                    { 180, "undone: the limiter back", [&] { phazite::undo (played); } },
                    { 220, "a latency of 1000 samples on the chords' track, the bass's path delayed within its ring",
                      [&]
                      {
                          latency = phazite::test::addTestDevice (played, keys, phazite::test::TestLatency::typeID);
                          phazite::setValueSetByHand (played, phazite::ParameterRef { latency, "samples" }, 1000.0f);
                      } },
                    { 260, "made 30,000: the bass's path past its ring, on a longer one that reads the past from it",
                      [&] { song.setParameter (latency, "samples", 30000.0f); } },
                    { 300, "a send added from the chords to the return (a path that comes)",
                      [&]
                      {
                          const phazite::UndoStep step (played, "Add Send");
                          send = phazite::addSend (played, keys, reverb, false);
                      } },
                    { 320, "the send switched before the fader (a path goes, another comes)",
                      [&]
                      {
                          const phazite::UndoStep step (played, "Change Send");
                          phazite::setPreFader (played, send, true);
                      } },
                    { 340, "the send taken away (a path goes)",
                      [&]
                      {
                          const phazite::UndoStep step (played, "Remove Send");
                          phazite::removeSend (played, send);
                      } },
                    { 360, "the bass moved into the bus",
                      [&]
                      {
                          const phazite::UndoStep step (played, "Move");
                          phazite::moveTrackIntoFolder (played, bass, bus);
                      } },
                    { 380, "a track deleted as it plays a note",
                      [&]
                      {
                          const phazite::UndoStep step (played, "Delete Track");
                          phazite::deleteTracks (played, { spare }, nullptr);
                      } },
                    { 400, "undone: the track back (paths that come)", [&] { phazite::undo (played); } },
                    { 420,
                      "the track deleted and the chords' latency made 4,000 samples longer (over 15 blocks) at once: the mix dipped until the "
                      "paths that grew read nothing again from before the leaving paths' fades ended, and back after",
                      [&]
                      {
                          const phazite::UndoStep step (played, "Delete Track");
                          phazite::deleteTracks (played, { spare }, nullptr);
                          phazite::setValueSetByHand (played, phazite::ParameterRef { latency, "samples" }, 34000.0f);
                      } },
                    // (The return's path grew by 30,000 samples as the send to it went at block 340, its sound holding the
                    // send's: the mix is held silent about that long after it and the edits that come within it (each moving the
                    // leave on), and the plan's new devices start then, each after its own latency: back by block 680.)
                    { 700, "nothing: the mix back after the holds", [] {} },
                } });
        report ("Edited while it plays: mute and solo in the middle of notes, a latency added and removed, and grown past a ring", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        CHECK (phazite::getEffects (song.context, bass).size() == 1);
        CHECK (m.lastLevel > 0.01f); // the mix back after the holds (each track after its own latency)
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: edited while it plays: Age's Mix moved through 100 % and back, and again after resting there")
    {
        // Its copy for the mix goes on for a second after the Mix reaches 100 %, then rests; as the Mix leaves 100 % after a
        // rest, the copy is made again in that block from what came in lately (Age.h).
        using phazite::devices::Age;
        TestSong song;
        const auto keys = song.firstTrack;
        song.setInstrument (keys, phazite::devices::Synth::typeID);
        const auto chords = song.addMidiClip (keys, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (chords, { 60 + beat % 3, 64, 67 }, beat, 0.75);

        const auto age = song.addEffect (keys, Age::typeID);
        song.setParameter (age, "bassMono", 100.0f);

        const auto m = playProjectAndMeasure (
            song.context,
            Playing { .edits = {
                          { 10, "the Mix at 50 %", [&] { song.setParameter (age, "mix", 0.5f); } },
                          { 30, "the Mix at 100 %", [&] { song.setParameter (age, "mix", 1.0f); } },
                          { 60, "back to 50 % within the second: the copy went on", [&] { song.setParameter (age, "mix", 0.5f); } },
                          { 80, "at 100 % again", [&] { song.setParameter (age, "mix", 1.0f); } },
                          { 300, "back to 50 % after resting there: the copy made again", [&] { song.setParameter (age, "mix", 0.5f); } },
                      } });
        report ("Edited while it plays: Age's Mix through 100 % and back", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        CHECK (dynamic_cast<Age&> (getPlayed (song, age).getProcessor()).getDryRebuilds() >= 2);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: edited while it plays: a send's lane and the LFO on it, a point added, the lane redrawn, set by hand, "
               "deleted and undone")
    {
        // Known bug 192: a send's level follows its lane and its track's modulators on the control points, the curve handed over
        // with each plan and the level's ramp carried from block to block in the send's own state.
        using phazite::BeatPosition;
        TestSong song;
        const auto keys = song.firstTrack;
        song.setInstrument (keys, phazite::devices::Synth::typeID);
        const auto chords = song.addMidiClip (keys, 0.0, 16.0);

        for (int beat = 0; beat < 16; ++beat)
            song.addChord (chords, { 60 + beat % 3, 64, 67 }, beat, 0.75);

        song.addReturn (keys, phazite::devices::Reverb::typeID, -6.0f);
        const phazite::ParameterRef level { phazite::getSends (song.context, keys).front().id, phazite::schema::sendLevelParameter };
        song.addAutomation (level,
                            { { BeatPosition(), 0.2f }, { BeatPosition::fromBeats (2.0), 0.8f }, { BeatPosition::fromBeats (4.0), 0.3f } });
        song.modulate (song.addModulator (keys, phazite::ModulatorType::lfo), level, 0.2f);
        auto& played = getSong (song);

        const auto m = playProjectAndMeasure (
            song.context,
            Playing {
                .edits = {
                    { 10, "a point added to the send's lane ahead of the playhead",
                      [&] { phazite::addAutomationPoint (song.context, level, { BeatPosition::fromBeats (1.0), 0.9f, 0.5f }); } },
                    { 30, "the lane redrawn under the playhead",
                      [&]
                      {
                          phazite::replaceAutomationPoints (
                              song.context, level, { BeatPosition(), BeatPosition::fromBeats (8.0) },
                              { { BeatPosition(), 0.7f, 0.0f }, { BeatPosition::fromBeats (8.0), 0.1f, -0.4f } });
                      } },
                    { 50, "the send's knob set by hand", [&] { phazite::getSendLevel (song.context, level.device).setValue (0.5f); } },
                    { 70, "the lane deleted", [&] { phazite::deleteAutomation (phazite::getParameter (song.context, level)); } },
                    { 90, "undone: the lane back", [&] { phazite::undo (played); } },
                    { 110, "the send switched before the fader",
                      [&]
                      {
                          const phazite::UndoStep step (played, "Change Send");
                          phazite::setPreFader (played, level.device, true);
                      } },
                } });
        report ("Edited while it plays: a send's lane and its LFO", m);
        CHECK (m.peakLevel > 0.01f); // it plays
        CHECK (phazite::getAutomationPoints (song.context, level).size() == 2);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: every synth taking all its voices at once for chords, the drum sampler taking its voices and cutting "
               "a pad, and sounds and tables swapped while they play")
    {
        // Sixteen notes held on each synth, then chords of eight on one sample a beat, each taking eight voices that fade out
        // in places of their own; thirty-two drum hits a sixteenth apart under a pad that cuts itself, then more, which take
        // voices; and the sampler's sound, the synth's table and a pad's sound swapped while they play.
        using namespace phazite::devices;
        TestSong song;
        auto& played = getSong (song);
        std::vector<phazite::DeviceID> synths;

        for (const auto* typeID : { Synth::typeID, FmSynth::typeID, Sampler::typeID })
        {
            const auto track = synths.empty() ? song.firstTrack : song.addTrack (typeID);
            synths.push_back (song.setInstrument (track, typeID));
            const auto clip = song.addMidiClip (track, 0.0, 8.0);

            for (int i = 0; i < 16; ++i)
                song.addNote (clip, 36 + i, 0.0, 8.0);

            for (int beat = 1; beat < 8; ++beat)
                song.addChord (clip, { 60, 62, 64, 65, 67, 69, 71, 72 }, beat, 0.9);
        }

        const auto drums = song.addTrack ("Drums");
        const auto sampler = song.setInstrument (drums, DrumSampler::typeID);
        phazite::setValueSetByHand (played, phazite::ParameterRef { sampler, "pad36CutItself" }, 1.0f);
        const auto beat = song.addMidiClip (drums, 0.0, 8.0);

        for (int i = 0; i < 128; ++i)
            song.addNote (beat, i % 3 == 0 ? 36 : 49 + i % 3, i * 0.0625, 0.05);

        const auto m = playProjectAndMeasure (
            song.context,
            Playing { .edits = {
                          { 30, "the sampler's sound swapped", [&] { phazite::setFactorySample (played, synths[2], 1); } },
                          { 50, "the synth's table swapped", [&] { phazite::setFactoryWavetable (played, synths[0], 0, 3); } },
                          { 70, "a drum pad's sound swapped",
                            [&] { phazite::setDeviceSound (played, sampler, phazite::schema::getPadSound (49, 0), "909 Clap"); } },
                      } });
        report ("Every synth taking all its voices at once, the drum sampler taking voices and cutting a pad, sounds swapped", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    // An effect that leaves the sound as it is and fires a JUCE assertion in one of its blocks, on whichever audio thread
    // plays it (the device's, or one of the engine's workers), when asked to.
    struct AssertingEffect final : phazite::devices::Processor
    {
        static constexpr const char* typeID = "phaziteTestAsserting";
        static constexpr phazite::devices::DeviceInfo info { .typeID = typeID,
                                                             .name = "Test Asserting",
                                                             .kind = phazite::devices::DeviceKind::effect };
        static inline std::atomic<int> blocksToAssertion { -1 }; // the blocks before the one it asserts in; none below 0

        void prepare (double, int) override {}
        void reset() override {}

        void process (const phazite::devices::ProcessContext&) override
        {
            if (blocksToAssertion.fetch_sub (1) == 0)
                jassertfalse; // on purpose
        }

        bool sleepsOnSilence() const noexcept override { return false; }
    };

    TEST_CASE ("Real-time audit: an assertion on the audio thread is the test's failure, by its place, and none of playback's allocations")
    {
        // Known bug 153: the assertion net locked and allocated on the audio thread, and what JUCE allocated for the
        // assertion's message showed here as the playback's. Now the net notes it there without either, the tests' thread
        // fails the test with its place (here it is declared, so it passes), and the audit leaves JUCE's message out.
        [[maybe_unused]] static const bool registered = []
        {
            phazite::registerDeviceType (AssertingEffect::info, []() -> std::unique_ptr<phazite::devices::Processor>
                                         { return std::make_unique<AssertingEffect>(); });
            return true;
        }();

        TestSong song;
        addTone (song, song.firstTrack);
        auto& played = getSong (song);
        phazite::test::addOwnDevice (played, song.firstTrack, AssertingEffect::typeID);

        const phazite::test::ExpectedAssertion onPurpose ("RealtimeAuditTests.cpp"); // the jassertfalse above, and only it
        AssertingEffect::blocksToAssertion = 60;                                     // after the warm-up's blocks (none)
        const auto m = playProjectAndMeasure (song.context);
        AssertingEffect::blocksToAssertion = -1;
        report ("An assertion on the audio thread", m);
        CHECK (m.peakLevel > 0.01f); // it plays

#if JUCE_LOG_ASSERTIONS
        CHECK (m.assertions == 1);
        CHECK (onPurpose.getCount() == 1);

        // JUCE made its message there, and only that, which the steady blocks leave out; where the monitor keeps no stacks
        // (under the sanitizers) it can't be told from playback's.
        if (AudioThreadMonitor::isAvailable() && AudioThreadMonitor::keepsStacks())
        {
            CHECK (m.ofAssertions.allocations > 0);
            checkSteadyPlaybackDoesNotAllocate (m);
        }
#else
        CHECK (m.assertions == 0); // no assertions in this build
#endif
    }
}
