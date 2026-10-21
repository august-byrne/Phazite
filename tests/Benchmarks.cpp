// SPDX-License-Identifier: AGPL-3.0-or-later
// Benchmarks (docs/performance-audit.md): what each device costs and how soon it sounds, how soon a live note sounds
// through Tracktion, what a project costs stopped and playing, and how long saving, opening and the UI's work take.
// ctest and CI skip them; run them from an optimised build (windows-profile or windows-release) with
//   phazite_tests.exe --test-suite=benchmark --no-skip
// and record the numbers in docs/performance-audit.md. Times depend on the machine: compare runs on the same one.
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "HostedDevice.h"
#include "TestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Maximizer.h"
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
#include "devices/wavetable/WavetableSynth.h"
#include "model/devices/internal/Devices.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/playlist/Playlist.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace
{
namespace dev = phazite::devices;
using phazite::test::HostedDevice;

constexpr double benchRate = HostedDevice::sampleRate;
constexpr int benchBlock = HostedDevice::blockSize;

double millisecondsOf (const std::function<void()>& f)
{
    const auto start = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
}

//==============================================================================
// One device, as a benchmark plays it.
struct DeviceCase
{
    // Every member has a default, so that the cases can leave any of them out (GCC's -Wmissing-field-initializers).
    juce::String name {};
    std::function<std::unique_ptr<dev::Processor>()> make {};
    const dev::DeviceInfo* info = nullptr;
    int notes = 0;                                    // for an instrument: held from the start; drums hit again every 12 blocks
    std::function<void (std::vector<float>&)> set {}; // changes to the defaults
    bool silentInput = false;                         // for an effect: silence instead of noise
};

template <typename Device>
std::function<std::unique_ptr<dev::Processor>()> maker()
{
    return [] { return std::make_unique<Device>(); };
}

std::vector<float> defaultsOf (const dev::DeviceInfo& info)
{
    std::vector<float> values;

    for (const auto& parameter : info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

bool isInstrument (const DeviceCase& c)
{
    return c.info->kind == dev::DeviceKind::instrument;
}

bool isDrums (const DeviceCase& c)
{
    return c.info == &dev::DrumSampler::info;
}

// process()'s time over 20 s of stereo audio, as a percentage of the audio's length (100 % = one core can't keep up).
double percentOfCore (const DeviceCase& c)
{
    auto processor = c.make();
    processor->prepare (benchRate, benchBlock);
    auto values = defaultsOf (*c.info);

    if (c.set)
        c.set (values);

    juce::AudioBuffer<float> buffer (2, benchBlock), noise (2, benchBlock);
    juce::Random random (7);

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < benchBlock; ++i)
            noise.setSample (channel, i, (random.nextFloat() * 2.0f - 1.0f) * 0.25f);

    std::vector<dev::NoteEvent> notes;

    for (int n = 0; n < c.notes; ++n)
        notes.push_back ({ .type = dev::NoteEvent::Type::noteOn, .sample = 0, .number = isDrums (c) ? 36 + n : 48 + n * 3, .value = 0.8f });

    const int numBlocks = juce::roundToInt (20.0 * benchRate / benchBlock);
    const int warmUp = 20;
    std::chrono::steady_clock::time_point start;

    for (int b = 0; b < warmUp + numBlocks; ++b)
    {
        if (b == warmUp)
            start = std::chrono::steady_clock::now();

        if (isInstrument (c) || c.silentInput)
            buffer.clear();
        else
            for (int channel = 0; channel < 2; ++channel)
                buffer.copyFrom (channel, 0, noise, channel, 0, benchBlock);

        const bool hit = isInstrument (c) && (b == 0 || (isDrums (c) && b % 12 == 0));
        const dev::ProcessContext context { .channels = buffer.getArrayOfWritePointers(),
                                            .numChannels = 2,
                                            .numSamples = benchBlock,
                                            .events = hit ? std::span<const dev::NoteEvent> (notes) : std::span<const dev::NoteEvent>(),
                                            .parameters = values };
        processor->process (context);
    }

    const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    return 100.0 * seconds / (numBlocks * benchBlock / benchRate);
}

// How long an instrument's note takes to be heard: the first sample above -60 dBFS, and above -30 dBFS, in ms.
std::pair<double, double> timeToSound (const DeviceCase& c)
{
    auto processor = c.make();
    processor->prepare (benchRate, benchBlock);
    const auto values = defaultsOf (*c.info);
    juce::AudioBuffer<float> buffer (2, benchBlock);
    const dev::NoteEvent note { .type = dev::NoteEvent::Type::noteOn, .sample = 0, .number = isDrums (c) ? 36 : 60, .value = 0.8f };
    double quiet = -1.0, loud = -1.0;

    for (int b = 0; b < 20 && loud < 0.0; ++b)
    {
        buffer.clear();
        const dev::ProcessContext context { .channels = buffer.getArrayOfWritePointers(),
                                            .numChannels = 2,
                                            .numSamples = benchBlock,
                                            .events =
                                                b == 0 ? std::span<const dev::NoteEvent> (&note, 1) : std::span<const dev::NoteEvent>(),
                                            .parameters = values };
        processor->process (context);

        for (int i = 0; i < benchBlock && loud < 0.0; ++i)
        {
            const auto level = std::max (std::abs (buffer.getSample (0, i)), std::abs (buffer.getSample (1, i)));
            const auto ms = (b * benchBlock + i) * 1000.0 / benchRate;

            if (quiet < 0.0 && level > 0.001f)
                quiet = ms;

            if (level > 0.0316f)
                loud = ms;
        }
    }

    return { quiet, loud };
}

//==============================================================================
std::shared_ptr<te::MidiInputDevice> findAllMidiInputs()
{
    auto& engine = phazite::test::getEngine();

    if (phazite::getAllMidiInputs (engine) == nullptr)
    {
        engine.getDeviceManager().rescanMidiDeviceList();
        phazite::test::runTimersUntil ([&engine] { return phazite::getAllMidiInputs (engine) != nullptr; });
    }

    return phazite::getAllMidiInputs (engine);
}

juce::String milliseconds (double ms, int decimals = 2)
{
    return juce::String (ms, decimals) + " ms";
}
} // namespace

//==============================================================================
TEST_CASE ("Benchmark: each device's CPU, latency and time to sound" * doctest::test_suite ("benchmark") * doctest::skip())
{
    const auto maxOf = [] (const dev::DeviceInfo& info, int index) { return info.parameters[(size_t) index].maximum; };

    const std::vector<DeviceCase> cases {
        { "Synth, 1 note", maker<dev::Synth>(), &dev::Synth::info, 1 },
        { "Synth, 8 notes", maker<dev::Synth>(), &dev::Synth::info, 8 },
        { "Synth, 16 notes, 7-voice unison", maker<dev::Synth>(), &dev::Synth::info, 16,
          [&] (std::vector<float>& v) { v[dev::Synth::unisonVoices] = maxOf (dev::Synth::info, dev::Synth::unisonVoices); } },
        { "Synth, idle", maker<dev::Synth>(), &dev::Synth::info, 0 },
        { "Wavetable, 1 note", maker<dev::WavetableSynth>(), &dev::WavetableSynth::info, 1 },
        { "Wavetable, 8 notes", maker<dev::WavetableSynth>(), &dev::WavetableSynth::info, 8 },
        { "Wavetable, 8 notes, full unison", maker<dev::WavetableSynth>(), &dev::WavetableSynth::info, 8, [&] (std::vector<float>& v)
          { v[dev::WavetableSynth::unisonVoices] = maxOf (dev::WavetableSynth::info, dev::WavetableSynth::unisonVoices); } },
        { "Wavetable, idle", maker<dev::WavetableSynth>(), &dev::WavetableSynth::info, 0 },
        { "Drum Sampler, 4 pads every 16th", maker<dev::DrumSampler>(), &dev::DrumSampler::info, 4 },
        { "Drum Sampler, idle", maker<dev::DrumSampler>(), &dev::DrumSampler::info, 0 },
        { "EQ", maker<dev::Equalizer>(), &dev::Equalizer::info },
        { "Filter", maker<dev::FilterEffect>(), &dev::FilterEffect::info },
        { "Compressor", maker<dev::Compressor>(), &dev::Compressor::info },
        { "Multiband", maker<dev::Multiband>(), &dev::Multiband::info },
        { "Limiter", maker<dev::Limiter>(), &dev::Limiter::info },
        { "Maximizer", maker<dev::Maximizer>(), &dev::Maximizer::info },
        { "Saturator", maker<dev::Saturator>(), &dev::Saturator::info },
        { "Delay", maker<dev::Delay>(), &dev::Delay::info },
        { "Reverb", maker<dev::Reverb>(), &dev::Reverb::info },
        { "Stereo Expander, spreading", maker<dev::StereoExpander>(), &dev::StereoExpander::info, 0,
          [] (std::vector<float>& v) { v[dev::StereoExpander::spread] = 1.0f; } },
        { "Phaser, six notches", maker<dev::Phaser>(), &dev::Phaser::info, 0,
          [] (std::vector<float>& v) { v[dev::Phaser::notches] = 5.0f; } },
        { "Flanger", maker<dev::Flanger>(), &dev::Flanger::info },
        { "Vocoder, 40 bands", maker<dev::Vocoder>(), &dev::Vocoder::info, 0,
          [] (std::vector<float>& v) { v[dev::Vocoder::bands] = 5.0f; } },
        { "Reverb, silence in", maker<dev::Reverb>(), &dev::Reverb::info, 0, {}, true },
        { "Multiband, silence in", maker<dev::Multiband>(), &dev::Multiband::info, 0, {}, true },
    };

    // Every one of Phazite's devices is here.
    for (const auto* info : phazite::getOwnDevices())
    {
        INFO (std::string (info->name));
        CHECK (std::any_of (cases.begin(), cases.end(), [info] (const DeviceCase& c) { return c.info == info; }));
    }

    juce::String report ("\n| Device | CPU (% of a core) | Latency | Heard after (-60 / -30 dB) |\n|---|---|---|---|\n");

    for (const auto& c : cases)
    {
        auto processor = c.make();
        processor->prepare (benchRate, benchBlock);
        const auto latency = processor->getLatencySamples();
        report << "| " << c.name << " | " << juce::String (percentOfCore (c), 3) << " | "
               << (latency == 0 ? juce::String ("0")
                                : juce::String (latency) + " samples (" + milliseconds (latency * 1000.0 / benchRate) + ")")
               << " | ";

        if (isInstrument (c) && c.notes == 1)
        {
            const auto [quiet, loud] = timeToSound (c);
            report << milliseconds (quiet) << " / " << milliseconds (loud);
        }

        report << " |\n";
    }

    MESSAGE (report);
}

TEST_CASE ("Benchmark: a live note's way through Tracktion" * doctest::test_suite ("benchmark") * doctest::skip())
{
    auto& engine = phazite::test::getEngine();
    REQUIRE (findAllMidiInputs() != nullptr);
    HostedDevice device;

    auto edit = te::Edit::createSingleTrackEdit (engine);
    edit->ensureNumberOfAudioTracks (2);
    auto* track = te::getAudioTracks (*edit)[0];
    track->pluginList.insertPlugin (edit->getPluginCache().createNewPlugin (dev::Synth::typeID, {}), 0, nullptr);
    phazite::routeLiveMidiInput (*edit);
    REQUIRE (phazite::getLiveMidiTrack (*edit) == track);
    edit->getTransport().ensureContextAllocated();
    phazite::test::settle (500);

    // Silence first, then a note played between two blocks, as the computer keyboard plays one.
    juce::String report ("\n");
    const auto playNote = [&] (const juce::String& what)
    {
        device.process (50);
        phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100));
        const auto samples = device.samplesToFirstSound();
        phazite::playIntoAllMidiInputs (engine, juce::MidiMessage::noteOff (1, 60));
        device.process (200);
        report << what << ": heard "
               << (samples < 0 ? juce::String ("never")
                               : juce::String (samples) + " samples (" + milliseconds (samples * 1000.0 / benchRate) + ")")
               << " after the next block starts\n";
    };

    playNote ("A synth on a track");

    // A limiter on another track: delay compensation holds back the track played too.
    auto limiter = edit->getPluginCache().createNewPlugin (dev::Limiter::typeID, {});
    te::getAudioTracks (*edit)[1]->pluginList.insertPlugin (limiter, 0, nullptr);
    phazite::test::settle (500);
    playNote ("With a limiter on another track");

    // Switched off, the limiter delays nothing.
    limiter->setEnabled (false);
    phazite::test::settle (500);
    playNote ("With that limiter switched off");
    limiter->setEnabled (true);

    // A limiter on the master too.
    edit->getMasterPluginList().insertPlugin (edit->getPluginCache().createNewPlugin (dev::Limiter::typeID, {}), 0, nullptr);
    phazite::test::settle (500);
    playNote ("With a limiter on the master too");

    MESSAGE (report);
    limiter = nullptr; // before its Edit
    edit = nullptr;
}

TEST_CASE ("Benchmark: a project's CPU, stopped and playing" * doctest::test_suite ("benchmark") * doctest::skip())
{
    auto& engine = phazite::test::getEngine();
    REQUIRE (findAllMidiInputs() != nullptr);
    HostedDevice device;

    const auto makeEdit = [&engine] (int numTracks, bool withDevices)
    {
        auto edit = te::Edit::createSingleTrackEdit (engine);
        edit->ensureNumberOfAudioTracks (numTracks);

        for (auto* track : te::getAudioTracks (*edit))
        {
            if (! withDevices)
                continue;

            auto& cache = edit->getPluginCache();
            track->pluginList.insertPlugin (cache.createNewPlugin (dev::Synth::typeID, {}), 0, nullptr);
            track->pluginList.insertPlugin (cache.createNewPlugin (dev::Equalizer::typeID, {}), 1, nullptr);
            track->pluginList.insertPlugin (cache.createNewPlugin (dev::Compressor::typeID, {}), 2, nullptr);
            track->pluginList.insertPlugin (cache.createNewPlugin (dev::Reverb::typeID, {}), 3, nullptr);

            // A chord on every beat for 32 beats.
            auto clip = track->insertMIDIClip (
                "Chords", edit->tempoSequence.toTime ({ te::BeatPosition(), te::BeatPosition::fromBeats (32.0) }), nullptr);

            for (int beat = 0; beat < 32; ++beat)
                for (int note : { 48, 52, 55, 59 })
                    clip->getSequence().addNote (note, te::BeatPosition::fromBeats (beat), te::BeatDuration::fromBeats (0.9), 100, 0,
                                                 nullptr);
        }

        phazite::routeLiveMidiInput (*edit);
        edit->getTransport().ensureContextAllocated();
        return edit;
    };

    juce::String report ("\n| Project (one audio thread) | Stopped | Playing |\n|---|---|---|\n");

    for (const auto& [numTracks, withDevices] : { std::pair (20, false), std::pair (20, true) })
    {
        auto edit = makeEdit (numTracks, withDevices);
        phazite::test::settle (800);
        const auto stopped = device.percentOfRealTime (500);
        edit->getTransport().play (false);
        phazite::test::settle (500);
        const auto playing = device.percentOfRealTime (500);
        edit->getTransport().stop (false, false);
        phazite::test::settle (300);
        report << "| " << numTracks << (withDevices ? " tracks, each a synth, an EQ, a compressor and a reverb, chords" : " empty tracks")
               << " | " << juce::String (stopped, 2) << " % | " << juce::String (playing, 2) << " % |\n";
        edit = nullptr;
        phazite::test::settle (300);
    }

    MESSAGE (report);
}

TEST_CASE ("Benchmark: saving and opening a big project" * doctest::test_suite ("benchmark") * doctest::skip())
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Big", error);
    REQUIRE (project != nullptr);
    auto& edit = project->getEdit();
    edit.ensureNumberOfAudioTracks (40);

    // 40 tracks, each a synth, an EQ and a compressor, and 16 bars of 16th notes: 10,240 notes.
    for (auto* track : te::getAudioTracks (edit))
    {
        auto& cache = edit.getPluginCache();
        track->pluginList.insertPlugin (cache.createNewPlugin (dev::Synth::typeID, {}), 0, nullptr);
        track->pluginList.insertPlugin (cache.createNewPlugin (dev::Equalizer::typeID, {}), 1, nullptr);
        track->pluginList.insertPlugin (cache.createNewPlugin (dev::Compressor::typeID, {}), 2, nullptr);
        auto clip = track->insertMIDIClip ("Notes", edit.tempoSequence.toTime ({ te::BeatPosition(), te::BeatPosition::fromBeats (64.0) }),
                                           nullptr);

        for (int step = 0; step < 256; ++step)
            clip->getSequence().addNote (48 + step % 24, te::BeatPosition::fromBeats (step * 0.25), te::BeatDuration::fromBeats (0.2), 100,
                                         0, nullptr);
    }

    phazite::test::settle (500);
    juce::String report ("\n");
    report << "Save (40 tracks, 10,240 notes): " << milliseconds (millisecondsOf ([&] { REQUIRE (project->save().wasOk()); }), 1);
    report << " (file " << juce::String (project->getFile().getSize() / 1024) << " KB)\n";
    phazite::test::settle (300);
    report << "Save again: " << milliseconds (millisecondsOf ([&] { REQUIRE (project->save().wasOk()); }), 1) << "\n";

    te::getAudioTracks (edit)[0]->setName ("Changed");
    phazite::test::settle (100);
    report << "Autosave: " << milliseconds (millisecondsOf ([&] { REQUIRE (project->autosave().wasOk()); }), 1) << "\n";

    const auto file = project->getFile();
    REQUIRE (project->save().wasOk());
    project = nullptr;
    phazite::test::settle (300);
    std::unique_ptr<phazite::Project> reopened;
    report << "Open: "
           << milliseconds (millisecondsOf ([&] { reopened = phazite::Project::open (phazite::test::getEngine(), file, false, error); }), 1)
           << "\n";
    REQUIRE (reopened != nullptr);
    MESSAGE (report);
}

TEST_CASE ("Benchmark: the UI's work on a big song" * doctest::test_suite ("benchmark") * doctest::skip())
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    auto* edit = getEdit (screen.session.getContext());
    REQUIRE (edit != nullptr);

    // 16 tracks with a synth, each 16 clips of 4 bars with 64 notes: 256 clips and 16,384 notes.
    edit->ensureNumberOfAudioTracks (16);
    te::MidiClip* firstClip = nullptr;

    for (auto* track : te::getAudioTracks (*edit))
    {
        if (phazite::getInstrument (*track) == nullptr)
            track->pluginList.insertPlugin (edit->getPluginCache().createNewPlugin (dev::Synth::typeID, {}), 0, nullptr);

        for (int c = 0; c < 16; ++c)
        {
            const auto start = te::BeatPosition::fromBeats (c * 16.0);
            auto clip =
                track->insertMIDIClip ("Part", edit->tempoSequence.toTime ({ start, start + te::BeatDuration::fromBeats (16.0) }), nullptr);

            for (int n = 0; n < 64; ++n)
                clip->getSequence().addNote (48 + (n * 7) % 24, te::BeatPosition::fromBeats (n * 0.25), te::BeatDuration::fromBeats (0.2),
                                             100, 0, nullptr);

            if (firstClip == nullptr)
                firstClip = clip.get();
        }
    }

    // The first clip selected, so that the piano roll shows it, with the other tracks' notes as ghosts.
    REQUIRE (firstClip != nullptr);
    getSelection (screen.session.getContext()).selectOnly (firstClip);
    phazite::test::dispatchMessagesUntil ([] { return false; }, 1500);
    juce::String report ("\n| The UI's work, 16 tracks, 256 clips, 16,384 notes | Time |\n|---|---|\n");

    // A parameter's change, as a knob's drag makes one at each mouse move, and what the views do with it.
    te::AutomatableParameter::Ptr cutoff;

    for (auto* track : te::getAudioTracks (*edit))
        for (auto* plugin : track->pluginList.getPlugins())
            if (cutoff == nullptr && plugin->getPluginType() == juce::String (dev::Synth::typeID))
                cutoff = plugin->getAutomatableParameterByID ("cutoff");

    REQUIRE (cutoff != nullptr);
    double total = 0.0;
    const int changes = 20;

    for (int i = 0; i < changes; ++i)
    {
        phazite::test::dispatchPendingMessages();
        cutoff->setParameter (1000.0f + 100.0f * (float) i, juce::sendNotification);
        total += millisecondsOf ([] { phazite::test::dispatchPendingMessages(); });
    }

    report << "| A parameter's change, as the views handle it | " << milliseconds (total / changes) << " |\n";

    // Painting, into a software image: the app draws with Direct2D, which fills pixels differently but walks the same
    // notes and clips. (createComponentSnapshot() makes a Direct2D image on Windows, which costs ~170 ms by itself.)
    const auto paint = [] (juce::Component& c, juce::Rectangle<int> area)
    {
        juce::Image image (juce::Image::ARGB, juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()), true,
                           juce::SoftwareImageType());
        juce::Graphics g (image);
        g.setOrigin (-area.getPosition());
        g.reduceClipRegion (area);
        return millisecondsOf ([&] { c.paintEntireComponent (g, false); });
    };

    const auto bounds = screen.shell.getLocalBounds();
    report << "| The whole window painted | " << milliseconds (paint (screen.shell, bounds), 1) << " |\n";

    const std::function<juce::Component*(juce::Component&, const std::function<bool (juce::Component&)>&)> find =
        [&find] (juce::Component& parent, const std::function<bool (juce::Component&)>& is) -> juce::Component*
    {
        for (auto* child : parent.getChildren())
        {
            if (is (*child))
                return child;

            if (auto* found = find (*child, is))
                return found;
        }

        return nullptr;
    };

    const auto columnOf = [&paint] (juce::Component& c)
    {
        double sum = 0.0;

        for (int i = 0; i < 10; ++i)
            sum += paint (c, { 200 + i * 20, 0, 11, c.getHeight() });

        return sum / 10;
    };

    report << "| An 11-px column of the window, as a playhead repaints | " << milliseconds (columnOf (screen.shell)) << " |\n";

    if (auto* playlist = find (screen.shell, [] (juce::Component& c) { return dynamic_cast<phazite::ui::Playlist*> (&c) != nullptr; }))
        report << "| ... of the playlist | " << milliseconds (columnOf (*playlist)) << " |\n";

    if (auto* pianoRoll = find (screen.shell, [] (juce::Component& c) { return dynamic_cast<phazite::ui::PianoRoll*> (&c) != nullptr; }))
        report << "| ... of the piano roll | " << milliseconds (columnOf (*pianoRoll)) << " |\n";

    // Saved, then a notification that changes nothing: the comparison with the saved copy goes through everything.
    auto* project = screen.session.getProject();
    REQUIRE (project != nullptr);
    REQUIRE (project->save().wasOk());
    phazite::test::dispatchMessagesUntil ([] { return false; }, 300);
    te::getAudioTracks (*edit)[5]->state.sendPropertyChangeMessage (te::IDs::name);
    report << "| The check for unsaved changes after a notification that changes nothing | "
           << milliseconds (millisecondsOf ([&] { project->hasUnsavedChanges(); })) << " |\n";
    report << "| Save | " << milliseconds (millisecondsOf ([&] { REQUIRE (project->save().wasOk()); }), 1) << " |\n";

    // Autosave: what the UI thread does (the devices' states and a copy of the song), the rest on a background thread.
    te::getAudioTracks (*edit)[3]->setName ("Changed");
    phazite::test::dispatchPendingMessages();
    report << "| Autosave, on the UI thread | " << milliseconds (millisecondsOf ([&] { project->autosaveInBackground(); }), 1) << " |\n";
    phazite::test::runTimersUntil ([project] { return ! project->isAutosaving(); }, 10000);

    // A note moved, as a drag moves it a grid step at a time: the views and Tracktion's rebuild of the playback graph.
    edit->getTransport().ensureContextAllocated();
    phazite::test::dispatchMessagesUntil ([] { return false; }, 300);
    auto* note = firstClip->getSequence().getNote (0);
    REQUIRE (note != nullptr);
    double moving = 0.0;

    for (int step = 1; step <= 10; ++step)
        moving += millisecondsOf (
            [&]
            {
                note->setStartAndLength (te::BeatPosition::fromBeats (0.25 * step), note->getLengthBeats(), &edit->getUndoManager());
                phazite::test::dispatchMessagesUntil ([] { return false; }, 30); // Tracktion rebuilds on a timer
            });

    report << "| A note moved a step, with what follows (the views, the graph's rebuild) | " << milliseconds (moving / 10 - 30.0) << " |\n";

    MESSAGE (report);
}
