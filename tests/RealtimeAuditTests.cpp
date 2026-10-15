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
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/eq/Equalizer.h"
#include "model/arrangement/Import.h"
#include "model/arrangement/Tracks.h"
#include "model/devices/DevicePlugin.h"
#include "model/engine/LoudnessMeterPlugin.h"
#include "model/mixer/Mixer.h"
#include "model/mixer/Sidechain.h"

#include <chrono>
#include <thread>

using namespace tracktion::literals;
using phazite::test::AudioThreadMonitor;

namespace
{
constexpr double auditSampleRate = 48000.0;
constexpr int auditBlockSize = 256;
constexpr double warmUpSeconds = 0.25; // the first blocks after starting playback
constexpr double steadySeconds = 1.0;  // then steady playback

//==============================================================================
// A two-second stereo sine wave file, so audio clips have something to play.
class SineFile
{
public:
    SineFile()
    {
        juce::AudioBuffer<float> buffer (2, juce::roundToInt (auditSampleRate * 2.0));

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto sample = 0.25f * std::sin (juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) auditSampleRate);
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
    edit.ensureNumberOfAudioTracks (index + 1);
    return *te::getAudioTracks (edit)[index];
}

// Adds a built-in device to a track, after its other devices and before its volume and level meter.
te::Plugin& addPlugin (te::Track& track, const juce::String& type)
{
    auto plugin = track.edit.getPluginCache().createNewPlugin (type, {});
    track.pluginList.insertPlugin (plugin, juce::jmax (0, track.pluginList.size() - 2), nullptr);
    return *plugin;
}

void addSineClip (te::AudioTrack& track, const SineFile& sine)
{
    track.insertWaveClip ("Sine", sine.getFile(), { te::TimeRange (0_tp, 2_tp) }, false);
}

// Tracktion's tone generator, a sound that reads no files. Tracktion doesn't register it with the others.
te::Plugin::Ptr createToneGenerator (te::Edit& edit)
{
    [[maybe_unused]] static const bool registered = [&edit]
    {
        edit.engine.getPluginManager().createBuiltInType<te::ToneGeneratorPlugin>();
        return true;
    }();

    return edit.getPluginCache().createNewPlugin (te::ToneGeneratorPlugin::xmlTypeName, {});
}

//==============================================================================
struct Measurement
{
    AudioThreadMonitor::Counts warmUp, steady;
    juce::StringArray callSites;
    float peakLevel = 0.0f;
};

// Plays the Edit from the start in real time on a separate "audio" thread, through Tracktion's hosted
// audio device, and measures that thread. Like the app, the device has no inputs unless asked for.
Measurement playAndMeasure (te::Edit& edit, int numInputChannels = 0)
{
    auto& deviceManager = edit.engine.getDeviceManager();
    auto& audioIO = deviceManager.getHostedAudioDeviceInterface();

    te::HostedAudioDeviceInterface::Parameters params;
    params.sampleRate = auditSampleRate;
    params.blockSize = auditBlockSize;
    params.inputChannels = numInputChannels;
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

            processFor (warmUpSeconds);
            m.warmUp = AudioThreadMonitor::getCounts();

            processFor (steadySeconds);
            const auto total = AudioThreadMonitor::getCounts();
            m.steady = { total.allocations - m.warmUp.allocations, total.frees - m.warmUp.frees };
        });
    audioThread.join();

    transport.stop (false, false);
    m.callSites = AudioThreadMonitor::describeCallSites();

    // Remove the hosted device again, as Tracktion's own tests do, so later tests start without one.
    deviceManager.closeDevices();
    deviceManager.removeHostedAudioDeviceInterface();
    deviceManager.deviceManager.closeAudioDevice();

    return m;
}

void report (const juce::String& scenario, const Measurement& m)
{
    juce::String text;
    text << scenario << ": peak output " << juce::Decibels::toString (juce::Decibels::gainToDecibels (m.peakLevel)) << "\n";

    if (AudioThreadMonitor::isAvailable())
    {
        text << "  audio thread, first " << warmUpSeconds << " s: " << m.warmUp.allocations << " allocations, " << m.warmUp.frees
             << " frees\n";
        text << "  audio thread, next " << steadySeconds << " s: " << m.steady.allocations << " allocations, " << m.steady.frees
             << " frees\n";

        for (const auto& site : m.callSites)
            text << "    " << site << "\n";
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
} // namespace

//==============================================================================
TEST_SUITE ("realtime-audit")
{
    // These read no files, so their steady playback is deterministic: it must not allocate.

    TEST_CASE ("Real-time audit: one empty track")
    {
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        const auto m = playAndMeasure (*edit);
        report ("One empty track", m);
        CHECK (m.peakLevel == 0.0f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: built-in effects and a send to a reverb return")
    {
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());

        auto& track = getTrack (*edit, 0);
        addPlugin (track, te::EqualiserPlugin::xmlTypeName);
        addPlugin (track, te::CompressorPlugin::xmlTypeName);
        addPlugin (track, te::AuxSendPlugin::xmlTypeName); // bus 0

        auto& returnTrack = getTrack (*edit, 1);
        addPlugin (returnTrack, te::AuxReturnPlugin::xmlTypeName); // bus 0
        addPlugin (returnTrack, te::ReverbPlugin::xmlTypeName);

        const auto m = playAndMeasure (*edit);
        report ("Built-in EQ and compressor, and a send to a reverb return", m);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: volume automation and an LFO on pan")
    {
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        auto* volume = track.getVolumePlugin();
        REQUIRE (volume != nullptr);

        auto& curve = volume->volParam->getCurve();
        curve.addPoint (0_tp, 0.5f, 0.0f, nullptr);
        curve.addPoint (1_tp, 0.8f, 0.0f, nullptr);
        curve.addPoint (2_tp, 0.5f, 0.0f, nullptr);

        auto lfo = track.getModifierList()->insertModifier (juce::ValueTree (te::IDs::LFO), -1, nullptr);
        REQUIRE (lfo != nullptr);
        volume->panParam->addModifier (*lfo, 0.5f, 0.0f, 0.0f);

        const auto m = playAndMeasure (*edit);
        report ("Volume automation and an LFO on pan", m);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: a mix with a bus, a reverb return and the loudness meter")
    {
        // Built with the functions the mixer uses (roadmap step 1.4), with a tone generator in place of a recording.
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        auto& tone = getTrack (*edit, 0);
        REQUIRE (phazite::insertDevice (tone, createToneGenerator (*edit)) != nullptr);

        auto* folder = phazite::insertFolder (*edit, &tone, nullptr); // a plain folder, until it becomes a bus
        REQUIRE (folder != nullptr);
        edit->moveTrack (&tone, te::TrackInsertPoint (folder, nullptr));
        phazite::makeBus (*folder);
        REQUIRE (phazite::isBus (*folder));

        auto* reverbReturn = phazite::addReturnTrack (*edit, "Reverb", nullptr);
        REQUIRE (reverbReturn != nullptr);
        REQUIRE (phazite::insertDevice (*reverbReturn, edit->getPluginCache().createNewPlugin (te::ReverbPlugin::xmlTypeName, {}))
                 != nullptr);
        REQUIRE (phazite::addSend (tone, phazite::getReturnBus (*reverbReturn), false) != nullptr);

        auto* meter = phazite::ensureLoudnessMeter (*edit);
        REQUIRE (meter != nullptr);

        const auto m = playAndMeasure (*edit);
        report ("A tone through a bus, with a send to a reverb return, into the loudness meter", m);
        CHECK (m.peakLevel > 0.01f);
        CHECK (meter->getReadings().momentaryValid); // it ran in the playback graph
        CHECK (meter->getReadings().momentaryLufs > -40.0f);
        checkSteadyPlaybackDoesNotAllocate (m);
    }

    TEST_CASE ("Real-time audit: Phazite's EQ, every band on and one automated, with its analyzer listening")
    {
        using phazite::devices::Equalizer;
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        REQUIRE (phazite::insertDevice (track, createToneGenerator (*edit)) != nullptr);

        const auto plugin = edit->getPluginCache().createNewPlugin (Equalizer::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::insertDevice (track, plugin) != nullptr);

        for (int band = 0; band < Equalizer::numBands; ++band)
        {
            device->getAutomatableParameters()[Equalizer::getParameterIndex (band, Equalizer::on)]->setParameter (1.0f,
                                                                                                                  juce::sendNotification);
            device->getAutomatableParameters()[Equalizer::getParameterIndex (band, Equalizer::gain)]->setParameter (3.0f,
                                                                                                                    juce::sendNotification);
        }

        auto& curve = device->findParameter ("band3Gain")->getCurve();
        curve.addPoint (0_tp, -12.0f, 0.0f, nullptr);
        curve.addPoint (1_tp, 12.0f, 0.0f, nullptr);

        auto& tap = dynamic_cast<Equalizer&> (device->getProcessor()).getOutputTap();
        tap.setListening (true);

        const auto m = playAndMeasure (*edit);
        report ("Phazite's EQ, every band on and one automated, with its analyzer listening", m);
        CHECK (m.peakLevel > 0.01f);
        checkSteadyPlaybackDoesNotAllocate (m);

        std::vector<float> tapped (32768);
        CHECK (tap.pull (tapped.data(), (int) tapped.size()) > 0);
    }

    TEST_CASE ("Real-time audit: Phazite's compressor, ducked by another track at 70 % mix, with its meters read")
    {
        using phazite::devices::Compressor;
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        auto& source = getTrack (*edit, 1);
        REQUIRE (phazite::insertDevice (track, createToneGenerator (*edit)) != nullptr);
        REQUIRE (phazite::insertDevice (source, createToneGenerator (*edit)) != nullptr);
        source.setMute (true); // still heard by the sidechain

        const auto plugin = edit->getPluginCache().createNewPlugin (Compressor::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::insertDevice (track, plugin) != nullptr);
        device->findParameter ("threshold")->setParameter (-40.0f, juce::sendNotification);
        device->findParameter ("mix")->setParameter (0.7f, juce::sendNotification);
        phazite::setSidechainSource (*device, &source);
        REQUIRE (phazite::getSidechainSource (*device) == &source);

        auto& compressor = dynamic_cast<Compressor&> (device->getProcessor());
        compressor.getReductionMeter().take();

        const auto m = playAndMeasure (*edit);
        report ("Phazite's compressor, ducked by another track at 70 % mix, with its meters read", m);
        CHECK (m.peakLevel > 0.001f);
        checkSteadyPlaybackDoesNotAllocate (m);
        CHECK (compressor.getReductionMeter().take() > 1.0f);
    }

    TEST_CASE ("Real-time audit: Phazite's limiter on the master, pushed 12 dB, with its meters read")
    {
        using phazite::devices::Limiter;
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        REQUIRE (phazite::insertDevice (getTrack (*edit, 0), createToneGenerator (*edit)) != nullptr);

        const auto plugin = edit->getPluginCache().createNewPlugin (Limiter::typeID, {});
        auto* device = dynamic_cast<phazite::DevicePlugin*> (plugin.get());
        REQUIRE (device != nullptr);
        REQUIRE (phazite::insertMasterDevice (*edit, plugin) != nullptr);
        device->findParameter ("gain")->setParameter (12.0f, juce::sendNotification);

        auto& limiter = dynamic_cast<Limiter&> (device->getProcessor());
        limiter.getReductionMeter().take();

        const auto m = playAndMeasure (*edit);
        report ("Phazite's limiter on the master, pushed 12 dB, with its meters read", m);
        CHECK (m.peakLevel > 0.01f);
        CHECK (m.peakLevel < juce::Decibels::decibelsToGain (-1.0f) * 1.001f); // its ceiling
        checkSteadyPlaybackDoesNotAllocate (m);
        CHECK (limiter.getReductionMeter().take() > 1.0f);
    }

    // These read audio files or use code Phazite is replacing, so they only report.

    TEST_CASE ("Real-time audit: an audio clip")
    {
        SineFile sine;
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        addSineClip (getTrack (*edit, 0), sine);

        const auto m = playAndMeasure (*edit);
        report ("An audio clip", m);
        CHECK (m.peakLevel > 0.01f);
    }

    TEST_CASE ("Real-time audit: a loop stretched to the song's tempo")
    {
        // As a loop dropped from the browser plays (roadmap step 1.5), stretched to the song's tempo with SoundTouch.
        SineFile sine; // 2 seconds: a bar at 120 BPM
        phazite::test::TemporaryFolder temp;
        const auto loop = temp.folder.getChildFile ("Loop 120 BPM.wav");
        REQUIRE (sine.getFile().copyFileTo (loop));

        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        edit->tempoSequence.getTempos()[0]->setBpm (100.0);
        auto* clip = dynamic_cast<te::WaveAudioClip*> (phazite::importFile (getTrack (*edit, 0), loop, te::BeatPosition()));
        REQUIRE (clip != nullptr);
        REQUIRE (clip->getAutoTempo());

        // Tracktion renders the stretched loop to a file in the background (a "proxy"), and plays that, so the audio
        // thread only reads a file; until it is ready, the clip is silent.
        const auto giveUpAt = juce::Time::getMillisecondCounter() + 20000;

        while (clip->getPlaybackFile().getSampleRate() <= 0.0 && juce::Time::getMillisecondCounter() < giveUpAt)
        {
            phazite::test::dispatchPendingMessages(); // the render finishing comes as a message
            juce::Thread::sleep (5);
        }

        REQUIRE (clip->getPlaybackFile().getSampleRate() > 0.0);

        const auto m = playAndMeasure (*edit);
        report ("A loop at 120 BPM stretched to 100 BPM, from its rendered proxy", m);
        CHECK (m.peakLevel > 0.01f);
    }

    TEST_CASE ("Real-time audit: a loop previewed in time")
    {
        // The browser's preview (src/model/engine/Preview.h): Tracktion stretches the loop as it plays, without a proxy.
        SineFile sine;
        phazite::test::TemporaryFolder temp;
        const auto loop = temp.folder.getChildFile ("Loop 120 BPM.wav");
        REQUIRE (sine.getFile().copyFileTo (loop));

        auto project = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        project->tempoSequence.getTempos()[0]->setBpm (100.0);
        auto matched = false;
        auto preview = te::Edit::createEditForPreviewingFile (phazite::test::getEngine(), loop, project.get(), true, false, &matched, {},
                                                              {}, false, {});
        REQUIRE (preview != nullptr);
        REQUIRE (matched);
        preview->getTransport().looping = true;

        const auto m = playAndMeasure (*preview);
        report ("A loop at 120 BPM previewed at 100 BPM, stretched as it plays", m);
        CHECK (m.peakLevel > 0.01f);
    }

    TEST_CASE ("Real-time audit: two tracks in a folder bus")
    {
        SineFile sine;
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());

        auto& first = getTrack (*edit, 0);
        auto& second = getTrack (*edit, 1);
        addSineClip (first, sine);
        addSineClip (second, sine);

        auto folder = edit->insertNewFolderTrack (te::TrackInsertPoint (nullptr, nullptr), nullptr, true);
        REQUIRE (folder != nullptr);
        edit->moveTrack (&first, te::TrackInsertPoint (folder.get(), nullptr));
        edit->moveTrack (&second, te::TrackInsertPoint (folder.get(), &first));
        addPlugin (*folder, te::CompressorPlugin::xmlTypeName);

        const auto m = playAndMeasure (*edit);
        report ("Two tracks in a folder bus with a compressor", m);
        CHECK (m.peakLevel > 0.01f);
    }

    TEST_CASE ("Real-time audit: MIDI into Tracktion's 4OSC synth")
    {
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        auto& track = getTrack (*edit, 0);
        track.pluginList.insertPlugin (edit->getPluginCache().createNewPlugin (te::FourOscPlugin::xmlTypeName, {}), 0, nullptr);

        auto clip = track.insertMIDIClip ("Notes", edit->tempoSequence.toTime ({ 0_bp, 8_bp }), nullptr);
        REQUIRE (clip != nullptr);

        for (int i = 0; i < 8; ++i)
            clip->getSequence().addNote (48 + i * 2, te::BeatPosition::fromBeats (i * 0.5), te::BeatDuration::fromBeats (0.4), 100, 0,
                                         nullptr);

        const auto m = playAndMeasure (*edit);
        report ("MIDI into Tracktion's 4OSC synth", m);
        CHECK (m.peakLevel > 0.01f);
    }

    // Phazite opens no audio inputs by default; this shows what happens once a user turns one on
    // (audio recording comes in roadmap step 3.4).
    TEST_CASE ("Real-time audit: two enabled audio inputs")
    {
        auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
        const auto m = playAndMeasure (*edit, 2);
        report ("One empty track, with two audio inputs enabled", m);
    }
}
