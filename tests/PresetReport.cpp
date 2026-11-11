// SPDX-License-Identifier: AGPL-3.0-or-later
// What each instrument preset that comes with Phazite (resources/presets) sounds like, in numbers, for whoever makes or
// checks them (docs/presets.md): each played as one note at its kind's pitch, at velocity 100 and again at 50 (for what
// velocity does), measured for its level, loudness, attack, decay, release, brightness as it goes on, pitch and width; each
// kit's pads one at a time; how far and how long each sidechain preset ducks a sound under each kit's kick; how much each
// saturator preset colours a sine; how far each exciter preset lifts each octave of noise at three levels; what each vinyl
// preset does to the pitch, the band and the sides, and its noise; and each EQ preset's response, band by band. It writes a
// report and WAV files to listen to: the note, and a phrase of its kind of
// sound (a bass line, a melody, chords, an arpeggio, a beat), and a pad pumping under each kick. ctest and CI skip it; run
// it from a build in the repository with
//   phazite_tests.exe --test-suite=preset-report --no-skip
// with the environment variable PHAZITE_PRESET_REPORT naming the folder for report.md and the WAV files (else a temporary
// folder, which it names).
#include "TestHelpers.h"
#include "devices/drums/DrumSampler.h"
#include "devices/drums/FactoryDrums.h"
#include "devices/dsp/Random.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/fm/FmSynth.h"
#include "devices/sampler/FactorySamples.h"
#include "devices/sampler/SampleAnalysis.h"
#include "devices/sampler/Sampler.h"
#include "devices/saturation/Exciter.h"
#include "devices/saturation/Saturator.h"
#include "devices/synth/Synth.h"
#include "devices/vinyl/Vinyl.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "devices/wavetable/WavetableSynth.h"
#include "engine/nodes/LoudnessMeter.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace
{
namespace devices = phazite::devices;
namespace keys = phazite::schema::preset;
using Audio = juce::AudioBuffer<float>;
using Events = std::vector<devices::NoteEvent>;

constexpr double rate = 48000.0;
constexpr int blockSize = 256;
constexpr int hop = 480;                  // 10 ms, the envelope's steps
constexpr double held = 2.0, after = 2.5; // a note's length and what follows it, in seconds

juce::String toString (std::string_view text)
{
    return juce::String (text.data(), text.size());
}

int at (double seconds)
{
    return (int) std::lround (seconds * rate);
}

struct Preset
{
    juce::File file;
    juce::String device, kind, name; // "Synth", "Bass", "Acid Bass"
    const devices::DeviceInfo* info = nullptr;
    std::vector<float> values;
    juce::var sounds;

    float get (std::string_view id) const
    {
        const auto index = devices::findParameter (*info, id);
        return index >= 0 ? values[(size_t) index] : 0.0f;
    }
};

std::optional<Preset> readPreset (const juce::File& file, devices::DeviceKind kind = devices::DeviceKind::instrument)
{
    const auto device = phazite::readPreset (file)[keys::device];

    for (const auto* info : phazite::getOwnDevices())
    {
        if (toString (info->typeID) != device[keys::type].toString() || info->kind != kind)
            continue;

        Preset preset;
        preset.file = file;
        preset.info = info;
        preset.device = toString (info->name);
        preset.kind = file.getParentDirectory().getFileName();
        preset.name = file.getFileNameWithoutExtension();
        preset.sounds = device[keys::sounds];

        for (const auto& parameter : info->parameters)
            preset.values.push_back (parameter.defaultValue);

        if (const auto* set = device[keys::parameters].getDynamicObject())
            for (const auto& [id, value] : set->getProperties())
                if (const auto index = devices::findParameter (*info, id.toString().toStdString()); index >= 0)
                    preset.values[(size_t) index] = devices::constrain (info->parameters[(size_t) index],
                                                                        phazite::readSavedValue (info->parameters[(size_t) index], value));

        return preset;
    }

    return {};
}

std::unique_ptr<devices::Processor> makeInstrument (const Preset& preset)
{
    const auto type = toString (preset.info->typeID);
    std::unique_ptr<devices::Processor> made;

    if (type == devices::Synth::typeID)
        made = std::make_unique<devices::Synth>();
    else if (type == devices::FmSynth::typeID)
        made = std::make_unique<devices::FmSynth>();
    else if (type == devices::WavetableSynth::typeID)
    {
        auto synth = std::make_unique<devices::WavetableSynth>();

        for (int osc = 0; osc < devices::WavetableSynth::numOscillators; ++osc)
            if (const auto name = preset.sounds[phazite::schema::getOscillatorWavetable (osc)].toString(); name.isNotEmpty())
                synth->setTable (osc, devices::getFactoryWavetable (devices::findFactoryWavetable (name.toStdString())));

        made = std::move (synth);
    }
    else if (type == devices::Sampler::typeID)
    {
        auto sampler = std::make_unique<devices::Sampler>();

        if (const auto name = preset.sounds[phazite::schema::samplerSample].toString(); name.isNotEmpty())
            sampler->setSound (devices::getFactorySample (devices::findFactorySample (name.toStdString())));

        made = std::move (sampler);
    }
    else if (type == devices::DrumSampler::typeID)
    {
        auto drums = std::make_unique<devices::DrumSampler>();

        for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
            if (const auto name = preset.sounds[phazite::schema::getPadSound (pad)].toString(); name.isNotEmpty())
                drums->setSound (pad, devices::getFactoryDrum (devices::findFactoryDrum (name.toStdString())));

        made = std::move (drums);
    }

    REQUIRE (made != nullptr);
    made->prepare (rate, blockSize);
    return made;
}

// The preset played: its events, in time order, over the samples given.
Audio render (const Preset& preset, Events events, int numSamples)
{
    auto processor = makeInstrument (preset);
    std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b) { return a.sample < b.sample; });
    Audio out (2, numSamples);
    out.clear();
    Audio buffer (2, blockSize);
    size_t next = 0;

    for (int done = 0; done < numSamples; done += blockSize)
    {
        const auto length = std::min (blockSize, numSamples - done);
        Events block;

        while (next < events.size() && events[next].sample < done + length)
        {
            auto event = events[next++];
            event.sample -= done;
            block.push_back (event);
        }

        buffer.clear();
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = length;
        context.parameters = preset.values;
        context.events = block;
        context.isPlaying = true;
        processor->process (context);

        for (int channel = 0; channel < 2; ++channel)
            out.copyFrom (channel, done, buffer, channel, 0, length);
    }

    return out;
}

void note (Events& events, double start, double length, int number, float velocity = 0.8f)
{
    events.push_back ({ devices::NoteEvent::Type::noteOn, at (start), number, velocity });
    events.push_back ({ devices::NoteEvent::Type::noteOff, at (start + length), number, 0.0f });
}

void writeWav (const Audio& audio, const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

//==============================================================================
float toDecibels (double gain)
{
    return gain > 1.0e-9 ? (float) (20.0 * std::log10 (gain)) : -180.0f;
}

// The sound's level in 10 ms steps, both sides together, in decibels.
std::vector<float> getEnvelope (const Audio& audio)
{
    std::vector<float> envelope;

    for (int start = 0; start + hop <= audio.getNumSamples(); start += hop)
    {
        auto energy = 0.0;

        for (int channel = 0; channel < 2; ++channel)
            for (int i = start; i < start + hop; ++i)
                energy += (double) audio.getSample (channel, i) * audio.getSample (channel, i);

        envelope.push_back (toDecibels (std::sqrt (energy / (2.0 * hop))));
    }

    return envelope;
}

// Where its spectrum's weight is, in hertz, over 4096 samples from a time: its brightness then.
float getCentroid (const Audio& audio, double seconds)
{
    constexpr int order = 12, size = 1 << order;
    const auto start = at (seconds);

    if (start + size > audio.getNumSamples())
        return 0.0f;

    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) (2 * size), 0.0f);

    for (int i = 0; i < size; ++i)
    {
        const auto window = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1));
        data[(size_t) i] = window * 0.5f * (audio.getSample (0, start + i) + audio.getSample (1, start + i));
    }

    fft.performFrequencyOnlyForwardTransform (data.data());
    auto weighted = 0.0, total = 0.0;

    for (int bin = 1; bin < size / 2; ++bin)
    {
        const auto frequency = bin * rate / size;

        if (frequency < 20.0 || frequency > 20000.0)
            continue;

        weighted += frequency * data[(size_t) bin];
        total += data[(size_t) bin];
    }

    return total > 1.0e-9 ? (float) (weighted / total) : 0.0f;
}

// Its loudness by EBU R128: integrated, and the loudest of its momentary readings (400 ms), which compares a short sound
// with a long one as the ear does.
struct Loudness
{
    float integrated = -99.0f, momentary = -99.0f;
};

Loudness getLoudness (const Audio& audio)
{
    phazite::engine::LoudnessMeter meter;
    meter.prepare (rate, blockSize);
    Loudness loudness;

    for (int done = 0; done < audio.getNumSamples(); done += blockSize)
    {
        const auto length = std::min (blockSize, audio.getNumSamples() - done);
        const float* channels[] = { audio.getReadPointer (0, done), audio.getReadPointer (1, done) };
        meter.process (channels, 2, length);

        if (const auto momentary = meter.getReadings().momentary)
            loudness.momentary = std::max (loudness.momentary, *momentary);
    }

    loudness.integrated = meter.getReadings().integrated.value_or (-99.0f);
    return loudness;
}

float getCorrelation (const Audio& audio, int numSamples)
{
    auto both = 0.0, left = 0.0, right = 0.0;

    for (int i = 0; i < std::min (numSamples, audio.getNumSamples()); ++i)
    {
        const auto l = (double) audio.getSample (0, i), r = (double) audio.getSample (1, i);
        both += l * r;
        left += l * l;
        right += r * r;
    }

    return left > 0.0 && right > 0.0 ? (float) (both / std::sqrt (left * right)) : 1.0f;
}

juce::String decibels (float value)
{
    return value <= -179.0f ? juce::String ("-inf") : juce::String (value, 1);
}

juce::String hertz (float value)
{
    return value <= 0.0f ? juce::String ("-") : juce::String (juce::roundToInt (value));
}

// A note's numbers: how loud, how it starts, decays and lets go, how bright it is as it goes on, its pitch and its width.
struct NoteMeasures
{
    Loudness loudness;
    float peak = 0.0f, highest = 0.0f, attack = 0.0f, quarter = 0.0f, second = 0.0f, end = 0.0f;
    juce::String release;
    float bright30 = 0.0f, bright250 = 0.0f, bright1000 = 0.0f;
    std::optional<double> pitch;
    float correlation = 1.0f;
};

NoteMeasures measureNote (const Audio& audio)
{
    NoteMeasures measured;
    measured.peak = toDecibels (audio.getMagnitude (0, audio.getNumSamples()));
    measured.loudness = getLoudness (audio);
    const auto envelope = getEnvelope (audio);
    const auto highest = *std::max_element (envelope.begin(), envelope.end());
    measured.highest = highest;
    const auto loudest = (int) (std::find_if (envelope.begin(), envelope.end(), [highest] (float level) { return level >= highest - 1.0f; })
                                - envelope.begin());
    measured.attack = (float) loudest * 10.0f;
    const auto level = [&] (double seconds)
    { return envelope[(size_t) std::min ((int) (seconds * 100.0), (int) envelope.size() - 1)] - highest; };
    measured.quarter = level (0.25);
    measured.second = level (1.0);
    measured.end = level (held - 0.02);

    const auto off = (int) (held * 100.0);
    const auto atOff = envelope[(size_t) off - 1];
    measured.release = ">" + juce::String ((int) (after * 1000.0));

    for (auto step = off; step < (int) envelope.size(); ++step)
    {
        if (envelope[(size_t) step] < std::max (atOff - 40.0f, -90.0f))
        {
            measured.release = juce::String ((step - off) * 10);
            break;
        }
    }

    if (atOff < -90.0f)
        measured.release = "(gone)";

    measured.bright30 = getCentroid (audio, 0.03);
    measured.bright250 = getCentroid (audio, 0.25);
    measured.bright1000 = getCentroid (audio, 1.0);

    Audio first (2, at (1.5));
    for (int channel = 0; channel < 2; ++channel)
        first.copyFrom (channel, 0, audio, channel, 0, first.getNumSamples());

    measured.pitch = devices::detectPitch (first, rate);
    measured.correlation = getCorrelation (audio, at (held));
    return measured;
}

// The note a preset's kind plays at: basses low, pads in the middle, the rest at middle C; a sampler's sound at its root.
int getTestNote (const Preset& preset)
{
    if (preset.device == "Sampler")
        return juce::roundToInt (preset.get ("mode")) == (int) devices::Sampler::Mode::slice ? devices::Sampler::firstSliceNote
                                                                                             : juce::roundToInt (preset.get ("root"));

    if (preset.kind == "Bass" || preset.kind == "Percussion")
        return 36;

    if (preset.kind == "Pad" || preset.kind == "Strings")
        return 48;

    return 60;
}

// A phrase in the preset's kind of sound, at 120 bpm, to listen to.
Events getPhrase (const Preset& preset, int& numSamples)
{
    Events events;
    constexpr double beat = 0.5;

    if (preset.device == "Sampler" && juce::roundToInt (preset.get ("mode")) == (int) devices::Sampler::Mode::slice)
    {
        for (int step = 0; step < 16; ++step)
            note (events, step * beat * 0.5, beat * 0.45, devices::Sampler::firstSliceNote + (step * 5) % 8);
    }
    else if (preset.kind == "Bass")
    {
        constexpr int line[] = { 36, 36, 48, 36, 39, 36, 43, 41, 36, 36, 48, 36, 46, 43, 39, 41 };

        for (int step = 0; step < 16; ++step)
            note (events, step * beat * 0.5, beat * 0.4, line[step]);

        note (events, 8 * beat, 4 * beat, 36);
    }
    else if (preset.kind == "Lead")
    {
        constexpr int melody[] = { 60, 63, 67, 70, 72, 70, 67, 63 };

        for (int step = 0; step < 8; ++step)
            note (events, step * beat, beat * 0.9, melody[step]);

        note (events, 8 * beat, 4 * beat, 72);
    }
    else if (preset.kind == "Pluck")
    {
        constexpr int arpeggio[] = { 60, 63, 67, 72, 75, 72, 67, 63 };

        for (int step = 0; step < 32; ++step)
            note (events, step * beat * 0.25, beat * 0.2, arpeggio[step % 8] - (step >= 16 ? 4 : 0));
    }
    else if (preset.kind == "Pad" || preset.kind == "Keys" || preset.kind == "Strings" || preset.kind == "Brass" || preset.kind == "Chords")
    {
        for (const auto tone : { 48, 51, 55, 58 })
            note (events, 0.0, 4 * beat * 0.95, tone);

        for (const auto tone : { 44, 48, 51, 55 })
            note (events, 4 * beat, 4 * beat * 0.95, tone);

        for (const auto tone : { 46, 50, 53, 58 })
            note (events, 8 * beat, 4 * beat * 0.95, tone);
    }
    else
    {
        note (events, 0.0, 6 * beat, 60);
    }

    numSamples = at (12 * beat + after);
    return events;
}

// A beat on a kit, two bars at 100 bpm, then each pad in turn.
Events getBeat (int& numSamples)
{
    Events events;
    constexpr double step = 60.0 / 100.0 / 4.0;
    constexpr int kicks[] = { 0, 6, 8, 16, 19, 24, 26 };
    const auto pad = [] (int index) { return devices::DrumSampler::kitFirstNote + index; };

    for (const auto each : kicks)
        note (events, each * step, step, pad (0));

    for (const auto each : { 4, 12, 20, 28 })
        note (events, each * step, step, pad (2));

    note (events, 28 * step, step, pad (3));

    for (int each = 0; each < 32; each += 2)
        note (events, each * step, step, each % 16 == 14 ? pad (10) : pad (6), each % 4 == 0 ? 0.8f : 0.55f);

    for (int index = 0; index < devices::DrumSampler::kitPads; ++index)
        note (events, (32 + index * 2) * step, step, pad (index));

    numSamples = at ((32 + 2 * devices::DrumSampler::kitPads) * step + 2.0);
    return events;
}

//==============================================================================
// The sidechain presets (the compressor's Sidechain kind), each ducking a sound under a kit's kick on every beat at 120 bpm.
constexpr double pumpBeat = 0.5; // seconds: a beat at 120 bpm
constexpr int pumpBeats = 8;

// The kit's kick on every beat, as its drum sampler plays it, at its kit's levels: what a sidechain hears of a drum track.
Audio renderKicks (const Preset& kit)
{
    Events kicks;

    for (int beat = 0; beat < pumpBeats; ++beat)
        note (kicks, beat * pumpBeat, 0.1, devices::DrumSampler::kitFirstNote);

    return render (kit, kicks, at (pumpBeat * pumpBeats));
}

// What the compressor's preset makes of a sound, with a key in its sidechain.
Audio compress (const Preset& preset, Audio audio, const Audio& key)
{
    devices::Compressor compressor;
    compressor.prepare (rate, blockSize);

    for (int done = 0; done < audio.getNumSamples(); done += blockSize)
    {
        const auto length = std::min (blockSize, audio.getNumSamples() - done);
        float* channels[] = { audio.getWritePointer (0, done), audio.getWritePointer (1, done) };
        const float* sidechain[] = { key.getReadPointer (0, done), key.getReadPointer (1, done) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = length;
        context.sidechain = sidechain;
        context.numSidechainChannels = 2;
        context.parameters = preset.values;
        context.isPlaying = true;
        compressor.process (context);
    }

    return audio;
}

// How a steady tone (a sine at -12 dBFS) ducks under the key: how far down at the beat, and how long from the beat until it
// is back within 3 dB and within 1 dB (at most the beat's 500 ms), averaged over the third to the eighth beats, once settled.
struct Pump
{
    float depth = 0.0f, within3 = 0.0f, within1 = 0.0f;
};

Pump measurePump (const Preset& preset, const Audio& key)
{
    Audio tone (2, key.getNumSamples());

    for (int i = 0; i < tone.getNumSamples(); ++i)
        for (int channel = 0; channel < 2; ++channel)
            tone.setSample (channel, i, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / rate));

    const auto in = getEnvelope (tone), out = getEnvelope (compress (preset, tone, key));
    constexpr auto hopsPerBeat = (int) (pumpBeat * rate) / hop;
    Pump sum;
    auto beats = 0;

    for (int beat = 2; beat < pumpBeats; ++beat)
    {
        const auto reduction = [&] (int step)
        {
            const auto index = (size_t) (beat * hopsPerBeat + step);
            return in[index] - out[index];
        };

        auto deepest = 0.0f;
        auto deepestAt = 0;

        for (int step = 0; step < hopsPerBeat; ++step)
        {
            if (reduction (step) > deepest)
            {
                deepest = reduction (step);
                deepestAt = step;
            }
        }

        const auto backWithin = [&] (float decibels)
        {
            for (auto step = deepestAt; step < hopsPerBeat; ++step)
                if (reduction (step) <= decibels)
                    return (float) (step * hop) * 1000.0f / (float) rate;

            return (float) pumpBeat * 1000.0f;
        };

        sum.depth += deepest;
        sum.within3 += backWithin (3.0f);
        sum.within1 += backWithin (1.0f);
        ++beats;
    }

    return { sum.depth / (float) beats, sum.within3 / (float) beats, sum.within1 / (float) beats };
}

//==============================================================================
// The saturator's presets: how much each colours a sine at a level, as its total harmonic distortion (the second to the
// tenth harmonics against the fundamental, in decibels), measured over 8192 samples after half a second, the sine on a
// bin of the transform so that each harmonic falls on one.
float measureDistortion (const Preset& preset, float peakDecibels)
{
    constexpr int order = 13, size = 1 << order, cycles = 37; // 216.8 Hz at 48 kHz
    const auto numSamples = at (0.5) + size;
    Audio audio (2, numSamples);
    const auto amplitude = juce::Decibels::decibelsToGain (peakDecibels);

    for (int i = 0; i < numSamples; ++i)
        for (int channel = 0; channel < 2; ++channel)
            audio.setSample (channel, i, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * cycles * i / size));

    devices::Saturator saturator;
    saturator.prepare (rate, blockSize);

    for (int done = 0; done < numSamples; done += blockSize)
    {
        float* channels[] = { audio.getWritePointer (0, done), audio.getWritePointer (1, done) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = std::min (blockSize, numSamples - done);
        context.parameters = preset.values;
        context.isPlaying = true;
        saturator.process (context);
    }

    std::vector<float> spectrum ((size_t) size * 2, 0.0f);
    const auto start = numSamples - size;

    for (int i = 0; i < size; ++i)
        spectrum[(size_t) i] =
            audio.getSample (0, start + i) * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

    juce::dsp::FFT (order).performFrequencyOnlyForwardTransform (spectrum.data());
    const auto energyAt = [&spectrum] (int bin) // the window spreads a harmonic over its bin and the two beside it
    {
        return (double) spectrum[(size_t) bin - 1] * spectrum[(size_t) bin - 1] + (double) spectrum[(size_t) bin] * spectrum[(size_t) bin]
               + (double) spectrum[(size_t) bin + 1] * spectrum[(size_t) bin + 1];
    };
    auto harmonics = 0.0;

    for (int harmonic = 2; harmonic <= 10; ++harmonic)
        harmonics += energyAt (harmonic * cycles);

    return toDecibels (std::sqrt (harmonics / energyAt (cycles)));
}

//==============================================================================
// The exciter's presets: how far each lifts the octaves around 2, 4, 8 and 16 kHz of pink noise at a level (its RMS, in
// dBFS), in decibels: what comes out against what went in, in each octave, over 65536 samples after half a second. The
// louder the noise, the further its top reaches into the exciter's curve, which holds it down and makes harmonics of it
// instead: so a preset that lifts less as the level rises is one that saturates there.
constexpr double exciterOctaves[] = { 2000.0, 4000.0, 8000.0, 16000.0 };

std::array<float, std::size (exciterOctaves)> measureLift (const Preset& preset, float rmsDecibels)
{
    constexpr int order = 16, size = 1 << order;
    const auto numSamples = at (0.5) + size;
    Audio audio (2, numSamples), dry (2, numSamples);

    // Pink noise, the same every time (Paul Kellet's filter of white noise), then leveled.
    devices::dsp::Random random (1234);
    float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f;
    auto energy = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto white = random.nextBipolar();
        b0 = 0.99765f * b0 + white * 0.0990460f;
        b1 = 0.96300f * b1 + white * 0.2965164f;
        b2 = 0.57000f * b2 + white * 1.0526913f;
        const auto pink = b0 + b1 + b2 + white * 0.1848f;
        dry.setSample (0, i, pink);
        energy += (double) pink * pink;
    }

    dry.applyGain (0, 0, numSamples, juce::Decibels::decibelsToGain (rmsDecibels) / (float) std::sqrt (energy / numSamples));
    dry.copyFrom (1, 0, dry, 0, 0, numSamples);
    audio.makeCopyOf (dry);

    devices::Exciter exciter;
    exciter.prepare (rate, blockSize);

    for (int done = 0; done < numSamples; done += blockSize)
    {
        float* channels[] = { audio.getWritePointer (0, done), audio.getWritePointer (1, done) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = std::min (blockSize, numSamples - done);
        context.parameters = preset.values;
        context.isPlaying = true;
        exciter.process (context);
    }

    // Each one's power in each octave.
    const auto octavePowers = [numSamples] (const Audio& sound)
    {
        std::vector<float> spectrum ((size_t) size * 2, 0.0f);
        const auto start = numSamples - size;

        for (int i = 0; i < size; ++i)
            spectrum[(size_t) i] =
                sound.getSample (0, start + i) * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

        juce::dsp::FFT (order).performFrequencyOnlyForwardTransform (spectrum.data());
        std::array<double, std::size (exciterOctaves)> powers {};

        for (size_t octave = 0; octave < std::size (exciterOctaves); ++octave)
        {
            const auto from = (int) std::lround (exciterOctaves[octave] / std::sqrt (2.0) * size / rate);
            const auto to = std::min (size / 2, (int) std::lround (exciterOctaves[octave] * std::sqrt (2.0) * size / rate));

            for (int bin = from; bin < to; ++bin)
                powers[octave] += (double) spectrum[(size_t) bin] * spectrum[(size_t) bin];
        }

        return powers;
    };

    const auto in = octavePowers (dry), out = octavePowers (audio);
    std::array<float, std::size (exciterOctaves)> lifts {};

    for (size_t octave = 0; octave < lifts.size(); ++octave)
        lifts[octave] = toDecibels (std::sqrt (out[octave] / in[octave]));

    return lifts;
}

//==============================================================================
// The vinyl's presets, each measured while the song plays, at 120 BPM: how far a 1 kHz sine's pitch swings over 6 s (from
// its 1st to its 99th percentile, in cents: its wow and its flutter together); what it does to pink noise in six bands (the
// third octaves at 63, 125, 1000, 4000, 8000 and 16000 Hz, its noise off); how far it narrows the sides (a 1 kHz sine with
// its sides out of step); the distortion of a sine at 1 and at 8 kHz at -12 dBFS (the second to the fifth harmonics under
// 20 kHz, against the tone); and its noise on silence: the hiss's level and the crackles, how many a second and the loudest.
struct VinylMeasures
{
    float swing = 0.0f, side = 0.0f, thd1k = 0.0f, thd8k = 0.0f, hiss = 0.0f, clicks = 0.0f, cracklePeak = 0.0f;
    std::array<float, 6> bands {};
};

constexpr double vinylBands[] = { 63.0, 125.0, 1000.0, 4000.0, 8000.0, 16000.0 };

// A preset's values with its noise off, or with one of its noises alone.
std::vector<float> vinylValues (const Preset& preset, bool hiss, bool crackle)
{
    auto values = preset.values;

    if (! hiss)
        values[devices::Vinyl::hiss] = devices::Vinyl::info.parameters[devices::Vinyl::hiss].minimum;

    if (! crackle)
        values[devices::Vinyl::crackle] = devices::Vinyl::info.parameters[devices::Vinyl::crackle].minimum;

    return values;
}

// A sound through the vinyl, the song playing, and how many clicks it made.
std::pair<Audio, int> playVinyl (const std::vector<float>& values, Audio audio)
{
    devices::Vinyl vinyl;
    vinyl.prepare (rate, blockSize);
    vinyl.getSurfaceNoise().takeClicks();

    for (int done = 0; done < audio.getNumSamples(); done += blockSize)
    {
        float* channels[] = { audio.getWritePointer (0, done), audio.getWritePointer (1, done) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = std::min (blockSize, audio.getNumSamples() - done);
        context.parameters = values;
        context.isPlaying = true;
        context.beat = done * 2.0 / rate;
        vinyl.process (context);
    }

    const auto clicks = vinyl.getSurfaceNoise().takeClicks();
    return { std::move (audio), clicks };
}

Audio makeSine (double frequency, float amplitude, double seconds, bool sidesApart = false)
{
    Audio audio (2, at (seconds));

    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const auto sample = amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * i / rate);
        audio.setSample (0, i, sample);
        audio.setSample (1, i, sidesApart ? -sample : sample);
    }

    return audio;
}

// A tone's level in a stretch, and its harmonics', by the FFT, each summed over 2 % either side, which holds a wow.
float getHarmonicDistortion (const Audio& audio, double frequency)
{
    constexpr int order = 16, size = 1 << order;
    std::vector<float> spectrum ((size_t) size * 2, 0.0f);
    const auto start = audio.getNumSamples() - size;

    for (int i = 0; i < size; ++i)
        spectrum[(size_t) i] =
            audio.getSample (0, start + i) * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

    juce::dsp::FFT (order).performFrequencyOnlyForwardTransform (spectrum.data());
    const auto energyAround = [&spectrum] (double hertz)
    {
        const auto centre = (int) std::lround (hertz * size / rate), width = std::max (3, (int) std::lround (0.02 * hertz * size / rate));
        auto energy = 0.0;

        for (int bin = std::max (1, centre - width); bin <= std::min (size / 2 - 1, centre + width); ++bin)
            energy += (double) spectrum[(size_t) bin] * spectrum[(size_t) bin];

        return energy;
    };

    auto harmonics = 0.0;

    for (int harmonic = 2; harmonic <= 5 && harmonic * frequency < 20000.0; ++harmonic)
        harmonics += energyAround (harmonic * frequency);

    return toDecibels (std::sqrt (harmonics / energyAround (frequency)));
}

VinylMeasures measureVinyl (const Preset& preset)
{
    VinylMeasures measures;
    const auto quiet = vinylValues (preset, false, false);

    // The pitch: the sine's phase every millisecond, against a steady one, as cents from how fast it moves.
    {
        const auto played = playVinyl (quiet, makeSine (1000.0, 0.25f, 6.5)).first;
        constexpr int window = 192, step = 48; // four of the tone's cycles, every millisecond
        std::vector<double> late;
        auto unwrapped = 0.0, last = 0.0;

        for (int start = at (0.5); start + window < played.getNumSamples(); start += step)
        {
            auto s = 0.0, c = 0.0;

            for (int i = start; i < start + window; ++i)
            {
                s += played.getSample (0, i) * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate);
                c += played.getSample (0, i) * std::cos (juce::MathConstants<double>::twoPi * 1000.0 * i / rate);
            }

            const auto phase = std::atan2 (c, s);
            unwrapped = late.empty() ? phase : unwrapped + std::remainder (phase - last, juce::MathConstants<double>::twoPi);
            last = phase;
            late.push_back (-unwrapped / juce::MathConstants<double>::twoPi * rate / 1000.0);
        }

        std::vector<double> cents;

        for (size_t i = 1; i < late.size(); ++i)
            cents.push_back (1200.0 * std::log2 (1.0 - (late[i] - late[i - 1]) / step));

        std::sort (cents.begin(), cents.end());
        measures.swing = cents.empty() ? 0.0f : (float) (cents[cents.size() * 99 / 100] - cents[cents.size() / 100]);
    }

    // The band, on pink noise (Paul Kellet's filter), each third octave's power out against in.
    {
        Audio noise (2, at (4.0));
        devices::dsp::Random random (1234);
        float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f;

        for (int i = 0; i < noise.getNumSamples(); ++i)
        {
            const auto white = random.nextBipolar();
            b0 = 0.99765f * b0 + white * 0.0990460f;
            b1 = 0.96300f * b1 + white * 0.2965164f;
            b2 = 0.57000f * b2 + white * 1.0526913f;
            const auto pink = 0.1f * (b0 + b1 + b2 + white * 0.1848f);
            noise.setSample (0, i, pink);
            noise.setSample (1, i, pink);
        }

        auto bandless = quiet;
        bandless[devices::Vinyl::distortion] = 0.0f;
        const auto played = playVinyl (bandless, noise).first;
        constexpr int order = 16, size = 1 << order;
        const auto powers = [&] (const Audio& sound)
        {
            std::vector<float> spectrum ((size_t) size * 2, 0.0f);
            const auto start = sound.getNumSamples() - size;

            for (int i = 0; i < size; ++i)
                spectrum[(size_t) i] =
                    sound.getSample (0, start + i) * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

            juce::dsp::FFT (order).performFrequencyOnlyForwardTransform (spectrum.data());
            std::array<double, std::size (vinylBands)> each {};

            for (size_t band = 0; band < each.size(); ++band)
            {
                const auto from = (int) std::lround (vinylBands[band] / std::pow (2.0, 1.0 / 6.0) * size / rate);
                const auto to = std::min (size / 2, (int) std::lround (vinylBands[band] * std::pow (2.0, 1.0 / 6.0) * size / rate));

                for (int bin = from; bin <= to; ++bin)
                    each[band] += (double) spectrum[(size_t) bin] * spectrum[(size_t) bin];
            }

            return each;
        };

        const auto in = powers (noise), out = powers (played);

        for (size_t band = 0; band < measures.bands.size(); ++band)
            measures.bands[band] = toDecibels (std::sqrt (out[band] / in[band]));
    }

    // The sides: a 1 kHz sine out of step on the two, and what is left of it.
    {
        const auto played = playVinyl (quiet, makeSine (1000.0, 0.25f, 1.5, true)).first;
        auto side = 0.0;

        for (int i = at (1.0); i < played.getNumSamples(); ++i)
        {
            const auto each = 0.5 * (played.getSample (0, i) - played.getSample (1, i));
            side += each * each;
        }

        measures.side = toDecibels (std::sqrt (side / (played.getNumSamples() - at (1.0))) / (0.25 / std::sqrt (2.0)));
    }

    // The groove's distortion.
    measures.thd1k = getHarmonicDistortion (playVinyl (quiet, makeSine (1000.0, 0.25f, 2.0)).first, 1000.0);
    measures.thd8k = getHarmonicDistortion (playVinyl (quiet, makeSine (8000.0, 0.25f, 2.0)).first, 8000.0);

    // Its noise on silence: the hiss alone, then the crackle alone.
    {
        Audio silence (2, at (3.0));
        silence.clear();
        const auto played = playVinyl (vinylValues (preset, true, false), silence).first;
        auto energy = 0.0;

        for (int channel = 0; channel < 2; ++channel)
            for (int i = at (0.5); i < played.getNumSamples(); ++i)
                energy += (double) played.getSample (channel, i) * played.getSample (channel, i);

        measures.hiss = toDecibels (std::sqrt (energy / (2.0 * (played.getNumSamples() - at (0.5)))));
    }

    {
        Audio silence (2, at (10.0));
        silence.clear();
        const auto [played, clicks] = playVinyl (vinylValues (preset, false, true), silence);
        measures.clicks = (float) clicks / 10.0f;
        measures.cracklePeak =
            toDecibels (std::max (played.getMagnitude (0, 0, played.getNumSamples()), played.getMagnitude (1, 0, played.getNumSamples())));
    }

    return measures;
}

//==============================================================================
// The EQ's presets: what each does to every frequency, its response at the third-octave bands' centres in decibels, from an
// impulse through it (its first 65536 samples, which every band of Phazite's EQ has settled in), at a quarter of full scale
// so that nothing in it clips.
constexpr float responseCentres[] { 31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f };

std::vector<float> measureResponse (const Preset& preset)
{
    constexpr int order = 16, size = 1 << order;
    constexpr float impulse = 0.25f;
    Audio audio (2, size);
    audio.clear();
    audio.setSample (0, 0, impulse);
    audio.setSample (1, 0, impulse);

    devices::Equalizer equalizer;
    equalizer.prepare (rate, blockSize);

    for (int done = 0; done < size; done += blockSize)
    {
        float* channels[] = { audio.getWritePointer (0, done), audio.getWritePointer (1, done) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = std::min (blockSize, size - done);
        context.parameters = preset.values;
        context.isPlaying = true;
        equalizer.process (context);
    }

    std::vector<float> spectrum ((size_t) size * 2, 0.0f);

    for (int i = 0; i < size; ++i)
        spectrum[(size_t) i] = audio.getSample (0, i) / impulse;

    juce::dsp::FFT (order).performFrequencyOnlyForwardTransform (spectrum.data());
    std::vector<float> response;

    for (const auto centre : responseCentres)
    {
        // The band's power, averaged over its third of an octave.
        const auto low = (int) std::floor (centre / std::pow (2.0f, 1.0f / 6.0f) * size / (float) rate);
        const auto high = std::max (low + 1, (int) std::ceil (centre * std::pow (2.0f, 1.0f / 6.0f) * size / (float) rate));
        auto power = 0.0;

        for (int bin = low; bin < high; ++bin)
            power += (double) spectrum[(size_t) bin] * spectrum[(size_t) bin];

        response.push_back (toDecibels ((float) std::sqrt (power / (high - low))));
    }

    return response;
}
} // namespace

TEST_CASE ("Preset report: what each instrument preset sounds like, in numbers and WAV files" * doctest::test_suite ("preset-report")
           * doctest::skip())
{
    auto folder = juce::File (juce::SystemStats::getEnvironmentVariable ("PHAZITE_PRESET_REPORT", {}));

    if (folder == juce::File())
        folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Phazite Preset Report");

    folder.createDirectory();
    const auto root = phazite::test::getFactoryPresetsFolder();
    auto files = root.findChildFiles (juce::File::findFiles, true, juce::String ("*") + phazite::presetExtension);
    files.sort();

    juce::StringArray lines, drumLines;
    lines.add ("# Phazite's instrument presets, measured");
    lines.add ({});
    lines.add (
        "Each played as one note at its kind's pitch (bass C2, pad C3, the rest C4; a sampler at its root, or its first slice), "
        "velocity 100, held 2 s. Levels in dBFS; LUFS = integrated, M max = the loudest momentary (400 ms) loudness; Hi = the highest 10 ms level; Atk = until within 1 dB of it; "
        "@0.25/@1/@1.9 = the level then against Hi; Rel = from the note's end to 40 dB under where it was then; "
        "Bright = spectral centroid at 30 ms, 250 ms and 1 s, in Hz; Pitch = what YIN hears, against the note played, in "
        "cents; Corr = left against right (1 is mono); Vel = the note at velocity 50 against 100: its level, and its "
        "brightness at 100 ms in octaves.");
    lines.add ({});
    lines.add (
        "| Preset | Note | Peak | LUFS | M max | Hi | Atk ms | @0.25 | @1 | @1.9 | Rel ms | Bright 30/250/1000 | Pitch | Corr | Vel dB | Vel oct | Chord pk |");
    lines.add ("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|");

    drumLines.add ({});
    drumLines.add ("## Kits, pad by pad");
    drumLines.add ({});
    drumLines.add (
        "Each pad hit alone at velocity 100: Peak, and M max = the loudest momentary (400 ms) loudness; Len = until 40 dB under its highest level; Bright at 5 ms and "
        "60 ms; Pitch = what YIN hears; Corr = left against right.");
    drumLines.add ({});
    drumLines.add ("| Kit | Pad | Sound | Peak | M max | Len ms | Bright 5/60 | Pitch Hz | Corr |");
    drumLines.add ("|---|---|---|---|---|---|---|---|---|");

    // First each instrument as a new one plays (its defaults, the measure the presets are leveled against), then the presets;
    // with PHAZITE_PRESET_FILTER, only those whose path holds it ("Synth/Bass").
    std::vector<Preset> presets;

    for (const auto* info : phazite::getOwnDevices())
    {
        if (info->kind != devices::DeviceKind::instrument)
            continue;

        Preset preset;
        preset.info = info;
        preset.device = toString (info->name);
        preset.kind = "Default";
        preset.name = "(new " + preset.device + ")";

        for (const auto& parameter : info->parameters)
            preset.values.push_back (parameter.defaultValue);

        if (toString (info->typeID) == devices::Sampler::typeID)
        {
            auto sounds = std::make_unique<juce::DynamicObject>();
            sounds->setProperty (phazite::schema::samplerSample, toString (devices::getFactorySampleNames()[0]));
            preset.sounds = juce::var (sounds.release());
        }

        presets.push_back (std::move (preset));
    }

    const auto filter = juce::SystemStats::getEnvironmentVariable ("PHAZITE_PRESET_FILTER", {});

    for (const auto& file : files)
        if (auto preset = readPreset (file);
            preset.has_value() && file.getRelativePathFrom (root).replaceCharacter ('\\', '/').contains (filter))
            presets.push_back (std::move (*preset));

    for (const auto& each : presets)
    {
        const auto* preset = &each;
        CAPTURE (preset->name.toStdString());
        const auto out = folder.getChildFile (preset->device).getChildFile (preset->kind);

        if (preset->device == "Drum Sampler")
        {
            int numSamples = 0;
            const auto beat = getBeat (numSamples);
            writeWav (render (*preset, beat, numSamples), out.getChildFile (preset->name + " - beat.wav"));

            for (int pad = devices::DrumSampler::kitFirstNote; pad < devices::DrumSampler::kitFirstNote + devices::DrumSampler::kitPads;
                 ++pad)
            {
                Events hit;
                note (hit, 0.0, 0.1, pad);
                const auto audio = render (*preset, hit, at (3.0));
                const auto envelope = getEnvelope (audio);
                const auto highest = *std::max_element (envelope.begin(), envelope.end());
                auto length = (int) envelope.size() * 10;

                for (auto step = (int) envelope.size() - 1; step >= 0; --step)
                {
                    if (envelope[(size_t) step] >= highest - 40.0f)
                    {
                        length = (step + 1) * 10;
                        break;
                    }
                }

                const auto pitch = devices::detectPitch (audio, rate);
                drumLines.add ("| " + preset->name + " | " + juce::String (pad) + " | "
                               + preset->sounds[phazite::schema::getPadSound (pad)].toString() + " | "
                               + decibels (toDecibels (audio.getMagnitude (0, audio.getNumSamples()))) + " | "
                               + decibels (getLoudness (audio).momentary) + " | " + juce::String (length) + " | "
                               + hertz (getCentroid (audio, 0.005)) + "/" + hertz (getCentroid (audio, 0.06)) + " | "
                               + (pitch.has_value() ? juce::String (440.0 * std::pow (2.0, (*pitch - 69.0) / 12.0), 1) : juce::String ("-"))
                               + " | " + juce::String (getCorrelation (audio, audio.getNumSamples()), 2) + " |");
            }

            continue;
        }

        const auto played = getTestNote (*preset);
        Events loud, soft;
        note (loud, 0.0, held, played, 0.8f);
        note (soft, 0.0, held, played, 0.4f);
        const auto audio = render (*preset, loud, at (held + after)), softAudio = render (*preset, soft, at (held + after));
        writeWav (audio, out.getChildFile (preset->name + " - note.wav"));
        const auto measured = measureNote (audio);
        const auto softer = measureNote (softAudio);
        const auto hard = getCentroid (audio, 0.1), gentle = getCentroid (softAudio, 0.1);

        // The chord Phazite's preset tests play (tests/FactoryPresetTests.cpp): a bass note and a triad over it, half a second.
        Events chord;

        for (const auto tone : { 36, 48, 52, 55 })
            note (chord, 0.0, 0.5, tone);

        const auto chordAudio = render (*preset, chord, at (1.5));
        const auto chordPeak = toDecibels (chordAudio.getMagnitude (0, chordAudio.getNumSamples()));

        int numSamples = 0;
        const auto phrase = getPhrase (*preset, numSamples);
        writeWav (render (*preset, phrase, numSamples), out.getChildFile (preset->name + " - phrase.wav"));

        const auto pitch =
            measured.pitch.has_value() ? juce::String (juce::roundToInt ((*measured.pitch - played) * 100.0)) : juce::String ("-");
        lines.add ("| " + preset->device + " / " + preset->kind + " / " + preset->name + " | " + juce::String (played) + " | "
                   + decibels (measured.peak) + " | " + decibels (measured.loudness.integrated) + " | "
                   + decibels (measured.loudness.momentary) + " | " + decibels (measured.highest) + " | "
                   + juce::String (juce::roundToInt (measured.attack)) + " | " + decibels (measured.quarter) + " | "
                   + decibels (measured.second) + " | " + decibels (measured.end) + " | " + measured.release + " | "
                   + hertz (measured.bright30) + "/" + hertz (measured.bright250) + "/" + hertz (measured.bright1000) + " | " + pitch
                   + " | " + juce::String (measured.correlation, 2) + " | " + decibels (softer.highest - measured.highest) + " | "
                   + (hard > 0.0f && gentle > 0.0f ? juce::String (std::log2 (gentle / hard), 2) : juce::String ("-")) + " | "
                   + decibels (chordPeak) + " |");
    }

    // The sidechain presets, each under every kit's kick (the kits whatever PHAZITE_PRESET_FILTER says), and the Warm Pad's
    // chord pumping under it, with the kick, to listen to.
    std::vector<Preset> kits, sidechains;

    for (const auto& file : files)
    {
        if (auto kit = readPreset (file); kit.has_value() && kit->device == "Drum Sampler")
            kits.push_back (std::move (*kit));
        else if (auto effect = readPreset (file, devices::DeviceKind::effect);
                 effect.has_value() && toString (effect->info->typeID) == devices::Compressor::typeID && effect->kind == "Sidechain"
                 && file.getRelativePathFrom (root).replaceCharacter ('\\', '/').contains (filter))
            sidechains.push_back (std::move (*effect));
    }

    juce::StringArray pumpLines;
    pumpLines.add ({});
    pumpLines.add ("## Sidechain presets, pumping");
    pumpLines.add ({});
    pumpLines.add ("Each ducks a steady tone (a sine at -12 dBFS) under a kit's kick on every beat at 120 bpm (a beat is 500 ms), "
                   "the kick as the kit's drum sampler plays it: Depth = how far down at the beat; Back 3/1 dB = how long from the "
                   "beat until the tone is back within 3 dB and within 1 dB (500 = not within the beat); the averages of the third to "
                   "the eighth beats. Beside it, a WAV of the Warm Pad's chord pumping under each kit's kick.");
    pumpLines.add ({});
    pumpLines.add ("| Preset | Kit's kick | Depth dB | Back 3 dB ms | Back 1 dB ms |");
    pumpLines.add ("|---|---|---|---|---|");

    const auto pad = readPreset (root.getChildFile ("Synth/Pad/Warm Pad" + juce::String (phazite::presetExtension)));
    Events chord;

    for (const auto tone : { 48, 55, 60, 64 })
        note (chord, 0.0, pumpBeat * pumpBeats, tone);

    const auto padAudio = pad.has_value() ? render (*pad, chord, at (pumpBeat * pumpBeats)) : Audio();
    std::vector<Audio> keys;

    for (const auto& kit : kits)
        keys.push_back (renderKicks (kit));

    for (const auto& sidechain : sidechains)
    {
        for (size_t k = 0; k < kits.size(); ++k)
        {
            const auto pump = measurePump (sidechain, keys[k]);
            pumpLines.add ("| " + sidechain.name + " | " + kits[k].name + " | " + decibels (pump.depth) + " | "
                           + juce::String (juce::roundToInt (pump.within3)) + " | " + juce::String (juce::roundToInt (pump.within1))
                           + " |");

            if (padAudio.getNumSamples() > 0)
            {
                auto pumped = compress (sidechain, padAudio, keys[k]);

                for (int channel = 0; channel < 2; ++channel)
                    pumped.addFrom (channel, 0, keys[k], channel, 0, keys[k].getNumSamples());

                writeWav (pumped, folder.getChildFile ("Compressor")
                                      .getChildFile ("Sidechain")
                                      .getChildFile (sidechain.name + " - " + kits[k].name + ".wav"));
            }
        }
    }

    // The saturator's presets, each on a sine at three levels.
    juce::StringArray driveLines;
    driveLines.add ({});
    driveLines.add ("## Saturator presets, colour");
    driveLines.add ({});
    driveLines.add ("Each on a sine at 217 Hz peaking at -18, -12 and -6 dBFS: its total harmonic distortion, the second to the tenth "
                    "harmonics against the fundamental, in dB (-40 dB is 1 %, -20 dB is 10 %).");
    driveLines.add ({});
    driveLines.add ("| Preset | THD at -18 | THD at -12 | THD at -6 |");
    driveLines.add ("|---|---|---|---|");

    for (const auto& file : files)
        if (auto effect = readPreset (file, devices::DeviceKind::effect);
            effect.has_value() && toString (effect->info->typeID) == devices::Saturator::typeID
            && file.getRelativePathFrom (root).replaceCharacter ('\\', '/').contains (filter))
            driveLines.add ("| " + effect->kind + " / " + effect->name + " | " + decibels (measureDistortion (*effect, -18.0f)) + " | "
                            + decibels (measureDistortion (*effect, -12.0f)) + " | " + decibels (measureDistortion (*effect, -6.0f))
                            + " |");

    // The exciter's presets, each on pink noise at three levels.
    juce::StringArray exciterLines;
    exciterLines.add ({});
    exciterLines.add ("## Exciter presets, what they add");
    exciterLines.add ({});
    exciterLines.add ("Each on pink noise at -30, -18 and -6 dBFS (RMS): how far it lifts the octaves around 2, 4, 8 and 16 kHz, in dB. "
                      "Where the lift falls as the level rises, the band reaches the exciter's curve there, which holds it down and "
                      "makes harmonics of it.");
    exciterLines.add ({});
    exciterLines.add ("| Preset | At -30: 2k / 4k / 8k / 16k | At -18 | At -6 |");
    exciterLines.add ("|---|---|---|---|");

    for (const auto& file : files)
    {
        if (auto effect = readPreset (file, devices::DeviceKind::effect);
            effect.has_value() && toString (effect->info->typeID) == devices::Exciter::typeID
            && file.getRelativePathFrom (root).replaceCharacter ('\\', '/').contains (filter))
        {
            auto line = "| " + effect->kind + " / " + effect->name;

            for (const auto level : { -30.0f, -18.0f, -6.0f })
            {
                juce::StringArray lifts;

                for (const auto lift : measureLift (*effect, level))
                    lifts.add (decibels (lift));

                line += " | " + lifts.joinIntoString (" / ");
            }

            exciterLines.add (line + " |");
        }
    }

    // The vinyl's presets, each on its test signals.
    juce::StringArray vinylLines;
    vinylLines.add ({});
    vinylLines.add ("## Vinyl presets, what they do");
    vinylLines.add ({});
    vinylLines.add ("Each while the song plays: Swing = a 1 kHz sine's pitch from its 1st to its 99th percentile, in cents (its wow and "
                    "flutter together); the band = pink noise's change in the third octaves at 63 Hz, 125 Hz, 1, 4, 8 and 16 kHz (its "
                    "noise off), in dB; Side = a 1 kHz sine out of step on the two sides, in dB; THD = the 2nd to 5th harmonics of a "
                    "sine at -12 dBFS, at 1 and 8 kHz, in dB; Hiss = on silence, RMS dBFS; Crackle = clicks a second, and the "
                    "loudest's peak in dBFS.");
    vinylLines.add ({});
    vinylLines.add ("| Preset | Swing ct | 63 / 125 / 1k / 4k / 8k / 16k | Side | THD 1k / 8k | Hiss | Crackle |");
    vinylLines.add ("|---|---|---|---|---|---|---|");

    for (const auto& file : files)
    {
        if (auto effect = readPreset (file, devices::DeviceKind::effect);
            effect.has_value() && toString (effect->info->typeID) == devices::Vinyl::typeID
            && file.getRelativePathFrom (root).replaceCharacter ('\\', '/').contains (filter))
        {
            const auto measured = measureVinyl (*effect);
            juce::StringArray bands;

            for (const auto band : measured.bands)
                bands.add (decibels (band));

            vinylLines.add ("| " + effect->kind + " / " + effect->name + " | " + juce::String (measured.swing, 1) + " | "
                            + bands.joinIntoString (" / ") + " | " + decibels (measured.side) + " | " + decibels (measured.thd1k) + " / "
                            + decibels (measured.thd8k) + " | " + decibels (measured.hiss) + " | " + juce::String (measured.clicks, 1)
                            + ", " + decibels (measured.cracklePeak) + " |");
        }
    }

    // The EQ's presets, each its response.
    juce::StringArray eqLines;
    eqLines.add ({});
    eqLines.add ("## EQ presets, response");
    eqLines.add ({});
    eqLines.add ("Each its gain at the centres of the third-octave bands, in dB, from an impulse through it.");
    eqLines.add ({});
    juce::String header ("| Preset |"), rule ("|---|");

    for (const auto centre : responseCentres)
    {
        const auto whole = juce::exactlyEqual (centre, std::floor (centre));
        header << " " << (centre < 1000.0f ? juce::String (centre, whole ? 0 : 1) : juce::String (centre / 1000.0f) + "k") << " |";
        rule << "---|";
    }

    eqLines.add (header);
    eqLines.add (rule);

    for (const auto& file : files)
        if (auto effect = readPreset (file, devices::DeviceKind::effect);
            effect.has_value() && toString (effect->info->typeID) == devices::Equalizer::typeID
            && file.getRelativePathFrom (root).replaceCharacter ('\\', '/').contains (filter))
        {
            juce::String line ("| " + effect->kind + " / " + effect->name + " |");

            for (const auto gain : measureResponse (*effect))
                line << " " << decibels (gain) << " |";

            eqLines.add (line);
        }

    lines.addArray (drumLines);
    lines.addArray (pumpLines);
    lines.addArray (driveLines);
    lines.addArray (exciterLines);
    lines.addArray (vinylLines);
    lines.addArray (eqLines);
    const auto report = folder.getChildFile ("report.md");
    REQUIRE (report.replaceWithText (lines.joinIntoString ("\n") + "\n"));
    MESSAGE ("The preset report is in " << folder.getFullPathName().toStdString());
}
