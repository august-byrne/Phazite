// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's drum sampler (src/devices/drums/DrumSampler.h): a pad on every note, the default kit on sixteen of them; its
// pads' one-shots and gated hits, level, pan, velocity, tuning, start, reverse, envelope, sweep, filter, mute, solo and
// choke groups, sound files swapped in as it plays, and the pads played from its editor; and in a project
// (src/model/devices/internal/DrumKit.h), putting a file on a pad as an undo step, which the project keeps a copy of, and the
// piano roll's rows named after the pads that play.
#include "GoldenFiles.h"
#include "TestClips.h"
#include "TestDevices.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "devices/drums/DrumSampler.h"
#include "devices/drums/FactoryDrums.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/internal/DeviceHandle.h"
#include "model/devices/internal/DrumKit.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

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

// A sound that rises from 0 to 1 along its length, at the tests' rate: where a pad reads it shows in what it plays.
std::unique_ptr<DrumSampler::Sound> makeRamp (int frames)
{
    auto sound = std::make_unique<DrumSampler::Sound>();
    sound->sampleRate = testRate;
    sound->audio.setSize (1, frames);

    for (int i = 0; i < frames; ++i)
        sound->audio.setSample (0, i, (float) i / (float) frames);

    return sound;
}

// How many times a sound crosses 0 going up, between two samples.
int countCrossings (const std::vector<float>& sound, size_t from, size_t to)
{
    auto crossings = 0;

    for (auto i = std::max<size_t> (from, 1); i < to && i < sound.size(); ++i)
        if (sound[i - 1] < 0.0f && sound[i] >= 0.0f)
            ++crossings;

    return crossings;
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

TEST_CASE ("The drum sampler's description holds together, a pad on every note, General MIDI's drums on its kit's and the hats "
           "choking each other")
{
    REQUIRE (devices::isValid (DrumSampler::info));
    CHECK (DrumSampler::info.kind == devices::DeviceKind::instrument);
    CHECK (DrumSampler::info.parameters.size() == (size_t) DrumSampler::outputParameter + 1);
    CHECK (DrumSampler::info.parameters.size() == 128 * 25 + 1);
    CHECK (DrumSampler::info.parameters[(size_t) DrumSampler::getParameterIndex (36, DrumSampler::level)].id == "pad36Level");
    CHECK (DrumSampler::info.parameters[(size_t) DrumSampler::getParameterIndex (127, DrumSampler::sweepTime)].name
           == "Pad 127 Sweep Time");

    // Found by their IDs at once, as a search through them all would find them.
    CHECK (devices::findParameter (DrumSampler::info, "pad51Choke") == DrumSampler::getParameterIndex (51, DrumSampler::choke));
    CHECK (devices::findParameter (DrumSampler::info, "pad0Level") == 0);
    CHECK (devices::findParameter (DrumSampler::info, "output") == DrumSampler::outputParameter);

    for (const auto* missing : { "pad036Level", "pad128Level", "pad36", "pad36Lev", "pad", "Level", "pad-1Level", "pad36Levels" })
    {
        CAPTURE (std::string (missing));
        CHECK (devices::findParameter (DrumSampler::info, missing) == -1);
    }

    for (size_t i = 0; i < DrumSampler::info.parameters.size(); i += 7)
        CHECK (devices::findParameter (DrumSampler::info, DrumSampler::info.parameters[i].id) == (int) i);

    CHECK (devices::getFactoryDrumNames()[0] == "Kick"); // the default kit, in the pads' order
    CHECK (devices::getFactoryDrumNames()[6] == "Closed Hat");

    const auto values = getDefaults();
    CHECK (values[(size_t) DrumSampler::getParameterIndex (42, DrumSampler::choke)] == doctest::Approx (1.0f));
    CHECK (values[(size_t) DrumSampler::getParameterIndex (46, DrumSampler::choke)] == doctest::Approx (1.0f));
    CHECK (values[(size_t) DrumSampler::getParameterIndex (36, DrumSampler::choke)] == doctest::Approx (0.0f));

    // The default kit on its sixteen pads, from the kick; the others empty.
    for (int pad = 0; pad < DrumSampler::numPads; ++pad)
    {
        CAPTURE (pad);
        const auto inKit = pad >= DrumSampler::kitFirstNote && pad < DrumSampler::kitFirstNote + DrumSampler::kitPads;
        CHECK ((DrumSampler::getDefaultSound (pad) != nullptr) == inKit);
    }

    CHECK (DrumSampler::getDefaultSound (36) == devices::getFactoryDrum (0));
    CHECK (DrumSampler::getDefaultSound (51) == devices::getFactoryDrum (15));
}

TEST_CASE ("The drum sampler's factory sounds: the default kit, then the kits', each at -6 dB, a kit's fading out at its end")
{
    const auto names = devices::getFactoryDrumNames();
    REQUIRE (names.size() >= (size_t) DrumSampler::kitPads * 8);
    CHECK (names.size() % (size_t) DrumSampler::kitPads == 0); // kits, each a sound for every pad of the kit
    CHECK (names[15] == "Ride");
    CHECK (devices::findFactoryDrum ("808 Kick") == DrumSampler::kitPads);
    CHECK (devices::findFactoryDrum ("Cowbell 3000") == -1);

    for (size_t i = 0; i < names.size(); ++i)
    {
        CAPTURE (std::string (names[i]));
        CHECK (devices::findFactoryDrum (names[i]) == (int) i); // no name twice

        const auto sound = devices::getFactoryDrum ((int) i);
        REQUIRE (sound != nullptr);
        CHECK (sound == devices::getFactoryDrum ((int) i)); // made once, while it is held
        const auto& audio = sound->audio;
        REQUIRE (audio.getNumSamples() > 0);
        CHECK (audio.getMagnitude (0, audio.getNumSamples()) == doctest::Approx (juce::Decibels::decibelsToGain (-6.0f)).epsilon (0.001));

        auto finite = true;

        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (int at = 0; at < audio.getNumSamples(); ++at)
                finite = finite && std::isfinite (audio.getSample (channel, at));

        CHECK (finite);

        // So that it stops without a click, however loud it still was.
        if (i >= (size_t) DrumSampler::kitPads)
            for (int channel = 0; channel < audio.getNumChannels(); ++channel)
                CHECK (std::abs (audio.getSample (channel, audio.getNumSamples() - 1)) < 1.0e-6f);
    }

    // The wide kit's snares, claps, hats and cymbals in stereo, a side apart; its kick in the middle.
    for (const auto* name : { "Wide Snare", "Wide Clap", "Wide Open Hat", "Wide Crash" })
    {
        CAPTURE (std::string (name));
        const auto wide = devices::getFactoryDrum (devices::findFactoryDrum (name));
        REQUIRE (wide->audio.getNumChannels() == 2);
        auto apart = 0.0f;

        for (int at = 0; at < wide->audio.getNumSamples(); ++at)
            apart = std::max (apart, std::abs (wide->audio.getSample (0, at) - wide->audio.getSample (1, at)));

        CHECK (apart > 0.05f);
    }

    CHECK (devices::getFactoryDrum (devices::findFactoryDrum ("Wide Kick"))->audio.getNumChannels() == 1);
}

TEST_CASE ("A drum pad plays its sound once, whole, however short its note")
{
    DrumSampler sampler;
    sampler.prepare (testRate, testBlockSize);
    const auto kit = devices::getFactoryDrum (0);
    const auto& kick = kit->audio;
    const auto played = play (sampler, getDefaults(), 48000, { noteOn (0, 36), { NoteEvent::Type::noteOff, 10, 36, 0.0f } });

    // The kit's kick, sample for sample, then silence.
    auto differs = 0.0f;

    for (int i = 0; i < kick.getNumSamples(); ++i)
        differs = std::max (differs, std::abs (played.left[(size_t) i] - kick.getSample (0, i)));

    CHECK (differs < 1.0e-5f);
    CHECK (soundingUntil (played.left) <= (size_t) kick.getNumSamples());
    CHECK (soundingUntil (played.left) > (size_t) kick.getNumSamples() - 4800);
}

TEST_CASE ("A drum pad's level, pan, velocity, tuning, hold and decay shape what it plays")
{
    const auto kit = devices::getFactoryDrum (0);
    const auto& kick = kit->audio;
    const auto loudest = kick.getMagnitude (0, kick.getNumSamples());

    auto values = getDefaults();
    set (values, 36, DrumSampler::level, -6.0f);
    set (values, 36, DrumSampler::pan, -1.0f); // all the way left
    DrumSampler sampler;
    sampler.prepare (testRate, testBlockSize);
    auto played = play (sampler, values, 48000, { noteOn (0, 36, 0.5f) });
    CHECK (peakOf (played.left, 0, 48000) == doctest::Approx (loudest * 0.5f * 0.5f).epsilon (0.01));
    CHECK (peakOf (played.right, 0, 48000) < 1.0e-6f);

    // Without velocity, a soft hit is as loud as a hard one; with half of it, it is turned down half as much.
    for (const auto& [amount, expected] : { std::pair (0.0f, 1.0f), std::pair (0.5f, 0.625f) })
    {
        CAPTURE (amount);
        values = getDefaults();
        set (values, 36, DrumSampler::velocity, amount);
        DrumSampler soft;
        soft.prepare (testRate, testBlockSize);
        played = play (soft, values, 48000, { noteOn (0, 36, 0.25f) });
        CHECK (peakOf (played.left, 0, 48000) == doctest::Approx (loudest * expected).epsilon (0.01));
    }

    // An octave up plays twice as fast, so it ends in half the time.
    values = getDefaults();
    set (values, 36, DrumSampler::tune, 12.0f);
    DrumSampler tuned;
    tuned.prepare (testRate, testBlockSize);
    played = play (tuned, values, 48000, { noteOn (0, 36) });
    CHECK (std::abs ((double) soundingUntil (played.left) - kick.getNumSamples() / 2.0) < 4800.0);

    // 50 cents up: a 1 kHz sine at 1029 Hz.
    values = getDefaults();
    set (values, 60, DrumSampler::fine, 50.0f);
    DrumSampler fine;
    fine.prepare (testRate, testBlockSize);
    fine.setSound (60, makeSine (1000.0, 0.5, testRate));
    played = play (fine, values, 24000, { noteOn (0, 60) });
    CHECK (std::abs (countCrossings (played.left, 0, 19200) - 412) <= 1);

    // A 50 ms hold, then a 10 ms decay to silence, cuts it short.
    values = getDefaults();
    set (values, 36, DrumSampler::hold, 50.0f);
    set (values, 36, DrumSampler::decay, 10.0f);
    DrumSampler cut;
    cut.prepare (testRate, testBlockSize);
    played = play (cut, values, 48000, { noteOn (0, 36) });
    CHECK (soundingUntil (played.left) <= (size_t) (0.06 * testRate) + 1);
    CHECK (peakOf (played.left, 0, 2400) > 0.1f);
    CHECK (peakOf (played.left, 2400 + 240, 2400 + 480) < peakOf (played.left, 2400 - 240, 2400) * 0.25f); // decaying
}

TEST_CASE ("A gated drum pad decays from its note's end; a one-shot plays on")
{
    // The crash, which rings for seconds: gated with a 20 ms decay, a note of 0.1 s.
    const auto tailOf = [] (DrumSampler::Mode mode)
    {
        auto values = getDefaults();
        set (values, 49, DrumSampler::mode, (float) mode);
        set (values, 49, DrumSampler::decay, 20.0f);
        DrumSampler sampler;
        sampler.prepare (testRate, testBlockSize);
        const auto played = play (sampler, values, 24000, { noteOn (0, 49), { NoteEvent::Type::noteOff, 4800, 49, 0.0f } });
        return soundingUntil (played.left);
    };

    CHECK (tailOf (DrumSampler::Mode::gate) <= (size_t) (4800 + 0.02 * testRate) + testBlockSize);
    CHECK (tailOf (DrumSampler::Mode::oneShot) == 24000);
}

TEST_CASE ("A drum pad plays from its start, reversed from its end, through its attack, swept to its pitch and filtered")
{
    constexpr int pad = 60, frames = 9600;
    const auto playRamp = [] (const std::vector<float>& values, float velocity = 1.0f)
    {
        DrumSampler sampler;
        sampler.prepare (testRate, testBlockSize);
        sampler.setSound (pad, makeRamp (frames));
        return play (sampler, values, frames + testBlockSize, { noteOn (0, pad, velocity) }).left;
    };

    // As it is: the ramp, sample for sample.
    auto played = playRamp (getDefaults());
    CHECK (played[0] == doctest::Approx (0.0f));
    CHECK (played[4800] == doctest::Approx (0.5f));

    // From halfway.
    auto values = getDefaults();
    set (values, pad, DrumSampler::start, 0.5f);
    played = playRamp (values);
    CHECK (played[0] == doctest::Approx (0.5f));
    CHECK (soundingUntil (played) <= (size_t) frames / 2 + 1);

    // Reversed: from its end down to nothing; and from halfway, from its middle.
    values = getDefaults();
    set (values, pad, DrumSampler::reverse, 1.0f);
    played = playRamp (values);
    CHECK (played[0] == doctest::Approx ((float) (frames - 1) / frames));
    CHECK (played[4800] == doctest::Approx (0.5f).epsilon (0.001));
    set (values, pad, DrumSampler::start, 0.5f);
    played = playRamp (values);
    CHECK (played[0] == doctest::Approx (0.5f).epsilon (0.001));

    // A 10 ms attack: in from nothing.
    values = getDefaults();
    set (values, pad, DrumSampler::attack, 10.0f);
    set (values, pad, DrumSampler::start, 0.5f);
    played = playRamp (values);
    CHECK (std::abs (played[0]) < 1.0e-6f);
    CHECK (played[240] == doctest::Approx (0.5f * (0.5f + 240.0f / frames)).epsilon (0.01));
    CHECK (played[960] == doctest::Approx (0.5f + 960.0f / frames).epsilon (0.01));

    // Swept down an octave to its pitch: a 1 kHz sine starts near 2 kHz.
    const auto sineCrossings = [] (float sweep)
    {
        auto swept = getDefaults();
        set (swept, pad, DrumSampler::sweep, sweep);
        DrumSampler sampler;
        sampler.prepare (testRate, testBlockSize);
        sampler.setSound (pad, makeSine (1000.0, 0.5, testRate));
        return countCrossings (play (sampler, swept, 4800, { noteOn (0, pad) }).left, 0, 480);
    };

    CHECK (std::abs (sineCrossings (0.0f) - 10) <= 1);
    CHECK (sineCrossings (12.0f) > 16);
    CHECK (sineCrossings (-12.0f) < 7);

    // Filtered: a low-pass at 200 Hz takes most of a 5 kHz sine away, a high-pass at 10 kHz most of a 200 Hz one, and a
    // band-pass keeps its middle as it is.
    const auto filtered =
        [] (DrumSampler::FilterType type, float cutoff, double frequency, float velocity, float toCutoff, float resonance = 0.0f)
    {
        auto through = getDefaults();
        set (through, pad, DrumSampler::filter, (float) type);
        set (through, pad, DrumSampler::cutoff, cutoff);
        set (through, pad, DrumSampler::resonance, resonance);
        set (through, pad, DrumSampler::velocity, 0.0f);
        set (through, pad, DrumSampler::velocityToCutoff, toCutoff);
        DrumSampler sampler;
        sampler.prepare (testRate, testBlockSize);
        sampler.setSound (pad, makeSine (frequency, 0.5, testRate));
        return peakOf (play (sampler, through, 24000, { noteOn (0, pad, velocity) }).left, 4800, 24000);
    };

    using Type = DrumSampler::FilterType;
    CHECK (filtered (Type::off, 200.0f, 5000.0, 1.0f, 0.0f) == doctest::Approx (0.5f).epsilon (0.01));
    CHECK (filtered (Type::lowPass, 200.0f, 5000.0, 1.0f, 0.0f) < 0.01f);
    CHECK (filtered (Type::highPass, 10000.0f, 200.0, 1.0f, 0.0f) < 0.01f);
    CHECK (filtered (Type::bandPass, 1000.0f, 1000.0, 1.0f, 0.0f, 0.5f) == doctest::Approx (0.5f).epsilon (0.02));
    CHECK (filtered (Type::bandPass, 1000.0f, 10000.0, 1.0f, 0.0f, 0.5f) < 0.07f);

    // Velocity closes it: a hard hit through a low-pass at 5 kHz keeps most of a 2 kHz sine; a soft one, its cutoff more than
    // three octaves down, little of it.
    CHECK (filtered (Type::lowPass, 5000.0f, 2000.0, 1.0f, 48.0f) > 0.35f);
    CHECK (filtered (Type::lowPass, 5000.0f, 2000.0, 0.1f, 48.0f) < 0.05f);
}

TEST_CASE ("A drum pad muted plays nothing, a soloed one leaves only the soloed playing, and a pad's level follows its value as it "
           "rings")
{
    const auto peakOfPad = [] (const std::vector<float>& values, int note)
    {
        DrumSampler sampler;
        sampler.prepare (testRate, testBlockSize);
        return peakOf (play (sampler, values, 9600, { noteOn (0, note) }).left, 0, 9600);
    };

    auto values = getDefaults();
    CHECK (peakOfPad (values, 36) > 0.1f);
    set (values, 36, DrumSampler::mute, 1.0f);
    CHECK (peakOfPad (values, 36) < 1.0e-6f);
    CHECK (peakOfPad (values, 38) > 0.1f);

    values = getDefaults();
    set (values, 38, DrumSampler::solo, 1.0f);
    CHECK (peakOfPad (values, 36) < 1.0e-6f);
    CHECK (peakOfPad (values, 38) > 0.1f);

    // Muted as it rings: it fades out within the fade's 10 ms.
    {
        auto changing = getDefaults();
        DrumSampler sampler;
        sampler.prepare (testRate, testBlockSize);
        const auto played = play (sampler, changing, 24000, { noteOn (0, 49) },
                                  [&changing] (int done)
                                  {
                                      if (done == 4864)
                                          set (changing, 49, DrumSampler::mute, 1.0f);
                                  });
        CHECK (peakOf (played.left, 4864 + 480 + testBlockSize, 24000) < 1.0e-6f);
    }

    // Turned down 12 dB as the crash rings: what follows is a quarter as loud.
    {
        auto changing = getDefaults();
        DrumSampler sampler, steady;
        sampler.prepare (testRate, testBlockSize);
        steady.prepare (testRate, testBlockSize);
        const auto played = play (sampler, changing, 24000, { noteOn (0, 49) },
                                  [&changing] (int done)
                                  {
                                      if (done == 9728)
                                          set (changing, 49, DrumSampler::level, -12.0f);
                                  });
        const auto unchanged = play (steady, getDefaults(), 24000, { noteOn (0, 49) });
        CHECK (peakOf (played.left, 12000, 24000)
               == doctest::Approx (peakOf (unchanged.left, 12000, 24000) * juce::Decibels::decibelsToGain (-12.0f)).epsilon (0.01));
    }
}

TEST_CASE ("A drum pad's layers share its hits: stacked, by velocity, in turn, or at random never twice in a row, each at its "
           "level")
{
    // Four short sines an octave apart, 200 Hz to 1.6 kHz, on pad 60's four layers: which one played shows in how often it
    // crosses 0 in its 50 ms (10 times for the first, 80 for the last).
    constexpr int pad = 60;
    DrumSampler sampler;
    sampler.prepare (testRate, testBlockSize);

    for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
        sampler.setSound (pad, layer, makeSine (200.0 * (1 << layer), 0.05, testRate));

    CHECK (sampler.hasOwnSound (pad, 3));
    const auto hitLayer = [&sampler] (const std::vector<float>& values, float velocity)
    {
        const auto played = play (sampler, values, 4800, { noteOn (0, pad, velocity) }).left;
        const auto crossings = countCrossings (played, 0, 2400);

        for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
            if (std::abs (crossings - (10 << layer)) <= 1)
                return layer;

        return -1; // more than one, or none
    };

    // Stacked, at first: all of them at once.
    auto values = getDefaults();
    CHECK (hitLayer (values, 1.0f) == -1);
    CHECK (peakOf (play (sampler, values, 4800, { noteOn (0, pad) }).left, 0, 2400) > 0.9f);

    // By velocity: the range split evenly, the softest hits the first's.
    set (values, pad, DrumSampler::layerMode, (float) DrumSampler::LayerMode::velocity);
    CHECK (hitLayer (values, 0.1f) == 0);
    CHECK (hitLayer (values, 0.3f) == 1);
    CHECK (hitLayer (values, 0.6f) == 2);
    CHECK (hitLayer (values, 0.9f) == 3);
    CHECK (hitLayer (values, 1.0f) == 3);

    // Each at its own level: the second 12 dB down.
    set (values, pad, DrumSampler::velocity, 0.0f);
    set (values, pad, DrumSampler::layer2Level, -12.0f);
    CHECK (peakOf (play (sampler, values, 4800, { noteOn (0, pad, 0.3f) }).left, 0, 2400)
           == doctest::Approx (0.5f * juce::Decibels::decibelsToGain (-12.0f)).epsilon (0.02));
    set (values, pad, DrumSampler::layer2Level, 0.0f);

    // In turn, round and round.
    set (values, pad, DrumSampler::layerMode, (float) DrumSampler::LayerMode::roundRobin);

    for (const auto expected : { 0, 1, 2, 3, 0, 1 })
        CHECK (hitLayer (values, 0.8f) == expected);

    // At random, never the same twice in a row, every one of them in a while.
    set (values, pad, DrumSampler::layerMode, (float) DrumSampler::LayerMode::random);
    std::array<int, DrumSampler::numLayers> times {};
    auto last = -1;

    for (int hit = 0; hit < 40; ++hit)
    {
        const auto layer = hitLayer (values, 0.8f);
        REQUIRE (layer >= 0);
        CHECK (layer != last);
        ++times[(size_t) layer];
        last = layer;
    }

    for (const auto count : times)
        CHECK (count > 0);

    // A layer without a sound is left out: in turn, the other three.
    sampler.setSound (pad, 1, nullptr);
    CHECK_FALSE (sampler.hasOwnSound (pad, 1));
    CHECK (sampler.getPadSound (pad, 1) == nullptr);
    set (values, pad, DrumSampler::layerMode, (float) DrumSampler::LayerMode::roundRobin);
    std::vector<int> turns;

    for (int hit = 0; hit < 6; ++hit)
        turns.push_back (hitLayer (values, 0.8f));

    CHECK (std::count (turns.begin(), turns.end(), 1) == 0);
    CHECK (std::count (turns.begin(), turns.end(), 0) == 2);
    CHECK (std::count (turns.begin(), turns.end(), 3) == 2);
}

TEST_CASE ("A drum pad off the default kit plays nothing until it is given a sound; any choke group chokes its pads")
{
    DrumSampler sampler;
    sampler.prepare (testRate, testBlockSize);
    CHECK (peakOf (play (sampler, getDefaults(), 9600, { noteOn (0, 60) }).left, 0, 9600) < 1.0e-6f);
    CHECK (sampler.getPadSound (60) == nullptr);
    sampler.setSound (60, makeSine (440.0, 0.5, testRate));
    CHECK (peakOf (play (sampler, getDefaults(), 9600, { noteOn (0, 60) }).left, 0, 9600) > 0.4f);

    // A click in group 8 with the long sine: the click stops it.
    auto click = std::make_unique<DrumSampler::Sound>();
    click->sampleRate = testRate;
    click->audio.setSize (1, 1);
    click->audio.setSample (0, 0, 0.5f);
    sampler.setSound (61, std::move (click));
    auto values = getDefaults();
    set (values, 60, DrumSampler::choke, 8.0f);
    set (values, 61, DrumSampler::choke, 8.0f);
    auto played = play (sampler, values, 24000, { noteOn (0, 60), noteOn (4800, 61) });
    CHECK (peakOf (played.left, 4800 + 600, 24000) < 1.0e-6f);

    // In no group, or another, it rings on.
    set (values, 61, DrumSampler::choke, 7.0f);
    played = play (sampler, values, 24000, { noteOn (0, 60), noteOn (4800, 61) });
    CHECK (peakOf (played.left, 4800 + 600, 24000) > 0.4f);
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
    CHECK_FALSE (sampler.hasOwnSound (36));

    // A 1 kHz sine at 44.1 kHz still sounds at 1 kHz.
    sampler.setSound (36, makeSine (1000.0, 0.5, 44100.0));
    CHECK (sampler.hasOwnSound (36));
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
                           sampler.setSound (36, nullptr); // back to the kick
                   });
    CHECK (peakOf (played.left, 2560, 4800) < 1.0e-6f);
    CHECK (peakOf (played.left, 4800, 9600) > 0.1f);
    CHECK_FALSE (sampler.hasOwnSound (36));

    // The editor plays a pad at the next block, which it waits for (the engine wakes for it).
    sampler.getPadMeter (38).take();
    CHECK_FALSE (sampler.isWaitingToPlay());
    sampler.audition (38, 0.8f);
    CHECK (sampler.isWaitingToPlay());
    played = play (sampler, getDefaults(), 4800, {});
    CHECK_FALSE (sampler.isWaitingToPlay());
    CHECK (peakOf (played.left, 0, 4800) > 0.1f);
    CHECK (sampler.getPadMeter (38).take() == doctest::Approx (0.8f));

    // One that waited longer than a moment, as while the sampler was off, plays never rather than late.
    sampler.audition (38, 0.8f);
    juce::Thread::sleep (300);
    juce::Time::getMillisecondCounter(); // the clock moves on, as the engine moves it after each block
    play (sampler, getDefaults(), 4800, {});
    CHECK (sampler.getPadMeter (38).take() == doctest::Approx (0.0f));
}

TEST_CASE ("Automation's menus list a drum sampler's parameters under their pads, those that play a sound or have automation")
{
    phazite::test::TestSong song;
    const auto drums = song.setInstrument (song.firstTrack, DrumSampler::typeID);
    const auto targets = [&song, drums] { return phazite::getAutomationTargets (song.context, drums); };
    const auto groupOf = [] (const std::vector<phazite::AutomationTarget>& all, const juce::String& id)
    {
        for (const auto& target : all)
            if (target.parameter.parameter == id)
                return std::optional<phazite::AutomationTarget> (target);

        return std::optional<phazite::AutomationTarget>();
    };

    // The default kit's sixteen pads, each its 25 parameters, named without their pads, under them; and the output.
    auto all = targets();
    CHECK (all.size() == 16 * DrumSampler::fieldsPerPad + 1);
    const auto kickLevel = groupOf (all, "pad36Level");
    REQUIRE (kickLevel.has_value());
    CHECK (kickLevel->group == juce::String ("Pad 36: Kick"));
    CHECK (kickLevel->name == juce::String ("Level"));
    CHECK (groupOf (all, "pad38Layer2Level")->name == juce::String ("Layer 2 Level"));
    CHECK (groupOf (all, "output")->group.isEmpty());
    CHECK_FALSE (groupOf (all, "pad60Level").has_value());

    // An empty pad's parameter with automation is listed too.
    song.addAutomation ({ drums, "pad60Cutoff" }, { { phazite::BeatPosition(), 1000.0f } });
    all = targets();
    REQUIRE (groupOf (all, "pad60Cutoff").has_value());
    CHECK (groupOf (all, "pad60Cutoff")->group == juce::String ("Pad 60"));
    CHECK_FALSE (groupOf (all, "pad60Level").has_value());
}

TEST_CASE ("A sound file put on a drum pad is one undo step, kept in the project's Audio folder, and plays when the project opens")
{
    phazite::test::TestSong song;
    phazite::test::TemporaryFolder elsewhere;
    const auto sample = elsewhere.folder.getChildFile ("My Kick.wav");
    writeWav (sample, 0.2);

    // The sampler as Phazite's engine plays the open song's.
    const auto playedSampler = [&song] (phazite::DeviceID id)
    {
        phazite::getSession (song.context)->update();
        return phazite::makeDeviceHandle (*phazite::findSong (song.context), id).getProcessor<DrumSampler>();
    };

    const auto trackID = song.firstTrack;
    const auto id = song.setInstrument (trackID, DrumSampler::typeID);
    auto& s = *phazite::findSong (song.context);
    REQUIRE (phazite::isDrumSampler (s, id));
    CHECK (phazite::getPadName (s, id, 36) == juce::String ("Kick"));
    CHECK (phazite::getPadName (s, id, 60).isEmpty());
    CHECK (phazite::isPadEmpty (s, id, 60));
    CHECK_FALSE (phazite::isPadEmpty (s, id, 36));

    // The track's drum rows are the pads that play, named after what they play.
    CHECK (phazite::isDrumTrack (s, trackID));
    auto rows = phazite::getSamplerSounds (s, trackID);
    REQUIRE (rows.has_value());
    REQUIRE (rows->size() == 16);
    CHECK ((*rows)[0].note == 36);
    CHECK ((*rows)[2].name == juce::String ("Snare"));

    // A file that isn't a sound changes nothing.
    const auto text = elsewhere.folder.getChildFile ("Notes.txt");
    REQUIRE (text.replaceWithText ("not a sound"));
    CHECK_FALSE (phazite::setPadSound (s, id, 36, text));

    REQUIRE (phazite::setPadSound (s, id, 36, sample));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Put My Kick on Pad 36"));
    const auto sounds = phazite::findDeviceState (s, id).getChildWithName (phazite::schema::SOUNDS);
    CHECK (sounds[phazite::schema::getPadSoundFile (36)].toString() == juce::String ("Audio/My Kick.wav"));
    CHECK (phazite::schema::getPadSoundFile (36).toString() == juce::String ("pad36SoundFile"));
    CHECK (song.getProject().getFolder().getChildFile ("Audio/My Kick.wav").existsAsFile());
    CHECK (phazite::hasPadSound (s, id, 36));
    CHECK (phazite::getPadName (s, id, 36) == juce::String ("My Kick"));
    CHECK (phazite::getSamplerSounds (s, trackID)->front().name == juce::String ("My Kick"));
    REQUIRE (playedSampler (id) != nullptr);
    CHECK (playedSampler (id)->hasOwnSound (36));

    phazite::undo (s);
    CHECK (phazite::getPadName (s, id, 36) == juce::String ("Kick"));
    CHECK_FALSE (playedSampler (id)->hasOwnSound (36));
    phazite::redo (s);
    CHECK (playedSampler (id)->hasOwnSound (36));

    // On an empty pad, it plays there too, and its row joins the others, in its note's place.
    REQUIRE (phazite::setPadSound (s, id, 60, sample));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Put My Kick on Pad 60"));
    rows = phazite::getSamplerSounds (s, trackID);
    REQUIRE (rows->size() == 17);
    CHECK (rows->back().note == 60);
    CHECK (rows->back().name == juce::String ("My Kick"));
    CHECK (playedSampler (id)->getPadSound (60) != nullptr);
    phazite::undo (s);
    CHECK (playedSampler (id)->getPadSound (60) == nullptr);

    // On the snare's second layer, beside its own: saved by its layer's name, played, and taken off again.
    REQUIRE (phazite::setPadSound (s, id, 38, sample, 1));
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Put My Kick on Pad 38's Layer 2"));
    CHECK (phazite::findDeviceState (s, id).getChildWithName (phazite::schema::SOUNDS)[phazite::schema::getPadSoundFile (38, 1)]
           == juce::var ("Audio/My Kick.wav"));
    CHECK (phazite::schema::getPadSoundFile (38, 1).toString() == juce::String ("pad38SoundFile2"));
    CHECK (phazite::getPadName (s, id, 38) == juce::String ("Snare"));
    CHECK (phazite::getPadName (s, id, 38, 1) == juce::String ("My Kick"));
    CHECK (playedSampler (id)->getPadSound (38, 1) != nullptr);
    phazite::clearPadSound (s, id, 38, 1);
    CHECK (phazite::describeUndo (&s) == juce::String ("Undo Clear Pad 38's Layer 2"));
    CHECK (phazite::getPadName (s, id, 38, 1).isEmpty());
    CHECK (playedSampler (id)->getPadSound (38, 1) == nullptr);

    // A pad with a sound on another layer than its first isn't empty.
    REQUIRE (phazite::setPadSound (s, id, 70, sample, 2));
    CHECK_FALSE (phazite::isPadEmpty (s, id, 70));
    CHECK (phazite::getSamplerSounds (s, trackID)->back().note == 70);
    phazite::undo (s);
    CHECK (phazite::isPadEmpty (s, id, 70));

    // Opened again from its file, the pad plays its file.
    song.reopen();
    auto& reopened = *phazite::findSong (song.context);
    const auto drums = phazite::getInstrument (reopened, trackID);
    REQUIRE (phazite::isDrumSampler (reopened, drums));
    REQUIRE (playedSampler (drums) != nullptr);
    CHECK (playedSampler (drums)->hasOwnSound (36));
    CHECK (phazite::getPadName (reopened, drums, 36) == juce::String ("My Kick"));

    // Back to the kit's sound, as an undo step.
    phazite::clearPadSound (reopened, drums, 36);
    CHECK (phazite::describeUndo (&reopened) == juce::String ("Undo Reset Pad 36"));
    CHECK_FALSE (playedSampler (drums)->hasOwnSound (36));
}

TEST_CASE ("A drum pad on a track of its own plays there, leaving its sampler's track, following that track's mute and effects, "
           "heard by sidechains, and comes back as an undo step")
{
    // Drums with a kick at the start and a snare a second in (120 bpm); and a bass track.
    const auto song = phazite::createSong();
    auto& model = *song;
    const auto tracks = phazite::ensureTracks (model, 2);
    const auto drums = tracks[0], bass = tracks[1];
    phazite::setTrackName (model, drums, "Drums");
    const auto sampler = phazite::setInstrument (model, drums, phazite::makeOwnDeviceState (model, DrumSampler::typeID));
    REQUIRE (sampler.isValid());
    using phazite::test::at, phazite::test::lasting;
    const auto clip = phazite::test::addMidiClip (model, drums, { at (0.0), at (4.0) }, "Beat");
    phazite::test::addMidiNote (model, clip, 36, at (0.0), lasting (0.25), 127);
    phazite::test::addMidiNote (model, clip, 38, at (2.0), lasting (0.25), 127);

    // The kick's peak in the first half second, and the snare's in the half second from one second.
    const auto peaks = [&model]
    {
        const auto audio = phazite::renderWithOwnEngine (model, 2 * 44100, 44100.0, 512);
        return std::pair (audio.getMagnitude (0, 0, 22050), audio.getMagnitude (0, 44100, 22050));
    };

    const auto [kick, snare] = peaks();
    REQUIRE (kick > 0.1f);
    REQUIRE (snare > 0.1f);

    // On a track of its own, right after the drums', named after it: heard as before.
    const auto padTrack = phazite::givePadItsOwnTrack (model, sampler, 36);
    REQUIRE (padTrack.isValid());
    CHECK (phazite::describeUndo (&model) == juce::String ("Undo Give Pad 36 Its Own Track"));
    CHECK (phazite::findPadTrack (model, sampler, 36) == padTrack);
    CHECK (phazite::givePadItsOwnTrack (model, sampler, 36) == padTrack); // it has one already
    CHECK_FALSE (phazite::findPadTrack (model, sampler, 38).isValid());
    REQUIRE (phazite::getPlayedPad (model, padTrack).has_value());
    CHECK (phazite::getPlayedPad (model, padTrack)->first == sampler);
    CHECK (phazite::getPlayedPad (model, padTrack)->second == 36);
    CHECK (phazite::getTrackIDs (model)[1] == padTrack);
    CHECK (phazite::findTrackState (model, padTrack)[phazite::schema::trackName].toString() == juce::String ("Kick"));

    auto [onItsOwn, snareStill] = peaks();
    CHECK (onItsOwn == doctest::Approx (kick).epsilon (0.01));
    CHECK (snareStill == doctest::Approx (snare).epsilon (0.01));

    // The drums muted: the kick with them, its track theirs as a folder's tracks are; its own track muted: the kick alone.
    phazite::setTrackMuted (model, drums, true);
    auto [kickWithDrumsMuted, snareWithDrumsMuted] = peaks();
    CHECK (kickWithDrumsMuted < 1.0e-4f);
    CHECK (snareWithDrumsMuted < 1.0e-4f);
    CHECK (phazite::isTrackMuted (model, padTrack, true));
    CHECK_FALSE (phazite::isTrackMuted (model, padTrack, false));
    phazite::setTrackMuted (model, drums, false);
    phazite::setTrackMuted (model, padTrack, true);
    auto [kickMuted, snareAlone] = peaks();
    CHECK (kickMuted < 1.0e-4f);
    CHECK (snareAlone == doctest::Approx (snare).epsilon (0.01));
    CHECK_FALSE (phazite::isTrackMuted (model, drums, true));
    phazite::setTrackMuted (model, padTrack, false);

    // The drums soloed: the kick plays with them; the kick's track soloed: the kick alone, the sampler playing it still.
    phazite::setTrackSoloed (model, drums, true);
    auto [kickWithDrumsSoloed, snareSoloed] = peaks();
    CHECK (kickWithDrumsSoloed == doctest::Approx (kick).epsilon (0.01));
    CHECK (snareSoloed == doctest::Approx (snare).epsilon (0.01));
    CHECK_FALSE (phazite::isSilencedBySolo (model, padTrack));
    CHECK (phazite::isSilencedBySolo (model, bass));
    phazite::setTrackSoloed (model, drums, false);
    phazite::setTrackSoloed (model, padTrack, true);
    auto [kickSoloed, snareSoloedOut] = peaks();
    CHECK (kickSoloed == doctest::Approx (kick).epsilon (0.01));
    CHECK (snareSoloedOut < 1.0e-4f);
    CHECK (phazite::isSilencedBySolo (model, drums));
    phazite::setTrackSoloed (model, padTrack, false);

    // Neither frozen nor bounced while it has pads on tracks of their own, which its render would leave out.
    CHECK (phazite::hasPadsOnOwnTracks (model, drums));
    CHECK_FALSE (phazite::hasPadsOnOwnTracks (model, bass));

    // Its own effects: a gain 12 dB down on its track turns the kick alone down.
    const auto gain = phazite::test::addTestDevice (model, padTrack, phazite::test::TestGain::typeID);
    phazite::setValueSetByHand (model, phazite::ParameterRef { gain, "gain" }, -12.0f);
    auto [kickDown, snareAsIs] = peaks();
    CHECK (kickDown == doctest::Approx (kick * juce::Decibels::decibelsToGain (-12.0f)).epsilon (0.02));
    CHECK (snareAsIs == doctest::Approx (snare).epsilon (0.01));

    // A sidechain on the bass listening to it hears the kick, and the kick alone, its track muted or not.
    const auto listener = phazite::test::addTestDevice (model, bass, phazite::test::TestSidechain::typeID);
    phazite::setSidechainSource (model, listener, padTrack);
    phazite::setTrackMuted (model, drums, true);
    phazite::setTrackMuted (model, padTrack, true);
    auto [heardKick, heardSnare] = peaks();
    CHECK (heardKick > 0.5f * kickDown);
    CHECK (heardSnare < 1.0e-4f);
    phazite::setTrackMuted (model, drums, false);
    phazite::setTrackMuted (model, padTrack, false);
    phazite::setSidechainSource (model, listener, {});

    // Back on the drums' track, its track gone, as one undo step; and undone, on its own again.
    phazite::bringPadBack (model, sampler, 36);
    CHECK (phazite::describeUndo (&model) == juce::String ("Undo Bring Pad 36 Back"));
    CHECK_FALSE (phazite::findPadTrack (model, sampler, 36).isValid());
    auto [kickBack, snareBack] = peaks();
    CHECK (kickBack == doctest::Approx (kick).epsilon (0.01));
    CHECK (snareBack == doctest::Approx (snare).epsilon (0.01));
    phazite::undo (model);
    CHECK (phazite::findPadTrack (model, sampler, 36) == padTrack);
    phazite::setTrackMuted (model, padTrack, true);
    CHECK (peaks().first < 1.0e-4f);
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
