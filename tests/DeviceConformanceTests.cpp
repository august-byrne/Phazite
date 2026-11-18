// SPDX-License-Identifier: AGPL-3.0-or-later
// The rules every one of Phazite's own devices keeps (the DSP in src/devices/), measured with the bench in DspProbe.h. The
// devices are the model's one list of them (getOwnDevices(), model/devices/internal/Devices.h): a device added there is held
// to every rule here without a line of this file changing; one whose modes were devices of their own (Motion) is measured in
// each mode, under the mode's name (the variants, below). Each is played as Phazite's engine plays it, on its runner
// (engine/nodes/DeviceRunner.h), the song playing, at 44.1, 48 and 96 kHz, and in blocks of 1, 64, 441, 512 and 4096 samples
// where a rule depends on the blocks:
//
// - Numbers: what comes out is always a finite number, and no louder than any sound is (devices::loudestSound): of silence,
//   of the loudest sound, of a block of no samples, on a mono track, of notes and controllers at their ends; a number, with
//   every parameter in turn at its lowest and its highest, with all of them there, and at settings drawn at random; and
//   an effect handed a block of what isn't sound (not numbers, infinities, values beyond any level) plays on, there and
//   after, exactly as if it had been handed silence.
// - Nothing: with nothing to play (no notes, or silence coming in from the start), a device plays nothing at all; nor after a
//   reset() that follows sound. And the same with the song stopped, as the engine plays it live (no exception: what the
//   runner's sleep leans on, which waits for no tail from a device that has heard nothing since it was reset or prepared).
// - Tail: once what a device is given has stopped and it has been silent for as long as the tail it reports
//   (Processor::getTailSeconds(); no shorter than its runner waits before it lets it sleep), it stays silent: what the
//   runner's sleep and the end of an export rely on (model/playback/OfflineRender.cpp).
// - Latency: an effect's sound comes out as late as it says (Processor::getLatencySamples()), measured by cross-correlation.
// - Mix: an effect with a mix (DeviceInfo.h, mixParameterID), at 0 %, passes what comes in, as late as its latency.
// - Blocks: what a device plays doesn't depend on how the samples are cut into blocks, with its values still; nor, with every
//   continuous parameter moved by automation, on how blocks cut the song's control points, which automation moves them on
//   (engine/automation/Curve.h).
// - Starting over: after reset(), the same input gives the same output; and prepared again for another rate while it
//   sounds, a device plays as one made for that rate.
// - Offset: nothing constant lies under an instrument's held notes, at its defaults and at settings drawn at random around
//   them (but for the parameters in the list after the exceptions, which are known to put one there).
//
// Each rule has a tolerance (the rules' table below). A device that breaks a rule by its design, or by a bug not fixed yet,
// is in the list of exceptions, with the reason and how far it may be off; the rule is still measured for it, and the test
// fails once it keeps the rule, so that the list stays exact. With the environment variable PHAZITE_DEVICE_REPORT set, each
// rule prints what it measured for each device.
#include "DspProbe.h"
#include "TestHelpers.h"
#include "devices/dsp/Crossover.h"
#include "devices/dsp/Random.h"
#include "model/devices/internal/Devices.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace devices = phazite::devices;
namespace probe = phazite::test::probe;
using probe::Sound;

constexpr double rates[] = { 44100.0, 48000.0, 96000.0 };
constexpr int blockSizes[] = { 1, 64, 441, 512, 4096 };
constexpr int usualBlock = 512;

//==============================================================================
// The rules, each with what it measures at its worst and how much of that it lets by.
enum class Rule
{
    numbers,
    nothing,
    nothingStopped,
    tail,
    latency,
    mix,
    blocks,
    automatedBlocks,
    startingOver,
    offset
};

struct RuleInfo
{
    Rule rule;
    const char* name;
    const char* measure; // what the number is; none for a rule that is kept or isn't
    double tolerance;
};

constexpr RuleInfo rules[] = {
    { Rule::numbers, "numbers", nullptr, 0.0 },
    { Rule::nothing, "nothing", nullptr, 0.0 },
    { Rule::nothingStopped, "nothing, stopped", nullptr, 0.0 },
    { Rule::tail, "tail", nullptr, 0.0 },
    // A tenth of a sample: what the measurement is good for (the parabola through a cross-correlation's peak) where a device
    // adds a little of its effect to the sound.
    { Rule::latency, "latency", "samples between the delay it says and the delay measured", 0.1 },
    // What came in, to the bit (a float's rounding, at the most).
    { Rule::mix, "mix", "dB under what came in that what comes out differs from it", -120.0 },
    // The same samples, but for a float's rounding where a compiler orders a sum otherwise at a block's end.
    { Rule::blocks, "blocks", "dB under its sound that it differs by", -90.0 },
    // A three-hundredth: a stretch between two control points that a block cuts in two is played in two calls, and what a
    // device glides over a call (a filter's coefficients, a gain) then glides in two steps.
    { Rule::automatedBlocks, "automated blocks", "dB under its sound that it differs by", -50.0 },
    { Rule::startingOver, "starting over", nullptr, 0.0 },
    // A fiftieth of how loud the notes are (34 dB under them).
    { Rule::offset, "offset", "of its notes' level", 0.02 },
};

const RuleInfo& getRuleInfo (Rule rule)
{
    for (const auto& each : rules)
        if (each.rule == rule)
            return each;

    FAIL ("a rule without its line in the table");
    return rules[0];
}

// The devices that don't keep a rule, each with why, and how much of what the rule measures it may show (or anything).
constexpr double anything = std::numeric_limits<double>::infinity();

struct Exception
{
    std::string_view typeID; // DeviceInfo::typeID
    Rule rule;
    double allowance;
    std::string_view why;
    std::string_view variant {}; // for a device measured in several modes (variants, below): the one it is for, by its name
};

constexpr Exception exceptions[] = {
    // By design.
    { "phaziteAge", Rule::nothing, anything,
      "Its noise (a record's surface, a tape's hiss) plays while the song does, sound or none coming in, until two seconds after "
      "the sound has stopped." },
    { "phaziteFilter", Rule::latency, 2.0,
      "Its latency is its oversampling's 6 samples; its filter, a low pass at first, delays the lows it passes by its own phase "
      "besides, 1.6 samples at 96 kHz. Its copy for the mix, which the filter doesn't touch, is held to the latency by the mix rule." },
    { "phaziteMultiband", Rule::latency, 12.0,
      "Its crossovers turn the phase of what they split (the bands add up to an all-pass), which delays the lows 3 samples at "
      "48 kHz and 9 at 96 kHz more than the look-ahead and oversampling it reports." },
    { "phaziteAge", Rule::latency, 12.0,
      "Its wow swings the sound's delay around the delay line's middle, which is its latency: over the sixth of a second "
      "measured, the record (or the tape) runs some samples early." },
    { "phaziteMotion", Rule::latency, 0.5, "The swept copies it adds, some milliseconds late, pull the peak a part of a sample.",
      "Motion (Chorus)" },
    { "phaziteMotion", Rule::latency, 0.5,
      "What comes out is the sound and a copy of it a few milliseconds later, as loud, which pulls the peak a part of a sample.",
      "Motion (Flanger)" },
    { "phaziteMotion", Rule::latency, 2.5,
      "Its all-pass stages turn the phase of what passes, which is its effect: the lows come out a sample or two late.",
      "Motion (Phaser)" },
    { "phaziteVocoder", Rule::latency, anything,
      "What comes out is its carrier, a buzz at first, shaped by what came in, not what came in." },
    { "phaziteSaturator", Rule::mix, -45.0,
      "It delays its copy for the mix itself, through the half-band filters its sound goes through (Processor::delaysDry()), so "
      "that the two meet in phase at any mix: 6.02 samples rather than the 6 it can report." },
    { "phaziteAge", Rule::mix, -5.0,
      "Its copy for the mix takes the sound's linear way at every mix, so that the two meet in phase: the oversampling's half-band "
      "filters and the delay line read between samples, and Bass Mono's all-pass while it is on (as at its defaults, 100 Hz), which "
      "turns the lows' phase: what the rule measures, 5.25 dB under what came in. At no mix, what came in through that all-pass "
      "as late as its latency, to 40 dB under, its level within a hundredth of a decibel to 5 kHz and 0.37 dB at 18 kHz (checked "
      "by tests of their own below)." },
    { "phaziteFM", Rule::offset, 0.1,
      "Frequency modulation puts sidebands at the carrier's frequency less whole multiples of the modulator's, which ratios a "
      "little apart from a whole multiple (Fine, Detune) put a few hertz from 0 Hz: heard as nothing, and no blocker takes them "
      "away without the lowest notes too. Its DC blocker (12 dB at 5 Hz) takes away what lies at 0 Hz and below a "
      "few hertz, a new synth's electric piano's included (0.0085 now, 0.08 with the 6 dB blocker before)." },

};

// Parameters left at their defaults while an instrument's settings are drawn for the offset rule, each of which puts an
// offset under the notes when it is turned: not as it should be (an instrument's output should have none; the suite's
// report describes it). The rule is measured with each of them drawn too, and the test fails once that keeps the rule.
// None now: the synth's Osc 2 Sync and the wavetable's Sync, Bend and Pulse Width warps, which were here, take away what
// they leave off centre as they play (Synth.cpp's getSyncedMean(), Wavetable::getArea() and getBentMean()).
struct LeftAtDefault
{
    std::string_view typeID, parameterID, why;
};

constexpr std::array<LeftAtDefault, 0> leftAtDefault {};

// Settings drawn on purpose for the offset rule, over drawn ones, where twelve draws seldom land: what is known to put an
// offset under the notes, which an instrument takes away (an oscillator ring-modulated or FM-ed by another at its pitch or
// an octave up, a synced wave driven hard).
struct OnPurpose
{
    std::string_view typeID;
    std::initializer_list<std::pair<std::string_view, float>> values;
};

const OnPurpose onPurpose[] = {
    { "phaziteWavetable",
      { { "osc1Warp", 7.0f },
        { "osc1WarpAmount", 1.0f },
        { "osc2On", 1.0f },
        { "osc1Pitch", 0.0f },
        { "osc2Pitch", 0.0f },
        { "osc1Fine", 0.0f },
        { "osc2Fine", 0.0f } } },
    { "phaziteWavetable",
      { { "osc1Warp", 6.0f },
        { "osc1WarpAmount", 0.6f },
        { "osc2On", 1.0f },
        { "osc1Pitch", 0.0f },
        { "osc2Pitch", 12.0f },
        { "osc1Fine", 0.0f },
        { "osc2Fine", 0.0f } } },
    { "phaziteWavetable",
      { { "osc2Warp", 7.0f },
        { "osc2WarpAmount", 0.8f },
        { "osc2On", 1.0f },
        { "osc1Pitch", 0.0f },
        { "osc2Pitch", 12.0f },
        { "osc1Fine", 0.0f },
        { "osc2Fine", 0.0f } } },
    { "phaziteSynth", { { "osc2Sync", 1.0f }, { "osc2Pitch", 5.0f }, { "drive", 18.0f }, { "filterOn", 1.0f } } },
};

// Devices measured once in each of their modes, each under a name of its own, with its mode's choice as if it were the
// default: Motion, whose modes are three effects that were devices of their own; Age in each medium; and the Delay with its
// modulation off and on, whose phase follows the song's place.
struct Variant
{
    std::string_view typeID, name, parameterID;
    float value;
};

constexpr Variant variants[] = {
    { "phaziteAge", "Age (Record)", "medium", 0.0f },          { "phaziteAge", "Age (Tape)", "medium", 1.0f },
    { "phaziteMotion", "Motion (Chorus)", "mode", 0.0f },      { "phaziteMotion", "Motion (Flanger)", "mode", 1.0f },
    { "phaziteMotion", "Motion (Phaser)", "mode", 2.0f },      { "phaziteDelay", "Delay", "modDepth", 0.0f },
    { "phaziteDelay", "Delay (Modulated)", "modDepth", 0.5f },
};

const Exception* findException (const devices::DeviceInfo& info, Rule rule)
{
    for (const auto& exception : exceptions)
        if (exception.typeID == info.typeID && exception.rule == rule && (exception.variant.empty() || exception.variant == info.name))
            return &exception;

    return nullptr;
}

juce::String toString (std::string_view text)
{
    return juce::String::fromUTF8 (text.data(), (int) text.size());
}

// What a device did against a rule: what the rule measures, at its worst, and what it did that no number says.
class Findings
{
public:
    void measure (double value, const juce::String& where)
    {
        if (value > worst)
        {
            worst = value;
            worstWhere = where;
        }
    }

    void fail (const juce::String& what)
    {
        if (++failures <= shown)
            text << (failures > 1 ? "; " : "") << what;
    }

    bool hasFailed() const noexcept { return failures > 0; }
    double getWorst() const noexcept { return worst; }
    const juce::String& getWorstWhere() const noexcept { return worstWhere; }
    juce::String getFailures() const { return failures > shown ? text + " (and " + juce::String (failures - shown) + " more)" : text; }

private:
    static constexpr int shown = 4;
    double worst = -std::numeric_limits<double>::infinity();
    juce::String worstWhere, text;
    int failures = 0;
};

// Whether what each rule measures for each device is printed: the environment variable PHAZITE_DEVICE_REPORT says.
bool isReporting()
{
    static const auto reporting = juce::SystemStats::getEnvironmentVariable ("PHAZITE_DEVICE_REPORT", {}).isNotEmpty();
    return reporting;
}

// Whether a device kept a rule: an exception is expected not to, and to stay within what it is allowed.
void judge (const devices::DeviceInfo& info, Rule rule, const Findings& findings)
{
    const auto& ruleInfo = getRuleInfo (rule);
    const auto name = toString (info.name), ruleName = juce::String (ruleInfo.name);
    const auto measured = ruleInfo.measure != nullptr && std::isfinite (findings.getWorst())
                              ? juce::String (findings.getWorst(), 3) + " " + ruleInfo.measure + findings.getWorstWhere()
                              : juce::String();
    const auto keeps = ! findings.hasFailed() && findings.getWorst() <= ruleInfo.tolerance;

    if (isReporting())
        std::printf ("%-18s %-17s %s %s\n", ruleInfo.name, name.toRawUTF8(), keeps ? "kept" : "BROKEN",
                     (measured + " " + findings.getFailures()).toRawUTF8());

    if (const auto* exception = findException (info, rule))
    {
        CHECK_MESSAGE (! keeps, (name + " keeps the rule \"" + ruleName + "\" now: take its exception out of the list"));
        CHECK_MESSAGE (findings.getWorst() <= exception->allowance,
                       (name + " is further off the rule \"" + ruleName + "\" than its exception allows ("
                        + juce::String (exception->allowance) + "): " + measured));

        if (exception->allowance < anything) // what no number says isn't allowed by a number
            CHECK_MESSAGE (! findings.hasFailed(), (name + " breaks the rule \"" + ruleName + "\": " + findings.getFailures()));

        return;
    }

    CHECK_MESSAGE (keeps, (name + " breaks the rule \"" + ruleName + "\" (tolerance " + juce::String (ruleInfo.tolerance) + "): " + measured
                           + " " + findings.getFailures()));
}

juce::String at (double rate)
{
    return " at " + juce::String (rate, 0) + " Hz";
}

juce::String at (double rate, int blockSize)
{
    return at (rate) + " in blocks of " + juce::String (blockSize);
}

bool isEffect (const devices::DeviceInfo& info)
{
    return info.kind == devices::DeviceKind::effect;
}

// An effect's sound, with its sidechain's if it has one.
int getNumChannels (const devices::DeviceInfo& info)
{
    return info.hasSidechain ? 4 : 2;
}

//==============================================================================
// What a device is given to play: an effect, noise as loud as said, on its sidechain too if it has one; an instrument, four
// notes from a low C up, one after another, held for most of the stretch and let go before its end.
struct Material
{
    Sound sound;
    std::vector<devices::NoteEvent> notes;
};

Material makeMaterial (const devices::DeviceInfo& info, int numSamples, std::uint32_t seed, float amplitude)
{
    if (isEffect (info))
        return { probe::makeNoise (numSamples, seed, amplitude, getNumChannels (info)), {} };

    Material material { probe::makeSilence (numSamples), {} };
    constexpr int notes[] = { 36, 43, 60, 72 };

    for (int i = 0; i < 4; ++i)
        material.notes.push_back (probe::noteOn (5 + i * (numSamples / 16), notes[i], 0.55f + 0.15f * (float) i));

    for (int i = 0; i < 4; ++i)
        material.notes.push_back (probe::noteOff (numSamples * 3 / 4 + i, notes[i]));

    return material;
}

Sound play (probe::Device& device, const Material& material, int blockSize)
{
    return device.played (material.sound, blockSize, material.notes);
}

// Whether a sound is one a device may play: every sample a number, and none beyond any level sound has.
bool isSound (const Sound& sound)
{
    return probe::isFinite (sound) && probe::getPeak (sound) <= (double) devices::loudestSound;
}

// Whether a parameter is a knob, which glides and automation moves, rather than a choice or a switch.
bool isContinuous (const devices::ParameterInfo& parameter)
{
    return parameter.unit != devices::Unit::choice && parameter.unit != devices::Unit::toggle;
}

//==============================================================================
// Numbers.

// A block of what isn't sound, on every channel: what isn't a number, the infinities, and values beyond any level sound has.
Sound makeWhatIsNotSound (int numSamples, int numChannels)
{
    constexpr float values[] = { std::numeric_limits<float>::quiet_NaN(),
                                 std::numeric_limits<float>::infinity(),
                                 -std::numeric_limits<float>::infinity(),
                                 1.0e30f,
                                 -1.0e30f,
                                 2.0f * devices::loudestSound };
    Sound sound (numChannels, numSamples);

    for (int channel = 0; channel < numChannels; ++channel)
        for (int i = 0; i < numSamples; ++i)
            sound.setSample (channel, i, values[(size_t) (i + channel) % std::size (values)]);

    return sound;
}

// Notes and the rest at their ends, for an instrument: the lowest and the highest note, as hard and as soft as notes come,
// the bend and controllers at their ends, the keys' pressure (a note's and the channel's, which Phazite's instruments take
// without using), a glide and a slide, every note let go, and a note that starts and ends on the stretch's last sample.
std::vector<devices::NoteEvent> makeNotesAtTheirEnds (int numSamples)
{
    using Type = devices::NoteEvent::Type;
    const auto eighth = numSamples / 8;
    return { probe::noteOn (0, 0, 1.0f),
             probe::noteOn (1, 127, 1.0f),
             probe::noteOn (2, 60, 1.0f / 127.0f),
             { Type::pitchBend, eighth, 0, 1.0f },
             { Type::controller, eighth + 1, 1, 1.0f }, // the modulation wheel
             { Type::pitchBend, 2 * eighth, 0, -1.0f },
             { Type::controller, 2 * eighth + 1, 64, 1.0f }, // the sustain pedal
             { Type::aftertouch, 2 * eighth + 2, 60, 1.0f }, // a note's pressure, and the channel's, at their ends
             { Type::channelPressure, 2 * eighth + 3, 0, 1.0f },
             { Type::aftertouch, 2 * eighth + 4, 99, 0.0f }, // a note that isn't held
             { Type::portamentoOn, 3 * eighth, 72, 0.8f },
             { Type::slide, 3 * eighth + 1, 84, 0.01f },
             probe::noteOff (4 * eighth, 0),
             probe::noteOff (4 * eighth, 127),
             { Type::controller, 5 * eighth, 64, 0.0f },
             { Type::channelPressure, 5 * eighth + 1, 0, 0.0f },
             { Type::allNotesOff, 6 * eighth, 0, 0.0f },
             probe::noteOn (6 * eighth + 1, 60, 1.0f),
             probe::noteOn (numSamples - 1, 64, 1.0f),
             probe::noteOff (numSamples - 1, 64) };
}

void checkNumbers (const devices::DeviceInfo& info, double rate, int blockSize, Findings& findings)
{
    const auto where = at (rate, blockSize);
    const auto length = blockSize == 1 ? 256 : std::max (512, blockSize); // a block is a call: fewer of the shortest
    const auto numChannels = getNumChannels (info);
    probe::Device device (info);
    device.prepare (rate, blockSize);

    const auto expectSound = [&] (const Sound& sound, const char* of)
    {
        if (! isSound (sound))
            findings.fail (juce::String ("what it plays of ") + of + " isn't sound" + where);
    };

    // A block of no samples first, and between the others.
    const auto loud = makeMaterial (info, length, 1, 1.0f);
    device.playNothing();
    expectSound (device.played (probe::makeSilence (length, numChannels), blockSize), "silence");
    expectSound (play (device, loud, blockSize), "the loudest sound");
    device.playNothing();

    if (isEffect (info))
    {
        // Its twin has heard the same, and hears silence in place of what isn't sound.
        probe::Device twin (info);
        twin.prepare (rate, blockSize);
        twin.playNothing();
        twin.played (probe::makeSilence (length, numChannels), blockSize);
        play (twin, loud, blockSize);
        twin.playNothing();

        const auto heard = device.played (makeWhatIsNotSound (length, numChannels), blockSize);
        const auto heardOfSilence = twin.played (probe::makeSilence (length, numChannels), blockSize);
        const auto next = makeMaterial (info, length, 2, 0.5f);
        const auto after = play (device, next, blockSize), afterSilence = play (twin, next, blockSize);
        expectSound (heard, "what isn't sound");
        expectSound (after, "the sound after what isn't");

        if (isSound (heard) && isSound (after)
            && (probe::getLargestDifference (heard, heardOfSilence) > 0.0 || probe::getLargestDifference (after, afterSilence) > 0.0))
            findings.fail ("what isn't sound isn't silence to it" + where);
    }
    else
    {
        expectSound (device.played (probe::makeSilence (length), blockSize, makeNotesAtTheirEnds (length)), "notes at their ends");
    }

    // On a mono track.
    auto mono = makeMaterial (info, length, 3, 1.0f);
    mono.sound = isEffect (info) ? probe::makeNoise (length, 3, 1.0f, 1) : probe::makeSilence (length, 1);
    expectSound (play (device, mono, blockSize), "a mono track's sound");
}

// Settings drawn at random, the same every time for a seed: each parameter anywhere along its knob's travel, or, around its
// defaults, no further from its default than a part of the travel, its choices and switches anywhere.
std::vector<float> drawSettings (const devices::DeviceInfo& info, devices::dsp::Random& random, float reach = 1.0f)
{
    std::vector<float> values;
    values.reserve (info.parameters.size());

    for (const auto& parameter : info.parameters)
    {
        const auto range = devices::getRange (parameter);
        const auto drawn = random.nextUnipolar();
        const auto place = isContinuous (parameter) && reach < 1.0f
                               ? std::clamp (range.convertTo0to1 (parameter.defaultValue) + reach * (2.0f * drawn - 1.0f), 0.0f, 1.0f)
                               : drawn;
        values.push_back (devices::constrain (parameter, range.convertFrom0to1 (place)));
    }

    return values;
}

void checkExtremes (const devices::DeviceInfo& info, double rate, int rateIndex, Findings& findings)
{
    // A block for each parameter to glide to its lowest, then one to its highest, the one before it gliding back meanwhile:
    // the loudest sound through all of it. A number is all that is asked here: an equalizer with every band at its highest
    // is louder than any sound, rightly.
    constexpr int blockSize = 64, most = 128;
    const auto material = makeMaterial (info, blockSize, 4, 1.0f);
    probe::Device device (info);
    device.prepare (rate, blockSize);
    const auto defaults = device.values;

    const auto expectNumbers = [&] (const juce::String& setting, int times)
    {
        for (int i = 0; i < times; ++i)
        {
            if (! probe::isFinite (play (device, material, blockSize)))
            {
                findings.fail ("what it plays with " + setting + " isn't a number" + at (rate));
                device.prepare (rate, blockSize); // anew, for the next
                return;
            }
        }
    };

    // Of a device with more parameters than `most` (the drum sampler's thousands), every so-many-th, others at each rate.
    const auto every = (info.parameters.size() + (size_t) most - 1) / (size_t) most;

    for (auto i = (size_t) rateIndex % every; i < info.parameters.size(); i += every)
    {
        for (const auto highest : { false, true })
        {
            device.values = defaults;
            device.values[i] = highest ? info.parameters[i].maximum : info.parameters[i].minimum;
            expectNumbers (toString (info.parameters[i].id) + (highest ? " at its highest" : " at its lowest"), 1);
        }
    }

    for (const auto highest : { false, true })
    {
        for (size_t i = 0; i < info.parameters.size(); ++i)
            device.values[i] = highest ? info.parameters[i].maximum : info.parameters[i].minimum;

        expectNumbers (highest ? "every parameter at its highest" : "every parameter at its lowest", 8);
    }

    devices::dsp::Random random (0x5e771235u + (std::uint32_t) rateIndex);

    for (int setting = 0; setting < 8; ++setting)
    {
        device.values = drawSettings (info, random);
        expectNumbers ("the settings drawn at random as number " + juce::String (setting + 1), 4);
    }
}

//==============================================================================
// Nothing, and the tail.
void checkNothing (const devices::DeviceInfo& info, double rate, Findings& findings)
{
    probe::Device device (info);
    device.prepare (rate, usualBlock);
    const auto peak = probe::getPeak (device.played (probe::makeSilence (8192, getNumChannels (info)), usualBlock));

    if (peak > 0.0)
        findings.fail ("with nothing to play it plays, as loud as " + juce::String (probe::inDecibels (peak), 1) + " dB" + at (rate));

    // After a reset() that follows sound (an instrument's notes, held), nothing either: the reset leaves nothing to ring.
    play (device, makeMaterial (info, 8192, 6, 1.0f), usualBlock);
    device.reset();
    const auto afterReset = probe::getPeak (device.played (probe::makeSilence (8192, getNumChannels (info)), usualBlock));

    if (afterReset > 0.0)
        findings.fail ("after a reset() that followed sound it plays, as loud as " + juce::String (probe::inDecibels (afterReset), 1)
                       + " dB" + at (rate));
}

// The same with the song stopped, as the engine plays it live (no render): from prepare(), and after a reset() that follows
// sound. Its runner lets it sleep only after a quarter of a second of silence, longer than what is played here, so that each
// block is the device's own (checked: no block slept).
void checkNothingStopped (const devices::DeviceInfo& info, double rate, Findings& findings)
{
    probe::Device device (info);
    device.isRendering = false;
    device.prepare (rate, usualBlock);
    constexpr int length = 8192;

    const auto check = [&] (const juce::String& when)
    {
        const auto before = device.getRunner().getBlocksWorked();
        auto silence = probe::makeSilence (length, getNumChannels (info));
        device.playStopped (silence, usualBlock);

        if (device.getRunner().getBlocksWorked() - before != (std::uint64_t) ((length + usualBlock - 1) / usualBlock))
            findings.fail ("its runner slept " + when + at (rate) + ", so what it plays wasn't heard");

        if (const auto peak = probe::getPeak (silence); peak > 0.0)
            findings.fail ("stopped, with nothing to play " + when + ", it plays, as loud as " + juce::String (probe::inDecibels (peak), 1)
                           + " dB" + at (rate));
    };

    check ("from prepare()");
    play (device, makeMaterial (info, length, 6, 1.0f), usualBlock);
    device.reset();
    check ("after a reset() that followed sound");
}

void checkTail (const devices::DeviceInfo& info, double rate, Findings& findings)
{
    // Silence is what the end of an export takes for it (model/playback/OfflineRender.cpp): 70 dB under full scale.
    constexpr float level = 0.5f, silence = 0.000316f;
    probe::Device device (info);
    device.prepare (rate, usualBlock);

    // A quarter of a second of sound (an instrument's notes, let go at its end), then nothing.
    const auto burst = (int) (0.25 * rate);
    auto material = makeMaterial (info, burst, 5, level);

    for (auto& note : material.notes)
        if (note.type == devices::NoteEvent::Type::noteOff)
            note.sample = burst - 1;

    play (device, material, usualBlock);

    // As long a silence as its runner waits for before it lets it sleep (engine/nodes/DeviceRunner.cpp), and the end of an
    // export before it stops: its tail, as it says it now.
    const auto tailSeconds = device.getTailSeconds();
    const auto hold = std::max ({ (int) std::ceil (tailSeconds * rate), (int) (0.25 * rate), 2 * device.getLatencySamples() });
    auto rest = probe::makeSilence (hold + (int) (0.5 * rate), getNumChannels (info));
    device.play (rest, usualBlock);
    int silentFor = 0;

    for (int i = 0; i < rest.getNumSamples(); ++i)
    {
        if (std::abs (rest.getSample (0, i)) <= silence && std::abs (rest.getSample (1, i)) <= silence)
        {
            ++silentFor;
            continue;
        }

        if (silentFor >= hold)
        {
            findings.fail ("silent for " + juce::String (silentFor / rate, 2) + " s, longer than its tail of "
                           + juce::String (tailSeconds, 2) + " s, it plays again" + at (rate));
            return;
        }

        silentFor = 0;
    }

    if (silentFor < (int) (0.25 * rate))
        findings.fail ("it still plays " + juce::String ((rest.getNumSamples() - silentFor) / rate, 2)
                       + " s after its sound has stopped, with a tail of " + juce::String (tailSeconds, 2) + " s" + at (rate));
}

//==============================================================================
// Latency.
void checkLatency (const devices::DeviceInfo& info, double rate, const std::vector<float>& values, Findings& findings)
{
    probe::Device device (info);
    device.values = values;
    device.prepare (rate, usualBlock);
    const auto latency = device.getLatencySamples();

    // Quiet noise, under any threshold, with nothing in the top octaves, where a filter's phase would move the peak.
    const auto in = probe::makeNoiseBelow (rate / 16.0, 8192 + latency, rate, 6, 0.01f);
    const auto out = device.played (in, usualBlock);

    for (int channel = 0; channel < 2; ++channel)
    {
        const auto delay = probe::findDelay (in, out, channel, latency + 256);
        const auto where = ": it says " + juce::String (latency) + " and is " + juce::String (delay.samples, 2) + " late" + at (rate);
        findings.measure (std::abs (delay.samples - latency), where);

        if (delay.likeness < 0.5)
            findings.fail ("what comes out isn't like what came in (" + juce::String (delay.likeness, 2) + ")" + where);
    }
}

// A device's settings that change its latency (Processor::getLatencySamplesFor()), each choice and switch in turn: with
// its defaults first.
std::vector<std::vector<float>> getSettingsWithLatenciesOfTheirOwn (const devices::DeviceInfo& info, double rate)
{
    probe::Device device (info);
    device.prepare (rate, usualBlock);
    std::vector<std::vector<float>> settings { device.values };
    const auto usual = device.getProcessor().getLatencySamplesFor (device.values);

    for (size_t i = 0; usual >= 0 && i < info.parameters.size(); ++i)
    {
        const auto& parameter = info.parameters[i];

        if (isContinuous (parameter))
            continue;

        for (auto value = parameter.minimum; value <= parameter.maximum; value += 1.0f)
        {
            auto values = device.values;
            values[i] = value;

            if (device.getProcessor().getLatencySamplesFor (values) != usual)
                settings.push_back (values);
        }
    }

    return settings;
}

//==============================================================================
// Mix.
void checkMix (const devices::DeviceInfo& info, double rate, Findings& findings)
{
    probe::Device device (info);
    device.set (devices::mixParameterID, 0.0f);
    device.prepare (rate, usualBlock);
    const auto latency = device.getLatencySamples();
    const auto in = probe::makeNoiseBelow (rate / 16.0, 8 * usualBlock, rate, 7, 0.5f);
    const auto out = device.played (in, usualBlock);

    if (! probe::isFinite (out))
    {
        findings.fail ("what comes out at no mix isn't a number" + at (rate));
        return;
    }

    // From the second block: over the first, the runner glides the mix from the device's default to where it is set (a known
    // bug of the runner's, which this suite's report describes).
    auto worst = 0.0;

    for (int channel = 0; channel < 2; ++channel)
        for (int i = std::max (latency, usualBlock); i < out.getNumSamples(); ++i)
            worst = std::max (worst, (double) std::abs (out.getSample (channel, i) - in.getSample (channel, i - latency)));

    findings.measure (probe::inDecibels (worst / probe::getPeak (in)), at (rate));
}

//==============================================================================
// Blocks.
void checkBlocks (const devices::DeviceInfo& info, double rate, bool automated, Findings& findings)
{
    constexpr int length = 4096, lengthInSingleSamples = 1024; // a block is a call: fewer of the shortest
    const auto material = makeMaterial (info, length, 8, 0.5f);

    const auto playIn = [&] (int blockSize, int numSamples)
    {
        probe::Device device (info);
        device.prepare (rate, blockSize);

        // Every continuous parameter (of a device with more than 64 parameters, every so-many-th) moves a tenth of its
        // knob's travel over the stretch, towards the end it is further from.
        if (automated)
        {
            const auto every = (info.parameters.size() + 63) / 64;

            for (size_t i = 0; i < info.parameters.size(); i += every)
            {
                const auto& parameter = info.parameters[i];

                if (! isContinuous (parameter))
                    continue;

                const auto range = devices::getRange (parameter);
                const auto place = range.convertTo0to1 (parameter.defaultValue);
                device.automate ((int) i, parameter.defaultValue, range.convertFrom0to1 (place < 0.5f ? place + 0.2f : place - 0.2f),
                                 2 * length);
            }
        }

        return device.played (probe::getPart (material.sound, 0, numSamples), blockSize, material.notes);
    };

    const auto reference = playIn (usualBlock, length);
    const auto peak = probe::getPeak (reference);

    if (! isSound (reference) || ! (peak > 0.0))
    {
        findings.fail ("it plays nothing to compare" + at (rate));
        return;
    }

    for (const auto blockSize : blockSizes)
    {
        // In single samples, automation would have nothing to cut: each call is a sample whatever moves.
        if (blockSize == usualBlock || (automated && blockSize == 1))
            continue;

        const auto numSamples = blockSize == 1 ? lengthInSingleSamples : length;
        const auto difference = probe::getLargestDifference (playIn (blockSize, numSamples), reference, 0, numSamples);
        findings.measure (probe::inDecibels (difference / peak), " in blocks of " + juce::String (blockSize) + at (rate));
    }
}

//==============================================================================
// Starting over.
void checkStartingOver (const devices::DeviceInfo& info, double rate, double otherRate, Findings& findings)
{
    constexpr int length = 4096;
    const auto material = makeMaterial (info, length, 9, 0.5f);
    probe::Device device (info);
    device.prepare (rate, usualBlock);
    const auto first = play (device, material, usualBlock);
    device.reset();
    const auto again = play (device, material, usualBlock);

    if (! isSound (again) || probe::getLargestDifference (first, again) > 0.0)
        findings.fail ("after reset() it plays another sound, " + juce::String (probe::getLargestDifference (first, again), 6) + " apart"
                       + at (rate));

    // For another rate, while it sounds: as one made for that rate.
    play (device, material, usualBlock);
    device.prepare (otherRate, usualBlock);
    probe::Device fresh (info);
    fresh.prepare (otherRate, usualBlock);
    const auto other = play (device, material, usualBlock), otherFresh = play (fresh, material, usualBlock);

    if (! isSound (other) || probe::getLargestDifference (other, otherFresh) > 0.0)
        findings.fail ("prepared again for " + juce::String (otherRate, 0) + " Hz after " + juce::String (rate, 0)
                       + " Hz, it plays another sound than one made for it, "
                       + juce::String (probe::getLargestDifference (other, otherFresh), 6) + " apart");
}

//==============================================================================
// Offset.
void checkOffset (const devices::DeviceInfo& info, double rate, const std::vector<float>& values, const juce::String& setting,
                  Findings& findings)
{
    // Four notes held for half a second: what lies under the second quarter of it, once what takes an offset away at a
    // note's start (a DC blocker of a few hertz) has settled.
    const auto quarter = (int) (0.25 * rate);
    const std::vector<devices::NoteEvent> notes { probe::noteOn (0, 48), probe::noteOn (0, 60), probe::noteOn (0, 67),
                                                  probe::noteOn (0, 72) };
    probe::Device device (info);
    device.values = values;
    device.prepare (rate, usualBlock);
    const auto sound = device.played (probe::makeSilence (2 * quarter), usualBlock, notes);

    for (int channel = 0; channel < 2; ++channel)
        if (probe::getRms (sound, channel, quarter) > 0.001) // 60 dB under full scale: something to lie under
            findings.measure (std::abs (probe::getOffsetShare (sound, channel, quarter)), " with " + setting + at (rate));
}

// The settings the offset rule draws for an instrument, around its defaults, the same every time: with the parameters that
// are left at their defaults for it there, but the one given (which is drawn with the others).
std::vector<std::vector<float>> drawSettingsForOffset (const devices::DeviceInfo& info, std::string_view alsoDrawn = {})
{
    constexpr int numSettings = 12;
    devices::dsp::Random random (0x0ff5e7u);
    std::vector<std::vector<float>> settings;

    for (int setting = 0; setting < numSettings; ++setting)
    {
        settings.push_back (drawSettings (info, random, 0.2f));

        for (const auto& left : leftAtDefault)
            if (const auto index = devices::findParameter (info, left.parameterID);
                left.typeID == info.typeID && left.parameterID != alsoDrawn && index >= 0)
                settings.back()[(size_t) index] = info.parameters[(size_t) index].defaultValue;
    }

    // Then those on purpose, each over the next drawn setting.
    for (size_t each = 0; each < std::size (onPurpose); ++each)
    {
        if (onPurpose[each].typeID != info.typeID)
            continue;

        auto values = drawSettings (info, random, 0.2f);

        for (const auto& [id, value] : onPurpose[each].values)
            if (const auto index = devices::findParameter (info, id); index >= 0)
                values[(size_t) index] = value;

        settings.push_back (std::move (values));
    }

    return settings;
}

// Every device, with its name where a check fails; a device with variants once for each, as a copy of its description
// named for it whose default is the variant's value.
template <typename Check>
void forEachDevice (Check&& check)
{
    const auto checkOne = [&check] (const devices::DeviceInfo& info)
    {
        CAPTURE (toString (info.name));
        const auto started = juce::Time::getMillisecondCounterHiRes();
        check (info);

        if (isReporting())
            std::printf ("%-36s took %.2f s\n", toString (info.name).toRawUTF8(),
                         (juce::Time::getMillisecondCounterHiRes() - started) * 0.001);
    };

    for (const auto* info : phazite::getOwnDevices())
    {
        auto hasVariants = false;

        for (const auto& variant : variants)
        {
            if (variant.typeID != info->typeID)
                continue;

            const auto index = devices::findParameter (*info, variant.parameterID);
            REQUIRE (index >= 0);
            std::vector<devices::ParameterInfo> parameters (info->parameters.begin(), info->parameters.end());
            parameters[(size_t) index].defaultValue = variant.value;
            auto described = *info;
            described.name = variant.name;
            described.parameters = parameters;
            checkOne (described);
            hasVariants = true;
        }

        if (! hasVariants)
            checkOne (*info);
    }
}
} // namespace

//==============================================================================
TEST_CASE ("Every device plays numbers: of silence, the loudest sound, no samples, a mono track and what isn't sound, in any blocks")
{
    forEachDevice (
        [] (const devices::DeviceInfo& info)
        {
            Findings findings;

            for (const auto rate : rates)
                for (const auto blockSize : blockSizes)
                    checkNumbers (info, rate, blockSize, findings);

            judge (info, Rule::numbers, findings);
        });
}

TEST_CASE ("Every device plays numbers with each parameter at its lowest and highest, with all of them there, and at random settings")
{
    forEachDevice (
        [] (const devices::DeviceInfo& info)
        {
            Findings findings;

            for (size_t i = 0; i < std::size (rates); ++i)
                checkExtremes (info, rates[i], (int) i, findings);

            judge (info, Rule::numbers, findings);
        });
}

TEST_CASE ("A device with nothing to play plays nothing")
{
    forEachDevice (
        [] (const devices::DeviceInfo& info)
        {
            Findings findings;

            for (const auto rate : rates)
                checkNothing (info, rate, findings);

            judge (info, Rule::nothing, findings);
        });
}

TEST_CASE ("A device with nothing to play plays nothing while the song is stopped, from prepare() and after a reset()")
{
    forEachDevice (
        [] (const devices::DeviceInfo& info)
        {
            Findings findings;

            for (const auto rate : rates)
                checkNothingStopped (info, rate, findings);

            judge (info, Rule::nothingStopped, findings);
        });
}

TEST_CASE ("A device that has been silent for as long as the tail it reports stays silent")
{
    forEachDevice (
        [] (const devices::DeviceInfo& info)
        {
            Findings findings;

            for (const auto rate : rates)
                checkTail (info, rate, findings);

            judge (info, Rule::tail, findings);
        });
}

TEST_CASE ("An effect's sound comes out as late as the latency it reports")
{
    forEachDevice (
        [] (const devices::DeviceInfo& info)
        {
            if (! isEffect (info))
                return;

            Findings findings;

            for (const auto rate : rates)
                for (const auto& values : getSettingsWithLatenciesOfTheirOwn (info, rate))
                    checkLatency (info, rate, values, findings);

            judge (info, Rule::latency, findings);
        });
}

TEST_CASE ("An effect at no mix passes what comes in, as late as its latency")
{
    forEachDevice (
        [] (const devices::DeviceInfo& info)
        {
            if (! isEffect (info) || devices::findParameter (info, devices::mixParameterID) < 0)
                return;

            Findings findings;

            for (const auto rate : rates)
                checkMix (info, rate, findings);

            judge (info, Rule::mix, findings);
        });
}

TEST_CASE ("What a device plays doesn't depend on how its samples are cut into blocks")
{
    forEachDevice (
        [] (const devices::DeviceInfo& info)
        {
            Findings findings;

            for (const auto rate : rates)
                checkBlocks (info, rate, false, findings);

            judge (info, Rule::blocks, findings);
        });
}

TEST_CASE ("What a device plays with its parameters automated doesn't depend on how blocks cut the song's control points")
{
    forEachDevice (
        [] (const devices::DeviceInfo& info)
        {
            Findings findings;

            for (const auto rate : rates)
                checkBlocks (info, rate, true, findings);

            judge (info, Rule::automatedBlocks, findings);
        });
}

TEST_CASE ("A device plays the same again after reset(), and prepared again for another rate as one made for it")
{
    forEachDevice (
        [] (const devices::DeviceInfo& info)
        {
            Findings findings;

            for (size_t i = 0; i < std::size (rates); ++i)
                checkStartingOver (info, rates[i], rates[(i + 1) % std::size (rates)], findings);

            judge (info, Rule::startingOver, findings);
        });
}

TEST_CASE ("Nothing constant lies under an instrument's notes, at its defaults and at settings around them")
{
    forEachDevice (
        [] (const devices::DeviceInfo& info)
        {
            if (isEffect (info))
                return;

            Findings findings;
            const auto defaults = probe::getDefaultValues (info);

            for (const auto rate : rates)
                checkOffset (info, rate, defaults, "its defaults", findings);

            CHECK_MESSAGE (std::isfinite (findings.getWorst()), "an instrument at its defaults plays notes to measure");
            int number = 0;

            for (const auto& values : drawSettingsForOffset (info))
                checkOffset (info, 48000.0, values, "the settings drawn at random as number " + juce::String (++number), findings);

            judge (info, Rule::offset, findings);
        });
}

TEST_CASE ("Age at no mix keeps the level of what comes in at every frequency, with Bass Mono on and off")
{
    // What the mix rule still holds it to (its exception): its copy for the mix, which takes the sound's linear way, turns the
    // phase of what came in and keeps its level: flat to a hundredth of a decibel to 5 kHz, and at the top within what its delay
    // line, read between samples as the sound's is, takes off: 0.04 dB at 10 kHz, 0.19 at 15 and 0.37 at 18 kHz at 44.1 kHz,
    // 0.03, 0.13 and 0.27 at 48 kHz. The half-band filters cost nothing in level, to a millionth of a decibel.
    const auto* info = phazite::findOwnDevice ("phaziteAge");
    REQUIRE (info != nullptr);
    const double frequencies[] = { 30.0, 100.0, 300.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0 };

    for (const auto bassMono : { 100.0f, 20.0f })
    {
        for (const auto rate : { 44100.0, 48000.0 })
        {
            const auto atNoMix = [info, rate, bassMono] (const Sound& in)
            {
                probe::Device device (*info);
                device.set (devices::mixParameterID, 0.0f);
                device.set ("bassMono", bassMono);
                device.prepare (rate, usualBlock);
                return device.played (in, usualBlock);
            };

            const auto response = probe::getResponse (atNoMix, frequencies, rate, 0.25f, 4800, 9600);

            for (size_t i = 0; i < std::size (frequencies); ++i)
            {
                INFO ("Bass Mono at " << bassMono << " Hz, " << frequencies[i] << " Hz at " << rate << " Hz: " << response[i] << " dB");
                CHECK (std::abs (response[i]) < (frequencies[i] <= 5000.0 ? 0.01 : frequencies[i] <= 10000.0 ? 0.05 : 0.4));
            }
        }
    }
}

TEST_CASE ("Finding 5: Age at no mix with Bass Mono on plays what came in through Bass Mono's all-pass, as late as its latency")
{
    // Its mix rule's exception allowed anything. What the copy for the mix does at no mix is what it says: what came in
    // through the all-pass that Bass Mono turns the lows' phase with, as late as the latency, but for the half-band filters'
    // and the delay line's few hundredths of a decibel at the top. Measured as the mix rule measures, on noise below a
    // sixteenth of the rate, against the all-pass played here.
    const auto* info = phazite::findOwnDevice ("phaziteAge");
    REQUIRE (info != nullptr);

    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        probe::Device device (*info);
        device.set (devices::mixParameterID, 0.0f);
        device.set ("bassMono", 100.0f);
        device.prepare (rate, usualBlock);
        const auto latency = device.getLatencySamples();
        const auto in = probe::makeNoiseBelow (rate / 16.0, 16 * usualBlock, rate, 7, 0.5f);
        const auto out = device.played (in, usualBlock);
        auto worst = 0.0;

        for (int channel = 0; channel < 2; ++channel)
        {
            devices::dsp::Crossover bassMono;
            bassMono.setFrequency (100.0, rate);
            std::vector<float> turned ((size_t) in.getNumSamples());

            for (int i = 0; i < in.getNumSamples(); ++i)
                turned[(size_t) i] = bassMono.processAllPass (in.getSample (channel, i));

            // From the eighth block: by then Bass Mono's split has glided in whole.
            for (int i = 8 * usualBlock; i < out.getNumSamples(); ++i)
                worst = std::max (worst, (double) std::abs (out.getSample (channel, i) - turned[(size_t) (i - latency)]));
        }

        const auto under = probe::inDecibels (worst / probe::getPeak (in));
        INFO ("at " << rate << " Hz it differs from the all-pass by " << under << " dB under what came in");
        CHECK (under < -40.0);
    }
}

TEST_CASE ("The exceptions name devices there are, each once for a rule")
{
    for (const auto& exception : exceptions)
    {
        CAPTURE (toString (exception.typeID));
        CHECK (phazite::findOwnDevice (exception.typeID) != nullptr);
        CHECK (! exception.why.empty());
        CHECK (std::count_if (std::begin (exceptions), std::end (exceptions),
                              [&] (const Exception& other)
                              {
                                  return other.typeID == exception.typeID && other.rule == exception.rule
                                         && (other.variant.empty() || exception.variant.empty() || other.variant == exception.variant);
                              })
               == 1);

        // A variant's exception names one of its device's variants, which are measured for it.
        if (! exception.variant.empty())
            CHECK (std::any_of (std::begin (variants), std::end (variants), [&] (const Variant& variant)
                                { return variant.typeID == exception.typeID && variant.name == exception.variant; }));
    }
}

TEST_CASE ("A parameter the offset rule leaves at its default still puts an offset under an instrument's notes")
{
    for (const auto& left : leftAtDefault)
    {
        CAPTURE (toString (left.typeID));
        CAPTURE (toString (left.parameterID));
        const auto* info = phazite::findOwnDevice (left.typeID);
        REQUIRE (info != nullptr);
        REQUIRE (devices::findParameter (*info, left.parameterID) >= 0);
        CHECK (! left.why.empty());

        // Drawn with the others, until a setting breaks the rule: once none does, it is left at its default for nothing.
        Findings findings;

        for (const auto& values : drawSettingsForOffset (*info, left.parameterID))
        {
            checkOffset (*info, 48000.0, values, "it drawn", findings);

            if (findings.getWorst() > getRuleInfo (Rule::offset).tolerance)
                break;
        }

        CHECK_MESSAGE (findings.getWorst() > getRuleInfo (Rule::offset).tolerance,
                       (toString (left.parameterID) + " puts no offset under the notes now: take it out of the list"));
    }
}

//==============================================================================
// The bench itself (DspProbe.h): each measurement, of a sound made to have what it measures.
TEST_CASE ("The probe reads a tone's level, a response, an offset and what isn't a number")
{
    constexpr double rate = 48000.0;
    auto tone = probe::makeSine (1000.0, 4800, rate, 0.25f);
    CHECK (probe::getToneLevel (tone, 0, 1000.0, rate) == doctest::Approx (0.25).epsilon (0.0001));
    CHECK (probe::getToneLevel (tone, 1, 1000.0, rate, 1203, 997) == doctest::Approx (0.25).epsilon (0.0001)); // no whole cycles
    CHECK (probe::getToneLevel (tone, 0, 3000.0, rate) < 0.0001);
    CHECK (probe::getPeak (tone) == doctest::Approx (0.25).epsilon (0.001));
    CHECK (probe::getRms (tone, 0) == doctest::Approx (0.25 / std::sqrt (2.0)).epsilon (0.001));
    CHECK (std::abs (probe::getMean (tone, 0)) < 1.0e-6);
    CHECK (std::abs (probe::getOffsetShare (tone, 0)) < 1.0e-4);
    CHECK (probe::inDecibels (0.5) == doctest::Approx (-6.0206).epsilon (0.0001));

    // Half as loud at each frequency: 6 dB down.
    const double frequencies[] = { 100.0, 1000.0, 10000.0 };
    const auto halve = [] (const Sound& in)
    {
        auto out = in;
        out.applyGain (0.5f);
        return out;
    };

    for (const auto decibels : probe::getResponse (halve, frequencies, rate, 0.1f, 480, 4800))
        CHECK (decibels == doctest::Approx (-6.0206).epsilon (0.001));

    // An offset of a fifth of the tone's amplitude: its share of the level of the two together.
    for (int i = 0; i < tone.getNumSamples(); ++i)
        tone.setSample (0, i, tone.getSample (0, i) + 0.05f);

    CHECK (probe::getMean (tone, 0) == doctest::Approx (0.05).epsilon (0.001));
    CHECK (probe::getOffsetShare (tone, 0, 100, 4096) == doctest::Approx (0.05 / std::sqrt (0.05 * 0.05 + 0.125 * 0.25)).epsilon (0.01));
    CHECK (std::abs (probe::getOffsetShare (tone, 1, 100, 4096)) < 1.0e-4);

    CHECK (probe::isFinite (tone));
    tone.setSample (1, 77, std::numeric_limits<float>::infinity());
    CHECK (probe::findNotFinite (tone) == 77);
    tone.setSample (0, 12, std::numeric_limits<float>::quiet_NaN());
    CHECK (probe::findNotFinite (tone) == 12);
    CHECK (probe::getLargestDifference (probe::makeImpulse (16, 3, 0.5f), probe::makeSilence (16)) == doctest::Approx (0.5));
}

TEST_CASE ("The probe finds a delay to a part of a sample, and aliases under a tone")
{
    constexpr double rate = 48000.0;

    // Noise 37 samples late, upside down and half as loud; and half a sample later still (the mean of two neighbours).
    const auto noise = probe::makeNoiseBelow (rate / 16.0, 4096, rate, 21, 0.5f);
    auto late = probe::makeSilence (4096), later = probe::makeSilence (4096);

    for (int channel = 0; channel < 2; ++channel)
    {
        for (int i = 38; i < 4096; ++i)
        {
            late.setSample (channel, i, -0.5f * noise.getSample (channel, i - 37));
            later.setSample (channel, i, 0.5f * (noise.getSample (channel, i - 37) + noise.getSample (channel, i - 38)));
        }
    }

    const auto found = probe::findDelay (noise, late, 0, 100);
    CHECK (found.samples == doctest::Approx (37.0).epsilon (0.0005));
    CHECK (found.likeness == doctest::Approx (1.0).epsilon (0.001));
    CHECK (probe::findDelay (noise, later, 1, 100).samples == doctest::Approx (37.5).epsilon (0.0005));
    CHECK (std::abs (probe::findDelay (noise, noise, 0, 100).samples) < 0.01);

    // Noise of its own on each side, and the same again for a seed.
    const auto white = probe::makeNoise (4096, 5), again = probe::makeNoise (4096, 5);
    CHECK (probe::getLargestDifference (white, again) <= 0.0);
    CHECK (probe::findDelay (white, probe::getPart (probe::join (probe::makeSilence (9), white), 0, 4096), 0, 20).samples
           == doctest::Approx (9.0).epsilon (0.0005));
    Sound sides (2, 4096);
    sides.copyFrom (0, 0, white, 1, 0, 4096);
    sides.copyFrom (1, 0, white, 0, 0, 4096);
    CHECK (probe::findDelay (white, sides, 0, 20).likeness < 0.1);

    // A clean tone, and one with another, 40 dB under it, that is no harmonic of it.
    const auto hertz = probe::onBin (1000.0, rate);
    auto tone = probe::makeSine (hertz, probe::aliasWindow, rate, 0.5f);
    CHECK (probe::getAliasesUnder (tone, 0, 0, hertz, rate) > 100.0);
    const auto alias = probe::makeSine (probe::onBin (2500.0, rate), probe::aliasWindow, rate, 0.005f);
    tone.addFrom (0, 0, alias, 0, 0, probe::aliasWindow);
    CHECK (probe::getAliasesUnder (tone, 0, 0, hertz, rate) == doctest::Approx (40.0).epsilon (0.01));
}

TEST_CASE ("The probe plays a device as the engine does, with its song's automation on the control points, and alone")
{
    // The saturator, driven: alone, without its runner, it plays as on its runner at all of its mix.
    const auto* info = phazite::findOwnDevice ("phaziteSaturator");
    REQUIRE (info != nullptr);
    probe::Device device (*info);
    CHECK (device.has (devices::mixParameterID));
    CHECK (! device.has ("no such parameter"));
    device.set ("drive", 12.0f);
    device.prepare (48000.0, 256);
    const auto noise = probe::makeNoise (2048, 31, 0.5f);
    const auto wet = device.played (noise, 256);
    CHECK (device.getSongSample() == 2048);

    auto alone = noise;
    const auto processor = phazite::createOwnDevice (info->typeID);
    processor->prepare (48000.0, 256);
    probe::playAlone (*processor, device.values, alone, 256);
    CHECK (probe::getLargestDifference (wet, alone) <= 0.0);

    // Automation: the drive along a line, and the same in any blocks that are whole control steps.
    const auto drive = devices::findParameter (*info, "drive");
    REQUIRE (drive >= 0);
    Sound first;

    for (const auto blockSize : { 256, 32 })
    {
        probe::Device automated (*info);
        automated.prepare (48000.0, 256);
        automated.automate (drive, 0.0f, 24.0f, 2048);
        const auto out = automated.played (noise, blockSize);
        CHECK (automated.getRunner().getValues()[(size_t) drive] > 20.0f); // where its automation has taken it

        if (blockSize == 256)
            first = out;
        else
            CHECK (probe::getLargestDifference (out, first) <= 0.0);
    }
}
