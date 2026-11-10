// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FmSynth.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace phazite::devices
{
namespace
{
    using Field = FmSynth::Field;
    constexpr int numOperators = FmSynth::numOperators;

    // The algorithms by what they look like: what each is good for is the editor's to say.
    constexpr std::string_view algorithmNames[] = { "Stack",     "Two Into Stack", "Chain And One",  "Three Into One",
                                                    "Two Pairs", "Stack And One",  "One Into Three", "Four Carriers" };
    constexpr std::string_view waves[] = { "Sine", "Triangle", "Saw", "Square" };
    constexpr std::string_view phases[] = { "Reset", "Free" };
    constexpr std::string_view lfoShapes[] = { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random" };
    constexpr std::string_view lfoTargets[] = { "Pitch", "Timbre", "Level", "Cutoff" };
    constexpr std::string_view voiceModes[] = { "Poly", "Mono" };

    static_assert (std::size (algorithmNames) == (size_t) FmSynth::numAlgorithms);

    // clang-format off
    // Which operators modulate each (bit j: operator j + 1), and which are heard: Yamaha's 4-operator algorithms, as its DX21,
    // DX100 and TX81Z have them, but for two that differ from these only in where the feedback is or are rarely used.
    constexpr FmSynth::Algorithm algorithms[] = {
        { { 0b0010, 0b0100, 0b1000, 0 }, 0b0001 }, // 4 > 3 > 2 > 1: a stack, growling basses, leads
        { { 0b0010, 0b1100, 0,      0 }, 0b0001 }, // 4 + 3 > 2 > 1: two into a stack, the TX81Z's Lately Bass
        { { 0b0110, 0,      0b1000, 0 }, 0b0001 }, // 4 > 3 + 2 > 1: a chain and a single, mallets and plucks
        { { 0b1110, 0,      0,      0 }, 0b0001 }, // 4 + 3 + 2 > 1: three into one, rich basses
        { { 0b0010, 0,      0b1000, 0 }, 0b0101 }, // 4 > 3 | 2 > 1: two pairs, the electric piano's, bells, brass
        { { 0,      0b0100, 0b1000, 0 }, 0b0011 }, // 4 > 3 > 2 | 1: a stack beside one more, a growl over a clean sub
        { { 0b1000, 0b1000, 0b1000, 0 }, 0b0111 }, // 4 > 3, 2, 1: one modulator under three carriers, brass, strings
        { { 0,      0,      0,      0 }, 0b1111 }, // 4 | 3 | 2 | 1: four side by side, organs, additive
    };

    // Each operator's parameters' IDs, saved in projects, and names.
    constexpr std::string_view operatorIDs[numOperators][FmSynth::fieldsPerOperator] = {
        { "op1On", "op1Wave", "op1Feedback", "op1Ratio", "op1Fine", "op1Detune", "op1Fixed", "op1Frequency", "op1Level", "op1Velocity",
          "op1KeyScale", "op1Attack", "op1Decay", "op1Sustain", "op1Release", "op1AttackCurve", "op1DecayCurve", "op1ReleaseCurve" },
        { "op2On", "op2Wave", "op2Feedback", "op2Ratio", "op2Fine", "op2Detune", "op2Fixed", "op2Frequency", "op2Level", "op2Velocity",
          "op2KeyScale", "op2Attack", "op2Decay", "op2Sustain", "op2Release", "op2AttackCurve", "op2DecayCurve", "op2ReleaseCurve" },
        { "op3On", "op3Wave", "op3Feedback", "op3Ratio", "op3Fine", "op3Detune", "op3Fixed", "op3Frequency", "op3Level", "op3Velocity",
          "op3KeyScale", "op3Attack", "op3Decay", "op3Sustain", "op3Release", "op3AttackCurve", "op3DecayCurve", "op3ReleaseCurve" },
        { "op4On", "op4Wave", "op4Feedback", "op4Ratio", "op4Fine", "op4Detune", "op4Fixed", "op4Frequency", "op4Level", "op4Velocity",
          "op4KeyScale", "op4Attack", "op4Decay", "op4Sustain", "op4Release", "op4AttackCurve", "op4DecayCurve", "op4ReleaseCurve" },
    };

    constexpr std::string_view operatorNames[numOperators][FmSynth::fieldsPerOperator] = {
        { "Op 1 On", "Op 1 Wave", "Op 1 Feedback", "Op 1 Ratio", "Op 1 Fine", "Op 1 Detune", "Op 1 Fixed", "Op 1 Frequency",
          "Op 1 Level", "Op 1 Velocity", "Op 1 Key Scale", "Op 1 Attack", "Op 1 Decay", "Op 1 Sustain", "Op 1 Release",
          "Op 1 Attack Curve", "Op 1 Decay Curve", "Op 1 Release Curve" },
        { "Op 2 On", "Op 2 Wave", "Op 2 Feedback", "Op 2 Ratio", "Op 2 Fine", "Op 2 Detune", "Op 2 Fixed", "Op 2 Frequency",
          "Op 2 Level", "Op 2 Velocity", "Op 2 Key Scale", "Op 2 Attack", "Op 2 Decay", "Op 2 Sustain", "Op 2 Release",
          "Op 2 Attack Curve", "Op 2 Decay Curve", "Op 2 Release Curve" },
        { "Op 3 On", "Op 3 Wave", "Op 3 Feedback", "Op 3 Ratio", "Op 3 Fine", "Op 3 Detune", "Op 3 Fixed", "Op 3 Frequency",
          "Op 3 Level", "Op 3 Velocity", "Op 3 Key Scale", "Op 3 Attack", "Op 3 Decay", "Op 3 Sustain", "Op 3 Release",
          "Op 3 Attack Curve", "Op 3 Decay Curve", "Op 3 Release Curve" },
        { "Op 4 On", "Op 4 Wave", "Op 4 Feedback", "Op 4 Ratio", "Op 4 Fine", "Op 4 Detune", "Op 4 Fixed", "Op 4 Frequency",
          "Op 4 Level", "Op 4 Velocity", "Op 4 Key Scale", "Op 4 Attack", "Op 4 Decay", "Op 4 Sustain", "Op 4 Release",
          "Op 4 Attack Curve", "Op 4 Decay Curve", "Op 4 Release Curve" },
    };

    // A new synth's operators (finished rather than blank): a soft electric piano in two pairs, as the DX7's E.PIANO 1 is
    // built. Operators 1 and 3 are its carriers, a few cents apart, so that they beat as a tine piano's tines do; operator 2,
    // at 14 times the pitch, strikes the tine's bright, bell-like attack and fades within a third of a second; operator 4, at
    // the pitch and fed back a little, is its body. Velocity mostly changes how bright it is, as a played piano does; nothing
    // sustains.
    struct OperatorDefaults
    {
        float ratio, detune, level, velocity, keyScale, feedback, attack, decay, sustain, release;
    };

    constexpr OperatorDefaults operatorDefaults[numOperators] = {
        { 1.0f,   4.0f, 1.0f,  0.25f, 0.0f,  0.0f, 1.0f, 3000.0f, 0.0f, 400.0f },
        { 14.0f,  0.0f, 0.3f,  0.9f,  0.25f, 0.0f, 0.5f, 350.0f,  0.0f, 300.0f },
        { 1.0f,  -4.0f, 1.0f,  0.25f, 0.0f,  0.0f, 1.0f, 2500.0f, 0.0f, 400.0f },
        { 1.0f,   0.0f, 0.45f, 0.7f,  0.0f,  0.1f, 1.0f, 1200.0f, 0.0f, 300.0f },
    };
    // clang-format on

    constexpr ParameterInfo makeTime (std::string_view id, std::string_view name, float minimum, float defaultValue)
    {
        return { .id = id,
                 .name = name,
                 .minimum = minimum,
                 .maximum = 10000.0f,
                 .defaultValue = defaultValue,
                 .unit = Unit::milliseconds,
                 .midpoint = 300.0f };
    }

    constexpr ParameterInfo makeChoice (std::string_view id, std::string_view name, std::span<const std::string_view> choices,
                                        float defaultValue)
    {
        return { .id = id,
                 .name = name,
                 .minimum = 0.0f,
                 .maximum = (float) choices.size() - 1.0f,
                 .defaultValue = defaultValue,
                 .unit = Unit::choice,
                 .choices = choices };
    }

    constexpr ParameterInfo makePercent (std::string_view id, std::string_view name, float defaultValue, float minimum = 0.0f,
                                         float maximum = 1.0f)
    {
        return { .id = id, .name = name, .minimum = minimum, .maximum = maximum, .defaultValue = defaultValue, .unit = Unit::percent };
    }

    constexpr ParameterInfo makeToggle (std::string_view id, std::string_view name, float defaultValue)
    {
        return { .id = id, .name = name, .maximum = 1.0f, .defaultValue = defaultValue, .unit = Unit::toggle };
    }

    constexpr ParameterInfo makeCurve (std::string_view id, std::string_view name, float defaultValue)
    {
        return { .id = id, .name = name, .minimum = -1.0f, .maximum = 1.0f, .defaultValue = defaultValue, .unit = Unit::percent };
    }

    constexpr auto makeParameters()
    {
        std::array<ParameterInfo, (size_t) FmSynth::outputLevel + 1> made {};
        made[FmSynth::algorithm] = makeChoice ("algorithm", "Algorithm", algorithmNames, 4.0f); // two pairs, the piano's
        made[FmSynth::timbre] = makePercent ("timbre", "Timbre", 1.0f, 0.0f, 2.0f);

        for (int op = 0; op < numOperators; ++op)
        {
            const auto& ids = operatorIDs[op];
            const auto& names = operatorNames[op];
            const auto& defaults = operatorDefaults[op];
            const auto at = [op] (Field field) { return (size_t) FmSynth::getParameterIndex (op, field); };
            made[at (Field::on)] = makeToggle (ids[Field::on], names[Field::on], 1.0f);
            made[at (Field::wave)] = makeChoice (ids[Field::wave], names[Field::wave], waves, 0.0f);
            made[at (Field::feedback)] = makePercent (ids[Field::feedback], names[Field::feedback], defaults.feedback, -1.0f, 1.0f);

            // Whole ratios and halves, the harmonics and the octave below, which keep a sound in tune; Fine between them.
            made[at (Field::ratio)] = { .id = ids[Field::ratio],
                                        .name = names[Field::ratio],
                                        .minimum = 0.5f,
                                        .maximum = 32.0f,
                                        .defaultValue = defaults.ratio,
                                        .unit = Unit::none,
                                        .midpoint = 4.0f,
                                        .step = 0.5f };
            made[at (Field::fine)] = { .id = ids[Field::fine],
                                       .name = names[Field::fine],
                                       .minimum = 0.0f,
                                       .maximum = 0.49f,
                                       .defaultValue = 0.0f,
                                       .unit = Unit::none,
                                       .step = 0.01f };
            made[at (Field::detune)] = { .id = ids[Field::detune],
                                         .name = names[Field::detune],
                                         .minimum = -50.0f,
                                         .maximum = 50.0f,
                                         .defaultValue = defaults.detune,
                                         .unit = Unit::cents };
            made[at (Field::fixed)] = makeToggle (ids[Field::fixed], names[Field::fixed], 0.0f);
            made[at (Field::frequency)] = { .id = ids[Field::frequency],
                                            .name = names[Field::frequency],
                                            .minimum = 1.0f,
                                            .maximum = 10000.0f,
                                            .defaultValue = 440.0f,
                                            .unit = Unit::hertz,
                                            .midpoint = 300.0f };
            made[at (Field::level)] = makePercent (ids[Field::level], names[Field::level], defaults.level);
            made[at (Field::velocity)] = makePercent (ids[Field::velocity], names[Field::velocity], defaults.velocity);
            made[at (Field::keyScale)] = makePercent (ids[Field::keyScale], names[Field::keyScale], defaults.keyScale, -1.0f, 1.0f);
            made[at (Field::attack)] = makeTime (ids[Field::attack], names[Field::attack], 0.5f, defaults.attack);
            made[at (Field::decay)] = makeTime (ids[Field::decay], names[Field::decay], 0.0f, defaults.decay);
            made[at (Field::sustain)] = makePercent (ids[Field::sustain], names[Field::sustain], defaults.sustain);
            made[at (Field::release)] = makeTime (ids[Field::release], names[Field::release], 0.5f, defaults.release);
            made[at (Field::attackCurve)] = makeCurve (ids[Field::attackCurve], names[Field::attackCurve], dsp::Adsr::analogAttackCurve);
            made[at (Field::decayCurve)] = makeCurve (ids[Field::decayCurve], names[Field::decayCurve], dsp::Adsr::analogSettleCurve);
            made[at (Field::releaseCurve)] = makeCurve (ids[Field::releaseCurve], names[Field::releaseCurve], dsp::Adsr::analogSettleCurve);
        }

        made[FmSynth::pitchAmount] = { .id = "pitchAmount",
                                       .name = "Pitch Amount",
                                       .minimum = -48.0f,
                                       .maximum = 48.0f,
                                       .defaultValue = 0.0f,
                                       .unit = Unit::semitones };
        made[FmSynth::pitchDecay] = { .id = "pitchDecay",
                                      .name = "Pitch Decay",
                                      .minimum = 1.0f,
                                      .maximum = 2000.0f,
                                      .defaultValue = 60.0f,
                                      .unit = Unit::milliseconds,
                                      .midpoint = 100.0f };
        made[FmSynth::keyTime] = makePercent ("keyTime", "Key Time", 0.25f);
        made[FmSynth::phase] = makeChoice ("phase", "Phase", phases, 0.0f);
        made[FmSynth::filterOn] = makeToggle ("filterOn", "Filter On", 0.0f);
        made[FmSynth::filterType] = makeChoice ("filterType", "Filter Type", dsp::filterModeNames, 0.0f);
        made[FmSynth::cutoff] = { .id = "cutoff",
                                  .name = "Cutoff",
                                  .minimum = 20.0f,
                                  .maximum = 20000.0f,
                                  .defaultValue = 5000.0f,
                                  .unit = Unit::hertz,
                                  .midpoint = 632.456f };
        made[FmSynth::resonance] = makePercent ("resonance", "Resonance", 0.1f);
        made[FmSynth::filterSlope] = makeChoice ("filterSlope", "Filter Slope", dsp::filterSlopeNames, 1.0f);
        made[FmSynth::keyTracking] = makePercent ("keyTracking", "Key Tracking", 0.5f);
        made[FmSynth::lfoOn] = makeToggle ("lfoOn", "LFO On", 0.0f);
        made[FmSynth::lfoShape] = makeChoice ("lfoShape", "LFO Shape", lfoShapes, 0.0f);
        made[FmSynth::lfoTarget] = makeChoice ("lfoTarget", "LFO Target", lfoTargets, 0.0f);
        made[FmSynth::lfoRate] = { .id = "lfoRate",
                                   .name = "LFO Rate",
                                   .minimum = 0.05f,
                                   .maximum = 20.0f,
                                   .defaultValue = 5.0f,
                                   .unit = Unit::hertz,
                                   .midpoint = 1.0f };
        made[FmSynth::lfoDepth] = makePercent ("lfoDepth", "LFO Depth", 0.25f);
        made[FmSynth::voiceMode] = makeChoice ("voiceMode", "Voices", voiceModes, 0.0f);
        made[FmSynth::glide] = { .id = "glide",
                                 .name = "Glide",
                                 .minimum = 0.0f,
                                 .maximum = 2000.0f,
                                 .defaultValue = 0.0f,
                                 .unit = Unit::milliseconds,
                                 .midpoint = 200.0f };
        made[FmSynth::bendRange] = { .id = "bendRange",
                                     .name = "Bend Range",
                                     .maximum = 24.0f,
                                     .defaultValue = 2.0f,
                                     .unit = Unit::semitones };
        made[FmSynth::spread] = { .id = "spread", .name = "Spread", .maximum = 50.0f, .defaultValue = 0.0f, .unit = Unit::cents };
        made[FmSynth::outputLevel] = { .id = "level",
                                       .name = "Level",
                                       .minimum = -36.0f,
                                       .maximum = 6.0f,
                                       .defaultValue = -6.0f,
                                       .unit = Unit::decibels };
        return made;
    }

    constexpr auto parameters = makeParameters();

    constexpr int sustainPedal = 64, modulationWheel = 1; // MIDI controller numbers
    constexpr double lowestCutoff = 20.0;
    constexpr float twoPi = juce::MathConstants<float>::twoPi;
    // Feedback at full, in radians for what the operator puts out at its full level: the DX7's feedback 7, one cycle, as
    // Dexed's engine (msfa) works it out, 2^(FB - 7) cycles, so that its 6, π, is at half: the brightest saw and the squarest
    // square, past which the sound turns rough and at full toward noise, as the DX7's 7 does.
    constexpr float strongestFeedback = twoPi;

    // What the carriers' sum is turned down by, 5 dB, so that a new FM synth plays its chords about as loud as a new synth
    // does and under full scale: its electric piano's two carriers start in phase and add up.
    constexpr float carrierHeadroom = 0.56234133f;

    template <typename Enum>
    Enum choiceOf (float value)
    {
        return static_cast<Enum> (std::lround (value));
    }

    // A band-limited wave's level at a place in its cycle (from 0 to 1), by straight lines between its samples.
    float readLevel (const Wavetable::Level& level, float place) noexcept
    {
        const auto length = (float) level.length;
        auto at = place * length;

        if (at >= length)
            at -= length;

        const auto index = (int) at;
        const auto* s = level.samples + index;
        return s[0] + (at - (float) index) * (s[1] - s[0]);
    }

    // What an algorithm's operators are, worked out once a block: each one on, each one heard, and each one's wave.
    struct Roles
    {
        std::array<bool, numOperators> isOn {}, carries {};
        std::array<FmSynth::Wave, numOperators> shapes {};
        int numCarriers = 0;
    };

    Roles getRoles (const FmSynth::Algorithm& shape, std::span<const float> values)
    {
        Roles roles;

        for (int i = 0; i < numOperators; ++i)
        {
            roles.isOn[(size_t) i] = values[(size_t) FmSynth::getParameterIndex (i, Field::on)] > 0.5f;
            roles.carries[(size_t) i] = ((shape.carriers >> i) & 1) != 0;
            roles.shapes[(size_t) i] = choiceOf<FmSynth::Wave> (values[(size_t) FmSynth::getParameterIndex (i, Field::wave)]);
            roles.numCarriers += roles.carries[(size_t) i] ? 1 : 0;
        }

        return roles;
    }

    // One sample, at twice the rate, of one of a voice's lanes: its operators from the highest down, each taking what
    // modulates it into its phase, in radians, and its own wave fed back, the last two of what it fed back averaged so that
    // it doesn't hunt: as it is (toward a saw), or squared (toward a square: a squared wave is the same in each half of its
    // cycle, so that it leaves only odd harmonics). Each operator's output at its level and envelope goes into outputs where
    // asked for; it returns what the carriers make, summed. The synth's oscillators and levels, and its editor's drawing's.
    template <typename Oscillators, typename Levels>
    forcedinline float runOperators (Oscillators& ops, const Levels& levels, const FmSynth::Algorithm& shape, const Roles& roles,
                                     const std::array<float, numOperators>& envelopes, float* outputs) noexcept
    {
        std::array<float, numOperators> made {}; // each operator's wave
        auto heard = 0.0f;

        for (int o = numOperators - 1; o >= 0; --o)
        {
            auto& op = ops[(size_t) o];
            const auto& level = levels[(size_t) o];

            if (! roles.isOn[(size_t) o] || op.silent)
            {
                op.before = op.fedBack;
                op.fedBack = 0.0f;

                if (outputs != nullptr)
                    outputs[o] = 0.0f;

                continue;
            }

            auto modulation = std::abs (level.feedback) * 0.5f * (op.fedBack + op.before);

            for (int from = o + 1; from < numOperators; ++from)
                if ((shape.modulatedBy[(size_t) o] >> from) & 1)
                    modulation += made[(size_t) from] * levels[(size_t) from].depth * envelopes[(size_t) from];

            auto place = op.phase + modulation / twoPi;
            place -= std::floor (place);
            auto wave = 0.0f;

            switch (roles.shapes[(size_t) o])
            {
                case FmSynth::Wave::sine:
                    wave = dsp::sineOfPhase (place);
                    break;
                case FmSynth::Wave::triangle:
                    wave = readLevel (op.triangle, place);
                    break;
                case FmSynth::Wave::saw:
                    wave = readLevel (op.saw, place);
                    break;
                case FmSynth::Wave::square:
                    wave = readLevel (op.saw, place < 0.5f ? place + 0.5f : place - 0.5f) - readLevel (op.saw, place);
                    break;
            }

            made[(size_t) o] = wave;
            const auto output = wave * envelopes[(size_t) o] * level.gain;
            op.before = op.fedBack;
            op.fedBack = level.feedback < 0.0f ? wave * output : output;
            op.phase += op.increment;

            if (op.phase >= 1.0f)
                op.phase -= 1.0f;

            if (roles.carries[(size_t) o])
                heard += wave * level.depth * envelopes[(size_t) o];

            if (outputs != nullptr)
                outputs[o] = output;
        }

        return heard;
    }
} // namespace

const DeviceInfo FmSynth::info { .typeID = typeID, .name = "FM", .kind = DeviceKind::instrument, .parameters = parameters };

const FmSynth::Algorithm& FmSynth::getAlgorithm (int index) noexcept
{
    return algorithms[(size_t) std::clamp (index, 0, numAlgorithms - 1)];
}

std::uint8_t FmSynth::getTargets (int algorithmIndex, int op) noexcept
{
    const auto& shape = getAlgorithm (algorithmIndex);
    std::uint8_t targets = 0;

    for (int below = 0; below < numOperators; ++below)
        if ((shape.modulatedBy[(size_t) below] >> op) & 1)
            targets = (std::uint8_t) (targets | (1 << below));

    return targets;
}

float FmSynth::getModulationIndex (float level) noexcept
{
    return 2.0f * twoPi * level * level;
}

FmSynth::FmSynth()
    : sawTable (dsp::getBandLimitedWaves().saw.get()),
      triangleTable (dsp::getBandLimitedWaves().triangle.get())
{
    // Where each voice's operators start while their phases run free: spread, so that the first notes don't all start alike.
    juce::Random spreadPhases (0x464d);

    for (auto& voice : voices)
        for (auto& lane : voice.lanes)
            for (auto& op : lane.ops)
                op.phase = spreadPhases.nextFloat();
}

void FmSynth::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    dcBlock = (float) std::exp (-juce::MathConstants<double>::twoPi * dcBlockHertz / sampleRate);

    for (auto& voice : voices)
    {
        for (auto& envelope : voice.envelopes)
            envelope.prepare (sampleRate);

        for (auto& lane : voice.lanes)
            lane.filter.prepare (dsp::getLongestCombDelay (lowestCutoff, sampleRate));
    }

    lfo.prepare (sampleRate);
    reset();
}

void FmSynth::reset()
{
    for (auto& voice : voices)
    {
        for (auto& envelope : voice.envelopes)
            envelope.reset();

        for (auto& lane : voice.lanes)
        {
            for (auto& op : lane.ops)
                op.fedBack = op.before = 0.0f;

            lane.down.reset();
            lane.filter.reset();
            lane.dcIn = lane.dcOut = 0.0f;
        }

        voice.note = -1;
        voice.held = voice.sustained = voice.active = false;
        voice.fade = 1.0f;
        voice.fadeStep = 0.0f;
        voice.filterIsSet = false;
    }

    lfo.reset();
    numHeld = 0;
    notesPlayed = 0;
    lastNote = -1.0f;
    bend = modWheel = 0.0f;
    pedalDown = false;
    shownMode = -1;
}

double FmSynth::getTailSeconds() const
{
    return (double) tailSeconds.load (std::memory_order_relaxed);
}

void FmSynth::releaseVoice (Voice& voice)
{
    voice.held = voice.sustained = false;

    for (auto& envelope : voice.envelopes)
        envelope.noteOff();
}

FmSynth::Voice& FmSynth::findVoiceFor (int note)
{
    // The voice playing this note already, then a free one, then the one ringing out longest, then the oldest: among those
    // that play notes, not the last, which fades out a voice taken (stealVoice()).
    const std::span<Voice> playing (voices.data(), (size_t) maxVoices);

    for (auto& voice : playing)
        if (voice.note == note && voice.active)
            return voice;

    for (auto& voice : playing)
        if (! voice.active)
            return voice;

    Voice* oldest = nullptr;

    for (auto& voice : playing)
        if (! voice.held && ! voice.sustained && (oldest == nullptr || voice.started < oldest->started))
            oldest = &voice;

    if (oldest != nullptr)
        return *oldest;

    for (auto& voice : playing)
        if (oldest == nullptr || voice.started < oldest->started)
            oldest = &voice;

    return *oldest;
}

void FmSynth::stealVoice (Voice& voice)
{
    // What it played fades out from a copy of it over a few milliseconds, while it starts the new note from silence. The copy
    // reuses the last voice's memory (every voice is prepared alike), so this doesn't allocate.
    auto& fading = voices[(size_t) maxVoices];
    fading = voice;
    fading.note = -1;
    fading.held = fading.sustained = false;
    fading.fadeStep = std::max (fading.fadeStep, (float) (1.0 / (stealFadeSeconds * sampleRate)));

    for (auto& envelope : voice.envelopes)
        envelope.reset();

    voice.active = false;
    voice.fade = 1.0f;
    voice.fadeStep = 0.0f;
}

void FmSynth::beginNote (Voice& voice, int note, float velocityValue, std::span<const float> values)
{
    // Its envelopes shorter up the keys, longer down them, from the note's first stage on (a stage keeps the times it
    // started with).
    voice.timeScale = std::clamp (std::exp2 (-values[keyTime] * ((float) note - 60.0f) / 12.0f), 0.125f, 8.0f);
    setEnvelopes (voice, values);

    // Each operator's level as velocity and the key make it, kept for the note.
    for (int i = 0; i < numOperators; ++i)
    {
        const auto sensitivity = values[(size_t) getParameterIndex (i, Field::velocity)];
        const auto keyScaleValue = values[(size_t) getParameterIndex (i, Field::keyScale)];
        const auto byVelocity = 1.0f - sensitivity + sensitivity * velocityValue * velocityValue;
        const auto byKey = std::clamp (std::exp2 (-keyScaleValue * ((float) note - 60.0f) / 12.0f), 0.0625f, 4.0f);
        voice.levels[(size_t) i].noteGain = byVelocity * byKey;
        voice.envelopes[(size_t) i].noteOn();
    }

    // Two lanes while it is spread, for the note's whole life; each from the start of its waves unless the phases run free.
    const auto resets = choiceOf<Phase> (values[phase]) == Phase::reset;
    voice.numLanes = values[spread] > 0.0f ? maxLanes : 1;

    for (auto& lane : voice.lanes)
    {
        for (auto& op : lane.ops)
        {
            op.fedBack = op.before = 0.0f;

            if (resets)
                op.phase = 0.0f;
        }

        lane.down.reset();
        lane.filter.reset();
        lane.dcIn = lane.dcOut = 0.0f;
    }

    voice.velocity = velocityValue;
    voice.pitchEnvelope = 1.0f;
    voice.filterIsSet = false;
    voice.active = true;
    voice.age = 0;
    voice.started = ++notesPlayed;

    // At its full level, whatever fade the voice was in when its last note ended (one cut as playback stopped can end its
    // release first): left fading, the new note would fade out at once.
    voice.fade = 1.0f;
    voice.fadeStep = 0.0f;
}

void FmSynth::setEnvelopes (Voice& voice, std::span<const float> values) noexcept
{
    const auto scale = voice.timeScale * 0.001f;

    for (int i = 0; i < numOperators; ++i)
    {
        const auto at = [i, &values] (Field field) { return values[(size_t) getParameterIndex (i, field)]; };
        auto& envelope = voice.envelopes[(size_t) i];
        envelope.setParameters (at (Field::attack) * scale, at (Field::decay) * scale, at (Field::sustain), at (Field::release) * scale);
        envelope.setCurves (at (Field::attackCurve), at (Field::decayCurve), at (Field::releaseCurve));
    }
}

void FmSynth::startNote (int note, float velocityValue, std::span<const float> values, bool portamento)
{
    const auto glidesIn = portamento && lastNote >= 0.0f;
    const auto from = glidesIn || (values[glide] > 0.0f && lastNote >= 0.0f) ? lastNote : (float) note;
    lastNote = (float) note;

    const auto seconds = values[glide] > 0.0f ? (double) values[glide] * 0.001 : portamentoSeconds;
    const auto slideFrom = [&] (float pitch)
    { return glidesIn ? std::abs ((float) note - pitch) / (float) std::max (1.0, seconds * sampleRate) : 0.0f; };

    if (choiceOf<int> (values[voiceMode]) == 1)
    {
        // Mono: the key goes on top of those held; with one held already, the voice glides to it without starting again.
        auto kept = 0;

        for (int i = 0; i < numHeld; ++i)
            if (heldKeys[(size_t) i] != note)
                heldKeys[(size_t) kept++] = heldKeys[(size_t) i];

        numHeld = std::min (kept, maxHeld - 1);
        heldKeys[(size_t) numHeld++] = note;

        auto& voice = voices[0];

        if (voice.fadeStep > 0.0f)
            stealVoice (voice);

        const auto legato = voice.active && voice.held;

        if (! legato)
        {
            if (voice.active)
                stealVoice (voice);

            beginNote (voice, note, velocityValue, values);
            voice.pitch = from;
        }

        voice.note = note;
        voice.target = (float) note;
        voice.held = true;
        voice.sustained = false;
        voice.slidePerSample = slideFrom (voice.pitch);
        return;
    }

    auto& voice = findVoiceFor (note);

    if (voice.active)
        stealVoice (voice);

    beginNote (voice, note, velocityValue, values);
    voice.note = note;
    voice.held = true;
    voice.sustained = false;
    voice.target = (float) note;
    voice.pitch = from;
    voice.slidePerSample = slideFrom (from);
}

void FmSynth::stopNote (int note, std::span<const float> values)
{
    if (choiceOf<int> (values[voiceMode]) == 1)
    {
        auto kept = 0;

        for (int i = 0; i < numHeld; ++i)
            if (heldKeys[(size_t) i] != note)
                heldKeys[(size_t) kept++] = heldKeys[(size_t) i];

        numHeld = kept;
        auto& voice = voices[0];

        if (voice.note != note || ! voice.held)
            return;

        if (numHeld > 0)
        {
            voice.note = heldKeys[(size_t) numHeld - 1];
            voice.target = (float) voice.note;
            voice.slidePerSample = 0.0f;
            lastNote = voice.target;
            return;
        }
    }

    for (auto& voice : voices)
    {
        if (voice.note == note && voice.held)
        {
            if (pedalDown)
            {
                voice.held = false;
                voice.sustained = true;
            }
            else
            {
                releaseVoice (voice);
            }
        }
    }
}

void FmSynth::slideTo (int note, float seconds)
{
    for (size_t i = 0; i < (size_t) maxVoices; ++i)
    {
        if (auto& voice = voices[i]; voice.active && (voice.held || voice.sustained) && voice.fadeStep <= 0.0f)
        {
            voice.target = (float) note;
            voice.slidePerSample = std::abs (voice.target - voice.pitch) / (float) std::max (1.0, (double) seconds * sampleRate);
        }
    }

    lastNote = (float) note;
}

void FmSynth::handle (const NoteEvent& event, std::span<const float> values)
{
    switch (event.type)
    {
        case NoteEvent::Type::noteOn:
        case NoteEvent::Type::portamentoOn:
            startNote (event.number, event.value, values, event.type == NoteEvent::Type::portamentoOn);
            break;

        case NoteEvent::Type::slide:
            slideTo (event.number, event.value);
            break;

        case NoteEvent::Type::noteOff:
            stopNote (event.number, values);
            break;

        case NoteEvent::Type::pitchBend:
            bend = event.value;
            break;

        case NoteEvent::Type::controller:
            if (event.number == sustainPedal)
            {
                pedalDown = event.value >= 0.5f;

                if (! pedalDown)
                    for (auto& voice : voices)
                        if (voice.sustained)
                            releaseVoice (voice);
            }
            else if (event.number == modulationWheel)
            {
                modWheel = event.value;
            }

            break;

        case NoteEvent::Type::allNotesOff:
        {
            numHeld = 0;
            pedalDown = false;
            const auto stopStep = (float) (1.0 / (stopFadeSeconds * sampleRate));

            for (auto& voice : voices)
            {
                if (! voice.active)
                    continue;

                releaseVoice (voice);

                if (! songPlaying && voice.age < justBegunSeconds * sampleRate)
                    voice.fadeStep = std::max (voice.fadeStep, stopStep);
            }

            break;
        }
    }
}

double FmSynth::getFilterMagnitude (std::span<const float> values, double cutoffHertz, double frequency, double rate)
{
    const dsp::FilterSettings settings { .mode = dsp::toFilterMode (values[filterType]),
                                         .frequency = cutoffHertz,
                                         .resonance = values[resonance],
                                         .steep = dsp::isSteep (values[filterSlope]),
                                         .q = dsp::getResonanceQ (values[resonance]) };
    return dsp::getMagnitude (dsp::makeMultimode (settings, rate), frequency, rate);
}

void FmSynth::render (float* left, float* right, int numSamples, std::span<const float> values)
{
    const auto algorithmIndex = choiceOf<int> (values[algorithm]);
    const auto& shape = getAlgorithm (algorithmIndex);
    const auto filterMode = dsp::toFilterMode (values[filterType]);
    const auto steep = dsp::isSteep (values[filterSlope]);
    const auto q = dsp::getResonanceQ (values[resonance]);
    const auto outputGain = dsp::decibelsToGain (values[outputLevel]);
    const auto lfoAim = choiceOf<LfoTarget> (values[lfoTarget]);
    const auto depth = values[lfoOn] > 0.5f ? std::clamp (values[lfoDepth] + modWheel, 0.0f, 1.0f) : 0.0f;
    const auto filtered = values[filterOn] > 0.5f;
    const auto lfoWave = choiceOf<dsp::LfoShape> (values[lfoShape]);
    const auto glideSamples = (double) values[glide] * 0.001 * sampleRate;
    const auto bent = bend * values[bendRange];
    const auto nyquistLimit = 0.45 * sampleRate;
    const auto oversampled = 2.0 * sampleRate;
    const auto pitchFall = (float) std::exp (-1.0 / (std::max (1.0f, values[pitchDecay]) * 0.001 * sampleRate));
    const auto pitchSweep = values[pitchAmount];
    const auto halfSpread = 0.5f * values[spread]; // each lane's cents from the note, while spread

    // What is heard is the carriers' sum, a square root of their number down, so that algorithms with more of them stay
    // about as loud, with the headroom.
    const auto roles = getRoles (shape, values);
    const auto carrierNorm = carrierHeadroom / std::sqrt ((float) std::max (1, roles.numCarriers));
    const auto limit = std::clamp (1.0 - 20000.0 / oversampled, 0.45, 0.9);

    for (int begin = 0; begin < numSamples; begin += controlStep)
    {
        const auto length = std::min (controlStep, numSamples - begin);
        std::array<float, controlStep> moved {};
        const auto glideStep = glideSamples <= 1.0 ? 1.0f : (float) (1.0 - std::exp (-length / glideSamples));

        for (int i = 0; i < length; ++i)
            moved[(size_t) i] = lfo.next (lfoWave);

        const auto pitchMove = lfoAim == LfoTarget::pitch ? depth * moved[0] : 0.0f;
        const auto cutoffMove = lfoAim == LfoTarget::cutoff ? depth * 24.0f * moved[0] : 0.0f;
        const auto timbreNow = values[timbre] * (lfoAim == LfoTarget::timbre ? std::max (0.0f, 1.0f + depth * moved[0]) : 1.0f);

        for (auto& voice : voices)
        {
            if (! voice.active)
                continue;

            voice.age = std::min (voice.age + length, 1 << 30);

            if (voice.slidePerSample > 0.0f)
            {
                const auto stepSize = voice.slidePerSample * (float) length;
                const auto gap = voice.target - voice.pitch;
                voice.pitch = std::abs (gap) <= stepSize ? voice.target : voice.pitch + std::copysign (stepSize, gap);
                voice.slidePerSample = std::abs (gap) <= stepSize ? 0.0f : voice.slidePerSample;
            }
            else
            {
                voice.pitch += (voice.target - voice.pitch) * glideStep;
            }

            // Each operator's level now, its depth or gain and its feedback's, and in each lane its pitch, at twice the rate;
            // one too high to play stays silent.
            const auto note = (double) (voice.pitch + bent + pitchMove + pitchSweep * voice.pitchEnvelope);
            const auto noteFrequency = dsp::noteToFrequency (note);

            for (int i = 0; i < numOperators; ++i)
            {
                const auto at = [i, &values] (Field field) { return values[(size_t) getParameterIndex (i, field)]; };
                auto& levels = voice.levels[(size_t) i];
                levels.gain = levels.noteGain * getCarrierGain (at (Field::level));
                levels.depth =
                    roles.carries[(size_t) i] ? levels.gain : levels.noteGain * getModulationIndex (at (Field::level)) * timbreNow;
                levels.feedback = std::clamp (at (Field::feedback) * timbreNow, -1.0f, 1.0f) * strongestFeedback;
                const auto tuned = at (Field::fixed) > 0.5f ? (double) at (Field::frequency)
                                                            : noteFrequency * (double) (at (Field::ratio) + at (Field::fine));

                for (int l = 0; l < voice.numLanes; ++l)
                {
                    const auto cents = at (Field::detune) + (voice.numLanes > 1 ? (l == 0 ? -halfSpread : halfSpread) : 0.0f);
                    const auto increment = std::max (0.0, tuned * std::exp2 ((double) cents / 1200.0)) / oversampled;
                    auto& op = voice.lanes[(size_t) l].ops[(size_t) i];
                    op.silent = increment >= limit;
                    op.increment = (float) increment;
                    const auto chosen = Wavetable::chooseLevel (limit / std::max (1.0e-9, increment));
                    op.saw = sawTable->getLevel (0, chosen);
                    op.triangle = triangleTable->getLevel (0, chosen);
                }
            }

            if (filtered)
            {
                const auto moves = values[keyTracking] * (voice.pitch - 60.0f) + cutoffMove;
                const auto cutoffHertz = std::clamp ((double) values[cutoff] * std::pow (2.0, moves / 12.0), lowestCutoff, nyquistLimit);
                const dsp::FilterSettings settings { .mode = filterMode,
                                                     .frequency = cutoffHertz,
                                                     .resonance = values[resonance],
                                                     .steep = steep,
                                                     .q = q };

                if (! voice.filterIsSet || ! dsp::areSame (settings, voice.filterSettings))
                {
                    const auto coefficients = dsp::makeMultimode (settings, sampleRate);

                    for (int l = 0; l < voice.numLanes; ++l)
                        voice.lanes[(size_t) l].filter.set (coefficients, length);

                    voice.filterSettings = settings;
                    voice.filterIsSet = true;
                }
            }

            for (int i = 0; i < length; ++i)
            {
                // Each operator's envelope, once a sample, which both lanes play.
                std::array<float, numOperators> envelopes {};

                for (int o = 0; o < numOperators; ++o)
                    envelopes[(size_t) o] = voice.envelopes[(size_t) o].next();

                voice.pitchEnvelope *= pitchFall;
                std::array<float, maxLanes> lanes {};

                for (int l = 0; l < voice.numLanes; ++l)
                {
                    auto& lane = voice.lanes[(size_t) l];
                    const auto first = runOperators (lane.ops, voice.levels, shape, roles, envelopes, nullptr) * carrierNorm;
                    const auto second = runOperators (lane.ops, voice.levels, shape, roles, envelopes, nullptr) * carrierNorm;
                    const auto out = lane.down.down (first, second);

                    // Without what FM leaves off centre (heavy feedback, a modulator a few cents off its carrier's ratio), which would
                    // offset what is heard.
                    const auto centred = out - lane.dcIn + dcBlock * lane.dcOut;
                    lane.dcIn = out;
                    lane.dcOut = centred;
                    lanes[(size_t) l] = filtered ? lane.filter.process (centred) : centred;
                }

                auto gain = outputGain * voice.fade;

                // A voice taken for another note, or cut as playback stopped, fades out; faded, it is free, and silent for the
                // rest of its step, where its filters still hold its sound (a note begun on it starts at its full level).
                if (voice.fadeStep > 0.0f && (voice.fade -= voice.fadeStep) <= 0.0f)
                {
                    voice.fade = 0.0f;
                    voice.fadeStep = 0.0f;
                    voice.active = false;

                    for (auto& envelope : voice.envelopes)
                        envelope.reset();
                }

                if (lfoAim == LfoTarget::level)
                    gain *= 1.0f - depth * 0.5f * (1.0f + moved[(size_t) i]);

                // Spread, the lower lane on the left and the higher on the right; a mono output takes both.
                const auto spreadOut = voice.numLanes > 1;

                if (right != nullptr)
                {
                    left[begin + i] += lanes[0] * gain;
                    right[begin + i] += lanes[spreadOut ? 1 : 0] * gain;
                }
                else
                {
                    left[begin + i] += (spreadOut ? 0.5f * (lanes[0] + lanes[1]) : lanes[0]) * gain;
                }
            }

            // Done once every carrier that is on has let go.
            auto sounding = false;

            for (int o = 0; o < numOperators; ++o)
                if (roles.carries[(size_t) o] && roles.isOn[(size_t) o] && voice.envelopes[(size_t) o].isActive())
                    sounding = true;

            if (! sounding)
            {
                voice.active = false;
                voice.note = -1;
            }
        }
    }
}

void FmSynth::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;

    if (block.numChannels == 0)
        return;

    songPlaying = block.isPlaying;

    // A switch between poly and mono lets go of every note, so that none is left hanging.
    const auto modeNow = choiceOf<int> (values[voiceMode]);

    if (modeNow != shownMode)
    {
        if (shownMode >= 0)
            for (auto& voice : voices)
                if (voice.active)
                    releaseVoice (voice);

        numHeld = 0;
        shownMode = modeNow;
    }

    // Each sounding voice's operators' envelopes, their times as its key scales them (a note sets its own as it starts).
    auto longestRelease = 0.0f;

    for (int i = 0; i < numOperators; ++i)
        longestRelease = std::max (longestRelease, values[(size_t) getParameterIndex (i, Field::release)]);

    for (auto& voice : voices)
        if (voice.active)
            setEnvelopes (voice, values);

    lfo.setRate (values[lfoRate]);
    tailSeconds.store (longestRelease * 0.001f * 8.0f, std::memory_order_relaxed); // the lowest notes' envelopes, at their longest

    auto* left = block.channels[0];
    auto* right = block.numChannels > 1 ? block.channels[1] : nullptr;
    size_t next = 0;

    for (int done = 0; done < block.numSamples;)
    {
        while (next < block.events.size() && block.events[next].sample <= done)
            handle (block.events[next++], values);

        const auto until = next < block.events.size() ? std::min (block.numSamples, block.events[next].sample) : block.numSamples;
        render (left + done, right != nullptr ? right + done : nullptr, until - done, values);
        done = until;
    }

    while (next < block.events.size())
        handle (block.events[next++], values);

    activeVoices.store ((int) std::count_if (voices.begin(), voices.begin() + maxVoices, [] (const Voice& each) { return each.active; }),
                        std::memory_order_relaxed);

    // For the editor: the newest voice, and the LFO.
    const Voice* newest = nullptr;

    for (size_t i = 0; i < (size_t) maxVoices; ++i)
        if (const auto& voice = voices[i]; voice.active && (newest == nullptr || voice.started > newest->started))
            newest = &voice;

    for (size_t o = 0; o < (size_t) numOperators; ++o)
        shown.envelopes[o].set (newest != nullptr ? newest->envelopes[o].getLevel() : 0.0f);

    shown.cutoff.set (newest != nullptr && newest->filterIsSet && values[filterOn] > 0.5f ? (float) newest->filterSettings.frequency
                                                                                          : 0.0f);
    shown.lfoPhase.set (lfo.getPhase());
}

void FmSynth::draw (std::span<const float> values, std::span<const float, numOperators> envelopeLevels, Drawing& drawing)
{
    // A note at middle C as render() plays it, at drawnPerCycle samples a cycle of its pitch rather than at twice the sample
    // rate, each operator's envelope held where given; two cycles first, so that what is fed back settles.
    const auto& shape = getAlgorithm (choiceOf<int> (values[algorithm]));
    const auto roles = getRoles (shape, values);
    const auto noteFrequency = dsp::noteToFrequency (60.0);
    const auto rate = noteFrequency * (double) drawnPerCycle;
    constexpr double limit = 0.45; // the highest an operator plays, as a part of the rate
    std::array<Oscillator, numOperators> ops {};
    std::array<OperatorLevel, numOperators> levels {};
    std::array<float, numOperators> envelopes {};
    const auto* saws = dsp::getBandLimitedWaves().saw.get();
    const auto* triangles = dsp::getBandLimitedWaves().triangle.get();

    for (int i = 0; i < numOperators; ++i)
    {
        const auto at = [i, &values] (Field field) { return values[(size_t) getParameterIndex (i, field)]; };
        auto& level = levels[(size_t) i];
        level.gain = getCarrierGain (at (Field::level));
        level.depth = roles.carries[(size_t) i] ? level.gain : getModulationIndex (at (Field::level)) * values[timbre];
        level.feedback = std::clamp (at (Field::feedback) * values[timbre], -1.0f, 1.0f) * strongestFeedback;
        const auto tuned =
            at (Field::fixed) > 0.5f ? (double) at (Field::frequency) : noteFrequency * (double) (at (Field::ratio) + at (Field::fine));
        const auto increment = std::max (0.0, tuned * std::exp2 ((double) at (Field::detune) / 1200.0)) / rate;
        auto& op = ops[(size_t) i];
        op.silent = increment >= limit;
        op.increment = (float) increment;
        const auto chosen = Wavetable::chooseLevel (limit / std::max (1.0e-9, increment));
        op.saw = saws->getLevel (0, chosen);
        op.triangle = triangles->getLevel (0, chosen);
        envelopes[(size_t) i] = std::clamp (envelopeLevels[(size_t) i], 0.0f, 1.0f);
    }

    const auto norm = carrierHeadroom / std::sqrt ((float) std::max (1, roles.numCarriers));
    std::array<float, numOperators> outputs {};

    for (int n = -2 * drawnPerCycle; n < drawnLength; ++n)
    {
        const auto heard = runOperators (ops, levels, shape, roles, envelopes, outputs.data()) * norm;

        if (n < 0)
            continue;

        drawing.heard[(size_t) n] = heard;

        for (size_t o = 0; o < (size_t) numOperators; ++o)
            drawing.operators[o][(size_t) n] = outputs[o];
    }
}
} // namespace phazite::devices
