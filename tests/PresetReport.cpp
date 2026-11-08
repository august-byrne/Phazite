// SPDX-License-Identifier: AGPL-3.0-or-later
// What each instrument preset that comes with Phazite (resources/presets) sounds like, in numbers, for whoever makes or
// checks them (docs/presets.md): each played as one note at its kind's pitch, at velocity 100 and again at 50 (for what
// velocity does), measured for its level, loudness, attack, decay, release, brightness as it goes on, pitch and width; and
// each kit's pads one at a time. It writes a report and WAV files to listen to: the note, and a phrase of its kind of sound
// (a bass line, a melody, chords, an arpeggio, a beat). ctest and CI skip it; run it from a build in the repository with
//   phazite_tests.exe --test-suite=preset-report --no-skip
// with the environment variable PHAZITE_PRESET_REPORT naming the folder for report.md and the WAV files (else a temporary
// folder, which it names).
#include "TestHelpers.h"
#include "devices/drums/DrumSampler.h"
#include "devices/drums/FactoryDrums.h"
#include "devices/fm/FmSynth.h"
#include "devices/sampler/FactorySamples.h"
#include "devices/sampler/SampleAnalysis.h"
#include "devices/sampler/Sampler.h"
#include "devices/synth/Synth.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "devices/wavetable/WavetableSynth.h"
#include "engine/nodes/LoudnessMeter.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/Devices.h"
#include "model/presets/PresetModel.h"
#include "model/presets/internal/Presets.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
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

std::optional<Preset> readPreset (const juce::File& file)
{
    const auto device = phazite::readPreset (file)[keys::device];

    for (const auto* info : phazite::getOwnDevices())
    {
        if (toString (info->typeID) != device[keys::type].toString() || info->kind != devices::DeviceKind::instrument)
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
                    preset.values[(size_t) index] = devices::constrain (info->parameters[(size_t) index], (float) value);

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
    std::vector<float> data (2 * size, 0.0f);

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
    const auto pad = [] (int index) { return devices::DrumSampler::firstNote + index; };

    for (const auto each : kicks)
        note (events, each * step, step, pad (0));

    for (const auto each : { 4, 12, 20, 28 })
        note (events, each * step, step, pad (2));

    note (events, 28 * step, step, pad (3));

    for (int each = 0; each < 32; each += 2)
        note (events, each * step, step, each % 16 == 14 ? pad (10) : pad (6), each % 4 == 0 ? 0.8f : 0.55f);

    for (int index = 0; index < devices::DrumSampler::numPads; ++index)
        note (events, (32 + index * 2) * step, step, pad (index));

    numSamples = at ((32 + 2 * devices::DrumSampler::numPads) * step + 2.0);
    return events;
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

            for (int pad = 0; pad < devices::DrumSampler::numPads; ++pad)
            {
                Events hit;
                note (hit, 0.0, 0.1, devices::DrumSampler::firstNote + pad);
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
                drumLines.add ("| " + preset->name + " | " + juce::String (pad + 1) + " | "
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

    lines.addArray (drumLines);
    const auto report = folder.getChildFile ("report.md");
    REQUIRE (report.replaceWithText (lines.joinIntoString ("\n") + "\n"));
    MESSAGE ("The preset report is in " << folder.getFullPathName().toStdString());
}
