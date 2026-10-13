// SPDX-License-Identifier: AGPL-3.0-or-later
// Makes Phazite Sounds, the free sounds Phazite offers to download when it first opens (docs/design.md, section 11):
// one-shots from CC0 sample libraries, trimmed, faded and normalized; drums and effects that Phazite synthesizes
// itself; and loops played from those one-shots. The pack and the recipes for its sounds (recipes.json) live in their
// own repository, phazite-sounds, so that Phazite's stays small; this tool, built with Phazite, renders them. Every
// sound is CC0 1.0, so songs made with them carry no conditions, and the pack's sources.json records where each came
// from.
//
// Usage:
//   phazite_soundkit <recipes.json> <folder with the downloaded sources> <pack folder>   renders the sounds
//   phazite_soundkit --zip <pack folder> <zip file>                                      makes the pack's download
//   phazite_soundkit --info <folder of sounds>                                           measures the sounds
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <map>
#include <utility>
#include <vector>

namespace
{
constexpr double rate = 44100.0;
constexpr double twoPi = juce::MathConstants<double>::twoPi;
using Buffer = juce::AudioBuffer<float>;

//==============================================================================
// Reading, shaping and writing sounds.
// Low-passes a buffer in place: four second-order sections, as an eighth-order Butterworth-like filter.
void lowPass (Buffer& buffer, double sampleRate, double cutoff)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        for (int section = 0; section < 4; ++section)
        {
            juce::IIRFilter filter;
            filter.setCoefficients (juce::IIRCoefficients::makeLowPass (sampleRate, cutoff, 0.707));
            filter.processSamples (buffer.getWritePointer (channel), buffer.getNumSamples());
        }
    }
}

// The sound at 44.1 kHz, as at most two channels; a stereo sound whose channels are the same becomes mono.
Buffer readSound (juce::AudioFormatManager& formats, const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        std::cerr << "Can't read " << file.getFullPathName() << std::endl;
        std::exit (1);
    }

    const auto channels = juce::jmin (2, (int) reader->numChannels);
    Buffer buffer (channels, (int) reader->lengthInSamples + 256); // the end padded for the resampler
    buffer.clear();
    reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, channels > 1);

    if (channels == 2)
    {
        auto same = true;

        for (int i = 0; i < buffer.getNumSamples() && same; ++i)
            same = std::abs (buffer.getSample (0, i) - buffer.getSample (1, i)) < 1.0e-4f;

        if (same)
            buffer.setSize (1, buffer.getNumSamples(), true);
    }

    if (std::abs (reader->sampleRate - rate) < 0.5)
    {
        buffer.setSize (buffer.getNumChannels(), (int) reader->lengthInSamples, true);
        return buffer;
    }

    // Under the new Nyquist frequency first, when the rate goes down.
    const auto ratio = reader->sampleRate / rate; // samples read for each one written

    if (ratio > 1.0)
        lowPass (buffer, reader->sampleRate, 0.45 * rate);

    const auto length = (int) std::floor ((double) reader->lengthInSamples / ratio);
    Buffer resampled (buffer.getNumChannels(), length);

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        juce::WindowedSincInterpolator interpolator;
        interpolator.process (ratio, buffer.getReadPointer (channel), resampled.getWritePointer (channel), length);
    }

    return resampled;
}

float getPeak (const Buffer& buffer)
{
    auto peak = 0.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        peak = juce::jmax (peak, buffer.getMagnitude (channel, 0, buffer.getNumSamples()));

    return peak;
}

void normalize (Buffer& buffer, float peakDecibels = -1.0f)
{
    if (const auto peak = getPeak (buffer); peak > 0.0f)
        buffer.applyGain (juce::Decibels::decibelsToGain (peakDecibels) / peak);
}

int findFirstAbove (const Buffer& buffer, float level)
{
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            if (std::abs (buffer.getSample (channel, i)) > level)
                return i;

    return 0;
}

int findLastAbove (const Buffer& buffer, float level)
{
    for (int i = buffer.getNumSamples(); --i >= 0;)
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            if (std::abs (buffer.getSample (channel, i)) > level)
                return i;

    return buffer.getNumSamples() - 1;
}

Buffer slice (const Buffer& buffer, int start, int length)
{
    Buffer part (buffer.getNumChannels(), length);
    part.clear();

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        part.copyFrom (channel, 0, buffer, channel, start, juce::jmin (length, buffer.getNumSamples() - start));

    return part;
}

void fadeIn (Buffer& buffer, double seconds)
{
    buffer.applyGainRamp (0, juce::jmin (buffer.getNumSamples(), (int) (seconds * rate)), 0.0f, 1.0f);
}

void fadeOut (Buffer& buffer, double seconds)
{
    const auto length = juce::jmin (buffer.getNumSamples(), (int) (seconds * rate));
    buffer.applyGainRamp (buffer.getNumSamples() - length, length, 1.0f, 0.0f);
}

// How a one-shot is shaped after it is read or made.
struct Shape
{
    double maxSeconds = 0.0;      // 0: as long as it rings, down to -60 dB
    double fadeOutSeconds = 0.01; // at its end
    bool reverse = false;
    bool keepLength = false; // a loop: no trimming
};

// Trimmed from its first sound to where it fades under -60 dB, shortened to a maximum, faded, and normalized.
Buffer shape (Buffer buffer, const Shape& how)
{
    normalize (buffer);

    if (! how.keepLength)
    {
        constexpr auto silence = 0.001f; // -60 dB
        const auto start = juce::jmax (0, findFirstAbove (buffer, silence) - (int) (0.0005 * rate));
        auto end = findLastAbove (buffer, silence) + (int) (0.005 * rate);

        if (how.maxSeconds > 0.0)
            end = juce::jmin (end, start + (int) (how.maxSeconds * rate));

        buffer = slice (buffer, start, juce::jmin (end, buffer.getNumSamples()) - start);
    }

    if (how.reverse)
        buffer.reverse (0, buffer.getNumSamples());

    fadeIn (buffer, how.reverse ? 0.05 : 0.0002);
    fadeOut (buffer, how.fadeOutSeconds);
    normalize (buffer);
    return buffer;
}

// The dither's seed for a sound, from its path in the pack: a sound always comes out as the same bytes, whatever the
// recipes add, remove or reorder around it.
juce::int64 getDitherSeed (const juce::String& path)
{
    const auto hash = juce::SHA256 (path.toUTF8()).getRawData();
    const auto* bytes = static_cast<const juce::uint8*> (hash.getData());
    juce::uint64 seed = 0;

    for (size_t i = 0; i < 8; ++i)
        seed = (seed << 8) | juce::uint64 { bytes[i] };

    return (juce::int64) seed;
}

// FLAC, 16 bits with triangular dither, at the best compression.
void writeSound (const Buffer& sound, const juce::File& file, juce::int64 seed)
{
    Buffer dithered (sound);
    juce::Random random (seed);

    for (int channel = 0; channel < dithered.getNumChannels(); ++channel)
        for (int i = 0; i < dithered.getNumSamples(); ++i)
            dithered.addSample (channel, i, (random.nextFloat() - random.nextFloat()) / 32768.0f);

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::FlacAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions {}
                                                                       .withSampleRate (rate)
                                                                       .withNumChannels (dithered.getNumChannels())
                                                                       .withBitsPerSample (16)
                                                                       .withQualityOptionIndex (8));

    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (dithered, 0, dithered.getNumSamples()))
    {
        std::cerr << "Can't write " << file.getFullPathName() << std::endl;
        std::exit (1);
    }
}

//==============================================================================
// Synthesis, after Sound On Sound's "Synth Secrets" on drums (2002) and the analysis of the 808's circuits by Werner,
// Abel and Smith (DAFx-14, ICMC 2014). Each recipe is seeded, so the sounds come out the same every time.
float noise (juce::Random& random)
{
    return random.nextFloat() * 2.0f - 1.0f;
}

float softClip (double x, double drive)
{
    return (float) (std::tanh (drive * x) / std::tanh (drive));
}

// A kick or an 808: a sine whose pitch falls from startHz to endHz, a click, and saturation.
Buffer makeKick (double startHz, double endHz, double pitchSeconds, double decaySeconds, double length, double click, double drive)
{
    Buffer sound (1, (int) (length * rate));
    juce::Random random (1);
    juce::IIRFilter clickFilter;
    clickFilter.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 2000.0, 0.707));
    auto phase = 0.0;

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        phase += twoPi * (endHz + (startHz - endHz) * std::exp (-t / pitchSeconds)) / rate;
        const auto body = std::sin (phase) * std::exp (-t / decaySeconds) * juce::jmin (1.0, t / 0.0005);
        const auto snap = click * clickFilter.processSingleSampleRaw (noise (random)) * std::exp (-t / 0.0025);
        sound.setSample (0, i, softClip (body + snap, drive));
    }

    return sound;
}

// A drum's shell, two tuned sines, and its snares, high-passed noise.
Buffer makeSnare()
{
    Buffer sound (1, (int) (0.45 * rate));
    juce::Random random (2);
    juce::IIRFilter highPass, smooth;
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 800.0, 0.707));
    smooth.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, 9000.0, 0.707));

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        const auto shell =
            0.6 * std::sin (twoPi * 185.0 * t) * std::exp (-t / 0.07) + 0.4 * std::sin (twoPi * 330.0 * t) * std::exp (-t / 0.045);
        const auto snares = 0.55 * smooth.processSingleSampleRaw (highPass.processSingleSampleRaw (noise (random))) * std::exp (-t / 0.11);
        sound.setSample (0, i, softClip ((shell + snares) * juce::jmin (1.0, t / 0.0005), 1.6));
    }

    return sound;
}

// Band-passed noise, struck four times about 10 ms apart, then a tail: several hands, as the 808's clap does it.
Buffer makeClap()
{
    Buffer sound (1, (int) (0.55 * rate));
    juce::Random random (3);
    juce::IIRFilter bandPass, highPass;
    bandPass.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, 1100.0, 1.3));
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 500.0, 0.707));
    constexpr std::array<double, 4> strikes { 0.0, 0.0105, 0.0205, 0.031 };

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        auto envelope = t >= strikes.back() ? std::exp (-(t - strikes.back()) / 0.13) : 0.0;

        for (const auto strike : strikes)
            if (t >= strike)
                envelope = juce::jmax (envelope, std::exp (-(t - strike) / 0.0045));

        sound.setSample (0, i, (float) (envelope * highPass.processSingleSampleRaw (bandPass.processSingleSampleRaw (noise (random)))));
    }

    return sound;
}

// Six square waves at the 808's cymbal frequencies, band-passed and high-passed: a hat, closed or open.
Buffer makeHat (double decaySeconds, double length)
{
    static constexpr std::array<double, 6> frequencies { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
    Buffer sound (1, (int) (length * rate));
    juce::Random random (4);
    juce::IIRFilter bandPass, highPass;
    bandPass.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, 7100.0, 1.0));
    highPass.setCoefficients (juce::IIRCoefficients::makeHighPass (rate, 6000.0, 0.707));

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        auto metal = 0.0;

        for (const auto frequency : frequencies)
            metal += std::fmod (t * frequency, 1.0) < 0.5 ? 1.0 : -1.0;

        const auto x = highPass.processSingleSampleRaw (bandPass.processSingleSampleRaw ((float) (metal / 6.0 + 0.15 * noise (random))));
        sound.setSample (0, i, (float) (x * std::exp (-t / decaySeconds)));
    }

    return sound;
}

// Two square waves, 540 and 800 Hz, through a band-pass: a strike, then a ringing tail.
Buffer makeCowbell()
{
    Buffer sound (1, (int) (0.5 * rate));
    juce::IIRFilter bandPass;
    bandPass.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, 2640.0, 1.5));

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        const auto tone = (std::fmod (t * 540.0, 1.0) < 0.5 ? 0.5 : -0.5) + (std::fmod (t * 800.0, 1.0) < 0.5 ? 0.5 : -0.5);
        const auto envelope = 0.7 * std::exp (-t / 0.006) + 0.3 * std::exp (-t / 0.11);
        sound.setSample (0, i, (float) (envelope * bandPass.processSingleSampleRaw ((float) tone)));
    }

    return sound;
}

// A pure sine bass, with a gentle start and a long fade.
Buffer makeSubBass (double frequency, double length)
{
    Buffer sound (1, (int) (length * rate));

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        sound.setSample (0, i, (float) (std::sin (twoPi * frequency * t) * juce::jmin (1.0, t / 0.004) * std::exp (-t / (length * 0.45))));
    }

    return sound;
}

// Stereo noise through a resonant low-pass whose cutoff moves exponentially from one frequency to another, with a
// sine sweeping along under it: risers, downlifters and sweeps. level gives the loudness over the sound, from 0 to 1.
template <typename Level>
Buffer makeSweep (double seconds, double fromHz, double toHz, double resonance, double toneFromHz, double toneToHz, Level level)
{
    Buffer sound (2, (int) (seconds * rate));
    juce::Random random (5);
    std::array<juce::IIRFilter, 2> filters;
    auto tonePhase = 0.0;

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto position = i / (double) sound.getNumSamples();

        if (i % 16 == 0)
            for (auto& filter : filters)
                filter.setCoefficients (juce::IIRCoefficients::makeLowPass (rate, fromHz * std::pow (toHz / fromHz, position), resonance));

        tonePhase += twoPi * toneFromHz * std::pow (toneToHz / toneFromHz, position) / rate;
        const auto tone = 0.12 * std::sin (tonePhase);

        for (int channel = 0; channel < 2; ++channel)
        {
            const auto x = filters[(size_t) channel].processSingleSampleRaw (noise (random)) + tone;
            sound.setSample (channel, i, (float) (x * level (position)));
        }
    }

    return sound;
}

// A low thud: a sine falling from fromHz to toHz, a burst of noise, and a darker tail of noise in stereo.
Buffer makeImpact (double fromHz, double toHz, double decaySeconds, double length, double brightness)
{
    Buffer sound (2, (int) (length * rate));
    juce::Random random (6);
    std::array<juce::IIRFilter, 2> burstFilters, tailFilters;

    for (size_t channel = 0; channel < 2; ++channel)
    {
        burstFilters[channel].setCoefficients (juce::IIRCoefficients::makeLowPass (rate, brightness, 0.707));
        tailFilters[channel].setCoefficients (juce::IIRCoefficients::makeLowPass (rate, 1500.0, 0.707));
    }

    auto phase = 0.0;

    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        const auto t = i / rate;
        phase += twoPi * (toHz + (fromHz - toHz) * std::exp (-t / 0.15)) / rate;
        const auto thud = std::sin (phase) * std::exp (-t / decaySeconds) * juce::jmin (1.0, t / 0.001);

        for (size_t channel = 0; channel < 2; ++channel)
        {
            const auto burst = 0.6 * burstFilters[channel].processSingleSampleRaw (noise (random)) * std::exp (-t / 0.04);
            const auto tail = 0.25 * tailFilters[channel].processSingleSampleRaw (noise (random)) * std::exp (-t / (decaySeconds * 1.2));
            sound.setSample ((int) channel, i, softClip (thud + burst + tail, 1.4));
        }
    }

    return sound;
}

double barsToSeconds (double bars, double bpm)
{
    return bars * 4.0 * 60.0 / bpm;
}

//==============================================================================
// Reading the recipes: recipes.json in the phazite-sounds repository, whose README describes it.
[[noreturn]] void fail (const juce::String& message)
{
    std::cerr << message << std::endl;
    std::exit (1);
}

// Stops at a name the recipes don't use, so that a misspelt one can't go unnoticed.
void checkNames (const juce::var& object, std::initializer_list<const char*> names)
{
    if (auto* properties = object.getDynamicObject())
        for (const auto& property : properties->getProperties())
            if (std::none_of (names.begin(), names.end(), [&property] (const char* name) { return property.name.toString() == name; }))
                fail ("Unknown \"" + property.name.toString() + "\" in " + juce::JSON::toString (object, true));
}

// A number the recipe must give.
double getNumber (const juce::var& recipe, const char* name)
{
    const auto& value = recipe[name];

    if (! (value.isInt() || value.isInt64() || value.isDouble()))
        fail ("This needs a number for \"" + juce::String (name) + "\": " + juce::JSON::toString (recipe, true));

    return (double) value;
}

// A sound's file in the pack: a path such as "Drums/Kicks/Kick.flac".
juce::String getFile (const juce::var& recipe)
{
    const auto file = recipe["file"].toString();

    if (! file.endsWith (".flac") || file.startsWith ("/") || file.containsAnyOf ("\\:") || file.contains (".."))
        fail ("A \"file\" is a path in the pack, such as \"Drums/Kicks/Kick.flac\": " + juce::JSON::toString (recipe, true));

    return file;
}

// The recipe's "shape", every value of which is optional.
Shape getShape (const juce::var& recipe)
{
    const auto& values = recipe["shape"];
    checkNames (values, { "maxSeconds", "fadeOutSeconds", "reverse", "keepLength" });
    Shape how;
    how.maxSeconds = values.getProperty ("maxSeconds", how.maxSeconds);
    how.fadeOutSeconds = values.getProperty ("fadeOutSeconds", how.fadeOutSeconds);
    how.reverse = values.getProperty ("reverse", how.reverse);
    how.keepLength = values.getProperty ("keepLength", how.keepLength);
    return how;
}

// Makes a sound from a recipe's "synth": its type, and the numbers that type's function above takes, by name.
Buffer synthesize (const juce::var& synth)
{
    const auto type = synth["type"].toString();
    const auto number = [&synth] (const char* name) { return getNumber (synth, name); };

    if (type == "kick") // kicks and 808s
        return makeKick (number ("startHz"), number ("endHz"), number ("pitchSeconds"), number ("decaySeconds"), number ("seconds"),
                         number ("click"), number ("drive"));

    if (type == "snare")
        return makeSnare();

    if (type == "clap")
        return makeClap();

    if (type == "hat")
        return makeHat (number ("decaySeconds"), number ("seconds"));

    if (type == "cowbell")
        return makeCowbell();

    if (type == "sine")
        return makeSubBass (number ("hz"), number ("seconds"));

    if (type == "impact")
        return makeImpact (number ("fromHz"), number ("toHz"), number ("decaySeconds"), number ("seconds"), number ("brightness"));

    if (type == "sweep")
    {
        // Its loudness over the sound: rising, falling, or up and down again.
        static const std::map<juce::String, std::function<double (double)>> levels {
            { "rise", [] (double p) { return p * p; } },
            { "fall", [] (double p) { return (1.0 - p) * (1.0 - p); } },
            { "swell", [] (double p) { return std::sin (juce::MathConstants<double>::pi * p); } },
        };

        const auto level = levels.find (synth["level"].toString());

        if (level == levels.end())
            fail ("A sweep's \"level\" is \"rise\", \"fall\" or \"swell\": " + juce::JSON::toString (synth, true));

        return makeSweep (barsToSeconds (number ("bars"), number ("bpm")), number ("fromHz"), number ("toHz"), number ("resonance"),
                          number ("toneFromHz"), number ("toneToHz"), level->second);
    }

    fail ("Unknown synth type \"" + type + "\": " + juce::JSON::toString (synth, true));
}

// Where the third-party sounds come from. Each one's url is pinned to the commit (or the download) the files were
// taken from, and a sound's path there is added to it.
struct Collection
{
    juce::String title, author, url, licenceEvidence; // what says the files are CC0
};

std::map<juce::String, Collection> readCollections (const juce::var& recipes)
{
    std::map<juce::String, Collection> collections;

    if (auto* object = recipes["collections"].getDynamicObject())
    {
        for (const auto& property : object->getProperties())
        {
            const auto& values = property.value;
            checkNames (values, { "title", "author", "url", "licenceEvidence" });
            Collection collection { values["title"].toString(), values["author"].toString(), values["url"].toString(),
                                    values["licenceEvidence"].toString() };

            if (collection.title.isEmpty() || collection.author.isEmpty() || ! collection.url.startsWith ("https://")
                || collection.licenceEvidence.isEmpty())
                fail ("The collection \"" + property.name.toString() + "\" needs a title, an author, an https url and licenceEvidence");

            collections[property.name.toString()] = std::move (collection);
        }
    }

    return collections;
}

// A sound taken from a collection: its path there, where it is in the sources folder, and anything worth noting.
struct Taken
{
    juce::String collection, path, localPath, note;
};

Taken readTaken (const juce::var& take)
{
    checkNames (take, { "collection", "path", "local", "note" });
    Taken taken { take["collection"].toString(), take["path"].toString(), take["local"].toString(), take["note"].toString() };

    if (taken.collection.isEmpty() || taken.path.isEmpty() || taken.localPath.isEmpty())
        fail ("A take needs a \"collection\", a \"path\" and a \"local\" path: " + juce::JSON::toString (take, true));

    return taken;
}

// A loop played from the kit: an instrument per line, with its hits on a grid of 16th notes ("X" accented, "x",
// "o" a ghost note, "r" two 32nd notes, "." none), over every bar.
struct Line
{
    juce::String sound; // a one-shot's file
    juce::String hits;
    float level;
    int chokeGroup; // hits in the same group cut each other off, as open and closed hats do
};

struct Loop
{
    juce::String file;
    double bpm = 120.0;
    int bars = 2;
    double swing = 0.0; // how late every other 16th note is, in 16th notes
    std::vector<Line> lines = {};
};

int getBars (const juce::var& recipe)
{
    const auto& bars = recipe["bars"];

    if (! bars.isInt() || (int) bars < 1)
        fail ("A loop needs a whole number of \"bars\": " + juce::JSON::toString (recipe, true));

    return (int) bars;
}

Loop readLoop (const juce::var& recipe, const juce::String& file)
{
    Loop loop { file, getNumber (recipe, "bpm"), getBars (recipe), (double) recipe.getProperty ("swing", 0.0), {} };
    const auto* lines = recipe["lines"].getArray();

    if (lines == nullptr || lines->isEmpty())
        fail (file + " needs \"lines\" to play, or a \"take\"");

    for (const auto& line : *lines)
    {
        checkNames (line, { "sound", "hits", "level", "chokeGroup" });
        const auto hits = line["hits"].toString();

        if (hits.length() != loop.bars * 16 || ! hits.containsOnly ("Xxor."))
            fail ("A line has a 16th note for every step of its loop's " + juce::String (loop.bars)
                  + " bars (X, x, o, r or .): " + juce::JSON::toString (line, true));

        loop.lines.push_back (
            { line["sound"].toString(), hits, (float) getNumber (line, "level"), (int) line.getProperty ("chokeGroup", 0) });
    }

    return loop;
}

// The pack's date ("2026-10-13"), at noon local time: a zip stores local times, so its files get that date anywhere.
juce::Time getDate (const juce::String& date)
{
    const auto parts = juce::StringArray::fromTokens (date, "-", "");

    if (date.length() != 10 || parts.size() != 3 || ! date.containsOnly ("0123456789-"))
        fail ("The pack's \"date\" is written as 2026-10-13, not \"" + date + "\"");

    return juce::Time (parts[0].getIntValue(), parts[1].getIntValue() - 1, parts[2].getIntValue(), 12, 0, 0, 0, true);
}

// Plays a loop's lines from the kit. Every hit rings until its sound ends or the next hit in its choke group starts,
// and what rings past the last bar comes in again at the start, so the loop repeats seamlessly.
Buffer playLoop (const Loop& loop, const std::map<juce::String, Buffer>& kit)
{
    const auto stepSeconds = 60.0 / loop.bpm / 4.0;
    const auto length = (int) std::lround (barsToSeconds (loop.bars, loop.bpm) * rate);
    Buffer out (2, length);
    out.clear();

    struct Hit
    {
        const Buffer* sound;
        int start;
        float level;
        int chokeGroup;
    };

    std::vector<Hit> hits;

    for (const auto& line : loop.lines)
    {
        const auto found = kit.find (line.sound);

        if (found == kit.end())
        {
            std::cerr << "No sound " << line.sound << " for " << loop.file << std::endl;
            std::exit (1);
        }

        for (int step = 0; step < line.hits.length(); ++step)
        {
            const auto symbol = line.hits[step];
            const auto time = (step + (step % 2 == 1 ? loop.swing : 0.0)) * stepSeconds;
            const auto at = [&] (double seconds) { return (int) std::lround (seconds * rate) % length; };

            if (symbol == 'X' || symbol == 'x' || symbol == 'o')
                hits.push_back ({ &found->second, at (time),
                                  line.level
                                      * (symbol == 'X'   ? 1.0f
                                         : symbol == 'x' ? 0.7f
                                                         : 0.35f),
                                  line.chokeGroup });

            if (symbol == 'r')
                for (const auto half : { 0.0, 0.5 })
                    hits.push_back ({ &found->second, at (time + half * stepSeconds), line.level * 0.6f, line.chokeGroup });
        }
    }

    for (const auto& hit : hits)
    {
        // Until the next hit of its choke group, if there is one, with a 5 ms fade.
        auto end = hit.sound->getNumSamples();

        if (hit.chokeGroup > 0)
        {
            for (const auto& other : hits)
            {
                if (&other == &hit || other.chokeGroup != hit.chokeGroup)
                    continue;

                const auto distance = (other.start - hit.start + length) % length;

                if (distance > 0)
                    end = juce::jmin (end, distance);
            }
        }

        const auto fadeLength = (int) (0.005 * rate);

        for (int i = 0; i < end; ++i)
        {
            const auto fade = end < hit.sound->getNumSamples() ? juce::jlimit (0.0f, 1.0f, (float) (end - i) / (float) fadeLength) : 1.0f;

            for (int channel = 0; channel < 2; ++channel)
            {
                const auto source = juce::jmin (channel, hit.sound->getNumChannels() - 1);
                out.addSample (channel, (hit.start + i) % length, hit.level * fade * hit.sound->getSample (source, i));
            }
        }
    }

    normalize (out);
    return out;
}

//==============================================================================
// Rendering the pack, and the list of where each sound came from (sources.json).
juce::var describeSource (const Taken& taken, const std::map<juce::String, Collection>& collections, const juce::File& sources)
{
    const auto found = collections.find (taken.collection);

    if (found == collections.end())
        fail ("No collection \"" + taken.collection + "\" for " + taken.path);

    const auto file = sources.getChildFile (taken.localPath);

    if (! file.existsAsFile())
        fail ("Missing source " + file.getFullPathName());

    const auto& collection = found->second;
    auto* source = new juce::DynamicObject();
    source->setProperty ("title", collection.title + ": " + taken.path + (taken.note.isNotEmpty() ? " (" + taken.note + ")" : ""));
    source->setProperty ("author", collection.author);
    source->setProperty ("url", collection.url + taken.path.replace (" ", "%20").replace (",", "%2C"));
    source->setProperty ("licence", "CC0-1.0");
    source->setProperty ("licenceEvidence", collection.licenceEvidence);
    source->setProperty ("sha256", juce::SHA256 (file).toHexString());
    return juce::var (source);
}

juce::String describeShaping (const Shape& how)
{
    juce::String text = how.keepLength ? "its exact length kept" : "trimmed from its first sound to -60 dB";

    if (how.maxSeconds > 0.0)
        text << ", shortened to " << how.maxSeconds << " s";

    if (how.reverse)
        text << ", reversed";

    return text << ", faded out over " << juce::roundToInt (how.fadeOutSeconds * 1000.0) << " ms, peak-normalized to -1 dBFS";
}

int render (const juce::File& recipesFile, const juce::File& sources, const juce::File& pack)
{
    juce::var recipes;

    if (const auto parsed = juce::JSON::parse (recipesFile.loadFileAsString(), recipes); parsed.failed() || ! recipes.isObject())
        fail ("Can't read " + recipesFile.getFullPathName() + " " + parsed.getErrorMessage());

    checkNames (recipes, { "about", "pack", "collections", "sounds", "loops" });
    const auto& about = recipes["pack"];
    checkNames (about, { "name", "version", "date" });
    const auto* sounds = recipes["sounds"].getArray();
    const auto* loops = recipes["loops"].getArray();

    if (about["name"].toString().isEmpty() || ! about["version"].isInt() || sounds == nullptr || loops == nullptr)
        fail (recipesFile.getFileName() + " needs the pack's name and version, and lists of sounds and loops");

    getDate (about["date"].toString()); // checked now, for --zip later
    const auto collections = readCollections (recipes);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats(); // WAV, AIFF, FLAC, Ogg Vorbis
    juce::Array<juce::var> manifest;
    juce::StringArray made;
    std::map<juce::String, Buffer> kit;

    const auto write = [&] (const juce::String& file, const Buffer& sound, const juce::String& origin, juce::Array<juce::var> from,
                            const juce::String& processing)
    {
        if (made.contains (file, true))
            fail (file + " is made twice");

        made.add (file);
        writeSound (sound, pack.getChildFile (file), getDitherSeed (file));

        auto* entry = new juce::DynamicObject();
        entry->setProperty ("file", file);
        entry->setProperty ("licence", "CC0-1.0");
        entry->setProperty ("made", origin);

        if (! from.isEmpty())
            entry->setProperty ("sources", from);

        entry->setProperty ("processing", processing + ", and written as 44.1 kHz 16-bit FLAC with triangular dither by phazite_soundkit");
        manifest.add (juce::var (entry));
    };

    for (const auto& recipe : *sounds)
    {
        checkNames (recipe, { "file", "take", "synth", "description", "shape" });
        const auto file = getFile (recipe);
        const auto how = getShape (recipe);
        Buffer sound;
        juce::Array<juce::var> from;
        juce::String origin;

        if (recipe.hasProperty ("synth"))
        {
            const auto description = recipe["description"].toString();

            if (description.isEmpty() || recipe.hasProperty ("take"))
                fail (file + " is synthesized, so it needs a \"description\" of how, and no \"take\"");

            sound = synthesize (recipe["synth"]);
            origin = "synthesized by phazite_soundkit (" + description + "), dedicated to the public domain";
        }
        else
        {
            const auto* takes = recipe["take"].getArray();

            if (takes == nullptr || takes->isEmpty())
                fail (file + " needs a \"take\" list or a \"synth\"");

            // Mixed, when there are several: each later one at half the level.
            for (int i = 0; i < takes->size(); ++i)
            {
                const auto taken = readTaken (takes->getReference (i));
                const auto part = readSound (formats, sources.getChildFile (taken.localPath));
                from.add (describeSource (taken, collections, sources));

                if (i == 0)
                {
                    sound = part;
                    continue;
                }

                sound.setSize (juce::jmax (sound.getNumChannels(), part.getNumChannels()),
                               juce::jmax (sound.getNumSamples(), part.getNumSamples()), true, true);

                for (int channel = 0; channel < sound.getNumChannels(); ++channel)
                    sound.addFrom (channel, 0, part, juce::jmin (channel, part.getNumChannels() - 1), 0, part.getNumSamples(), 0.5f);
            }

            origin = takes->size() == 1 ? "taken from the CC0 source below" : "mixed from the CC0 sources below";
        }

        sound = shape (std::move (sound), how);
        write (file, sound, origin, from, describeShaping (how));
        kit[file] = std::move (sound);
    }

    for (const auto& recipe : *loops)
    {
        const auto file = getFile (recipe);

        if (recipe.hasProperty ("take"))
        {
            // A recorded loop, cut to exactly its bars.
            checkNames (recipe, { "file", "bpm", "bars", "take", "shape" });
            const auto bpm = getNumber (recipe, "bpm");
            const auto bars = getBars (recipe);
            const auto how = getShape (recipe);
            const auto taken = readTaken (recipe["take"]);
            auto sound = readSound (formats, sources.getChildFile (taken.localPath));
            sound = slice (sound, 0, (int) std::lround (barsToSeconds (bars, bpm) * rate));
            sound = shape (std::move (sound), how);
            write (file, sound, "taken from the CC0 source below", { describeSource (taken, collections, sources) },
                   "cut to exactly " + juce::String (bars) + " bars at " + juce::String (juce::roundToInt (bpm)) + " BPM, faded out over "
                       + juce::String (juce::roundToInt (how.fadeOutSeconds * 1000.0)) + " ms, peak-normalized to -1 dBFS");
            continue;
        }

        checkNames (recipe, { "file", "bpm", "bars", "swing", "lines" });
        const auto loop = readLoop (recipe, file);
        juce::String pattern;

        for (const auto& line : loop.lines)
            pattern << line.sound << ": " << line.hits << "; ";

        write (file, playLoop (loop, kit),
               "played by phazite_soundkit from the sounds above, to a pattern written for Phazite (16th notes; X accent, x hit, o ghost, "
               "r two 32nds): "
                   + pattern.trimEnd().dropLastCharacters (1),
               {},
               juce::String (loop.bars) + " bars at " + juce::String (juce::roundToInt (loop.bpm)) + " BPM, peak-normalized to -1 dBFS");
    }

    auto* root = new juce::DynamicObject();
    root->setProperty (
        "about", "Where each of these sounds came from. All of them are CC0 1.0: in the public domain, free to use in any way, in any "
                 "song, without credit. Made by phazite_soundkit (tools/soundkit in Phazite's repository) from recipes.json; see "
                 "README.txt.");
    root->setProperty ("pack", about["name"]);
    root->setProperty ("version", about["version"]);
    root->setProperty ("date", about["date"]);
    root->setProperty ("sounds", manifest);
    pack.getChildFile ("sources.json").replaceWithText (juce::JSON::toString (juce::var (root)) + "\n", false, false, "\n");
    std::cout << "Wrote " << manifest.size() << " sounds to " << pack.getFullPathName() << std::endl;

    // Files left from earlier recipes would go out with the pack without being listed.
    auto result = 0;

    for (const auto& entry : juce::RangedDirectoryIterator (pack, true, "*.flac"))
    {
        if (const auto path = entry.getFile().getRelativePathFrom (pack).replaceCharacter ('\\', '/'); ! made.contains (path))
        {
            std::cerr << "Not in " << recipesFile.getFileName() << ", so delete it: " << path << std::endl;
            result = 1;
        }
    }

    return result;
}

// The pack's download: every file in a folder named after the pack, in a fixed order and with the pack's date, so the
// same pack always makes the same zip. FLAC is stored as it is; it doesn't compress any further.
int makeZip (const juce::File& pack, const juce::File& zipFile)
{
    const auto manifest = juce::JSON::parse (pack.getChildFile ("sources.json"));
    const auto name = manifest["pack"].toString();

    if (name.isEmpty())
        fail ("No sources.json with the pack's name in " + pack.getFullPathName() + "; render the pack first");

    if (zipFile.isAChildOf (pack))
        fail ("The zip can't go in the pack's folder");

    const auto date = getDate (manifest["date"].toString());
    std::vector<std::pair<juce::String, juce::File>> files;

    for (const auto& entry : juce::RangedDirectoryIterator (pack, true, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles))
        files.emplace_back (entry.getFile().getRelativePathFrom (pack).replaceCharacter ('\\', '/'), entry.getFile());

    std::sort (files.begin(), files.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
    juce::ZipFile::Builder builder;

    for (const auto& [path, file] : files)
    {
        auto stream = std::make_unique<juce::FileInputStream> (file);

        if (! stream->openedOk())
            fail ("Can't read " + file.getFullPathName());

        builder.addEntry (std::move (stream), file.hasFileExtension ("flac") ? 0 : 9, name + "/" + path, date);
    }

    zipFile.deleteFile();

    {
        juce::FileOutputStream out (zipFile);

        if (! out.openedOk() || ! builder.writeToStream (out, nullptr) || ! out.getStatus().wasOk())
            fail ("Can't write " + zipFile.getFullPathName());
    }

    std::cout << zipFile.getFileName() << ": " << files.size() << " files, " << zipFile.getSize() << " bytes, SHA-256 "
              << juce::SHA256 (zipFile).toHexString() << std::endl;
    return 0;
}
} // namespace

namespace
{
// For checking the sounds without listening: each one's length, channels, peak and RMS level, and spectral centroid
// (where its energy is, on average: low for a kick, high for a hat).
int describeSounds (const juce::File& folder)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    constexpr int order = 12;
    constexpr int size = 1 << order;

    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*.flac"))
    {
        const auto sound = readSound (formats, entry.getFile());
        const auto samples = sound.getNumSamples();
        auto sumOfSquares = 0.0, weighted = 0.0, total = 0.0;

        for (int channel = 0; channel < sound.getNumChannels(); ++channel)
            for (int i = 0; i < samples; ++i)
                sumOfSquares += juce::square ((double) sound.getSample (channel, i));

        // The centroid of the magnitude spectra of its frames, weighted by their energy.
        juce::dsp::FFT fft (order);
        juce::dsp::WindowingFunction<float> window ((size_t) size, juce::dsp::WindowingFunction<float>::hann);
        std::vector<float> frame ((size_t) size * 2);

        for (int start = 0; start < samples; start += size / 2)
        {
            std::fill (frame.begin(), frame.end(), 0.0f);

            for (int i = 0; i < size && start + i < samples; ++i)
                for (int channel = 0; channel < sound.getNumChannels(); ++channel)
                    frame[(size_t) i] += sound.getSample (channel, start + i);

            window.multiplyWithWindowingTable (frame.data(), (size_t) size);
            fft.performFrequencyOnlyForwardTransform (frame.data());

            for (int bin = 1; bin < size / 2; ++bin)
            {
                const auto magnitude = (double) frame[(size_t) bin];
                weighted += magnitude * bin * rate / size;
                total += magnitude;
            }
        }

        const auto rms = std::sqrt (sumOfSquares / juce::jmax (1, samples * sound.getNumChannels()));
        std::cout << entry.getFile().getRelativePathFrom (folder).replace ("\\", "/") << ": " << juce::String (samples / rate, 3) << " s, "
                  << sound.getNumChannels() << " ch, peak " << juce::String (juce::Decibels::gainToDecibels (getPeak (sound)), 1)
                  << " dB, RMS " << juce::String (juce::Decibels::gainToDecibels ((float) rms), 1) << " dB, centroid "
                  << juce::roundToInt (total > 0.0 ? weighted / total : 0.0) << " Hz" << std::endl;
    }

    return 0;
}
} // namespace

int main (int argc, char* argv[])
{
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const auto argument = [&] (int index) { return cwd.getChildFile (argv[index]); };
    const juce::String mode (argc > 1 ? argv[1] : "");

    if (argc == 3 && mode == "--info")
        return describeSounds (argument (2));

    if (argc == 4 && mode == "--zip")
        return makeZip (argument (2), argument (3));

    if (argc == 4 && ! mode.startsWith ("--"))
        return render (argument (1), argument (2), argument (3));

    std::cerr << "Usage: phazite_soundkit <recipes.json> <folder with the downloaded sources> <pack folder>" << std::endl
              << "       phazite_soundkit --zip <pack folder> <zip file>" << std::endl
              << "       phazite_soundkit --info <folder of sounds>" << std::endl;
    return 2;
}
