// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's drum sampler (src/devices/drums/DrumSampler.h): its pads' one-shots, level, pan, tuning, length and choke
// groups, sound files swapped in as it plays, and the pads played from its editor; and in a project
// (src/model/devices/internal/DrumKit.h), putting a file on a pad as an undo step, which the project keeps a copy of, and the
// piano roll's rows named after the pads.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "devices/drums/DrumSampler.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/DrumSamplerPlugin.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"

#include <cmath>
#include <vector>

namespace
{
namespace devices = phazite::devices;
using devices::DrumSampler;
using devices::NoteEvent;
constexpr double testRate = 48000.0; // the kit's own rate, so it plays sample for sample
constexpr int testBlockSize = 256;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : DrumSampler::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

void set (std::vector<float>& values, int pad, DrumSampler::Field field, float value)
{
    values[(size_t) DrumSampler::getParameterIndex (pad, field)] = value;
}

NoteEvent noteOn (int sample, int note, float velocity = 1.0f)
{
    return { NoteEvent::Type::noteOn, sample, note, velocity };
}

struct Stereo
{
    std::vector<float> left, right;
};

// Plays the sampler in blocks, with events at their samples from the start; beforeBlock (the samples done) is called
// before each block. The song plays in the blocks that begin before playingUntil.
template <typename BeforeBlock>
Stereo play (DrumSampler& sampler, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events,
             BeforeBlock beforeBlock, int playingUntil = 0)
{
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    Stereo played;
    std::vector<NoteEvent> inBlock;

    for (int done = 0; done < numSamples; done += testBlockSize)
    {
        const auto length = std::min (testBlockSize, numSamples - done);
        beforeBlock (done);
        buffer.clear();
        inBlock.clear();

        for (auto event : events)
        {
            if (event.sample >= done && event.sample < done + length)
            {
                event.sample -= done;
                inBlock.push_back (event);
            }
        }

        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext block;
        block.channels = channels;
        block.numChannels = 2;
        block.numSamples = length;
        block.parameters = values;
        block.events = inBlock;
        block.isPlaying = done < playingUntil;
        sampler.process (block);

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

Stereo play (DrumSampler& sampler, const std::vector<float>& values, int numSamples, const std::vector<NoteEvent>& events)
{
    return play (sampler, values, numSamples, events, [] (int) {});
}

float peakOf (const std::vector<float>& sound, size_t from, size_t to)
{
    auto peak = 0.0f;

    for (auto i = from; i < to && i < sound.size(); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}

// The last sample that isn't silent, plus one.
size_t soundingUntil (const std::vector<float>& sound)
{
    for (auto i = sound.size(); i-- > 0;)
        if (std::abs (sound[i]) > 1.0e-6f)
            return i + 1;

    return 0;
}

std::unique_ptr<DrumSampler::Sound> makeSine (double frequency, double seconds, double rate)
{
    auto sound = std::make_unique<DrumSampler::Sound>();
    sound->sampleRate = rate;
    sound->audio.setSize (1, (int) (seconds * rate));

    for (int i = 0; i < sound->audio.getNumSamples(); ++i)
        sound->audio.setSample (0, i, (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * frequency * i / rate)));

    return sound;
}

void writeWav (const juce::File& file, double seconds)
{
    const auto sine = makeSine (440.0, seconds, 44100.0);
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    const auto options = juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (24);
    auto writer = juce::WavAudioFormat().createWriterFor (stream, options);
    REQUIRE (writer != nullptr);
    REQUIRE (writer->writeFromAudioSampleBuffer (sine->audio, 0, sine->audio.getNumSamples()));
}
} // namespace

TEST_CASE ("The drum sampler's description holds together, with General MIDI's drums on its pads and the hats choking each other")
{
    REQUIRE (devices::isValid (DrumSampler::info));
    CHECK (DrumSampler::info.kind == devices::DeviceKind::instrument);
    CHECK (DrumSampler::info.parameters.size() == (size_t) DrumSampler::outputParameter + 1);
    CHECK (devices::findParameter (DrumSampler::info, "pad16Choke") == DrumSampler::getParameterIndex (15, DrumSampler::choke));
    CHECK (DrumSampler::getKitName (0) == "Kick");
    CHECK (DrumSampler::getKitName (6) == "Closed Hat");

    const auto values = getDefaults();
    CHECK (values[(size_t) DrumSampler::getParameterIndex (6, DrumSampler::choke)] == doctest::Approx (1.0f));
    CHECK (values[(size_t) DrumSampler::getParameterIndex (10, DrumSampler::choke)] == doctest::Approx (1.0f));
    CHECK (values[(size_t) DrumSampler::getParameterIndex (0, DrumSampler::choke)] == doctest::Approx (0.0f));

    for (int pad = 0; pad < DrumSampler::numPads; ++pad)
        CHECK (DrumSampler::getKitSound (pad).audio.getNumSamples() > 0);
}

TEST_CASE ("A drum pad plays its sound once, whole, however short its note")
{
    DrumSampler sampler;
    sampler.prepare (testRate, testBlockSize);
    const auto& kick = DrumSampler::getKitSound (0).audio;
    const auto played = play (sampler, getDefaults(), 48000, { noteOn (0, 36), { NoteEvent::Type::noteOff, 10, 36, 0.0f } });

    // The kit's kick, sample for sample, then silence.
    auto differs = 0.0f;

    for (int i = 0; i < kick.getNumSamples(); ++i)
        differs = std::max (differs, std::abs (played.left[(size_t) i] - kick.getSample (0, i)));

    CHECK (differs < 1.0e-5f);
    CHECK (soundingUntil (played.left) <= (size_t) kick.getNumSamples());
    CHECK (soundingUntil (played.left) > (size_t) kick.getNumSamples() - 4800);
}

TEST_CASE ("A drum pad's level, pan, velocity, tuning and length shape what it plays")
{
    const auto& kick = DrumSampler::getKitSound (0).audio;
    const auto loudest = kick.getMagnitude (0, kick.getNumSamples());

    auto values = getDefaults();
    set (values, 0, DrumSampler::level, -6.0f);
    set (values, 0, DrumSampler::pan, -1.0f); // all the way left
    DrumSampler sampler;
    sampler.prepare (testRate, testBlockSize);
    auto played = play (sampler, values, 48000, { noteOn (0, 36, 0.5f) });
    CHECK (peakOf (played.left, 0, 48000) == doctest::Approx (loudest * 0.5f * 0.5f).epsilon (0.01));
    CHECK (peakOf (played.right, 0, 48000) < 1.0e-6f);

    // An octave up plays twice as fast, so it ends in half the time.
    values = getDefaults();
    set (values, 0, DrumSampler::tune, 12.0f);
    DrumSampler tuned;
    tuned.prepare (testRate, testBlockSize);
    played = play (tuned, values, 48000, { noteOn (0, 36) });
    CHECK (std::abs ((double) soundingUntil (played.left) - kick.getNumSamples() / 2.0) < 4800.0);

    // A 50 ms length cuts it short, fading out over 10 ms.
    values = getDefaults();
    set (values, 0, DrumSampler::length, 50.0f);
    DrumSampler cut;
    cut.prepare (testRate, testBlockSize);
    played = play (cut, values, 48000, { noteOn (0, 36) });
    CHECK (soundingUntil (played.left) <= (size_t) (0.06 * testRate) + 1);
    CHECK (peakOf (played.left, 0, 2400) > 0.1f);
}

TEST_CASE ("A closed hat chokes the open one, as the two are in one choke group")
{
    const auto openHatTail = [] (bool closeIt)
    {
        DrumSampler sampler;
        sampler.prepare (testRate, testBlockSize);
        std::vector<NoteEvent> events { noteOn (0, 46) }; // the open hat

        if (closeIt)
            events.push_back (noteOn (4800, 42)); // the closed hat, 100 ms later

        const auto played = play (sampler, getDefaults(), 24000, events);
        return peakOf (played.left, 14400, 24000); // from 0.3 s, after the closed hat is over
    };

    CHECK (openHatTail (false) > 0.01f);
    CHECK (openHatTail (true) < 1.0e-6f);
}

TEST_CASE ("A drum pad plays a sound of its own at its pitch, swapped in as the sampler plays, and the editor can play pads")
{
    DrumSampler sampler;
    sampler.prepare (testRate, testBlockSize);
    CHECK_FALSE (sampler.hasOwnSound (0));

    // A 1 kHz sine at 44.1 kHz still sounds at 1 kHz.
    sampler.setSound (0, makeSine (1000.0, 0.5, 44100.0));
    CHECK (sampler.hasOwnSound (0));
    auto played = play (sampler, getDefaults(), 24000, { noteOn (0, 36) });
    auto crossings = 0;

    for (size_t i = 1; i < 19200; ++i)
        if (played.left[i - 1] < 0.0f && played.left[i] >= 0.0f)
            ++crossings;

    CHECK (std::abs (crossings - 400) <= 1); // 0.4 s of 1 kHz

    // A new sound while the old one plays: the old one stops at the next block, and the new one plays from the next hit.
    played = play (sampler, getDefaults(), 9600, { noteOn (0, 36), noteOn (4800, 36) },
                   [&sampler] (int done)
                   {
                       if (done == 2560)
                           sampler.setSound (0, nullptr); // back to the kick
                   });
    CHECK (peakOf (played.left, 2560, 4800) < 1.0e-6f);
    CHECK (peakOf (played.left, 4800, 9600) > 0.1f);
    CHECK_FALSE (sampler.hasOwnSound (0));

    // The editor plays a pad at the next block.
    sampler.getPadMeter (2).take();
    sampler.audition (2, 0.8f);
    played = play (sampler, getDefaults(), 4800, {});
    CHECK (peakOf (played.left, 0, 4800) > 0.1f);
    CHECK (sampler.getPadMeter (2).take() == doctest::Approx (0.8f));
}

TEST_CASE ("A sound file put on a drum pad is one undo step, kept in the project's Audio folder, and plays when the project opens")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder project, elsewhere;
    const auto projectFile = project.folder.getChildFile ("Song.tracktionedit");
    const auto sample = elsewhere.folder.getChildFile ("My Kick.wav");
    writeWav (sample, 0.2);

    {
        auto edit = te::createEmptyEdit (engine, projectFile);
        auto* track = te::getAudioTracks (*edit)[0];
        auto plugin = edit->getPluginCache().createNewPlugin (DrumSampler::typeID, {});
        REQUIRE (phazite::setInstrument (*track, plugin) != nullptr);
        REQUIRE (phazite::isDrumSampler (*plugin));
        CHECK (phazite::getPadName (*plugin, 0) == juce::String ("Kick"));

        // The track's drum rows are the pads, named after what they play.
        CHECK (phazite::isDrumTrack (*track));
        auto rows = phazite::getSamplerSounds (*track);
        REQUIRE (rows.has_value());
        REQUIRE (rows->size() == 16);
        CHECK ((*rows)[0].note == 36);
        CHECK ((*rows)[2].name == juce::String ("Snare"));

        // A file that isn't a sound changes nothing.
        const auto text = elsewhere.folder.getChildFile ("Notes.txt");
        REQUIRE (text.replaceWithText ("not a sound"));
        CHECK_FALSE (phazite::setPadSound (*plugin, 0, text));

        REQUIRE (phazite::setPadSound (*plugin, 0, sample));
        CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Put My Kick on Pad 1"));
        CHECK (plugin->state[phazite::schema::getPadSound (0)].toString() == juce::String ("Audio/My Kick.wav"));
        CHECK (project.folder.getChildFile ("Audio/My Kick.wav").existsAsFile());
        CHECK (phazite::getPadName (*plugin, 0) == juce::String ("My Kick"));
        CHECK (phazite::getSamplerSounds (*track)->front().name == juce::String ("My Kick"));
        auto& sampler = dynamic_cast<phazite::DrumSamplerPlugin&> (*plugin).getSampler();
        CHECK (sampler.hasOwnSound (0));

        phazite::undo (*edit);
        CHECK (phazite::getPadName (*plugin, 0) == juce::String ("Kick"));
        CHECK_FALSE (sampler.hasOwnSound (0));
        phazite::redo (*edit);
        CHECK (sampler.hasOwnSound (0));

        te::EditFileOperations (*edit).save (false, true, false);
    }

    // Opened again, the pad plays its file.
    auto edit = phazite::loadEdit (engine, projectFile);
    REQUIRE (edit != nullptr);
    auto* drums = dynamic_cast<phazite::DrumSamplerPlugin*> (phazite::getInstrument (*te::getAudioTracks (*edit)[0]));
    REQUIRE (drums != nullptr);
    CHECK (drums->getSampler().hasOwnSound (0));
    CHECK (drums->getPadName (0) == juce::String ("My Kick"));

    // Back to the kit's sound, as an undo step.
    phazite::clearPadSound (*drums, 0);
    CHECK (phazite::describeUndo (edit.get()) == juce::String ("Undo Reset Pad 1"));
    CHECK_FALSE (drums->getSampler().hasOwnSound (0));
}

TEST_CASE ("Stopping playback cuts the hits that had only just begun, and lets the others ring")
{
    // A hit, then all notes off at 0.5 s: from 20 ms after it, what is left of the hit.
    const auto leftAfter = [] (int note, int hitAt, bool stopped)
    {
        constexpr int offAt = 24064; // a block's start
        DrumSampler sampler;
        sampler.prepare (testRate, testBlockSize);
        const auto played = play (
            sampler, getDefaults(), 48000, { noteOn (hitAt, note), { NoteEvent::Type::allNotesOff, offAt, 0, 0.0f } }, [] (int) {},
            stopped ? offAt : 48000);
        return peakOf (played.left, offAt + 960, offAt + 9600);
    };

    // The open hat begun 5 ms before Stop, as the loop's first hit is when Stop comes as it goes round: gone.
    CHECK (leftAfter (46, 24064 - 240, true) < 1.0e-6f);

    // A jump while playing lets it ring, and so does Stop a hit begun long before (the crash, 0.5 s before).
    CHECK (leftAfter (46, 24064 - 240, false) > 0.01f);
    CHECK (leftAfter (49, 0, true) > 0.01f);
}
