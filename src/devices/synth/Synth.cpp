// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Synth.h"
#include "devices/dsp/Math.h"
#include "devices/dsp/NoteValues.h"
#include "devices/wavetable/FactoryWavetables.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view warps[] = { "None", "Sync", "Bend +", "Bend -", "Pulse Width", "Mirror", "FM", "Ring Mod" };
    constexpr std::string_view subOctaves[] = { "-1 Octave", "-2 Octaves" };
    constexpr std::string_view subShapes[] = { "Sine", "Triangle", "Saw", "Square" };
    constexpr std::string_view unisonCounts[] = { "1", "2", "3", "4", "5", "6", "7", "8" };
    constexpr std::string_view unisonStacks[] = { "Off", "Octave", "Two Octaves", "Fifth", "Power Chord" };
    constexpr std::string_view lfoShapes[] = { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random" };
    constexpr std::string_view modSources[] = { "None",     "Envelope 2", "Envelope 3", "LFO 1",  "LFO 2",
                                                "Velocity", "Note",       "Mod Wheel",  "Random", "Drift" };
    constexpr std::string_view modTargets[] = {
        "None",          "Osc 1 Position", "Osc 1 Warp",   "Osc 1 Pitch",  "Osc 1 Level",  "Osc 2 Position", "Osc 2 Warp",   "Osc 2 Pitch",
        "Osc 2 Level",   "Pitch",          "Sub Level",    "Noise Level",  "Cutoff",       "Resonance",      "Volume",       "Pan",
        "Unison Detune", "Mod 1 Amount",   "Mod 2 Amount", "Mod 3 Amount", "Mod 4 Amount", "Mod 5 Amount",   "Mod 6 Amount", "Mod 7 Amount",
        "Mod 8 Amount"
    };
    constexpr std::string_view voiceModes[] = { "Poly", "Mono" };
    constexpr std::string_view glideModes[] = { "Always", "Legato" };
    constexpr std::string_view phases[] = { "Reset", "Free" }; // the FM synth's

    // The note values an LFO in time with the song takes (devices/dsp/NoteValues.h).
    constexpr auto& divisions = dsp::noteValueNames;
    constexpr auto& divisionLengths = dsp::noteValueLengths;

    // An envelope's time, from minimum to 10 s, with 300 ms halfway along its knob.
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

    constexpr ParameterInfo makePercent (std::string_view id, std::string_view name, float defaultValue, float minimum = 0.0f)
    {
        return { .id = id, .name = name, .minimum = minimum, .defaultValue = defaultValue, .unit = Unit::percent };
    }

    // How an envelope's stage bends (dsp::Adsr::setCurves()): from -100 % (fast at first) through 0 (straight) to 100 % (slow at
    // first), an analog envelope's curve at first.
    constexpr ParameterInfo makeCurve (std::string_view id, std::string_view name, float defaultValue)
    {
        return makePercent (id, name, defaultValue, -1.0f);
    }

    constexpr ParameterInfo makeToggle (std::string_view id, std::string_view name, float defaultValue)
    {
        return { .id = id, .name = name, .maximum = 1.0f, .defaultValue = defaultValue, .unit = Unit::toggle };
    }

    constexpr ParameterInfo makePitch (std::string_view id, std::string_view name)
    {
        return { .id = id, .name = name, .minimum = -36.0f, .maximum = 36.0f, .defaultValue = 0.0f, .unit = Unit::semitones };
    }

    constexpr ParameterInfo makeFine (std::string_view id, std::string_view name)
    {
        return { .id = id, .name = name, .minimum = -100.0f, .maximum = 100.0f, .defaultValue = 0.0f, .unit = Unit::cents };
    }

    constexpr ParameterInfo makeRate (std::string_view id, std::string_view name, float defaultValue)
    {
        return { .id = id,
                 .name = name,
                 .minimum = 0.05f,
                 .maximum = 20.0f,
                 .defaultValue = defaultValue,
                 .unit = Unit::hertz,
                 .scale = Scale::logarithmic };
    }

    constexpr float sawPosition = 2.0f / 3.0f; // the saw in the first factory table, Basic Shapes
    constexpr float quarterNote = dsp::quarterNoteValue;

    // In the order of Synth::Parameter. The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        makeToggle ("osc1On", "Osc 1 On", 1.0f),
        makePercent ("osc1Position", "Osc 1 Position", sawPosition),
        makePitch ("osc1Pitch", "Osc 1 Pitch"),
        makeFine ("osc1Fine", "Osc 1 Fine"),
        makePercent ("osc1Level", "Osc 1 Level", 0.75f),
        makeChoice ("osc1Warp", "Osc 1 Warp", warps, 0.0f),
        makePercent ("osc1WarpAmount", "Osc 1 Warp Amount", 0.0f),
        makeToggle ("osc2On", "Osc 2 On", 0.0f),
        makePercent ("osc2Position", "Osc 2 Position", sawPosition),
        makePitch ("osc2Pitch", "Osc 2 Pitch"),
        makeFine ("osc2Fine", "Osc 2 Fine"),
        makePercent ("osc2Level", "Osc 2 Level", 0.75f),
        makeChoice ("osc2Warp", "Osc 2 Warp", warps, 0.0f),
        makePercent ("osc2WarpAmount", "Osc 2 Warp Amount", 0.0f),
        makePercent ("subLevel", "Sub Level", 0.0f),
        makeChoice ("subOctave", "Sub Octave", subOctaves, 0.0f),
        makeChoice ("subShape", "Sub Shape", subShapes, 0.0f),
        makePercent ("noiseLevel", "Noise Level", 0.0f),
        makeChoice ("unisonVoices", "Unison Voices", unisonCounts, 0.0f),
        { .id = "unisonDetune", .name = "Unison Detune", .maximum = 100.0f, .defaultValue = 20.0f, .unit = Unit::cents },
        makePercent ("unisonSpread", "Unison Spread", 0.8f),
        makePercent ("unisonPosition", "Unison Position Spread", 0.0f),
        makeChoice ("unisonStack", "Unison Stack", unisonStacks, 0.0f),
        { .id = "unisonDrift", .name = "Unison Drift", .maximum = 50.0f, .defaultValue = 0.0f, .unit = Unit::cents },
        makeToggle ("filterOn", "Filter On", 1.0f),
        makeChoice ("filterType", "Filter Type", dsp::filterModeNames, 0.0f),
        { .id = "cutoff",
          .name = "Filter Cutoff",
          .minimum = 20.0f,
          .maximum = 20000.0f,
          .defaultValue = 2500.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        makePercent ("resonance", "Filter Resonance", 0.1f),
        makeChoice ("filterSlope", "Filter Slope", dsp::filterSlopeNames, 1.0f),
        { .id = "drive", .name = "Filter Drive", .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        makePercent ("keyTracking", "Filter Key Tracking", 0.5f),
        makeTime ("env1Attack", "Envelope 1 Attack", 0.5f, 2.0f), // never instant, so a note doesn't click
        makeTime ("env1Decay", "Envelope 1 Decay", 0.0f, 300.0f),
        makePercent ("env1Sustain", "Envelope 1 Sustain", 0.8f),
        makeTime ("env1Release", "Envelope 1 Release", 0.5f, 200.0f),
        makeCurve ("env1AttackCurve", "Envelope 1 Attack Curve", dsp::Adsr::analogAttackCurve),
        makeCurve ("env1DecayCurve", "Envelope 1 Decay Curve", dsp::Adsr::analogSettleCurve),
        makeCurve ("env1ReleaseCurve", "Envelope 1 Release Curve", dsp::Adsr::analogSettleCurve),
        makeTime ("env2Attack", "Envelope 2 Attack", 0.0f, 1.0f),
        makeTime ("env2Decay", "Envelope 2 Decay", 0.0f, 400.0f),
        makePercent ("env2Sustain", "Envelope 2 Sustain", 0.2f),
        makeTime ("env2Release", "Envelope 2 Release", 0.0f, 300.0f),
        makeCurve ("env2AttackCurve", "Envelope 2 Attack Curve", dsp::Adsr::analogAttackCurve),
        makeCurve ("env2DecayCurve", "Envelope 2 Decay Curve", dsp::Adsr::analogSettleCurve),
        makeCurve ("env2ReleaseCurve", "Envelope 2 Release Curve", dsp::Adsr::analogSettleCurve),
        makeTime ("env3Attack", "Envelope 3 Attack", 0.0f, 1.0f),
        makeTime ("env3Decay", "Envelope 3 Decay", 0.0f, 800.0f),
        makePercent ("env3Sustain", "Envelope 3 Sustain", 0.0f),
        makeTime ("env3Release", "Envelope 3 Release", 0.0f, 300.0f),
        makeCurve ("env3AttackCurve", "Envelope 3 Attack Curve", dsp::Adsr::analogAttackCurve),
        makeCurve ("env3DecayCurve", "Envelope 3 Decay Curve", dsp::Adsr::analogSettleCurve),
        makeCurve ("env3ReleaseCurve", "Envelope 3 Release Curve", dsp::Adsr::analogSettleCurve),
        makeChoice ("lfo1Shape", "LFO 1 Shape", lfoShapes, 0.0f),
        makeRate ("lfo1Rate", "LFO 1 Rate", 2.0f),
        makeToggle ("lfo1Sync", "LFO 1 Sync", 0.0f),
        makeChoice ("lfo1Division", "LFO 1 Division", divisions, quarterNote),
        makeToggle ("lfo1Retrigger", "LFO 1 Retrigger", 1.0f),
        makeTime ("lfo1FadeIn", "LFO 1 Fade In", 0.0f, 0.0f),
        makeChoice ("lfo2Shape", "LFO 2 Shape", lfoShapes, 1.0f),
        makeRate ("lfo2Rate", "LFO 2 Rate", 0.5f),
        makeToggle ("lfo2Sync", "LFO 2 Sync", 0.0f),
        makeChoice ("lfo2Division", "LFO 2 Division", divisions, quarterNote),
        makeToggle ("lfo2Retrigger", "LFO 2 Retrigger", 0.0f),
        makeTime ("lfo2FadeIn", "LFO 2 Fade In", 0.0f, 0.0f),
        // A new synth's first slot opens its filter with each note, as a pluck does.
        makeChoice ("mod1Source", "Mod 1 Source", modSources, (float) Synth::ModSource::envelope2),
        makeChoice ("mod1Target", "Mod 1 Target", modTargets, (float) Synth::ModTarget::cutoff),
        makePercent ("mod1Amount", "Mod 1 Amount", 0.25f, -1.0f),
        makeChoice ("mod2Source", "Mod 2 Source", modSources, 0.0f),
        makeChoice ("mod2Target", "Mod 2 Target", modTargets, 0.0f),
        makePercent ("mod2Amount", "Mod 2 Amount", 0.0f, -1.0f),
        makeChoice ("mod3Source", "Mod 3 Source", modSources, 0.0f),
        makeChoice ("mod3Target", "Mod 3 Target", modTargets, 0.0f),
        makePercent ("mod3Amount", "Mod 3 Amount", 0.0f, -1.0f),
        makeChoice ("mod4Source", "Mod 4 Source", modSources, 0.0f),
        makeChoice ("mod4Target", "Mod 4 Target", modTargets, 0.0f),
        makePercent ("mod4Amount", "Mod 4 Amount", 0.0f, -1.0f),
        makeChoice ("mod5Source", "Mod 5 Source", modSources, 0.0f),
        makeChoice ("mod5Target", "Mod 5 Target", modTargets, 0.0f),
        makePercent ("mod5Amount", "Mod 5 Amount", 0.0f, -1.0f),
        makeChoice ("mod6Source", "Mod 6 Source", modSources, 0.0f),
        makeChoice ("mod6Target", "Mod 6 Target", modTargets, 0.0f),
        makePercent ("mod6Amount", "Mod 6 Amount", 0.0f, -1.0f),
        makeChoice ("mod7Source", "Mod 7 Source", modSources, 0.0f),
        makeChoice ("mod7Target", "Mod 7 Target", modTargets, 0.0f),
        makePercent ("mod7Amount", "Mod 7 Amount", 0.0f, -1.0f),
        makeChoice ("mod8Source", "Mod 8 Source", modSources, 0.0f),
        makeChoice ("mod8Target", "Mod 8 Target", modTargets, 0.0f),
        makePercent ("mod8Amount", "Mod 8 Amount", 0.0f, -1.0f),
        makeChoice ("voiceMode", "Voices", voiceModes, 0.0f),
        { .id = "glide",
          .name = "Glide",
          .minimum = 0.0f,
          .maximum = 2000.0f,
          .defaultValue = 0.0f,
          .unit = Unit::milliseconds,
          .midpoint = 200.0f },
        makeChoice ("glideMode", "Glide Mode", glideModes, 0.0f),
        makeChoice ("phase", "Phase", phases, 0.0f),
        { .id = "bendRange", .name = "Bend Range", .minimum = 0.0f, .maximum = 24.0f, .defaultValue = 2.0f, .unit = Unit::semitones },
        makePercent ("velocity", "Velocity", 0.6f),
        { .id = "level", .name = "Level", .minimum = -36.0f, .maximum = 6.0f, .defaultValue = -6.0f, .unit = Unit::decibels },
    };

    static_assert (std::size (parameters) == (size_t) Synth::level + 1);
    static_assert (Synth::getParameter (1, Synth::oscWarpAmount) == Synth::osc2WarpAmount);
    static_assert (Synth::getEnvelopeParameter (2, Synth::envReleaseCurve) == Synth::env3ReleaseCurve);
    static_assert (Synth::getLfoParameter (1, Synth::lfoFadeIn) == Synth::lfo2FadeIn);
    static_assert (Synth::getModParameter (Synth::numModSlots - 1, Synth::modAmount) == Synth::mod8Amount);
    static_assert (std::size (modTargets) == (size_t) Synth::ModTarget::slot8Amount + 1);
    static_assert (Synth::getAmountTarget (Synth::numModSlots - 1) == Synth::ModTarget::slot8Amount);
    static_assert (std::size (modSources) == (size_t) Synth::ModSource::drift + 1);
    static_assert (std::size (glideModes) == (size_t) Synth::GlideMode::legato + 1);
    static_assert (std::size (phases) == (size_t) Synth::Phase::free + 1);
    static_assert (std::size (unisonStacks) == (size_t) Synth::Stack::powerChord + 1);

    constexpr int sustainPedal = 64, modulationWheel = 1; // MIDI controller numbers
    constexpr std::uint32_t noiseSeed = 0x7ab1e5u, unisonSeed = 0x0715e7u, lfoSeed = 0x1f0e5u, randomSeed = 0x2a7d0u;
    constexpr std::uint32_t driftSeed = 0xd71f77u;
    constexpr std::uint32_t wanderSeed = 0x3a9c51e7u; // the first slot's Drift: far from the unison drift's, whose seeds run on by voice

    // How far Osc Warp Amount takes the Sync warp's reading, by modulation: 2^(3 a) times the note's cycle, so a third past the
    // knob's end, 16 times (four octaves, as far as a classic poly synth's sync sweep reaches); the knob's own 0 to 100 % is 1 to 8 times.
    constexpr float maxSyncAmount = 4.0f / 3.0f;

    // The unison's drift: each copy's pitch wanders toward a new place every so often, gliding there, at the pace of the
    // Drift source (driftWanderSeconds), but each copy at a pace of its own, from half to twice that, so that no two move together,
    // as ZynAddSubFX gives each copy of its unison its own vibrato rate. Zyn's copies swing faster (0.5 to 2 Hz), but at
    // this pace a held note's beating already never repeats, as theirs doesn't.
    constexpr double driftWanderSeconds = 0.45, driftGlideSeconds = 0.3;
    constexpr double centsToLog = 0.69314718055994531 / 1200.0; // a pitch in cents as the natural logarithm of its ratio

    // The filter's drive fades in and out over this as it comes on or off.
    constexpr double driveFadeSeconds = 32.0 / 48000.0;

    // A stack's steps, in semitones, in Synth::Stack's order.
    constexpr float stackSteps[][Synth::maxStackSteps] = {
        { 0.0f, 0.0f, 0.0f }, { 0.0f, 12.0f, 0.0f }, { 0.0f, 12.0f, 24.0f }, { 0.0f, 7.0f, 0.0f }, { 0.0f, 7.0f, 12.0f },
    };
    constexpr int stackLengths[] = { 1, 2, 3, 2, 3 };

    static_assert (std::size (stackSteps) == std::size (unisonStacks) && std::size (stackLengths) == std::size (unisonStacks));

    // What 100 % of a modulation slot moves each destination by.
    constexpr float pitchRange = 24.0f;   // semitones
    constexpr float cutoffRange = 96.0f;  // semitones: eight octaves
    constexpr float detuneRange = 100.0f; // cents

    constexpr double lowestCutoff = 20.0; // where modulation can take the filter, at the lowest

    template <typename Enum>
    Enum choiceOf (float value)
    {
        return static_cast<Enum> (std::lround (value));
    }

    // How many times faster than the note a warp reads its wave at most, for the level that keeps it from aliasing.
    float getWarpSpeed (Synth::Warp warp, float amount, float otherRatio) noexcept
    {
        switch (warp)
        {
            case Synth::Warp::sync:
                return std::exp2 (3.0f * amount);
            case Synth::Warp::bendUp:
            case Synth::Warp::bendDown:
                return 1.0f + 7.0f * amount;
            case Synth::Warp::pulseWidth:
                return 1.0f / (1.0f - 0.95f * amount);
            case Synth::Warp::mirror:
                return amount > 0.0f ? 2.0f : 1.0f;
            case Synth::Warp::fm:
                return std::min (32.0f, 1.0f + 6.2831853f * amount * otherRatio);
            case Synth::Warp::none:
            case Synth::Warp::ringMod:
                break;
        }

        return 1.0f;
    }

    // A frame pair's wave at a place in its cycle, from 0 to 1.
    float readAt (const Wavetable::Level& from, const Wavetable::Level& to, float morph, float place) noexcept
    {
        const auto scaled = place * (float) from.length;
        const auto a = readCubic (from, scaled);
        return morph > 0.0f ? a + morph * (readCubic (to, scaled) - a) : a;
    }

    float wrap (float place) noexcept
    {
        place -= std::floor (place);
        return place < 1.0f ? place : 0.0f;
    }
} // namespace

void Synth::place (const Wavetable& table, Reader& reader, float frame) noexcept
{
    const auto lastFrame = table.getNumFrames() - 1;
    const auto firstFrame = std::min ((int) frame, lastFrame);

    if (firstFrame != reader.firstFrame)
    {
        reader.firstFrame = firstFrame;
        reader.from = table.getLevel (firstFrame, reader.mipLevel);
        reader.to = table.getLevel (std::min (firstFrame + 1, lastFrame), reader.mipLevel);
    }

    reader.morph = frame - (float) firstFrame;
}

// One oscillator's unison voice over a stretch, its warp chosen once rather than at each sample, as render() plays it sample
// by sample. Not for FM, which reads the other oscillator's last sample. Its increment, its place in the table, what its warp
// reads by and what it leaves off centre each where their lines have got to at each sample.
void Synth::renderStep (const OscillatorRun& run, Reader& reader, float* out, int length) noexcept
{
    const auto& table = *run.table;
    const auto& shape = run.shape;
    const auto frameMoves = reader.frame.isMoving();
    auto t = reader.phase;

    if (! frameMoves)
        place (table, reader, reader.frame.now());

    // The next sample's increment, and its frame pair where the position's line has got to.
    const auto next = [&] (int i) noexcept
    {
        if (frameMoves)
            place (table, reader, reader.frame.at (i + 1));

        return reader.increment.at (i + 1);
    };

    const auto advance = [&t] (float dt) noexcept { t = t + dt < 1.0f ? t + dt : t + dt - 1.0f; };

    switch (run.warp)
    {
        case Warp::sync:
            for (int i = 0; i < length; ++i)
            {
                const auto dt = next (i), ratio = shape.at (i + 1);
                out[i] = readAt (reader.from, reader.to, reader.morph, wrap (t * ratio));

                // The jump back to the wave's start when the note's cycle ends, smoothed (PolyBLEP).
                if (t < dt || t > 1.0f - dt)
                {
                    const auto jump =
                        readAt (reader.from, reader.to, reader.morph, 0.0f) - readAt (reader.from, reader.to, reader.morph, wrap (ratio));
                    out[i] += 0.5f * jump * dsp::polyBlep (t, dt);
                }

                advance (dt);
            }

            break;

        case Warp::bendUp:
        case Warp::bendDown:
            for (int i = 0; i < length; ++i)
            {
                const auto dt = next (i), bent = shape.at (i + 1);
                out[i] = readAt (reader.from, reader.to, reader.morph, wrap (t * bent / (1.0f + (bent - 1.0f) * t)));
                advance (dt);
            }

            break;

        case Warp::pulseWidth:
            for (int i = 0; i < length; ++i)
            {
                const auto dt = next (i), width = shape.at (i + 1);
                out[i] = readAt (reader.from, reader.to, reader.morph, t < width ? wrap (t / width) : 0.0f);
                advance (dt);
            }

            break;

        case Warp::mirror:
            for (int i = 0; i < length; ++i)
            {
                // Read forward to the middle, then back: heard as much as the amount says.
                const auto dt = next (i), amount = shape.at (i + 1);
                out[i] = readAt (reader.from, reader.to, reader.morph, t);

                if (amount > 0.0f)
                    out[i] +=
                        amount * (readAt (reader.from, reader.to, reader.morph, wrap (t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t)) - out[i]);

                advance (dt);
            }

            break;

        case Warp::fm: // render() plays it sample by sample
        case Warp::none:
        case Warp::ringMod:
            for (int i = 0; i < length; ++i)
            {
                const auto dt = next (i);
                out[i] = readAt (reader.from, reader.to, reader.morph, t);
                advance (dt);
            }

            break;
    }

    // Without what its warp leaves off centre.
    if (reader.offset.isMoving())
    {
        for (int i = 0; i < length; ++i)
            out[i] -= reader.offset.at (i + 1);
    }
    else if (const auto offset = reader.offset.now(); ! juce::exactlyEqual (offset, 0.0f))
    {
        for (int i = 0; i < length; ++i)
            out[i] -= offset;
    }

    reader.phase = t;
    reader.increment.advance (length);
    reader.frame.advance (length);
    reader.offset.advance (length);
}

const DeviceInfo Synth::info { .typeID = typeID, .name = "Synth", .kind = DeviceKind::instrument, .parameters = parameters };

std::span<const float> Synth::getStackSteps (Stack stack) noexcept
{
    const auto index = (size_t) std::clamp ((int) stack, 0, (int) std::size (stackSteps) - 1);
    return { stackSteps[index], (size_t) stackLengths[index] };
}

namespace
{
    // What a block's parameters say, read once for all its voices.
    struct Settings
    {
        std::array<bool, Synth::numOscillators> on {};
        std::array<Synth::Warp, Synth::numOscillators> warp {};
        std::array<float, Synth::numOscillators> position {}, pitch {}, level {}, warpAmount {};
        float subLevel = 0.0f, noiseLevel = 0.0f;
        int subOctaves = 1;
        dsp::Waveform subWave = dsp::Waveform::sine;
        int unison = 1;
        float detune = 0.0f, positionSpread = 0.0f, drift = 0.0f;
        std::array<float, Synth::maxUnison> places {}, lefts {}, rights {};
        std::span<const float> stack; // the semitones above the note its copies take in turn (Synth::getStackSteps())
        bool stereo = false;
        bool filterOn = false, steep = true;
        dsp::FilterMode filterMode = dsp::FilterMode::lowPass;
        float cutoff = 20000.0f, resonance = 0.0f, driveGain = 1.0f, keyTracking = 0.0f;
        std::array<dsp::LfoShape, 2> lfoShapes {};
        std::array<bool, 2> lfoRetrigger {};
        std::array<float, 2> lfoFades {}; // the samples over which each note's share of each LFO comes in; 0, at once
        std::array<Synth::ModSource, Synth::numModSlots> sources {};
        std::array<Synth::ModTarget, Synth::numModSlots> targets {};
        std::array<float, Synth::numModSlots> amounts {};
        bool movesAmounts = false;                       // a slot aims at another's amount
        std::array<bool, Synth::numModSlots> wanders {}; // the slots that play the Drift source
        float outputGain = 1.0f, bent = 0.0f;
    };

    bool isAmountTarget (Synth::ModTarget target) noexcept
    {
        return target >= Synth::ModTarget::slot1Amount && target <= Synth::ModTarget::slot8Amount;
    }

    Settings readSettings (std::span<const float> values, float bend, bool hasRight, double sampleRate)
    {
        Settings settings;

        for (int osc = 0; osc < Synth::numOscillators; ++osc)
        {
            const auto value = [values, osc] (Synth::OscillatorField field) { return values[(size_t) Synth::getParameter (osc, field)]; };
            settings.on[(size_t) osc] = value (Synth::oscOn) > 0.5f;
            settings.warp[(size_t) osc] = choiceOf<Synth::Warp> (value (Synth::oscWarp));
            settings.position[(size_t) osc] = value (Synth::oscPosition);
            settings.pitch[(size_t) osc] = value (Synth::oscPitch) + 0.01f * value (Synth::oscFine);
            settings.level[(size_t) osc] = value (Synth::oscLevel);
            settings.warpAmount[(size_t) osc] = value (Synth::oscWarpAmount);
        }

        settings.subLevel = values[Synth::subLevel];
        settings.subOctaves = choiceOf<int> (values[Synth::subOctave]) + 1;
        settings.subWave = choiceOf<dsp::Waveform> (values[Synth::subShape]);
        settings.noiseLevel = values[Synth::noiseLevel];

        // Unison: each voice's place from one side to the other, which sets its detune and its level on each side, at a
        // level that keeps the note as loud (each the square root of their number down).
        settings.unison = std::clamp (choiceOf<int> (values[Synth::unisonVoices]) + 1, 1, Synth::maxUnison);
        settings.detune = values[Synth::unisonDetune];
        settings.positionSpread = values[Synth::unisonPosition];
        settings.stack = Synth::getStackSteps (choiceOf<Synth::Stack> (values[Synth::unisonStack]));
        settings.drift = values[Synth::unisonDrift];
        settings.stereo = settings.unison > 1 && hasRight;
        const auto norm = 1.0f / std::sqrt ((float) settings.unison);

        for (int u = 0; u < settings.unison; ++u)
        {
            const auto place = settings.unison > 1 ? 2.0f * (float) u / (float) (settings.unison - 1) - 1.0f : 0.0f;
            const auto angle = 0.78539816f * (1.0f + place * values[Synth::unisonSpread]); // 0 to pi/2
            settings.places[(size_t) u] = place;
            settings.lefts[(size_t) u] = settings.stereo ? norm * 1.41421356f * std::cos (angle) : norm;
            settings.rights[(size_t) u] = settings.stereo ? norm * 1.41421356f * std::sin (angle) : norm;
        }

        settings.filterOn = values[Synth::filterOn] > 0.5f;
        settings.filterMode = dsp::toFilterMode (values[Synth::filterType]);
        settings.steep = dsp::isSteep (values[Synth::filterSlope]);
        settings.cutoff = values[Synth::cutoff];
        settings.resonance = values[Synth::resonance];
        settings.driveGain = dsp::decibelsToGain (values[Synth::drive]);
        settings.keyTracking = values[Synth::keyTracking];

        for (int lfo = 0; lfo < 2; ++lfo)
        {
            settings.lfoShapes[(size_t) lfo] = choiceOf<dsp::LfoShape> (values[(size_t) Synth::getLfoParameter (lfo, Synth::lfoShape)]);
            settings.lfoRetrigger[(size_t) lfo] = values[(size_t) Synth::getLfoParameter (lfo, Synth::lfoRetrigger)] > 0.5f;
            settings.lfoFades[(size_t) lfo] =
                (float) (values[(size_t) Synth::getLfoParameter (lfo, Synth::lfoFadeIn)] * 0.001 * sampleRate);
        }

        for (int slot = 0; slot < Synth::numModSlots; ++slot)
        {
            const auto source = choiceOf<Synth::ModSource> (values[(size_t) Synth::getModParameter (slot, Synth::modSource)]);
            const auto target = choiceOf<Synth::ModTarget> (values[(size_t) Synth::getModParameter (slot, Synth::modTarget)]);
            settings.sources[(size_t) slot] = source;
            settings.targets[(size_t) slot] = target;
            settings.amounts[(size_t) slot] = values[(size_t) Synth::getModParameter (slot, Synth::modAmount)];

            if (source != Synth::ModSource::none && target != Synth::ModTarget::none)
            {
                settings.movesAmounts = settings.movesAmounts || isAmountTarget (target);
                settings.wanders[(size_t) slot] = source == Synth::ModSource::drift;
            }
        }

        settings.outputGain = dsp::decibelsToGain (values[Synth::level]);
        settings.bent = bend * values[Synth::bendRange];
        return settings;
    }
} // namespace

//==============================================================================
Synth::Synth()
{
    for (int osc = 0; osc < numOscillators; ++osc)
    {
        owned[(size_t) osc] = getFactoryWavetable (0);
        stepTables[(size_t) osc] = owned[(size_t) osc].get(); // no audio thread yet
        playedPositions[(size_t) osc].store (sawPosition, std::memory_order_relaxed);
    }
}

Synth::~Synth() = default;

void Synth::setTable (int osc, std::shared_ptr<const Wavetable> table)
{
    if (! juce::isPositiveAndBelow (osc, numOscillators))
        return;

    if (table == nullptr)
        table = getFactoryWavetable (0);

    freeRetired();

    if (table == owned[(size_t) osc])
        return;

    // The one given before, if the audio thread never took it, goes at once: so that however many are given while the synth
    // plays nothing (a muted track's), at most the one it took last and the newest wait. If it took it, it reads it until
    // the step under way ends: until the count moves on, the next step taking the new one (a step counts before it takes).
    if (pending[(size_t) osc].exchange (table.get()) == nullptr)
        retired.push_back ({ std::move (owned[(size_t) osc]), stepsBegun.load() + 1 });

    owned[(size_t) osc] = std::move (table);
}

std::shared_ptr<const Wavetable> Synth::getTable (int osc) const
{
    return juce::isPositiveAndBelow (osc, numOscillators) ? owned[(size_t) osc] : nullptr;
}

void Synth::freeRetired()
{
    const auto done = stepsBegun.load();
    retired.erase (std::remove_if (retired.begin(), retired.end(), [done] (const Retired& old) { return done >= old.safeAfter; }),
                   retired.end());
}

double Synth::getFilterMagnitude (std::span<const float> values, double cutoffHertz, double frequency, double rate)
{
    // As render() sets its voices' filters up, before modulation.
    return dsp::getMagnitude (dsp::makeMultimode ({ .mode = dsp::toFilterMode (values[filterType]),
                                                    .frequency = cutoffHertz,
                                                    .resonance = values[resonance],
                                                    .steep = dsp::isSteep (values[filterSlope]),
                                                    .q = dsp::getSynthResonanceQ (values[resonance]) },
                                                  rate),
                              frequency, rate);
}

float Synth::getPlayedPosition (int osc) const noexcept
{
    return juce::isPositiveAndBelow (osc, numOscillators) ? playedPositions[(size_t) osc].load (std::memory_order_relaxed) : 0.0f;
}

void Synth::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;

    // A harmonic may play up to where its alias, folded back from the sample rate, is above 20 kHz.
    aliasLimit = std::clamp (1.0 - 20000.0 / sampleRate, 0.45, 0.9);
    const auto controlRate = sampleRate / controlStep;
    driftWanderSteps = (float) (driftWanderSeconds * controlRate);
    driftFollow = (float) (1.0 - std::exp (-1.0 / (driftGlideSeconds * controlRate)));
    driveFadeStep = (float) std::min (1.0, 1.0 / (driveFadeSeconds * sampleRate));
    for (auto& blocker : dcBlockers)
        blocker.prepare (sampleRate);

    for (auto& voice : voices)
    {
        voice.amp.prepare (sampleRate);
        voice.noise.prepare (sampleRate);
        voice.envelope2.prepare (controlRate);
        voice.envelope3.prepare (controlRate);

        for (auto& lfo : voice.lfos)
            lfo.prepare (controlRate);

        for (auto& filter : voice.filters)
            filter.prepare (dsp::getLongestCombDelay (lowestCutoff, sampleRate));
    }

    for (auto& lfo : freeLfos)
        lfo.prepare (controlRate);

    reset();
}

void Synth::reset()
{
    for (size_t index = 0; index < voices.size(); ++index)
    {
        auto& voice = voices[index];
        voice.amp.reset();
        voice.envelope2.reset();
        voice.envelope3.reset();
        voice.note = -1;
        voice.held = voice.sustained = false;
        voice.stepPending = true;
        voice.fade = 1.0f;
        voice.fadeStep = 0.0f;
        voice.controlAhead = 0;
        voice.hasPlayed = false;
        voice.noise.setSeed (noiseSeed + (std::uint32_t) index);
        voice.phases.setSeed (unisonSeed + (std::uint32_t) index);
        voice.drifting.setSeed (driftSeed + (std::uint32_t) index);
        // Each slot's Drift its own, the first slot's from the seed the one Drift had before each slot had its own.
        for (size_t slot = 0; slot < voice.wanderings.size(); ++slot)
            voice.wanderings[slot].setSeed (wanderSeed + (std::uint32_t) index + (std::uint32_t) slot * 0x9e3779b9u);

        voice.slotWanders = {};

        for (auto& oscillator : voice.oscillators)
            oscillator.wanders = {};

        for (auto& filter : voice.filters)
            filter.reset();
    }

    for (size_t i = 0; i < freeLfos.size(); ++i)
        freeLfos[i].reset (0.0f, lfoSeed + (std::uint32_t) i);

    for (auto& blocker : dcBlockers)
        blocker.reset();

    freeLfoValues.fill (0.0f);
    freeLfosAhead = 0;
    noteRandom.setSeed (randomSeed);
    numHeld = 0;
    notesPlayed = 0;
    grid.reset();
    lastNote = -1.0f;
    bend = modWheel = 0.0f;
    pedalDown = false;
    stoppedPlaying = false;
    shownMode = -1;
}

double Synth::getTailSeconds() const
{
    return (double) tailSeconds.load (std::memory_order_relaxed);
}

//==============================================================================
void Synth::releaseVoice (Voice& voice)
{
    voice.held = voice.sustained = false;
    voice.amp.noteOff();
    voice.envelope2.noteOff();
    voice.envelope3.noteOff();
}

void Synth::letGoOf (int note)
{
    // A key played again while its key or the pedal still holds its voice lets the voice go as its key coming up would: it
    // rings out over its release while the new note takes a voice of its own, and the key's first note-off lets go of that,
    // so that a key never holds two voices (Synth::letGoOf()).
    for (size_t i = 0; i < (size_t) maxVoices; ++i)
        if (auto& voice = voices[i]; voice.note == note && (voice.held || voice.sustained))
            releaseVoice (voice);
}

Synth::Voice& Synth::findVoice()
{
    // A free voice, then the one ringing out longest, then the oldest: among those that play notes, not the places that
    // fade out voices taken (stealVoice()). A key played again as it rings out takes one too, as in most polyphonic synths: its tail
    // rings on untouched, and the new note starts from its waves' starts and its envelopes'.
    const std::span<Voice> playing (voices.data(), (size_t) maxVoices);

    for (auto& voice : playing)
        if (! voice.amp.isActive())
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

void Synth::stealVoice (Voice& voice)
{
    // What it played fades out from a copy of it, over a few milliseconds, while it starts the new note from silence:
    // no jump to the new note at the old one's level. The copy goes to a place that fades nothing, or else to the one
    // nearest silence, which is cut; it reuses that place's memory (every voice is prepared alike), so this doesn't allocate.
    auto* nearest = &voices[(size_t) maxVoices];

    for (auto i = (size_t) maxVoices; i < voices.size() && nearest->amp.isActive(); ++i)
        if (const auto& place = voices[i];
            ! place.amp.isActive() || place.fade * place.amp.getLevel() < nearest->fade * nearest->amp.getLevel())
            nearest = &voices[i];

    auto& fading = *nearest;
    fading = voice;
    fading.note = -1;
    fading.held = fading.sustained = false;
    fading.fadeStep = std::max (fading.fadeStep, (float) (1.0 / (stealFadeSeconds * sampleRate)));
    voice.amp.reset();
    voice.envelope2.reset();
    voice.envelope3.reset();
    voice.fade = 1.0f;
    voice.fadeStep = 0.0f;
}

// A voice that was silent starts from scratch: while the phases reset, its waves too, the first unison voice of each oscillator
// from the start of its wave, the others anywhere, so they don't start in phase and sweep; while they run free (Phase::free),
// its waves go on from where they were when it last played, unless it never has. Its LFOs that start with the note from theirs.
void Synth::startVoice (Voice& voice, bool resets)
{
    if (resets || ! voice.hasPlayed)
    {
        for (auto& oscillator : voice.oscillators)
        {
            for (size_t u = 0; u < oscillator.readers.size(); ++u)
            {
                oscillator.readers[u].phase = u == 0 ? 0.0f : voice.phases.nextUnipolar();
                oscillator.readers[u].last = 0.0f;
            }
        }

        voice.sub.setPhase (0.0f);
    }

    voice.hasPlayed = true;

    for (auto& filter : voice.filters)
        filter.reset();

    for (auto& shaper : voice.drives)
        shaper.reset();

    voice.driveBlend = 0.0f;

    for (size_t i = 0; i < voice.lfos.size(); ++i)
        voice.lfos[i].reset (0.0f, lfoSeed + (std::uint32_t) (notesPlayed * 2 + i));
}

// Each note's unison voices from places of their own, part of the way to their next, each at its own pace: no two notes
// beat alike, as ZynAddSubFX draws its unison again for every note. A voice starts about as far out as its wandering
// keeps it (the mean of two places: gliding, it never gets all the way to one), so that a held note's unison is as wide
// at its start as later.
void Synth::startDrift (Voice& voice) noexcept
{
    for (auto& oscillator : voice.oscillators)
    {
        for (auto& wander : oscillator.wanders)
        {
            wander.now = 0.5f * (voice.drifting.nextBipolar() + voice.drifting.nextBipolar());
            wander.to = voice.drifting.nextBipolar();
            wander.pace = std::exp2 (voice.drifting.nextBipolar());
            wander.stepsLeft = std::max (1, (int) (voice.drifting.nextUnipolar() * driftWanderSteps / wander.pace));
        }
    }
}

bool Synth::movesOn (int& ahead, bool whatever) noexcept
{
    if (! whatever && ahead > controlStep / 2)
        return false;

    ahead += controlStep;
    return true;
}

void Synth::moveDrift (Voice& voice, int unison) noexcept
{
    for (auto& oscillator : voice.oscillators)
    {
        for (int u = 0; u < unison; ++u)
        {
            auto& wander = oscillator.wanders[(size_t) u];

            if (--wander.stepsLeft <= 0)
            {
                wander.to = voice.drifting.nextBipolar();
                wander.stepsLeft = std::max (1, (int) (driftWanderSteps / wander.pace));
            }

            wander.now += (wander.to - wander.now) * std::min (1.0f, driftFollow * wander.pace);
        }
    }
}

// The Drift source, one for each slot, as an analog voice's oscillators and cutoff each wander on their own:
// each note from somewhere of its own, toward a new place every 0.45 s, gliding there with a time constant of 0.3 s; a control step at a time, as Envelopes 2
// and 3 and the LFOs move (movesOn()).
void Synth::startWander (Voice& voice) noexcept
{
    for (size_t slot = 0; slot < voice.slotWanders.size(); ++slot)
    {
        auto& wander = voice.slotWanders[slot];
        wander.now = voice.wanderings[slot].nextBipolar();
        wander.to = voice.wanderings[slot].nextBipolar();
        wander.stepsLeft = std::max (1, juce::roundToInt (driftWanderSteps));
    }
}

void Synth::moveWander (Voice& voice, int slot) noexcept
{
    auto& wander = voice.slotWanders[(size_t) slot];

    if (--wander.stepsLeft <= 0)
    {
        wander.to = voice.wanderings[(size_t) slot].nextBipolar();
        wander.stepsLeft = std::max (1, juce::roundToInt (driftWanderSteps));
    }

    wander.now += (wander.to - wander.now) * driftFollow;
}

bool Synth::isAnotherKeyHeld (int note) const noexcept
{
    for (size_t i = 0; i < (size_t) maxVoices; ++i)
        if (const auto& voice = voices[i]; voice.held && voice.note != note && voice.amp.isActive())
            return true;

    return false;
}

void Synth::startNote (int note, float velocityValue, std::span<const float> values, bool portamento)
{
    const auto sensitivity = values[velocity];
    const auto gain = 1.0f - sensitivity + sensitivity * velocityValue * velocityValue;
    const auto glidesIn = portamento && lastNote >= 0.0f;
    const auto resets = choiceOf<Phase> (values[phase]) == Phase::reset;

    // Glide from the note before: always, or only for a note played over a held one (legato), as a TB-303 slides.
    const auto glides = values[glide] > 0.0f && lastNote >= 0.0f
                        && (choiceOf<GlideMode> (values[glideMode]) == GlideMode::always || isAnotherKeyHeld (note));
    const auto from = glidesIn || glides ? lastNote : (float) note;
    lastNote = (float) note;

    // A portamento note slides in evenly over Glide's time, or a short one while Glide is off.
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

        // A voice still fading out after playback stopped hands that fade to the voice that fades out what is taken.
        if (voice.fadeStep > 0.0f)
            stealVoice (voice);

        const auto legato = voice.amp.isActive() && voice.held;
        voice.note = note;
        voice.target = (float) note;
        voice.held = true;
        voice.sustained = false;

        if (! legato)
        {
            if (! voice.amp.isActive())
                startVoice (voice, resets);

            voice.velocity = velocityValue;
            voice.velocityGain = gain;
            voice.random = noteRandom.nextUnipolar();
            voice.pitch = from;
            voice.started = ++notesPlayed;
            voice.age = 0;
            voice.amp.noteOn();
            voice.envelope2.noteOn();
            voice.envelope3.noteOn();
            voice.stepPending = true;
            startDrift (voice);
            startWander (voice);
        }

        voice.slidePerSample = slideFrom (voice.pitch);
        return;
    }

    letGoOf (note);
    auto& voice = findVoice();

    if (voice.amp.isActive())
        stealVoice (voice);

    startVoice (voice, resets);
    voice.note = note;
    voice.held = true;
    voice.sustained = false;
    voice.velocity = velocityValue;
    voice.velocityGain = gain;
    voice.random = noteRandom.nextUnipolar();
    voice.target = (float) note;
    voice.pitch = from;
    voice.slidePerSample = slideFrom (from);
    voice.started = ++notesPlayed;
    voice.age = 0;
    voice.amp.noteOn();
    voice.envelope2.noteOn();
    voice.envelope3.noteOn();
    voice.stepPending = true;
    voice.controlAhead = 0;
    startDrift (voice);
    startWander (voice);
}

void Synth::stopNote (int note, std::span<const float> values)
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

        // Back to the key held before, if one still is.
        if (numHeld > 0)
        {
            voice.note = heldKeys[(size_t) numHeld - 1];
            voice.target = (float) voice.note;
            voice.slidePerSample = 0.0f;
            lastNote = voice.target;
            return;
        }

        if (pedalDown)
        {
            voice.held = false;
            voice.sustained = true;
        }
        else
        {
            releaseVoice (voice);
        }

        return;
    }

    // The voice its key holds, one at most (letGoOf()): what rings out of the key already rings on, its release not begun
    // again.
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

void Synth::slideTo (int note, float seconds)
{
    // Every voice that sounds, to the slide note's pitch, evenly over its length; a note after it glides from there.
    for (size_t i = 0; i < (size_t) maxVoices; ++i)
    {
        if (auto& voice = voices[i]; voice.amp.isActive() && (voice.held || voice.sustained) && voice.fadeStep <= 0.0f)
        {
            voice.target = (float) note;
            voice.slidePerSample = std::abs (voice.target - voice.pitch) / (float) std::max (1.0, (double) seconds * sampleRate);
        }
    }

    lastNote = (float) note;
}

void Synth::handle (const NoteEvent& event, std::span<const float> values)
{
    switch (event.type)
    {
        case NoteEvent::Type::noteOn:
        case NoteEvent::Type::portamentoOn:
            stoppedPlaying = false;
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

        case NoteEvent::Type::aftertouch: // it doesn't play by pressure
        case NoteEvent::Type::channelPressure:
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
            stoppedPlaying = stoppedPlaying || ! songPlaying;

            // Playback stopped or jumped: every note lets go. Those that had only just begun when it stopped fade out at
            // once too, so that nothing the song began just before Stop sounds after it: the loop's first notes, when Stop
            // came as it went round.
            const auto stopStep = (float) (1.0 / (stopFadeSeconds * sampleRate));

            for (auto& voice : voices)
            {
                if (! voice.amp.isActive())
                    continue;

                releaseVoice (voice);

                if (! songPlaying && voice.age < justBegunSeconds * sampleRate)
                    voice.fadeStep = std::max (voice.fadeStep, stopStep);
            }

            break;
        }
    }
}

//==============================================================================
namespace
{
    // How much each modulation destination is moved, summed over the slots aimed at it.
    using Moves = std::array<float, (size_t) Synth::ModTarget::unisonDetune + 1>;

    float moveOf (const Moves& moves, Synth::ModTarget target) noexcept
    {
        return moves[(size_t) target];
    }
} // namespace

void Synth::render (float* left, float* right, int numSamples, std::span<const float> values, bool playing, std::int64_t songSample)
{
    const auto settings = readSettings (values, bend, right != nullptr, sampleRate);
    const auto glideSamples = (double) values[glide] * 0.001 * sampleRate;
    const auto nyquistLimit = 0.45 * sampleRate;

    // Each oscillator's pitch as a ratio of the other's, for the warps that follow it.
    const std::array<float, numOscillators> otherRatios { std::exp2 ((settings.pitch[1] - settings.pitch[0]) / 12.0f),
                                                          std::exp2 ((settings.pitch[0] - settings.pitch[1]) / 12.0f) };

    grid.startBlock (playing, songSample);

    for (int start = 0; start < numSamples;)
    {
        // Control steps on the song's own grid while it plays, a running count while it is stopped (dsp/ControlGrid.h), so
        // that the modulators and the glide move at their rate however the samples come and however the passage was reached:
        // a step a block's end or an event cuts short goes on in the next stretch.
        const auto stretch = grid.next (numSamples - start);
        const auto newStep = stretch.newStep;
        const auto length = stretch.length;
        const auto untilNextStep = stretch.untilNextStep;
        const auto glideStep = glideSamples <= 1.0 ? 1.0f : (float) (1.0 - std::exp (-untilNextStep / glideSamples));

        if (newStep)
        {
            // The step before is finished: what it read can go (setTable()), and this one takes the tables given since, if any
            // were. Counted first, so that the message thread's count after it gave one is of a step that takes it.
            stepsBegun.fetch_add (1);

            for (size_t osc = 0; osc < stepTables.size(); ++osc)
                if (const auto* given = pending[osc].exchange (nullptr); given != nullptr)
                    stepTables[osc] = given;
        }

        // The LFOs that run free, a step each control step, kept up with the samples played where the grid cuts one short.
        if (newStep && movesOn (freeLfosAhead))
            for (size_t i = 0; i < freeLfos.size(); ++i)
                freeLfoValues[i] = freeLfos[i].next (settings.lfoShapes[i]);

        freeLfosAhead -= length;

        for (auto& voice : voices)
        {
            if (! voice.amp.isActive())
                continue;

            const auto ageAtStretch = voice.age; // the samples since its note began, at this stretch's start
            voice.age = std::min (voice.age + length, 1 << 30);

            //==================================================================
            // Once a control step, and when the voice (re)starts: the pitch, the modulation, the oscillators' frames and
            // levels, the filter and gains. A stretch that goes on with a step keeps them.
            if (newStep || voice.stepPending)
            {
                // Its own control-rate parts (Envelopes 2 and 3, its LFOs, its drift) move a step at each step and as it
                // starts, kept up with the samples it plays where a step runs short (movesOn()).
                const auto movesOnNow = movesOn (voice.controlAhead, voice.stepPending);

                // What the step works out goes there in a straight line over the step (Line); a note's first step starts at
                // its values, as do unison voices and oscillators that come in and a warp chosen anew.
                const auto starts = voice.stepPending;
                const auto previousUnison = voice.unison;
                const auto glideLength = untilNextStep > 1 ? untilNextStep : untilNextStep + controlStep;
                const auto setLine = [glideLength] (Line& line, float value, bool atOnce)
                {
                    if (atOnce)
                        line.jump (value);
                    else
                        line.glide (value, glideLength);
                };

                voice.stepPending = false;
                voice.unison = settings.unison;

                // A slide moves evenly until it gets there, a glide part of the way each step.
                if (voice.slidePerSample > 0.0f)
                {
                    const auto step = voice.slidePerSample * (float) untilNextStep;
                    const auto gap = voice.target - voice.pitch;
                    voice.pitch = std::abs (gap) <= step ? voice.target : voice.pitch + std::copysign (step, gap);
                    voice.slidePerSample = std::abs (gap) <= step ? 0.0f : voice.slidePerSample;
                }
                else
                {
                    voice.pitch += (voice.target - voice.pitch) * glideStep;
                }

                std::array<float, (size_t) ModSource::drift + 1> sources {};
                if (movesOnNow)
                {
                    voice.envelope2Now = voice.envelope2.next();
                    voice.envelope3Now = voice.envelope3.next();

                    for (size_t i = 0; i < voice.lfos.size(); ++i)
                        if (settings.lfoRetrigger[i])
                            voice.lfosNow[i] = voice.lfos[i].next (settings.lfoShapes[i]);

                    // Each slot's Drift, only while the slot plays it.
                    for (int slot = 0; slot < numModSlots; ++slot)
                        if (settings.wanders[(size_t) slot])
                            moveWander (voice, slot);
                }

                sources[(size_t) ModSource::envelope2] = voice.envelope2Now;
                sources[(size_t) ModSource::envelope3] = voice.envelope3Now;

                for (size_t i = 0; i < voice.lfos.size(); ++i)
                {
                    sources[(size_t) ModSource::lfo1 + i] = settings.lfoRetrigger[i] ? voice.lfosNow[i] : freeLfoValues[i];

                    // Each note's share of the LFO comes in from nothing over its fade, from the note's start (a legato note
                    // in mono mode goes on with its voice's), whether the LFO starts with the note or runs on for every
                    // note.
                    if (const auto fade = settings.lfoFades[i]; fade > 0.0f)
                        sources[(size_t) ModSource::lfo1 + i] *= std::min (1.0f, (float) ageAtStretch / fade);
                }

                sources[(size_t) ModSource::velocity] = voice.velocity;
                sources[(size_t) ModSource::note] = (voice.pitch - 60.0f) / 48.0f;
                sources[(size_t) ModSource::modWheel] = modWheel;
                sources[(size_t) ModSource::random] = voice.random;
                // A slot's source, Drift being the slot's own wander.
                const auto sourceOf = [&sources, &settings, &voice] (size_t slot)
                {
                    return settings.sources[slot] == ModSource::drift ? voice.slotWanders[slot].now
                                                                      : sources[(size_t) settings.sources[slot]];
                };

                // The slots that aim at other slots' amounts first, each by its own amount as set, so that nothing loops:
                // one that aims at its own does nothing, and an amount moved stays within its range.
                auto amounts = settings.amounts;

                if (settings.movesAmounts)
                {
                    for (size_t slot = 0; slot < (size_t) numModSlots; ++slot)
                    {
                        const auto target = settings.targets[slot];
                        const auto moved = (size_t) target - (size_t) ModTarget::slot1Amount;

                        if (settings.sources[slot] != ModSource::none && isAmountTarget (target) && moved != slot)
                            amounts[moved] += sourceOf (slot) * settings.amounts[slot];
                    }

                    for (auto& amount : amounts)
                        amount = std::clamp (amount, -1.0f, 1.0f);
                }

                Moves moves {};

                for (size_t slot = 0; slot < (size_t) numModSlots; ++slot)
                    if (settings.sources[slot] != ModSource::none && settings.targets[slot] != ModTarget::none
                        && ! isAmountTarget (settings.targets[slot]))
                        moves[(size_t) settings.targets[slot]] += sourceOf (slot) * amounts[slot];

                const auto basePitch = voice.pitch + settings.bent + pitchRange * moveOf (moves, ModTarget::pitch);
                const auto detune = std::max (0.0f, settings.detune + detuneRange * moveOf (moves, ModTarget::unisonDetune));

                // Each unison voice's pitch as a ratio of its oscillator's, the same for both (docs/archive/performance-audit.md, A4):
                // its place in the detune, and the step of the stack it takes in its turn.
                std::array<double, maxUnison> unisonRatios {};
                const auto stackLength = settings.stack.size();

                for (int u = 0; u < settings.unison; ++u)
                    unisonRatios[(size_t) u] = std::exp2 (
                        ((double) (settings.places[(size_t) u] * detune) * 0.01 + (double) settings.stack[(size_t) u % stackLength])
                        / 12.0);

                // The unison's drift: each voice of each oscillator a little further on its own way.
                const auto drifting = settings.drift > 0.0f;

                if (drifting && movesOnNow)
                    moveDrift (voice, settings.unison);

                for (int osc = 0; osc < numOscillators; ++osc)
                {
                    auto& run = voice.oscillators[(size_t) osc];
                    const auto* table = stepTables[(size_t) osc];
                    const auto wasOn = run.on && ! starts;
                    const auto wasWarp = run.warp;
                    const auto* wasTable = run.table;
                    run.on = settings.on[(size_t) osc] && table != nullptr;

                    if (! run.on)
                        continue;

                    const auto targetOf = [osc] (ModTarget forFirst) { return (ModTarget) ((int) forFirst + osc * 4); };
                    run.warp = settings.warp[(size_t) osc];
                    run.table = table;
                    run.position =
                        std::clamp (settings.position[(size_t) osc] + moveOf (moves, targetOf (ModTarget::osc1Position)), 0.0f, 1.0f);
                    // Sync's modulation reaches past the knob's end, to 16 times (maxSyncAmount).
                    run.warpAmount = std::clamp (settings.warpAmount[(size_t) osc] + moveOf (moves, targetOf (ModTarget::osc1Warp)), 0.0f,
                                                 run.warp == Warp::sync ? maxSyncAmount : 1.0f);
                    run.syncRatio = std::exp2 (3.0f * run.warpAmount);
                    run.bend = run.warp == Warp::bendUp ? 1.0f + 7.0f * run.warpAmount : 1.0f / (1.0f + 7.0f * run.warpAmount);
                    run.pulseWidth = 1.0f - 0.95f * run.warpAmount;

                    // Its level and what its warp reads by glide; a warp chosen anew starts at its value.
                    setLine (run.level,
                             std::clamp (settings.level[(size_t) osc] + moveOf (moves, targetOf (ModTarget::osc1Level)), 0.0f, 1.0f),
                             ! wasOn);

                    const auto shape = [&run]
                    {
                        switch (run.warp)
                        {
                            case Warp::sync:
                                return run.syncRatio;
                            case Warp::bendUp:
                            case Warp::bendDown:
                                return run.bend;
                            case Warp::pulseWidth:
                                return run.pulseWidth;
                            case Warp::mirror:
                            case Warp::fm:
                            case Warp::ringMod:
                                return run.warpAmount;
                            case Warp::none:
                                break;
                        }

                        return 0.0f;
                    }();

                    setLine (run.shape, shape, ! wasOn || wasWarp != run.warp);

                    const auto semitones =
                        (double) (basePitch + settings.pitch[(size_t) osc] + pitchRange * moveOf (moves, targetOf (ModTarget::osc1Pitch)));
                    const auto speed = getWarpSpeed (run.warp, run.warpAmount, otherRatios[(size_t) osc]);

                    // Each unison voice's pitch, then the level that its fastest allows: for each step of the stack its
                    // own, so that the voices on the note keep the harmonics those above it can't play.
                    const auto oscillatorIncrement = dsp::noteToFrequency (semitones) / sampleRate;
                    std::array<double, maxStackSteps> fastest {};

                    for (int u = 0; u < settings.unison; ++u)
                    {
                        auto ratio = unisonRatios[(size_t) u];

                        if (drifting)
                        {
                            // As far off as its wander has taken it: a few cents, so e^x by its first terms is within a
                            // hundredth of a cent.
                            const auto x = (double) (settings.drift * run.wanders[(size_t) u].now) * centsToLog;
                            ratio *= 1.0 + x + 0.5 * x * x;
                        }

                        const auto increment = std::min (0.49, oscillatorIncrement * ratio);
                        setLine (run.readers[(size_t) u].increment, (float) increment, ! wasOn || u >= previousUnison);
                        auto& ofStep = fastest[(size_t) u % stackLength];
                        ofStep = std::max (ofStep, increment);
                    }

                    std::array<int, maxStackSteps> mipLevels {};

                    for (size_t step = 0; step < stackLength; ++step)
                        mipLevels[step] = Wavetable::chooseLevel (aliasLimit / std::max (1.0e-9, fastest[step] * (double) speed));

                    const auto lastFrame = table->getNumFrames() - 1;
                    const auto bendPlace = Wavetable::findBend (run.bend); // once an oscillator: its readers share its bend

                    for (int u = 0; u < settings.unison; ++u)
                    {
                        auto& reader = run.readers[(size_t) u];
                        const auto fresh = ! wasOn || u >= previousUnison;
                        const auto spread = settings.places[(size_t) u] * settings.positionSpread * 0.5f;
                        const auto frame = std::clamp (run.position + spread, 0.0f, 1.0f) * (float) lastFrame;
                        const auto firstFrame = std::min ((int) frame, lastFrame);
                        const auto mipLevel = mipLevels[(size_t) u % stackLength];
                        const auto nextFrame = std::min (firstFrame + 1, lastFrame);
                        const auto morph = frame - (float) firstFrame;

                        // Its place glides as one number, frame and morph, its frame pair looked up again where it crosses
                        // into another (place()), at the level chosen once a step; on another table, it starts there.
                        setLine (reader.frame, frame, fresh || table != wasTable);
                        reader.mipLevel = mipLevel;
                        reader.firstFrame = -1;
                        reader.left = settings.lefts[(size_t) u];
                        reader.right = settings.rights[(size_t) u];

                        // What its warp leaves off centre, which it takes away: every frame is centred over its whole cycle,
                        // but sync reads a part of its last cycle, bend reads the cycle unevenly, and pulse width holds the
                        // wave's start for the rest of the cycle (Wavetable::getArea(), getBentMean()). Worked out where the
                        // step takes its place, and glided to.
                        const auto morphed = [&] (auto&& of) { return (1.0f - morph) * of (firstFrame) + morph * of (nextFrame); };
                        auto offset = 0.0f;

                        switch (run.warp)
                        {
                            case Warp::sync:
                            {
                                const auto cut = run.syncRatio - std::floor (run.syncRatio);
                                offset = morphed ([&] (int each) { return table->getArea (each, mipLevel, cut); }) / run.syncRatio;
                                break;
                            }

                            case Warp::bendUp:
                            case Warp::bendDown:
                                offset = morphed ([&] (int each) { return table->getBentMean (each, mipLevel, bendPlace); });
                                break;

                            case Warp::pulseWidth:
                                offset =
                                    (1.0f - run.pulseWidth)
                                    * readAt (table->getLevel (firstFrame, mipLevel), table->getLevel (nextFrame, mipLevel), morph, 0.0f);
                                break;

                            case Warp::none:
                            case Warp::mirror:
                            case Warp::fm:
                            case Warp::ringMod:
                                break;
                        }

                        setLine (reader.offset, offset, fresh);
                    }
                }

                const auto subGain = std::clamp (settings.subLevel + moveOf (moves, ModTarget::subLevel), 0.0f, 1.0f);
                setLine (voice.subGain, subGain, starts);
                setLine (voice.noiseGain, std::clamp (settings.noiseLevel + moveOf (moves, ModTarget::noiseLevel), 0.0f, 1.0f), starts);

                if (subGain > 0.0f || voice.subGain.now() > 0.0f)
                    voice.sub.setFrequency (dsp::noteToFrequency ((double) basePitch - 12.0 * settings.subOctaves), sampleRate);

                voice.filtering = settings.filterOn;

                if (settings.filterOn)
                {
                    // The same filter on both sides; a comb's delay glides to its new length over the step.
                    const auto moved = settings.keyTracking * (voice.pitch - 60.0f) + cutoffRange * moveOf (moves, ModTarget::cutoff);
                    const auto frequency =
                        std::clamp ((double) settings.cutoff * std::exp2 ((double) moved / 12.0), lowestCutoff, nyquistLimit);
                    const auto filterResonance = settings.resonance + moveOf (moves, ModTarget::resonance);
                    const auto coefficients = dsp::makeMultimode ({ .mode = settings.filterMode,
                                                                    .frequency = frequency,
                                                                    .resonance = filterResonance,
                                                                    .steep = settings.steep,
                                                                    .q = dsp::getSynthResonanceQ (filterResonance) },
                                                                  sampleRate);

                    // Glided to over the step, and after a step of one sample (the grid cut it short), over the next one too.
                    for (auto& side : voice.filters)
                        side.set (coefficients, untilNextStep > 1 ? untilNextStep : untilNextStep + controlStep);

                    voice.driveGain = settings.driveGain;
                    voice.cutoffNow = (float) frequency;
                }

                const auto volume = std::clamp (1.0f + moveOf (moves, ModTarget::volume), 0.0f, 2.0f);
                const auto angle = 0.78539816f * (1.0f + std::clamp (moveOf (moves, ModTarget::pan), -1.0f, 1.0f));
                setLine (voice.gainLeft, volume * 1.41421356f * std::cos (angle), starts);
                setLine (voice.gainRight, volume * 1.41421356f * std::sin (angle), starts);
            }

            voice.controlAhead -= length;

            //==================================================================
            // Each sample: as many unison voices as the step set readers for, spread across the sides if more than one.
            auto& osc1Run = voice.oscillators[0];
            auto& osc2Run = voice.oscillators[1];
            const auto unison = voice.unison;
            const auto stereo = unison > 1 && right != nullptr;

            // Without FM, which reads the other oscillator sample by sample, each oscillator's unison voices play the stretch
            // first (renderStep()), and are mixed after: the same sums, in the same order (docs/archive/performance-audit.md, A6).
            const auto blockwise = ! ((osc1Run.on && osc1Run.warp == Warp::fm) || (osc2Run.on && osc2Run.warp == Warp::fm));
            std::array<float, controlStep> stepLefts {}, stepRights {};

            if (blockwise)
            {
                for (int u = 0; u < unison; ++u)
                {
                    std::array<std::array<float, controlStep>, numOscillators> raws {};

                    for (int osc = 0; osc < numOscillators; ++osc)
                        if (auto& run = voice.oscillators[(size_t) osc]; run.on)
                            renderStep (run, run.readers[(size_t) u], raws[(size_t) osc].data(), length);

                    const auto& panned = osc1Run.on ? osc1Run.readers[(size_t) u] : osc2Run.readers[(size_t) u];

                    for (int i = 0; i < length; ++i)
                    {
                        // Ring modulation: by the other oscillator as it is now.
                        auto out1 = raws[0][(size_t) i], out2 = raws[1][(size_t) i];

                        if (osc1Run.on && osc1Run.warp == Warp::ringMod && osc2Run.on)
                        {
                            const auto amount = osc1Run.shape.at (i + 1);
                            out1 *= 1.0f - amount + amount * raws[1][(size_t) i];
                        }

                        if (osc2Run.on && osc2Run.warp == Warp::ringMod && osc1Run.on)
                        {
                            const auto amount = osc2Run.shape.at (i + 1);
                            out2 *= 1.0f - amount + amount * raws[0][(size_t) i];
                        }

                        const auto sound =
                            (osc1Run.on ? out1 * osc1Run.level.at (i + 1) : 0.0f) + (osc2Run.on ? out2 * osc2Run.level.at (i + 1) : 0.0f);
                        stepLefts[(size_t) i] += sound * panned.left;
                        stepRights[(size_t) i] += sound * panned.right;
                    }

                    if (length > 0)
                    {
                        osc1Run.readers[(size_t) u].last = raws[0][(size_t) length - 1];
                        osc2Run.readers[(size_t) u].last = raws[1][(size_t) length - 1];
                    }
                }
            }

            for (int i = 0; i < length; ++i)
            {
                auto soundLeft = stepLefts[(size_t) i], soundRight = stepRights[(size_t) i];

                for (int u = 0; u < unison && ! blockwise; ++u)
                {
                    std::array<float, numOscillators> raw {};

                    for (int osc = 0; osc < numOscillators; ++osc)
                    {
                        auto& run = voice.oscillators[(size_t) osc];

                        if (! run.on)
                            continue;

                        auto& reader = run.readers[(size_t) u];
                        const auto& other = voice.oscillators[(size_t) (1 - osc)];
                        const auto otherLast = other.on ? other.readers[(size_t) u].last : 0.0f;
                        const auto t = reader.phase, dt = reader.increment.at (i + 1), amount = run.shape.at (i + 1);
                        Synth::place (*run.table, reader, reader.frame.at (i + 1));
                        auto at = t;

                        // What its warp reads by, gliding: the shape's line is sync's ratio, bend's, the pulse width or the
                        // amount.
                        switch (run.warp)
                        {
                            case Warp::sync:
                                at = wrap (t * amount);
                                break;

                            case Warp::bendUp:
                            case Warp::bendDown:
                                at = wrap (t * amount / (1.0f + (amount - 1.0f) * t));
                                break;

                            case Warp::pulseWidth:
                                at = t < amount ? wrap (t / amount) : 0.0f;
                                break;

                            case Warp::fm:
                                at = wrap (t + amount * otherLast);
                                break;

                            case Warp::none:
                            case Warp::mirror:
                            case Warp::ringMod:
                                break;
                        }

                        auto value = readAt (reader.from, reader.to, reader.morph, at);

                        if (run.warp == Warp::mirror && amount > 0.0f)
                        {
                            // Read forward to the middle, then back: heard as much as the amount says.
                            const auto mirrored =
                                readAt (reader.from, reader.to, reader.morph, wrap (t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t));
                            value += amount * (mirrored - value);
                        }
                        else if (run.warp == Warp::sync && (t < dt || t > 1.0f - dt))
                        {
                            // The jump back to the wave's start when the note's cycle ends, smoothed (PolyBLEP).
                            const auto jump = readAt (reader.from, reader.to, reader.morph, 0.0f)
                                              - readAt (reader.from, reader.to, reader.morph, wrap (amount));
                            value += 0.5f * jump * dsp::polyBlep (t, dt);
                        }

                        raw[(size_t) osc] = value - reader.offset.at (i + 1);
                        reader.phase = t + dt < 1.0f ? t + dt : t + dt - 1.0f;
                    }

                    // Ring modulation: by the other oscillator as it is now.
                    auto out1 = raw[0], out2 = raw[1];

                    if (osc1Run.on && osc1Run.warp == Warp::ringMod && osc2Run.on)
                    {
                        const auto amount = osc1Run.shape.at (i + 1);
                        out1 *= 1.0f - amount + amount * raw[1];
                    }

                    if (osc2Run.on && osc2Run.warp == Warp::ringMod && osc1Run.on)
                    {
                        const auto amount = osc2Run.shape.at (i + 1);
                        out2 *= 1.0f - amount + amount * raw[0];
                    }

                    osc1Run.readers[(size_t) u].last = raw[0];
                    osc2Run.readers[(size_t) u].last = raw[1];

                    const auto sound =
                        (osc1Run.on ? out1 * osc1Run.level.at (i + 1) : 0.0f) + (osc2Run.on ? out2 * osc2Run.level.at (i + 1) : 0.0f);
                    const auto& panned = osc1Run.on ? osc1Run.readers[(size_t) u] : osc2Run.readers[(size_t) u];
                    soundLeft += sound * panned.left;
                    soundRight += sound * panned.right;
                }

                // Without unison, one sound in the middle.
                if (! stereo)
                    soundRight = soundLeft;

                // The sub and the noise, in the middle.
                auto centre = 0.0f;

                if (const auto subGain = voice.subGain.at (i + 1); subGain > 0.0f)
                    centre += subGain * voice.sub.next (settings.subWave);

                if (const auto noiseGain = voice.noiseGain.at (i + 1); noiseGain > 0.0f)
                    centre += noiseGain * voice.noise.next();

                soundLeft += centre;
                soundRight += centre;

                // The filter, its drive saturating what goes in (its quiet parts keeping their level) at twice the rate so
                // that it doesn't alias, faded in and out over driveFadeSeconds as it comes on or off: both sides through it
                // while unison spreads the voice, otherwise one.
                const auto channels = stereo ? 2 : 1;
                std::array<float, 2> sides { soundLeft, soundRight };
                const auto driveWanted = voice.filtering && voice.driveGain > 1.0001f ? 1.0f : 0.0f;
                voice.driveBlend += std::clamp (driveWanted - voice.driveBlend, -driveFadeStep, driveFadeStep);

                for (int channel = 0; channel < channels; ++channel)
                {
                    auto x = sides[(size_t) channel];

                    if (voice.filtering)
                    {
                        if (voice.driveBlend > 0.0f)
                        {
                            const auto gain = std::max (1.0f, voice.driveGain);
                            const auto driven = voice.drives[(size_t) channel].process (x * gain, dsp::SoftCurve {}) / gain;
                            x += voice.driveBlend * (driven - x);
                        }

                        x = voice.filters[(size_t) channel].process (x);
                    }

                    sides[(size_t) channel] = x;
                }

                if (! stereo)
                    sides[1] = sides[0];

                const auto gain = voice.amp.next() * voice.velocityGain * settings.outputGain * voice.fade;

                // A voice taken for another note, or cut as playback stopped, fades out; faded, it is free.
                if (voice.fadeStep > 0.0f && (voice.fade -= voice.fadeStep) <= 0.0f)
                {
                    voice.fade = 1.0f;
                    voice.fadeStep = 0.0f;
                    voice.amp.reset();
                }
                left[start + i] += sides[0] * gain * voice.gainLeft.at (i + 1);

                if (right != nullptr)
                    right[start + i] += sides[1] * gain * voice.gainRight.at (i + 1);
            }

            // Its lines move on by the stretch (its readers' in renderStep(), unless FM played them sample by sample).
            for (auto& run : voice.oscillators)
            {
                if (! run.on)
                    continue;

                run.level.advance (length);
                run.shape.advance (length);

                for (int u = 0; u < unison && ! blockwise; ++u)
                {
                    auto& reader = run.readers[(size_t) u];
                    reader.increment.advance (length);
                    reader.frame.advance (length);
                    reader.offset.advance (length);
                }
            }

            voice.subGain.advance (length);
            voice.noiseGain.advance (length);
            voice.gainLeft.advance (length);
            voice.gainRight.advance (length);

            if (! voice.amp.isActive())
                voice.note = -1;
        }

        start += length;
    }
}

void Synth::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;

    if (block.numChannels == 0)
        return;

    songPlaying = block.isPlaying;

    // A switch between poly and mono lets go of every note, so that none is left hanging.
    const auto mode = choiceOf<int> (values[voiceMode]);

    if (mode != shownMode)
    {
        if (shownMode >= 0)
            for (auto& voice : voices)
                if (voice.amp.isActive())
                    releaseVoice (voice);

        numHeld = 0;
        shownMode = mode;
    }

    const auto setEnvelope = [values] (dsp::Adsr& envelope, int number)
    {
        const auto value = [values, number] (EnvelopeField field) { return values[(size_t) getEnvelopeParameter (number, field)]; };
        envelope.setParameters (value (envAttack) * 0.001f, value (envDecay) * 0.001f, value (envSustain), value (envRelease) * 0.001f);
        envelope.setCurves (value (envAttackCurve), value (envDecayCurve), value (envReleaseCurve));
    };

    // The LFOs' rates: in hertz, or a note value at the song's tempo, a bar's as long as its time signature makes it.
    std::array<double, 2> rates {};

    for (size_t i = 0; i < rates.size(); ++i)
    {
        const auto lfo = (int) i;
        const auto synced = values[(size_t) getLfoParameter (lfo, lfoSync)] > 0.5f;
        const auto division =
            std::clamp (choiceOf<int> (values[(size_t) getLfoParameter (lfo, lfoDivision)]), 0, (int) std::size (divisionLengths) - 1);
        rates[i] = synced ? block.tempo / 60.0 / divisionLengths[(size_t) division].inBeats (block.barLength)
                          : (double) values[(size_t) getLfoParameter (lfo, lfoRate)];
        freeLfos[i].setRate (rates[i]);
    }

    for (auto& voice : voices)
    {
        setEnvelope (voice.amp, 0);
        setEnvelope (voice.envelope2, 1);
        setEnvelope (voice.envelope3, 2);

        for (size_t i = 0; i < voice.lfos.size(); ++i)
            voice.lfos[i].setRate (rates[i]);
    }

    tailSeconds.store (values[env1Release] * 0.001f, std::memory_order_relaxed);

    auto* left = block.channels[0];
    auto* right = block.numChannels > 1 ? block.channels[1] : nullptr;
    size_t next = 0;
    const auto sounding = [this]
    { return std::any_of (voices.begin(), voices.end(), [] (const Voice& each) { return each.amp.isActive(); }); };
    const auto soundedBefore = sounding();

    // A stretch at a time, between the events.
    for (int done = 0; done < block.numSamples;)
    {
        while (next < block.events.size() && block.events[next].sample <= done)
            handle (block.events[next++], values);

        const auto until = next < block.events.size() ? std::min (block.numSamples, block.events[next].sample) : block.numSamples;
        render (left + done, right != nullptr ? right + done : nullptr, until - done, values, block.isPlaying,
                block.songSample + (block.isPlaying ? done : 0));
        done = until;
    }

    // Events at the block's end, for the next one.
    while (next < block.events.size())
        handle (block.events[next++], values);

    // Without what ring modulation and FM leave off centre at whole ratios (the product of two waves at one pitch has a part
    // at 0 Hz, as FM's sidebands do): on its sum, one blocker a side (dsp/DcBlocker.h), only while an oscillator that is on
    // has one of those warps, or the filter's drive is on (its curve leaves a lopsided wave off centre, as a saw and a
    // pulse together are). Otherwise its waves are centred as they are made (the other warps by Wavetable::getArea() and
    // getBentMean()), and its sum is left as it is, bit for bit, as the blocker would change its waveform: a slow tail after
    // each note. Once playback has stopped and no note sounds in the block, what the blockers still ring with fades out, so
    // that nothing sounds on after Stop.
    auto wanted = false;

    for (int osc = 0; osc < numOscillators; ++osc)
    {
        const auto warp = choiceOf<Warp> (values[(size_t) getParameter (osc, oscWarp)]);
        wanted = wanted || (values[(size_t) getParameter (osc, oscOn)] > 0.5f && (warp == Warp::fm || warp == Warp::ringMod));
    }
    wanted = wanted || (values[filterOn] > 0.5f && values[drive] > 0.0f);
    const auto quiet = stoppedPlaying && ! soundedBefore && ! sounding();

    for (size_t side = 0; side < (right != nullptr ? 2u : 1u); ++side)
        dcBlockers[side].process (side == 0 ? left : right, block.numSamples, wanted, quiet);

    // What the editor shows: where the newest note plays in each table, or without one where the knob is.
    const Voice* newest = nullptr;
    auto active = 0;

    for (const auto& voice : voices)
    {
        if (! voice.amp.isActive())
            continue;

        ++active;

        if (newest == nullptr || voice.started > newest->started)
            newest = &voice;
    }

    for (int osc = 0; osc < numOscillators; ++osc)
        playedPositions[(size_t) osc].store (newest != nullptr ? newest->oscillators[(size_t) osc].position
                                                               : values[(size_t) getParameter (osc, oscPosition)],
                                             std::memory_order_relaxed);

    // The newest note's envelopes and cutoff, and where each LFO is: the newest note's, for one that starts again with each.
    shown.envelopes[0].set (newest != nullptr ? newest->amp.getLevel() : 0.0f);
    shown.envelopes[1].set (newest != nullptr ? newest->envelope2.getLevel() : 0.0f);
    shown.envelopes[2].set (newest != nullptr ? newest->envelope3.getLevel() : 0.0f);
    shown.cutoff.set (newest != nullptr && newest->filtering ? newest->cutoffNow : 0.0f);

    for (size_t i = 0; i < freeLfos.size(); ++i)
    {
        const auto retriggered = values[(size_t) getLfoParameter ((int) i, lfoRetrigger)] > 0.5f;
        shown.lfoPhases[i].set (retriggered ? (newest != nullptr ? newest->lfos[i].getPhase() : -1.0f) : freeLfos[i].getPhase());
    }

    activeVoices.store (active, std::memory_order_relaxed);
}
} // namespace phazite::devices
