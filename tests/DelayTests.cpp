// SPDX-License-Identifier: AGPL-3.0-or-later
// The delay as DSP (src/devices/delay/Delay.h), measured: its parameters; at its defaults, sample for sample what it played
// before step S6.6; its echoes on the beat, as late as their time says, bouncing in ping-pong; a time for each side; the
// Clean character's straight line and Tape's harmonics; a feedback over 100 % held by the loop's curve; the modulation's
// swing, its sides a quarter of a cycle apart and where the song is; the duck; the freeze; the two transitions; and all of it
// in blocks of any size.
#include "DspProbe.h"
#include "TestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Filters.h"
#include "devices/dsp/Math.h"
#include "devices/dsp/NoteValues.h"
#include "model/devices/internal/Parameters.h"
#include "model/presets/internal/Presets.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace devices = phazite::devices;
namespace dsp = devices::dsp;
namespace probe = phazite::test::probe;
using devices::Delay;
constexpr double rate = 48000.0;
constexpr double twoPi = juce::MathConstants<double>::twoPi;

std::vector<float> getDefaults()
{
    std::vector<float> values;

    for (const auto& parameter : Delay::info.parameters)
        values.push_back (parameter.defaultValue);

    return values;
}

std::vector<float> with (std::initializer_list<std::pair<std::string_view, float>> changes, std::vector<float> values = getDefaults())
{
    for (const auto& [id, value] : changes)
    {
        const auto index = devices::findParameter (Delay::info, id);
        REQUIRE (index >= 0);
        values[(size_t) index] = value;
    }

    return values;
}

// A time in milliseconds, the cuts open and no feedback: each echo what came in, as late as the time says.
std::vector<float> plain (float milliseconds, std::initializer_list<std::pair<std::string_view, float>> changes = {})
{
    return with (changes,
                 with ({ { "sync", 0.0f }, { "time", milliseconds }, { "feedback", 0.0f }, { "lowCut", 20.0f }, { "highCut", 20000.0f } }));
}

struct Stereo
{
    std::vector<float> left, right;
};

struct Transport
{
    bool playing = true;
    double tempo = 120.0;
    std::int64_t from = 0;                         // the song's sample as the first block starts
    std::int64_t loop = 0;                         // a loop's length from there, in samples, which it goes round; 0 for none
    std::function<double (int)> tempoAt = nullptr; // the tempo as a block starts, given its first sample, where it moves
};

using Input = std::function<std::pair<float, float> (int)>;
using Values = std::function<std::vector<float> (int)>;

// Plays a prepared delay in blocks on a sound (input gives each side's sample at a time), with the values each block starts
// with (given the block's first sample), the song playing (or not) from a sample at a tempo.
Stereo play (Delay& delay, int numSamples, int blockSize, const Input& input, const Values& values, Transport transport = {})
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    Stereo played;
    played.left.reserve ((size_t) numSamples);
    played.right.reserve ((size_t) numSamples);

    for (int done = 0; done < numSamples; done += blockSize)
    {
        const auto length = std::min (blockSize, numSamples - done);

        for (int i = 0; i < length; ++i)
        {
            const auto [left, right] = input (done + i);
            buffer.setSample (0, i, left);
            buffer.setSample (1, i, right);
        }

        const auto now = values (done);
        float* channels[] = { buffer.getWritePointer (0), buffer.getWritePointer (1) };
        devices::ProcessContext context;
        context.channels = channels;
        context.numChannels = 2;
        context.numSamples = length;
        context.parameters = now;
        context.isPlaying = transport.playing;
        context.tempo = transport.tempoAt != nullptr ? transport.tempoAt (done) : transport.tempo;
        context.songSample = transport.from + (transport.playing ? (transport.loop > 0 ? done % transport.loop : done) : 0);
        context.beat = (double) context.songSample * transport.tempo / 60.0 / rate;
        context.bars = context.beat / 4.0;
        delay.process (context);

        for (int i = 0; i < length; ++i)
        {
            played.left.push_back (buffer.getSample (0, i));
            played.right.push_back (buffer.getSample (1, i));
        }
    }

    return played;
}

// The same on a new delay, its values staying, in blocks of 480.
Stereo play (const std::vector<float>& values, int numSamples, const Input& input, Transport transport = {})
{
    Delay delay;
    delay.prepare (rate, 480);
    return play (delay, numSamples, 480, input, [&] (int) { return values; }, transport);
}

Input both (const std::function<float (int)>& mono)
{
    return [mono] (int t)
    {
        const auto sample = mono (t);
        return std::pair { sample, sample };
    };
}

std::function<float (int)> sine (double frequency, float amplitude, int until = std::numeric_limits<int>::max())
{
    return [=] (int t) { return t < until ? amplitude * (float) std::sin (twoPi * frequency * t / rate) : 0.0f; };
}

std::vector<float> makeNoise (int numSamples, int seed, float amplitude)
{
    juce::Random random (seed);
    std::vector<float> noise ((size_t) numSamples);

    for (auto& sample : noise)
        sample = amplitude * (2.0f * random.nextFloat() - 1.0f);

    return noise;
}

float getPeak (const std::vector<float>& sound, size_t from, size_t to)
{
    auto peak = 0.0f;

    for (auto i = from; i < std::min (to, sound.size()); ++i)
        peak = std::max (peak, std::abs (sound[i]));

    return peak;
}

double getRms (const std::vector<float>& sound, size_t from, size_t to)
{
    auto sum = 0.0;

    for (auto i = from; i < to; ++i)
        sum += (double) sound[i] * sound[i];

    return std::sqrt (sum / (double) std::max<size_t> (1, to - from));
}

double toDecibels (double gain)
{
    return 20.0 * std::log10 (std::max (gain, 1.0e-15));
}

// The largest step from one sample to the next in a stretch of a sound.
float getLargestStep (const std::vector<float>& sound, size_t from, size_t to)
{
    auto largest = 0.0f;

    for (auto i = std::max<size_t> (from, 1); i < std::min (to, sound.size()); ++i)
        largest = std::max (largest, std::abs (sound[i] - sound[i - 1]));

    return largest;
}

// A tone's sine and cosine parts in a stretch of a sound, by least squares (the stretch need not hold whole cycles).
struct Tone
{
    double sinePart = 0.0, cosinePart = 0.0;

    double getLevel() const { return std::sqrt (sinePart * sinePart + cosinePart * cosinePart); }
    double getPhase() const { return std::atan2 (cosinePart, sinePart); }
};

Tone getTone (const float* sound, int numSamples, double frequency, double sampleRate)
{
    double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto s = std::sin (twoPi * frequency * i / sampleRate), c = std::cos (twoPi * frequency * i / sampleRate);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        ys += sound[i] * s;
        yc += sound[i] * c;
    }

    const auto determinant = ss * cc - sc * sc;
    return { (ys * cc - yc * sc) / determinant, (yc * ss - ys * sc) / determinant };
}

double getToneLevel (const std::vector<float>& sound, size_t from, size_t numSamples, double frequency)
{
    return getTone (sound.data() + from, (int) numSamples, frequency, rate).getLevel();
}

// How late a steady tone is in a sound, every hop samples from a sample on, from its phase over four of its cycles against
// the tone that went in (sin at the sound's own sample): followed on from reading to reading, the first put nearest `near`.
std::vector<double> getLateness (const std::vector<float>& sound, double frequency, int hop, int from, double near)
{
    const auto window = (int) std::lround (rate / frequency) * 4;
    const auto cycle = rate / frequency;
    std::vector<double> lateness;
    auto unwrapped = 0.0, last = 0.0;
    auto first = true;

    for (int start = from; start + window < (int) sound.size(); start += hop)
    {
        auto s = 0.0, c = 0.0;

        for (int i = start; i < start + window; ++i)
        {
            s += sound[(size_t) i] * std::sin (twoPi * frequency * i / rate);
            c += sound[(size_t) i] * std::cos (twoPi * frequency * i / rate);
        }

        const auto phase = std::atan2 (c, s);

        if (first)
        {
            unwrapped = phase;
            const auto late = -unwrapped / twoPi * cycle;
            unwrapped -= std::round ((near - late) / cycle) * twoPi;
            first = false;
        }
        else
        {
            unwrapped += std::remainder (phase - last, twoPi);
        }

        last = phase;
        lateness.push_back (-unwrapped / twoPi * cycle);
    }

    return lateness;
}

// The swing at a frequency in readings taken every hop samples, less their mean: its sine and cosine parts.
Tone getSwing (const std::vector<double>& readings, double frequency, int hop)
{
    auto mean = 0.0;

    for (const auto reading : readings)
        mean += reading;

    mean /= (double) readings.size();
    std::vector<float> centred;

    for (const auto reading : readings)
        centred.push_back ((float) (reading - mean));

    return getTone (centred.data(), (int) centred.size(), frequency, rate / hop);
}

// The pitch of a tone, in cents from a frequency, between each two of its rising zero crossings (placed between samples) in
// a stretch: the furthest from it.
double getFurthestCents (const std::vector<float>& sound, size_t from, size_t to, double frequency)
{
    auto lastCrossing = -1.0, furthest = 0.0;

    for (auto i = std::max<size_t> (from, 1); i < to; ++i)
    {
        if (sound[i - 1] < 0.0f && sound[i] >= 0.0f)
        {
            const auto crossing = (double) (i - 1) + sound[i - 1] / (sound[i - 1] - sound[i]);

            if (lastCrossing >= 0.0)
                furthest = std::max (furthest, std::abs (1200.0 * std::log2 (rate / (crossing - lastCrossing) / frequency)));

            lastCrossing = crossing;
        }
    }

    return furthest;
}

// The level in each third of an octave from 25 Hz to 16 kHz in a stretch of a sound, in decibels: the power of its spectrum
// there, averaged over Hann-windowed segments of 2^order samples one after another.
std::vector<double> getBandLevels (const std::vector<float>& sound, size_t from, size_t length, double sampleRate, int order = 12)
{
    const auto size = (size_t) 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> power (size / 2, 0.0);
    std::vector<float> data (2 * size);
    auto segments = 0;

    for (auto start = from; start + size <= from + length && start + size <= sound.size(); start += size)
    {
        std::fill (data.begin(), data.end(), 0.0f);

        for (size_t i = 0; i < size; ++i)
            data[i] = sound[start + i] * (float) (0.5 - 0.5 * std::cos (twoPi * (double) i / (double) size));

        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        for (size_t k = 0; k < size / 2; ++k)
            power[k] += (double) data[k] * data[k];

        ++segments;
    }

    std::vector<double> levels;

    for (int band = 0; band < 29; ++band)
    {
        const auto centre = 25.0 * std::pow (2.0, band / 3.0);
        const auto lowest = centre * std::pow (2.0, -1.0 / 6.0), highest = centre * std::pow (2.0, 1.0 / 6.0);
        auto sum = 0.0;

        for (size_t k = 1; k < size / 2; ++k)
            if (const auto hertz = (double) k * sampleRate / (double) size; hertz >= lowest && hertz < highest)
                sum += power[k];

        levels.push_back (10.0 * std::log10 (std::max (sum / std::max (1, segments), 1.0e-30)));
    }

    return levels;
}

// The largest difference between two sounds' levels in thirds of an octave, in decibels, over the bands between two
// frequencies where the first is within 60 dB of its loudest band.
double getLargestBandDifference (const std::vector<double>& first, const std::vector<double>& second, double lowestHertz = 0.0,
                                 double highestHertz = 1.0e9)
{
    const auto loudest = *std::max_element (first.begin(), first.end());
    auto largest = 0.0;

    for (size_t band = 0; band < first.size(); ++band)
        if (const auto hertz = 25.0 * std::pow (2.0, (double) band / 3.0);
            hertz >= lowestHertz && hertz <= highestHertz && first[band] > loudest - 60.0)
            largest = std::max (largest, std::abs (first[band] - second[band]));

    return largest;
}
// Plays a prepared delay as play() does, in blocks of 48, noting each block in which a fade was under way (and a block on
// either side of it), sample by sample.
struct Marked
{
    Stereo sound;
    std::vector<char> fading;
};

Marked playMarked (Delay& delay, int numSamples, const Input& input, const Values& values, Transport transport = {})
{
    constexpr int blockSize = 48;
    Marked marked;
    std::vector<char> blocks;

    for (int done = 0; done < numSamples; done += blockSize)
    {
        const auto started = delay.getFadesStarted();
        auto part = transport;
        part.from = transport.from + done;
        part.tempoAt = transport.tempoAt == nullptr
                           ? nullptr
                           : std::function<double (int)> ([&transport, done] (int t) { return transport.tempoAt (done + t); });
        const auto played = play (
            delay, std::min (blockSize, numSamples - done), blockSize, [&input, done] (int t) { return input (done + t); },
            [&values, done] (int t) { return values (done + t); }, part);
        marked.sound.left.insert (marked.sound.left.end(), played.left.begin(), played.left.end());
        marked.sound.right.insert (marked.sound.right.end(), played.right.begin(), played.right.end());
        blocks.push_back (delay.getFadesUnderWay() > 0 || delay.getFadesStarted() != started ? 1 : 0);
    }

    for (size_t block = 0; block < blocks.size(); ++block)
    {
        const auto near =
            blocks[block] != 0 || (block > 0 && blocks[block - 1] != 0) || (block + 1 < blocks.size() && blocks[block + 1] != 0);

        for (int i = 0; i < blockSize; ++i)
            marked.fading.push_back (near ? 1 : 0);
    }

    marked.fading.resize (marked.sound.left.size());
    return marked;
}

// The pitch of a tone, in cents from a frequency, between each two of its rising zero crossings in a stretch, leaving out
// those with a marked sample between them: the furthest from it, and how many it measured.
struct Pitch
{
    double cents = 0.0;
    int periods = 0;
};

Pitch getFurthestCentsOutside (const std::vector<float>& sound, size_t from, size_t to, double frequency, const std::vector<char>& marked)
{
    Pitch pitch;
    auto lastCrossing = -1.0;
    auto lastAt = from;

    for (auto i = std::max<size_t> (from, 1); i < to; ++i)
    {
        if (sound[i - 1] < 0.0f && sound[i] >= 0.0f)
        {
            const auto crossing = (double) (i - 1) + sound[i - 1] / (sound[i - 1] - sound[i]);
            const auto clear = std::none_of (marked.begin() + (std::ptrdiff_t) lastAt, marked.begin() + (std::ptrdiff_t) i + 1,
                                             [] (char mark) { return mark != 0; });

            if (lastCrossing >= 0.0 && clear)
            {
                pitch.cents = std::max (pitch.cents, std::abs (1200.0 * std::log2 (rate / (crossing - lastCrossing) / frequency)));
                ++pitch.periods;
            }

            lastCrossing = crossing;
            lastAt = i - 1;
        }
    }

    return pitch;
}
// The delay as it played before step S6.6, sample for sample (its Delay.cpp at 5227a470): one time for both sides, every
// echo bent by the soft saturation, a feedback of at most 95 %. Its values read from the parameters it had.
class DelayAsBefore
{
public:
    DelayAsBefore()
    {
        timeSmoothing = (float) (1.0 - std::exp (-1.0 / (0.05 * rate)));

        for (auto& line : lines)
            line.prepare ((int) std::ceil (5.0 * rate) + 1);
    }

    void process (float* left, float* right, int numSamples, const std::vector<float>& values, double tempo)
    {
        auto seconds = (double) values[Delay::time] * 0.001;

        if (values[Delay::sync] > 0.5f)
        {
            const auto index = (size_t) juce::jlimit (0, 13, (int) std::lround (values[Delay::note]));
            seconds = dsp::noteValueLengths[index].inBeats (4.0) * 60.0 / std::max (tempo, 1.0);
        }

        seconds = std::min (seconds, 5.0);
        const auto target = (float) juce::jlimit (1.0, 5.0 * rate, seconds * rate);

        if (first)
        {
            delaySamples = target;
            first = false;
        }

        const auto amount = values[Delay::feedback];
        const auto bounce = values[Delay::pingPong] > 0.5f;
        const auto lowCutFilter = dsp::makeFilter (dsp::FilterShape::highPass, values[Delay::lowCut], rate, 0.70710678);
        const auto highCutFilter = dsp::makeFilter (dsp::FilterShape::lowPass, values[Delay::highCut], rate, 0.70710678);

        for (int i = 0; i < numSamples; ++i)
        {
            delaySamples += (target - delaySamples) * timeSmoothing;
            const auto back = delaySamples - 1.0f;
            const auto echoLeft = highCuts[0].process (lowCuts[0].process (lines[0].read (back), lowCutFilter), highCutFilter);
            const auto echoRight = highCuts[1].process (lowCuts[1].process (lines[1].read (back), lowCutFilter), highCutFilter);
            const auto backLeft = dsp::softClip (echoLeft * amount), backRight = dsp::softClip (echoRight * amount);
            const auto inLeft = left[i], inRight = right[i];

            if (bounce)
            {
                lines[0].push (0.5f * (inLeft + inRight) + backRight);
                lines[1].push (backLeft);
            }
            else
            {
                lines[0].push (inLeft + backLeft);
                lines[1].push (inRight + backRight);
            }

            left[i] = echoLeft;
            right[i] = echoRight;
        }
    }

private:
    float timeSmoothing = 0.0f, delaySamples = 1.0f;
    bool first = true;
    std::array<dsp::DelayLine, 2> lines;
    std::array<dsp::Filter, 2> lowCuts {}, highCuts {};
};
} // namespace

TEST_CASE ("The delay describes itself: its parameters in the order its sound takes them, with their ranges and defaults")
{
    CHECK (devices::isValid (Delay::info));

    struct Expected
    {
        std::string_view id, name;
        float minimum, maximum, defaultValue;
    };

    const Expected expected[] = {
        { "sync", "Sync", 0.0f, 1.0f, 1.0f },
        { "note", "Note", 0.0f, 13.0f, 6.0f },
        { "time", "Time", 1.0f, 2000.0f, 375.0f },
        { "link", "Link", 0.0f, 1.0f, 1.0f },
        { "noteRight", "Note Right", 0.0f, 13.0f, 6.0f },
        { "timeRight", "Time Right", 1.0f, 2000.0f, 375.0f },
        { "feedback", "Feedback", 0.0f, 1.1f, 0.35f },
        { "pingPong", "Ping-Pong", 0.0f, 1.0f, 0.0f },
        { "character", "Character", 0.0f, 1.0f, 0.0f },
        { "modRate", "Mod Rate", 0.05f, 10.0f, 0.5f },
        { "modDepth", "Mod Depth", 0.0f, 1.0f, 0.0f },
        { "duck", "Duck", 0.0f, 1.0f, 0.0f },
        { "freeze", "Freeze", 0.0f, 1.0f, 0.0f },
        { "transition", "Transition", 0.0f, 1.0f, 0.0f },
        { "lowCut", "Low Cut", 20.0f, 2000.0f, 100.0f },
        { "highCut", "High Cut", 1000.0f, 20000.0f, 8000.0f },
        { "mix", "Mix", 0.0f, 1.0f, 0.25f },
    };

    REQUIRE (Delay::info.parameters.size() == std::size (expected));

    for (size_t i = 0; i < std::size (expected); ++i)
    {
        const auto& parameter = Delay::info.parameters[i];
        CAPTURE (std::string (expected[i].id));
        CHECK (parameter.id == expected[i].id);
        CHECK (parameter.name == expected[i].name);
        CHECK (parameter.minimum == doctest::Approx (expected[i].minimum));
        CHECK (parameter.maximum == doctest::Approx (expected[i].maximum));
        CHECK (parameter.defaultValue == doctest::Approx (expected[i].defaultValue));
    }

    // The note values to a bar, as the shared list names them; the characters and the transitions by their names.
    CHECK (Delay::info.parameters[Delay::note].choices.back() == "1 bar");
    CHECK (Delay::info.parameters[Delay::character].choices[0] == "Tape");
    CHECK (Delay::info.parameters[Delay::character].choices[1] == "Clean");
    CHECK (Delay::info.parameters[Delay::transition].choices[0] == "Repitch");
    CHECK (Delay::info.parameters[Delay::transition].choices[1] == "Fade");
    CHECK (Delay::info.parameters[Delay::modRate].scale == devices::Scale::logarithmic);
}

TEST_CASE ("The delay's times: a note value at the song's tempo or milliseconds, the right side's its own while Link is off")
{
    auto values = getDefaults();
    CHECK (Delay::getDelaySeconds (values, 120.0, 4.0) == doctest::Approx (0.375)); // a dotted eighth
    values = with ({ { "note", 8.0f } });                                           // a quarter
    CHECK (Delay::getDelaySeconds (values, 120.0, 4.0) == doctest::Approx (0.5));
    CHECK (Delay::getDelaySeconds (values, 90.0, 4.0) == doctest::Approx (60.0 / 90.0));
    CHECK (Delay::getDelaySeconds (values, 120.0, 3.0) == doctest::Approx (0.5)); // a note value in any time signature

    // A bar, the song's: three quarter notes in 6/8; at 30 BPM in 4/4, 8 s, but at most 5.
    values = with ({ { "note", 13.0f } });
    CHECK (Delay::getDelaySeconds (values, 120.0, 3.0) == doctest::Approx (1.5));
    CHECK (Delay::getDelaySeconds (values, 120.0, 4.0) == doctest::Approx (2.0));
    CHECK (Delay::getDelaySeconds (values, 30.0, 4.0) == doctest::Approx (Delay::maximumSeconds));
    values = with ({ { "sync", 0.0f }, { "time", 250.0f } });
    CHECK (Delay::getDelaySeconds (values, 120.0, 4.0) == doctest::Approx (0.25));

    // Linked, the right side takes the left's time; unlinked, its own, a note value or milliseconds as Sync says.
    values = with ({ { "note", 8.0f }, { "noteRight", 11.0f }, { "time", 100.0f }, { "timeRight", 130.0f } });
    CHECK (Delay::getDelaySeconds (values, 120.0, 4.0, 1) == doctest::Approx (0.5));
    values[Delay::link] = 0.0f;
    CHECK (Delay::getDelaySeconds (values, 120.0, 4.0, 0) == doctest::Approx (0.5));
    CHECK (Delay::getDelaySeconds (values, 120.0, 4.0, 1) == doctest::Approx (1.0)); // a half note
    values[Delay::sync] = 0.0f;
    CHECK (Delay::getDelaySeconds (values, 120.0, 4.0, 0) == doctest::Approx (0.1));
    CHECK (Delay::getDelaySeconds (values, 120.0, 4.0, 1) == doctest::Approx (0.13));
}

TEST_CASE ("The delay at its defaults plays exactly what it played before step S6.6, at the settings it had")
{
    // Every new parameter at its default: the old device's loop, sample for sample, on noise in both sides, synced and free,
    // ping-pong or not, with the feedback, the cuts and the tempo across their ranges.
    const auto noise = makeNoise (96000, 11, 0.5f), other = makeNoise (96000, 12, 0.5f);

    struct Setting
    {
        std::vector<float> values;
        double tempo;
    };

    const Setting settings[] = {
        { getDefaults(), 120.0 },
        { with ({ { "note", 0.0f }, { "feedback", 0.95f }, { "pingPong", 1.0f } }), 97.0 },
        { with ({ { "sync", 0.0f }, { "time", 44.0f }, { "feedback", 0.8f }, { "lowCut", 20.0f }, { "highCut", 20000.0f } }), 120.0 },
        { with ({ { "sync", 0.0f }, { "time", 1.0f }, { "feedback", 0.6f }, { "pingPong", 1.0f }, { "lowCut", 2000.0f } }), 120.0 },
        { with ({ { "note", 13.0f }, { "feedback", 0.0f }, { "highCut", 1000.0f } }), 140.0 },
    };

    for (size_t s = 0; s < std::size (settings); ++s)
    {
        CAPTURE (s);
        const auto& setting = settings[s];
        const auto played = play (setting.values, (int) noise.size(),
                                  [&] (int t) { return std::pair { noise[(size_t) t], other[(size_t) t] }; }, { .tempo = setting.tempo });

        DelayAsBefore before;
        auto left = noise, right = other;

        for (int done = 0; done < (int) left.size(); done += 480)
            before.process (left.data() + done, right.data() + done, 480, setting.values, setting.tempo);

        auto differences = 0;

        for (size_t i = 0; i < left.size(); ++i)
            if (! juce::exactlyEqual (played.left[i], left[i]) || ! juce::exactlyEqual (played.right[i], right[i]))
                ++differences;

        CHECK (differences == 0);
    }
}

TEST_CASE ("The delay: echoes on the beat at the song's tempo, fewer each time, and bouncing in ping-pong")
{
    // An impulse with a quarter-note echo at 120 BPM: the first echo half a second later, the second smaller.
    const auto impulse = [] (int t) { return std::pair { t == 0 ? 1.0f : 0.0f, t == 0 ? 1.0f : 0.0f }; };
    auto values = with ({ { "note", 8.0f } });
    Delay delay;
    delay.prepare (rate, 480);
    const auto out = play (delay, 60000, 480, impulse, [&] (int) { return values; });

    CHECK (getPeak (out.left, 0, 23900) < 1.0e-4f);
    const auto first = getPeak (out.left, 23900, 24500), second = getPeak (out.left, 47900, 48500);
    CHECK (first > 0.05f);
    CHECK (second > 0.01f);
    CHECK (second < first * 0.5f);
    CHECK (delay.getTailSeconds() > 1.0);

    // Ping-pong: the first echo on the left, the next on the right.
    values[Delay::pingPong] = 1.0f;
    delay.reset();
    const auto bounced = play (delay, 60000, 480, impulse, [&] (int) { return values; });
    CHECK (getPeak (bounced.left, 23900, 24500) > 0.02f);
    CHECK (getPeak (bounced.right, 23900, 24500) < 1.0e-3f);
    CHECK (getPeak (bounced.right, 47900, 48500) > 0.005f);
}

TEST_CASE ("The delay's echo comes exactly as late as its time says, however short")
{
    // It read its line before it pushed the new sample, and took nothing off for it: each echo a sample late, 2 % long at
    // 1 ms, which detuned a comb made with it. The echo is what came in through its band, exactly as late (whole
    // samples at these rates).
    for (const auto sampleRate : { 48000.0, 96000.0 })
    {
        for (const auto milliseconds : { 1.0f, 2.5f, 10.0f })
        {
            const auto values = plain (milliseconds);
            Delay delay;
            delay.prepare (sampleRate, 480);
            const auto noise = makeNoise (4800, 7, 0.5f);
            const auto out =
                play (delay, (int) noise.size(), 480, both ([&noise] (int t) { return noise[(size_t) t]; }), [&] (int) { return values; });

            // What came in, as late as set, through the band.
            const auto late = (int) std::lround (milliseconds * 0.001 * sampleRate);
            const auto lowCut = dsp::makeFilter (dsp::FilterShape::highPass, 20.0, sampleRate, 0.70710678);
            const auto highCut = dsp::makeFilter (dsp::FilterShape::lowPass, 20000.0, sampleRate, 0.70710678);
            dsp::Filter low, high;
            auto largest = 0.0f;

            for (size_t i = 0; i < noise.size(); ++i)
            {
                const auto in = i >= (size_t) late ? noise[i - (size_t) late] : 0.0f;
                largest = std::max (largest, std::abs (out.left[i] - high.process (low.process (in, lowCut), highCut)));
            }

            INFO (milliseconds << " ms at " << sampleRate << " Hz: the echo differs by " << toDecibels (largest / 0.5f) << " dB");
            CHECK (largest < 1.0e-5f);
        }
    }
}

TEST_CASE ("The delay with Link off: each side's echoes at its own time, and in ping-pong they cross from one time to the other")
{
    // 90 ms on the left, 130 on the right: an impulse on both sides comes back on each at its own time.
    const auto impulse = [] (int t) { return std::pair { t == 0 ? 1.0f : 0.0f, t == 0 ? 1.0f : 0.0f }; };
    const auto values = with ({ { "link", 0.0f }, { "timeRight", 130.0f }, { "feedback", 0.5f } }, plain (90.0f));
    const auto out = play (values, 24000, impulse);
    const auto peakAt = [] (const std::vector<float>& sound, size_t from, size_t to)
    {
        auto at = from;

        for (auto i = from; i < to; ++i)
            if (std::abs (sound[i]) > std::abs (sound[at]))
                at = i;

        return (int) at;
    };

    CHECK (std::abs (peakAt (out.left, 0, 6000) - 4320) <= 1);    // 90 ms
    CHECK (std::abs (peakAt (out.right, 0, 7000) - 6240) <= 1);   // 130 ms
    CHECK (std::abs (peakAt (out.left, 5000, 9500) - 8640) <= 1); // and again 90 ms later
    CHECK (getPeak (out.right, 0, 6000) < 1.0e-4f);

    // Ping-pong: the sound goes in on the left, comes back there at 90 ms, crosses to the right and comes back 130 ms after
    // that, then on the left 90 ms later again.
    const auto bounced = play (with ({ { "pingPong", 1.0f } }, values), 24000, impulse);
    CHECK (std::abs (peakAt (bounced.left, 0, 6000) - 4320) <= 1);
    CHECK (std::abs (peakAt (bounced.right, 0, 12000) - (4320 + 6240)) <= 1);
    CHECK (std::abs (peakAt (bounced.left, 6000, 16000) - (4320 + 6240 + 4320)) <= 1);
    CHECK (getPeak (bounced.right, 0, 10000) < 1.0e-4f);
}

TEST_CASE ("The delay's Clean character gives an echo back exactly, the Feedback's gain down; Tape bends it with harmonics")
{
    // A 1 kHz sine for exactly the time, 500 ms, then nothing: the first echo is it through the band, the second the first
    // again through the loop. 200 ms into each, the band's start has rung out.
    const auto window = (size_t) 9600; // 200 cycles
    const auto lowCut = dsp::makeFilter (dsp::FilterShape::highPass, 20.0, rate, 0.70710678);
    const auto highCut = dsp::makeFilter (dsp::FilterShape::lowPass, 20000.0, rate, 0.70710678);
    const auto band = dsp::getMagnitude (lowCut, 1000.0, rate) * dsp::getMagnitude (highCut, 1000.0, rate);

    for (const auto decibels : { -20.0f, -3.0f })
    {
        for (const auto onTape : { false, true })
        {
            CAPTURE (decibels);
            CAPTURE (onTape);
            const auto values = plain (500.0f, { { "feedback", 0.8f }, { "character", onTape ? 0.0f : 1.0f } });
            const auto out = play (values, 72000, both (sine (1000.0, dsp::decibelsToGain (decibels), 24000)));
            const auto first = getToneLevel (out.left, 24000 + window, window, 1000.0);
            const auto second = getToneLevel (out.left, 48000 + window, window, 1000.0);

            auto loudestHarmonic = 0.0;

            for (int harmonic = 2; harmonic <= 5; ++harmonic)
                loudestHarmonic = std::max (loudestHarmonic, getToneLevel (out.left, 48000 + window, window, 1000.0 * harmonic));

            const auto harmonics = toDecibels (loudestHarmonic / second);
            CAPTURE (harmonics);

            if (! onTape)
            {
                CHECK (harmonics < -100.0);
                CHECK (toDecibels (second / first) == doctest::Approx (toDecibels (0.8 * band)).epsilon (0.0001));
            }
            else if (decibels > -10.0f)
            {
                CHECK (harmonics > -60.0); // the saturation bends a loud echo
            }
        }
    }

    // The curve: a straight line to full scale, rounding off toward 2, as steep as the line and as straight where they join.
    CHECK (juce::exactlyEqual (Delay::bendClean (0.5f), 0.5f));
    CHECK (juce::exactlyEqual (Delay::bendClean (-1.0f), -1.0f));
    CHECK (Delay::bendClean (1.001f) == doctest::Approx (1.001f).epsilon (1.0e-6));
    CHECK (Delay::bendClean (3.0f) == doctest::Approx (1.0f + std::tanh (2.0f)));
    CHECK (Delay::bendClean (5.0f) < 2.0f);
    CHECK (Delay::bendClean (100.0f) <= 2.0f);
    CHECK (Delay::bendClean (-100.0f) >= -2.0f);
}

TEST_CASE ("The delay's feedback above 100 % swells until the loop holds it, finite and under +7 dBFS for a minute, in both "
           "characters")
{
    // A minute of noise in at 110 %, with the cuts at their defaults, wide open and narrow.
    const auto noise = makeNoise (48000 * 60, 3, 0.25f);

    for (const auto onTape : { true, false })
    {
        for (const auto& cuts : { std::pair { 100.0f, 8000.0f }, std::pair { 20.0f, 20000.0f }, std::pair { 500.0f, 2000.0f } })
        {
            CAPTURE (onTape);
            CAPTURE (cuts.first);
            const auto values = with (
                { { "feedback", 1.1f }, { "character", onTape ? 0.0f : 1.0f }, { "lowCut", cuts.first }, { "highCut", cuts.second } });
            Delay delay;
            delay.prepare (rate, 512);
            const auto out =
                play (delay, (int) noise.size(), 512, both ([&noise] (int t) { return noise[(size_t) t]; }), [&] (int) { return values; });

            auto finite = true;

            for (size_t i = 0; i < out.left.size(); ++i)
                finite = finite && std::isfinite (out.left[i]) && std::isfinite (out.right[i]);

            CHECK (finite);
            const auto peak = std::max (getPeak (out.left, 0, out.left.size()), getPeak (out.right, 0, out.right.size()));
            CHECK (toDecibels (peak) < 7.0);
            CHECK (delay.getTailSeconds() == doctest::Approx (Delay::longestTailSeconds)); // held: its longest
        }
    }

    // It swells: a short burst, then silence, at 105 %, its echoes louder after a few seconds than the first.
    const auto values = plain (100.0f, { { "feedback", 1.05f }, { "lowCut", 100.0f }, { "highCut", 8000.0f } });
    const auto out = play (values, 48000 * 4, both (sine (440.0, 0.05f, 2400)));
    CHECK (getRms (out.left, 48000 * 3, 48000 * 4) > 2.0 * getRms (out.left, 4800, 9600));
}

TEST_CASE ("The delay's modulation wavers each line's time by 4 ms either way at 100 %, the right a quarter of a cycle ahead")
{
    // A steady 1 kHz sine, 100 ms late, its time swung at 0.5 Hz: how late it comes out, every 5 ms for two of its cycles,
    // swings 192 samples (4 ms) either way at 0.5 Hz on each side, the right's swing a quarter of a cycle ahead of the left's.
    const auto values = plain (100.0f, { { "modDepth", 1.0f }, { "modRate", 0.5f } });
    const auto out = play (values, 48000 * 5, both (sine (1000.0, 0.5f)));
    const auto hop = 240;
    const auto twoCycles = [&] (const std::vector<float>& sound)
    {
        auto lateness = getLateness (sound, 1000.0, hop, 9600, 4800.0);
        REQUIRE (lateness.size() >= 800);
        lateness.resize (800); // 4 s
        return getSwing (lateness, 0.5, hop);
    };
    const auto left = twoCycles (out.left), right = twoCycles (out.right);
    CHECK (left.getLevel() == doctest::Approx (192.0).epsilon (0.02));
    CHECK (right.getLevel() == doctest::Approx (192.0).epsilon (0.02));

    // A pitch that swings 2 pi 0.5 Hz 4 ms = 1.26 % either way, 21.7 cents.
    const auto cents = getFurthestCents (out.left, 9600, out.left.size(), 1000.0);
    CHECK (cents == doctest::Approx (1200.0 * std::log2 (1.0 + twoPi * 0.5 * 0.004)).epsilon (0.1));

    const auto ahead = std::remainder (right.getPhase() - left.getPhase(), twoPi);
    CHECK (ahead == doctest::Approx (twoPi / 4.0).epsilon (0.03));

    // At 0 % it isn't there: at any rate, sample for sample what plays without it.
    const auto noise = makeNoise (48000, 5, 0.5f);
    const auto input = both ([&noise] (int t) { return noise[(size_t) t]; });
    const auto still = play (getDefaults(), 48000, input), atNone = play (with ({ { "modRate", 7.0f } }), 48000, input);
    auto differences = 0;

    for (size_t i = 0; i < still.left.size(); ++i)
        if (! juce::exactlyEqual (still.left[i], atNone.left[i]) || ! juce::exactlyEqual (still.right[i], atNone.right[i]))
            ++differences;

    CHECK (differences == 0);
}

TEST_CASE ("The delay's modulation runs on from its rate: two renders agree, a start takes it from the song, a loop doesn't move it")
{
    // A sine that is the song's (its phase from the song's sample), 50 ms late, its time swung at 3 Hz.
    const auto values = plain (50.0f, { { "modDepth", 0.7f }, { "modRate", 3.0f } });
    const auto tone = [] (std::int64_t from)
    { return both ([from] (int t) { return 0.5f * (float) std::sin (twoPi * 700.0 * (double) (from + t) / rate); }); };

    // Two renders, each from a delay made ready, play the same samples.
    const auto render = [&] (std::int64_t from, int numSamples)
    {
        Delay delay;
        delay.prepare (rate, 480);
        return play (delay, numSamples, 480, tone (from), [&] (int) { return values; }, { .from = from });
    };

    const auto first = render (0, 48000 * 3), second = render (0, 48000 * 3);
    auto differences = 0;

    for (size_t i = 0; i < first.left.size(); ++i)
        if (! juce::exactlyEqual (first.left[i], second.left[i]) || ! juce::exactlyEqual (first.right[i], second.right[i]))
            ++differences;

    CHECK (differences == 0);

    // One that starts a second in takes its phase from where the song is: once its line holds what came after the start and
    // its cuts have rung out what came before, it plays what the render from the song's start plays there.
    const auto later = render (48000, 48000 * 2);
    auto largest = 0.0f;

    for (size_t i = 24000; i < later.left.size(); ++i)
        largest =
            std::max ({ largest, std::abs (later.left[i] - first.left[48000 + i]), std::abs (later.right[i] - first.right[48000 + i]) });

    CHECK (largest < 1.0e-5f);

    // A loop of a bar at 120 BPM (2 s), gone round ten times at 0.8 Hz (1.6 cycles each time round), with a steady 1 kHz sine
    // coming in: the phase runs on through each wrap, so the echo's pitch swings no further there than the modulation's own
    // slope takes it (21.7 cents at 100 % and 0.8 Hz is 2 pi 0.8 Hz 4 ms; at 35 % a third of that), where the phase chased
    // the song's place within 10 ms at each wrap before.
    const auto swinging = plain (100.0f, { { "modDepth", 0.35f }, { "modRate", 0.8f } });
    Delay looped;
    looped.prepare (rate, 480);
    const auto wrapped = play (looped, 96000 * 10, 480, both (sine (1000.0, 0.5f)), [&] (int) { return swinging; }, { .loop = 96000 });
    const auto most = 1200.0 * std::log2 (1.0 + twoPi * 0.8 * 0.35 * Delay::mostModulationSeconds);
    CHECK (getFurthestCents (wrapped.left, 9600, wrapped.left.size(), 1000.0) < most * 1.05);
    CHECK (getFurthestCents (wrapped.right, 9600, wrapped.right.size(), 1000.0) < most * 1.05);

    // Stopped, it runs free: it still swings.
    const auto stopped = play (values, 48000 * 2, tone (0), { .playing = false });
    CHECK (getSwing (getLateness (stopped.left, 700.0, 240, 4800, 2400.0), 3.0, 240).getLevel() > 100.0);
}

TEST_CASE ("The delay's Mod Rate moved while it plays bends the echoes' pitch no more than the modulation at the faster rate")
{
    // A minute into the song, the Mod Rate moved from 0.8 to 2 Hz over a second, block by block, as automation moves it,
    // at 35 % depth, a steady 1 kHz sine echoing 100 ms late: the phase runs on, faster, and never jumps.
    Delay delay;
    delay.prepare (rate, 480);
    const auto out = play (delay, 48000 * 3, 480, both (sine (1000.0, 0.5f)),
                           [] (int t)
                           {
                               const auto along = std::clamp ((float) (t - 48000) / 48000.0f, 0.0f, 1.0f);
                               return plain (100.0f, { { "modDepth", 0.35f }, { "modRate", 0.8f + 1.2f * along } });
                           },
                           { .from = 48000 * 60 });

    const auto fastest = 1200.0 * std::log2 (1.0 + twoPi * 2.0 * 0.35 * Delay::mostModulationSeconds);
    const auto cents = getFurthestCents (out.left, 9600, out.left.size(), 1000.0);
    CAPTURE (fastest);
    CHECK (cents > 0.5 * fastest);
    CHECK (cents < fastest * 1.05);
    CHECK (getFurthestCents (out.right, 9600, out.right.size(), 1000.0) < fastest * 1.05);
}

TEST_CASE ("The delay's duck turns its echoes down while sound comes in, and they come back after it")
{
    // A phrase of a 1 kHz sine at -6 dBFS for a second, then silence; echoes every 250 ms, half as loud each time, the
    // band open.
    const auto phrase = both (sine (1000.0, 0.5f, 48000));
    const auto at = [&] (float duckAmount)
    { return play (plain (250.0f, { { "feedback", 0.5f }, { "duck", duckAmount } }), 48000 * 2, phrase); };
    const auto undocked = at (0.0f), full = at (1.0f), half = at (0.5f);

    // During it: 24 dB down at 100 %, 12 at 50 %.
    const auto during = [&undocked] (const Stereo& ducked)
    { return toDecibels (getRms (ducked.left, 24000, 47000) / getRms (undocked.left, 24000, 47000)); };
    CHECK (during (full) == doctest::Approx (-24.0).epsilon (0.02));
    CHECK (during (half) == doctest::Approx (-12.0).epsilon (0.02));

    // After it, back within 300 ms at the level the loop holds: what goes round was never ducked.
    for (const auto* ducked : { &full, &half })
    {
        const auto back = (size_t) (48000 + 14400);
        CHECK (std::abs (toDecibels (getRms (ducked->left, back, back + 4800) / getRms (undocked.left, back, back + 4800))) < 0.05);
    }

    // A sound under -36 dBFS ducks nothing: sample for sample what plays without the duck.
    const auto quiet = both (sine (1000.0, dsp::decibelsToGain (-40.0f), 48000));
    const auto quietUndocked = play (plain (250.0f, { { "feedback", 0.5f } }), 48000, quiet);
    const auto quietDucked = play (plain (250.0f, { { "feedback", 0.5f }, { "duck", 1.0f } }), 48000, quiet);
    auto differences = 0;

    for (size_t i = 0; i < quietDucked.left.size(); ++i)
        if (! juce::exactlyEqual (quietDucked.left[i], quietUndocked.left[i]))
            ++differences;

    CHECK (differences == 0);
    CHECK (Delay::getDuckDecibels (1.0f, -12.0f) == doctest::Approx (24.0f));
    CHECK (Delay::getDuckDecibels (1.0f, -24.0f) == doctest::Approx (12.0f));
    CHECK (Delay::getDuckDecibels (0.5f, 0.0f) == doctest::Approx (12.0f));
    CHECK (Delay::getDuckDecibels (1.0f, -36.0f) == doctest::Approx (0.0f));
}

TEST_CASE ("The delay's freeze holds what is in its lines, going round unchanged, hears nothing new, and lets go without a click")
{
    // Noise for 300 ms, echoes every 375 ms at 50 %, frozen at 500 ms for 30 s while a 3 kHz sine comes in, then let go.
    const auto noise = makeNoise (14400, 9, 0.5f);
    const auto frozenFrom = 24000, frozenUntil = 24000 + 48000 * 30, length = frozenUntil + 48000 * 3;
    const auto input = both (
        [&noise] (int t)
        { return t < (int) noise.size() ? noise[(size_t) t] : (t > 48000 ? 0.3f * (float) std::sin (twoPi * 3000.0 * t / rate) : 0.0f); });
    const auto values = with ({ { "sync", 0.0f }, { "time", 375.0f }, { "feedback", 0.5f } });
    Delay delay;
    delay.prepare (rate, 480);
    const auto out = play (delay, length, 480, input,
                           [&] (int t) { return with ({ { "freeze", t >= frozenFrom && t < frozenUntil ? 1.0f : 0.0f } }, values); });
    CHECK (delay.getTailSeconds() < Delay::longestTailSeconds); // let go: its tail as the feedback has it again

    // Each pass of the lines, once frozen, as loud as the first, within 0.01 dB, 30 s on.
    const auto pass = (size_t) 18000;
    const auto firstPass = getRms (out.left, (size_t) frozenFrom + pass, (size_t) frozenFrom + 2 * pass);
    const auto lastPass = getRms (out.left, (size_t) frozenUntil - 2 * pass, (size_t) frozenUntil - pass);
    CHECK (firstPass > 0.01);
    CHECK (std::abs (toDecibels (lastPass / firstPass)) < 0.01);

    // Nothing of the sine that came in meanwhile in the echoes: sample for sample what plays with silence coming in.
    Delay silent;
    silent.prepare (rate, 480);
    const auto unheard =
        play (silent, 48000 * 3, 480, both ([&noise] (int t) { return t < (int) noise.size() ? noise[(size_t) t] : 0.0f; }),
              [&] (int t) { return with ({ { "freeze", t >= frozenFrom ? 1.0f : 0.0f } }, values); });
    auto differences = 0;

    for (size_t i = 48000; i < unheard.left.size(); ++i)
        if (! juce::exactlyEqual (unheard.left[i], out.left[i]) || ! juce::exactlyEqual (unheard.right[i], out.right[i]))
            ++differences;

    CHECK (differences == 0);

    // Let go, the echoes decay again: the sine plays in them, and what was held falls.
    CHECK (getToneLevel (out.left, (size_t) frozenUntil + 48000 * 2, 48000, 3000.0) > 0.05);

    // Switched on and off every 400 ms over a steady 200 Hz sine, its echoes at 50 %: no step further than the sine's own
    // could go (about 0.025 at the level its echoes reach), where a switch made at once jumps by as much as the echo is loud.
    Delay switching;
    switching.prepare (rate, 480);
    const auto clicks = play (switching, 48000 * 4, 480, both (sine (200.0, 0.5f)),
                              [&] (int t) { return with ({ { "freeze", (t / 19200) % 2 == 1 ? 1.0f : 0.0f } }, values); });
    CHECK (getPeak (clicks.left, 24000, clicks.left.size()) > 0.3f);
    CHECK (getLargestStep (clicks.left, 24000, clicks.left.size()) < 0.05f);
}
TEST_CASE ("Frozen, the delay plays on the echo heard as the freeze came on, through its cuts, and holds no offset")
{
    // Dub Echo's narrow cuts (350 Hz to 2.5 kHz), a quarter of a second at 50 %, Tape: a low tone (a kick's sub), tones in the
    // band and a high one (a hat's), each a whole number of cycles in the time, with an offset of 0.2, frozen at 2 s. What
    // the lines hold is the sound as it came in, whole; what is heard is it as the cuts read it, before the freeze and after.
    const auto tones = [] (int t)
    {
        auto sample = 0.2f;

        for (const auto hertz : { 52.0, 200.0, 500.0, 1000.0, 2000.0, 8000.0 })
            sample += 0.12f * (float) std::sin (twoPi * hertz * t / rate);

        return sample;
    };

    const auto values = plain (250.0f, { { "feedback", 0.5f }, { "lowCut", 350.0f }, { "highCut", 2500.0f } });
    Delay delay;
    delay.prepare (rate, 480);
    const auto out =
        play (delay, 48000 * 4, 480, both (tones), [&] (int t) { return with ({ { "freeze", t >= 96000 ? 1.0f : 0.0f } }, values); });

    // In the second before and the second after, the same in every third of an octave within 0.5 dB.
    const auto before = getBandLevels (out.left, 52000, 32768, rate, 15), after = getBandLevels (out.left, 100000, 32768, rate, 15);
    CHECK (getLargestBandDifference (before, after) < 0.5);
    CHECK (std::abs (toDecibels (getRms (out.left, 100000, 132768) / getRms (out.left, 52000, 84768))) < 0.5);

    // Nothing of the offset comes out, frozen or not.
    auto mean = 0.0;

    for (size_t i = 100000; i < 132768; ++i)
        mean += out.left[i];

    CHECK (std::abs (mean / 32768.0) < 1.0e-3);
}

TEST_CASE ("Frozen at a time between samples, the delay's lines go round unchanged: noise keeps every third of an octave for 30 s")
{
    // A dotted eighth at 120 BPM and 44.1 kHz is 16537.5 samples. Noise for half a second, frozen there, the cuts open: what
    // goes round copies whole samples (16538 apart), so that a read between samples, pushed back, doesn't take the top off
    // pass after pass (12 kHz was 15 dB down in 4 s). What is heard, read between samples once, repeats every 16538 samples.
    constexpr double sampleRate = 44100.0;
    constexpr int blockSize = 441, frozenFrom = 22050, period = 16538, length = 32768;
    const auto noise = makeNoise (frozenFrom, 13, 0.5f);
    const auto values = with ({ { "lowCut", 20.0f }, { "highCut", 20000.0f } });
    Delay delay;
    delay.prepare (sampleRate, blockSize);
    const auto first = frozenFrom + 44100, last = first + 79 * period; // a second after the freeze, and 29.6 s after that
    const auto out =
        play (delay, last + length + blockSize, blockSize, both ([&noise] (int t) { return t < frozenFrom ? noise[(size_t) t] : 0.0f; }),
              [&] (int t) { return with ({ { "freeze", t >= frozenFrom ? 1.0f : 0.0f } }, values); });

    const auto early = getBandLevels (out.left, (size_t) first, length, sampleRate, 15);
    const auto late = getBandLevels (out.left, (size_t) last, length, sampleRate, 15);
    CHECK (getLargestBandDifference (early, late) < 0.1);
    CHECK (getRms (out.left, (size_t) last, (size_t) (last + length)) > 0.01);
}

TEST_CASE ("Frozen, a change of time stretches the delay's loop between samples without a tick, and settles it on whole samples")
{
    // A 2 kHz sine at 0.5 into a dotted eighth, Sync on, the cuts open, frozen at a second; then the tempo automated from 120
    // to 121 BPM over 4 s, as the loop goes round. Cut by whole samples, the loop dropped or doubled a sample each time its
    // rounded length stepped (some 150 times), each a step of 0.14 in the tone, there for good. Stretched between samples
    // while the time moves, and settled on whole samples as it rests again, no step between two samples goes past 1.5 times
    // the largest the steady loop makes (the sine's own, and the echo's read between samples).
    const auto values = [] (int t)
    { return with ({ { "lowCut", 20.0f }, { "highCut", 20000.0f }, { "freeze", t >= 48000 ? 1.0f : 0.0f } }); };
    const auto playAt = [&] (std::function<double (int)> tempoAt)
    {
        Delay delay;
        delay.prepare (rate, 480);
        return play (delay, 48000 * 7, 480, both (sine (2000.0, 0.5f)), values, { .tempoAt = tempoAt });
    };

    const auto steady = playAt ([] (int) { return 120.0; });
    const auto moving = playAt ([] (int t) { return 120.0 + std::clamp ((double) (t - 96000) / 192000.0, 0.0, 1.0); });
    const auto steadyStep = getLargestStep (steady.left, 72000, steady.left.size());
    CAPTURE (steadyStep);
    CHECK (steadyStep > 0.1f);
    CHECK (getLargestStep (moving.left, 72000, moving.left.size()) < 1.5f * steadyStep);
    CHECK (getLargestStep (moving.right, 72000, moving.right.size()) < 1.5f * steadyStep);

    // Noise frozen, the tempo moved the same way, then 30 s more frozen: the loop on whole samples again (about 17851 of them
    // at 121 BPM: Repitch's glide comes to rest a sample or two from the time set, where its steps grow too small for its
    // float), what is heard read between them as the time says. What comes out repeats a loop's length later sample for
    // sample, and 30 s on, every third of an octave is within 0.1 dB.
    const auto noise = makeNoise (24000, 17, 0.5f);
    constexpr int length = 32768;
    const auto first = 288000;
    const auto numSamples = first + 82 * 17860 + length;
    Delay delay;
    delay.prepare (rate, 480);
    const auto out =
        play (delay, numSamples, 480, both ([&noise] (int t) { return t < (int) noise.size() ? noise[(size_t) t] : 0.0f; }),
              [] (int t) { return with ({ { "lowCut", 20.0f }, { "highCut", 20000.0f }, { "freeze", t >= 24000 ? 1.0f : 0.0f } }); },
              { .tempoAt = [] (int t) { return 120.0 + std::clamp ((double) (t - 48000) / 192000.0, 0.0, 1.0); } });

    // The loop's length: where what comes out is most like itself.
    auto period = 0;
    auto closest = 1.0e9;

    for (int lag = 17830; lag <= 17880; ++lag)
    {
        auto difference = 0.0;

        for (int i = 0; i < 4096; ++i)
            difference = std::max (difference, (double) std::abs (out.left[(size_t) (first + i)] - out.left[(size_t) (first + lag + i)]));

        if (difference < closest)
        {
            closest = difference;
            period = lag;
        }
    }

    CAPTURE (period);
    CHECK (closest < 1.0e-5);
    const auto last = first + 80 * period;
    CHECK (getLargestBandDifference (getBandLevels (out.left, (size_t) first, length, rate, 15),
                                     getBandLevels (out.left, (size_t) last, length, rate, 15))
           < 0.1);
    CHECK (getRms (out.left, (size_t) last, (size_t) (last + length)) > 0.01);
}

TEST_CASE ("After a sleep, the delay swings as one that never slept: its modulation's phase moves on with the song's samples")
{
    // Through its runner, Mod Depth at 50 %: a second of noise, nine of silence (it falls asleep once its tail has passed,
    // live, where a render plays on), then three of noise. From half a second into the second sound, live plays what the
    // render plays, sample for sample.
    const auto sound = probe::join (probe::join (probe::makeNoise (48000, 3, 0.5f), probe::makeSilence (48000 * 9)),
                                    probe::makeNoise (48000 * 3, 4, 0.5f));
    const auto playIt = [&sound] (bool rendering, std::uint64_t& worked)
    {
        probe::Device device (Delay::info);
        device.set ("modDepth", 0.5f);
        device.isRendering = rendering;
        device.prepare (rate, 480);
        auto played = device.played (sound, 480);
        worked = device.getRunner().getBlocksWorked();
        return played;
    };

    std::uint64_t liveBlocks = 0, renderBlocks = 0;
    const auto live = playIt (false, liveBlocks), render = playIt (true, renderBlocks);
    CHECK (liveBlocks < renderBlocks); // it slept
    CHECK (probe::getLargestDifference (live, render, 48000 * 10 + 24000) < 1.0e-5);
}
TEST_CASE ("The delay's transitions: Repitch glides to a new time, bending the echoes' pitch; Fade doesn't glide, and doesn't click")
{
    // The time a function of where the block starts: 375 ms, changed at a second, and again 20 ms later (during the fade).
    const auto changing = [] (float transitionValue, float firstChange, float secondChange) -> Values
    {
        return [=] (int t)
        {
            const auto milliseconds = t < 48000 ? 375.0f : (t < 48960 ? firstChange : secondChange);
            return plain (milliseconds, { { "transition", transitionValue } });
        };
    };

    // A steady 1 kHz sine, the time changed by whole numbers of its cycles (500 ms, then 625), so that a read at the old time
    // and one at the new are in step: Repitch bends the echo's pitch far as it glides; Fade doesn't glide the pitch at all
    // (within 2 cents). This shows no glide, not the crossing itself, which a jump to the new time would pass too: what Fade
    // keeps of the level as it crosses is the next case's.
    for (const auto fades : { false, true })
    {
        CAPTURE (fades);
        Delay delay;
        delay.prepare (rate, 480);
        const auto out = play (delay, 48000 * 3, 480, both (sine (1000.0, 0.5f)), changing (fades ? 1.0f : 0.0f, 500.0f, 625.0f));
        const auto cents = getFurthestCents (out.left, 24000, out.left.size(), 1000.0);
        CAPTURE (cents);

        if (fades)
            CHECK (cents < 2.0);
        else
            CHECK (cents > 100.0);
    }

    // A 200 Hz sine, the times out of step with it: Fade crosses from one read to the other, the second change after the
    // first's fade, with no step further than the sine's own could go (0.013, up to 0.019 for two reads out of step crossed at
    // equal power), where a jump to the new time steps up to 1.
    Delay delay;
    delay.prepare (rate, 480);
    const auto out = play (delay, 48000 * 2, 480, both (sine (200.0, 0.5f)), changing (1.0f, 401.3f, 433.7f));
    CHECK (getLargestStep (out.left, 24000, out.left.size()) < 0.025f);
    CHECK (delay.getFadesStarted() == 4); // two changes, a fade on each side for each

    // Each time reached: an echo of the sine as late as the last time says.
    const auto late = getLateness (out.left, 200.0, 480, 48000 + 9600, 433.7 * 48.0);
    CHECK (late.back() == doctest::Approx (433.7 * 48.0).epsilon (0.002));
}

TEST_CASE ("The delay's Fade crosses at equal power: noise keeps its level through a change of time")
{
    // Noise in, 50 ms late, the time changed to 73 ms at 200 ms with Fade: the two reads are unrelated, as a note value switched
    // is, and the sum of their powers as they cross is the level. Each 5 ms of what comes out, its power averaged over 16
    // noises, stays within 1 dB of the level before the change and after its fade (an amplitude crossfade was 3 dB down
    // halfway).
    constexpr int frame = 240, change = 9600, numSamples = 19200;
    std::vector<double> power ((size_t) (numSamples / frame), 0.0);

    for (int seed = 0; seed < 16; ++seed)
    {
        const auto noise = makeNoise (numSamples, 100 + seed, 0.5f);
        Delay delay;
        delay.prepare (rate, 480);
        const auto out = play (delay, numSamples, 480, both ([&noise] (int t) { return noise[(size_t) t]; }),
                               [] (int t) { return plain (t < change ? 50.0f : 73.0f, { { "transition", 1.0f } }); });
        CHECK (delay.getFadesStarted() == 2);

        for (size_t f = 0; f < power.size(); ++f)
            power[f] += getRms (out.left, f * frame, (f + 1) * frame) * getRms (out.left, f * frame, (f + 1) * frame);
    }

    auto level = 0.0;
    auto counted = 0;

    for (auto f = (size_t) (4800 / frame); f < power.size(); ++f)
        if (f < (size_t) (change / frame) || f >= (size_t) ((change + 2400) / frame))
        {
            level += power[f];
            ++counted;
        }

    level /= counted;

    for (auto f = (size_t) (4800 / frame); f < power.size(); ++f)
    {
        CAPTURE (f);
        CHECK (std::abs (10.0 * std::log10 (power[f] / level)) < 1.0);
    }
}

TEST_CASE ("The delay's Fade never bends the pitch past 10 cents: a nudge glides in, a jump and a steep move fade")
{
    // A steady 1 kHz sine, 0.5, echoed with Fade, the cuts open, no feedback; the pitch of what comes out between each two
    // rising zero crossings, wherever no fade is under way (a crossing of two reads of a steady tone turns its phase, which
    // isn't a bend of the time), held within 12 cents.
    const auto withFade = [] (std::initializer_list<std::pair<std::string_view, float>> changes)
    { return with (changes, with ({ { "feedback", 0.0f }, { "lowCut", 20.0f }, { "highCut", 20000.0f }, { "transition", 1.0f } })); };

    struct Case
    {
        const char* name;
        int numSamples;
        Values values;
        std::function<double (int)> tempoAt;
    };

    const Case cases[] = {
        // A nudge of 0.5 ms: under 1 ms behind, it glides at 10 cents, and no fade starts.
        { "a nudge", 48000 * 3, [=] (int t) { return withFade ({ { "sync", 0.0f }, { "time", t < 48000 ? 375.0f : 375.5f } }); }, nullptr },
        // A typed change of 19 ms at a time of 2 s: it fades.
        { "a typed change", 48000 * 5, [=] (int t) { return withFade ({ { "sync", 0.0f }, { "time", t < 144000 ? 2000.0f : 2019.0f } }); },
          nullptr },
        // The time ramped from 375 to 400 ms over 2 s (21 cents' worth): it glides, falls behind, fades, and again.
        { "a ramp of the time", 48000 * 4,
          [=] (int t)
          {
              const auto along = std::clamp ((float) (t - 48000) / 96000.0f, 0.0f, 1.0f);
              return withFade ({ { "sync", 0.0f }, { "time", 375.0f + 25.0f * along } });
          },
          nullptr },
        // A dotted eighth, Sync on, the tempo ramped from 120 to 150 BPM over 2 s (375 ms to 300: 4 % of real time).
        { "a ramp of the tempo", 48000 * 4, [=] (int) { return withFade ({}); },
          [] (int t) { return 120.0 + 30.0 * std::clamp ((double) (t - 48000) / 96000.0, 0.0, 1.0); } },
    };

    for (const auto& each : cases)
    {
        const auto name = std::string (each.name);
        CAPTURE (name);
        Delay delay;
        delay.prepare (rate, 480);
        const auto out = playMarked (delay, each.numSamples, both (sine (1000.0, 0.5f)), each.values, { .tempoAt = each.tempoAt });
        const auto pitch = getFurthestCentsOutside (out.sound.left, 24000, out.sound.left.size(), 1000.0, out.fading);
        MESSAGE (name << ": " << delay.getFadesStarted() / 2 << " fades a side; " << pitch.periods
                      << " cycles measured between fades, the furthest " << pitch.cents << " cents off");
        CHECK (pitch.cents < 12.0);

        if (name == "a nudge")
        {
            CHECK (delay.getFadesStarted() == 0);
            CHECK (pitch.periods > 2000);
        }
        else
        {
            CHECK (delay.getFadesStarted() >= 2);
        }

        if (name == "a ramp of the time")
            CHECK (pitch.periods > 500); // it glides between its fades
    }

    // Noise through the tempo's ramp: the comb its fades one after another make, in thirds of an octave from 1 to 8 kHz
    // against the noise that went in, over the ramp.
    const auto noise = makeNoise (48000 * 4, 31, 0.5f);
    Delay combing;
    combing.prepare (rate, 480);
    const auto combed =
        play (combing, 48000 * 4, 480, both ([&noise] (int t) { return noise[(size_t) t]; }), [=] (int) { return withFade ({}); },
              { .tempoAt = [] (int t) { return 120.0 + 30.0 * std::clamp ((double) (t - 48000) / 96000.0, 0.0, 1.0); } });
    const auto comb = getLargestBandDifference (getBandLevels (noise, 48000 - 17000, 96000, rate),
                                                getBandLevels (combed.left, 48000, 96000, rate), 1000.0, 8000.0);
    MESSAGE ("the tempo's ramp combs noise by " << comb << " dB in thirds of an octave from 1 to 8 kHz");
    CHECK (comb < 1.0);

    // A tempo that moves slower than 10 cents' worth (120 to 126 BPM over eight seconds) glides all the way: no fade.
    Delay gentle;
    gentle.prepare (rate, 480);
    play (gentle, 48000 * 8, 480, both (sine (1000.0, 0.5f)), [=] (int) { return withFade ({}); },
          { .tempoAt = [] (int t) { return 120.0 + 6.0 * t / (48000.0 * 8.0); } });
    CHECK (gentle.getFadesStarted() == 0);

    // A change of 3 ms fades.
    Delay shorter;
    shorter.prepare (rate, 480);
    play (shorter, 9600, 480, both ([&noise] (int t) { return noise[(size_t) t]; }),
          [] (int t) { return plain (t < 4800 ? 100.0f : 103.0f, { { "transition", 1.0f } }); });
    CHECK (shorter.getFadesStarted() == 2);
}
TEST_CASE ("The delay's Fade swings each of its two reads by its own time's swing: a short time with the modulation doesn't click")
{
    // 3 ms faded to 5 ms with the modulation at 100 %: near the shortest time the swing is less (2 ms at 3 ms, the whole 4 ms
    // at 5), each read's from its own time, so that nothing steps as the fade ends (the smaller of the two swung both, and
    // the read jumped by up to 2 ms there). A 200 Hz sine steps no further than its own slope and the modulation's take it.
    Delay delay;
    delay.prepare (rate, 480);
    const auto out =
        play (delay, 48000 * 2, 480, both (sine (200.0, 0.5f)), [] (int t)
              { return plain (t < 48000 ? 3.0f : 5.0f, { { "transition", 1.0f }, { "modDepth", 1.0f }, { "modRate", 0.3f } }); });
    CHECK (delay.getFadesStarted() == 2);
    CHECK (getLargestStep (out.left, 4800, out.left.size()) < 0.025f);
    CHECK (getLargestStep (out.right, 4800, out.right.size()) < 0.025f);
}

TEST_CASE ("The delay's duck holds the loudest of the last 20 ms: a bass ducks its echoes without a ripple")
{
    // A 50 Hz sine at -6 dBFS for a second, echoes every 250 ms at 50 %: the echoes ducked by the whole 24 dB, the gain
    // steady within 0.3 dB through each cycle (falling 60 dB in 250 ms between the half-cycles, it moved 2.4 dB).
    const auto phrase = both (sine (50.0, 0.5f, 48000));
    const auto undocked = play (plain (250.0f, { { "feedback", 0.5f } }), 48000, phrase);
    const auto ducked = play (plain (250.0f, { { "feedback", 0.5f }, { "duck", 1.0f } }), 48000, phrase);
    auto lowest = 1000.0, highest = -1000.0;

    for (size_t start = 28800; start + 48 <= 47000; start += 48)
    {
        const auto plainLevel = getRms (undocked.left, start, start + 48);

        if (plainLevel < 0.05)
            continue;

        const auto gain = toDecibels (getRms (ducked.left, start, start + 48) / plainLevel);
        lowest = std::min (lowest, gain);
        highest = std::max (highest, gain);
    }

    CAPTURE (lowest);
    CAPTURE (highest);
    CHECK (highest - lowest < 0.3);
    CHECK (highest == doctest::Approx (-24.0).epsilon (0.02));
}

TEST_CASE ("The delay's presets: Dub Swell swells long and dies away; Chorus Echo swings its echo 10 to 20 cents")
{
    const auto readValues = [] (const char* name)
    {
        const auto preset = phazite::readPreset (phazite::test::getFactoryPresetsFolder().getChildFile ("Delay").getChildFile (name));
        REQUIRE (preset.isObject());
        std::vector<float> values;

        for (const auto& parameter : Delay::info.parameters)
            values.push_back (phazite::readSavedValue (
                parameter, preset["device"]["parameters"][juce::Identifier (juce::String (parameter.id.data(), parameter.id.size()))]));

        return values;
    };

    // Dub Swell, at 97 % with its narrow cuts: a burst of noise, then silence; half a minute on, its echoes at least 12 dB under
    // where they were in the first seconds (past 100 %, they would ring on as a steady tone until the Feedback came down).
    const auto swell = readValues ("Creative/Dub Swell.phazitepreset");
    CHECK (swell[Delay::feedback] < 1.0f);
    const auto noise = makeNoise (4800, 41, 0.5f);
    const auto out = play (swell, 48000 * 32, both ([&noise] (int t) { return t < (int) noise.size() ? noise[(size_t) t] : 0.0f; }));
    CHECK (toDecibels (getRms (out.left, 48000 * 31, 48000 * 32) / getRms (out.left, 0, 48000 * 3)) < -12.0);

    // Chorus Echo's first echo (its feedback left out, which would sum echoes swung at other moments) bends a steady tone 10
    // to 20 cents either way.
    auto chorus = readValues ("Creative/Chorus Echo.phazitepreset");
    chorus[Delay::feedback] = 0.0f;
    const auto bent = play (chorus, 48000 * 4, both (sine (1000.0, 0.5f)));
    const auto cents = getFurthestCents (bent.left, 4800, bent.left.size(), 1000.0);
    CHECK (cents > 10.0);
    CHECK (cents < 20.0);
}
TEST_CASE ("The delay plays the same in blocks of 32 samples as of 512, everything on")
{
    // Link off, ping-pong, a feedback over 100 %, the modulation and the duck at work; its character changed, a note value
    // switched while it fades, and the freeze switched on and off, each at a block's start in both.
    const auto noise = makeNoise (48000 * 4, 21, 0.25f), other = makeNoise (48000 * 4, 22, 0.25f);
    const Values values = [] (int t)
    {
        return with ({ { "link", 0.0f },
                       { "noteRight", t < 512 * 140 ? 8.0f : 5.0f },
                       { "pingPong", 1.0f },
                       { "feedback", 1.05f },
                       { "character", t < 512 * 94 ? 1.0f : 0.0f },
                       { "modDepth", 0.6f },
                       { "modRate", 2.0f },
                       { "duck", 0.5f },
                       { "freeze", t >= 512 * 188 && t < 512 * 235 ? 1.0f : 0.0f },
                       { "transition", 1.0f } });
    };

    const auto playIn = [&] (int size)
    {
        Delay delay;
        delay.prepare (rate, 512);
        return play (delay, (int) noise.size(), size, [&] (int t) { return std::pair { noise[(size_t) t], other[(size_t) t] }; }, values);
    };

    const auto big = playIn (512), small = playIn (32);
    auto worst = 0.0f;

    for (size_t i = 0; i < big.left.size(); ++i)
        worst = std::max ({ worst, std::abs (big.left[i] - small.left[i]), std::abs (big.right[i] - small.right[i]) });

    CHECK (getPeak (big.left, 0, big.left.size()) > 0.1f);
    CHECK (worst < 1.0e-5f);
}
