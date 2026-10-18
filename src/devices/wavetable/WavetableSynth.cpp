// SPDX-License-Identifier: AGPL-3.0-or-later
#include "WavetableSynth.h"
#include "FactoryWavetables.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    using Synth = WavetableSynth;

    constexpr std::string_view warps[] = { "None", "Sync", "Bend +", "Bend -", "Pulse Width", "Mirror", "FM", "Ring Mod" };
    constexpr std::string_view subOctaves[] = { "-1 Octave", "-2 Octaves" };
    constexpr std::string_view subShapes[] = { "Sine", "Triangle", "Saw", "Square" };
    constexpr std::string_view unisonCounts[] = { "1", "2", "3", "4", "5", "6", "7", "8" };
    constexpr std::string_view lfoShapes[] = { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random" };
    constexpr std::string_view modSources[] = { "None",     "Envelope 2", "Envelope 3", "LFO 1", "LFO 2",
                                                "Velocity", "Note",       "Mod Wheel",  "Random" };
    constexpr std::string_view modTargets[] = { "None",         "Osc 1 Position", "Osc 1 Warp", "Osc 1 Pitch",
                                                "Osc 1 Level",  "Osc 2 Position", "Osc 2 Warp", "Osc 2 Pitch",
                                                "Osc 2 Level",  "Pitch",          "Sub Level",  "Noise Level",
                                                "Cutoff",       "Resonance",      "Volume",     "Pan",
                                                "Unison Detune" };
    constexpr std::string_view voiceModes[] = { "Poly", "Mono" };

    // The note values an LFO in time with the song takes, and their lengths in beats (quarter notes): T for a triplet, D
    // for dotted; a bar is four beats.
    constexpr std::string_view divisions[] = { "1/32", "1/16T", "1/16", "1/16D", "1/8T",  "1/8",    "1/8D",   "1/4T",  "1/4",
                                               "1/4D", "1/2T",  "1/2",  "1/2D",  "1 bar", "2 bars", "4 bars", "8 bars" };
    constexpr double divisionBeats[] = { 0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5, 0.75, 2.0 / 3.0, 1.0,
                                         1.5,   4.0 / 3.0, 2.0,  3.0,   4.0,       8.0, 16.0, 32.0 };
    static_assert (std::size (divisions) == std::size (divisionBeats));

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
                 .midpoint = 1.0f };
    }

    constexpr float sawPosition = 2.0f / 3.0f; // the saw in the first factory table, Basic Shapes
    constexpr float quarterNote = 8.0f;        // among the divisions

    // In the order of WavetableSynth::Parameter. The IDs are saved in projects.
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
        makeToggle ("filterOn", "Filter On", 1.0f),
        makeChoice ("filterType", "Filter Type", dsp::filterModeNames, 0.0f),
        { .id = "cutoff",
          .name = "Filter Cutoff",
          .minimum = 20.0f,
          .maximum = 20000.0f,
          .defaultValue = 2500.0f,
          .unit = Unit::hertz,
          .midpoint = 632.456f }, // the middle of the range, on a log scale
        makePercent ("resonance", "Filter Resonance", 0.1f),
        makeChoice ("filterSlope", "Filter Slope", dsp::filterSlopeNames, 1.0f),
        { .id = "drive", .name = "Filter Drive", .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels },
        makePercent ("keyTracking", "Filter Key Tracking", 0.5f),
        makeTime ("env1Attack", "Envelope 1 Attack", 0.5f, 2.0f), // never instant, so a note doesn't click
        makeTime ("env1Decay", "Envelope 1 Decay", 0.0f, 300.0f),
        makePercent ("env1Sustain", "Envelope 1 Sustain", 0.8f),
        makeTime ("env1Release", "Envelope 1 Release", 0.5f, 200.0f),
        makeTime ("env2Attack", "Envelope 2 Attack", 0.0f, 1.0f),
        makeTime ("env2Decay", "Envelope 2 Decay", 0.0f, 400.0f),
        makePercent ("env2Sustain", "Envelope 2 Sustain", 0.2f),
        makeTime ("env2Release", "Envelope 2 Release", 0.0f, 300.0f),
        makeTime ("env3Attack", "Envelope 3 Attack", 0.0f, 1.0f),
        makeTime ("env3Decay", "Envelope 3 Decay", 0.0f, 800.0f),
        makePercent ("env3Sustain", "Envelope 3 Sustain", 0.0f),
        makeTime ("env3Release", "Envelope 3 Release", 0.0f, 300.0f),
        makeChoice ("lfo1Shape", "LFO 1 Shape", lfoShapes, 0.0f),
        makeRate ("lfo1Rate", "LFO 1 Rate", 2.0f),
        makeToggle ("lfo1Sync", "LFO 1 Sync", 0.0f),
        makeChoice ("lfo1Division", "LFO 1 Division", divisions, quarterNote),
        makeToggle ("lfo1Retrigger", "LFO 1 Retrigger", 1.0f),
        makeChoice ("lfo2Shape", "LFO 2 Shape", lfoShapes, 1.0f),
        makeRate ("lfo2Rate", "LFO 2 Rate", 0.5f),
        makeToggle ("lfo2Sync", "LFO 2 Sync", 0.0f),
        makeChoice ("lfo2Division", "LFO 2 Division", divisions, quarterNote),
        makeToggle ("lfo2Retrigger", "LFO 2 Retrigger", 0.0f),
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
        { .id = "bendRange", .name = "Bend Range", .minimum = 0.0f, .maximum = 24.0f, .defaultValue = 2.0f, .unit = Unit::semitones },
        makePercent ("velocity", "Velocity", 0.6f),
        { .id = "level", .name = "Level", .minimum = -36.0f, .maximum = 6.0f, .defaultValue = -6.0f, .unit = Unit::decibels },
    };

    static_assert (std::size (parameters) == (size_t) Synth::level + 1);
    static_assert (Synth::getParameter (1, Synth::oscWarpAmount) == Synth::osc2WarpAmount);
    static_assert (Synth::getLfoParameter (1, Synth::lfoRetrigger) == Synth::lfo2Retrigger);
    static_assert (Synth::getModParameter (Synth::numModSlots - 1, Synth::modAmount) == Synth::mod8Amount);
    static_assert (std::size (modTargets) == (size_t) Synth::ModTarget::unisonDetune + 1);
    static_assert (std::size (modSources) == (size_t) Synth::ModSource::random + 1);

    // In a mixer slot: where the first oscillator plays in its table, and the filter.
    constexpr std::string_view keyControls[] = { "osc1Position", "cutoff" };

    constexpr int sustainPedal = 64, modulationWheel = 1; // MIDI controller numbers
    constexpr std::uint32_t noiseSeed = 0x7ab1e5u, unisonSeed = 0x0715e7u, lfoSeed = 0x1f0e5u, randomSeed = 0x2a7d0u;

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

const DeviceInfo WavetableSynth::info { .typeID = typeID,
                                        .name = "Wavetable",
                                        .kind = DeviceKind::instrument,
                                        .parameters = parameters,
                                        .keyControls = keyControls };

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
        float detune = 0.0f, positionSpread = 0.0f;
        std::array<float, Synth::maxUnison> places {}, lefts {}, rights {};
        bool stereo = false;
        bool filterOn = false, steep = true;
        dsp::FilterMode filterMode = dsp::FilterMode::lowPass;
        float cutoff = 20000.0f, resonance = 0.0f, driveGain = 1.0f, keyTracking = 0.0f;
        std::array<dsp::LfoShape, 2> lfoShapes {};
        std::array<bool, 2> lfoRetrigger {};
        std::array<Synth::ModSource, Synth::numModSlots> sources {};
        std::array<Synth::ModTarget, Synth::numModSlots> targets {};
        std::array<float, Synth::numModSlots> amounts {};
        float outputGain = 1.0f, bent = 0.0f;
    };

    Settings readSettings (std::span<const float> values, float bend, bool hasRight)
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
        }

        for (int slot = 0; slot < Synth::numModSlots; ++slot)
        {
            settings.sources[(size_t) slot] = choiceOf<Synth::ModSource> (values[(size_t) Synth::getModParameter (slot, Synth::modSource)]);
            settings.targets[(size_t) slot] = choiceOf<Synth::ModTarget> (values[(size_t) Synth::getModParameter (slot, Synth::modTarget)]);
            settings.amounts[(size_t) slot] = values[(size_t) Synth::getModParameter (slot, Synth::modAmount)];
        }

        settings.outputGain = dsp::decibelsToGain (values[Synth::level]);
        settings.bent = bend * values[Synth::bendRange];
        return settings;
    }
} // namespace

//==============================================================================
WavetableSynth::WavetableSynth()
{
    for (int osc = 0; osc < numOscillators; ++osc)
    {
        owned[(size_t) osc] = getFactoryWavetable (0);
        tables[(size_t) osc].store (owned[(size_t) osc].get(), std::memory_order_release);
        playedPositions[(size_t) osc].store (sawPosition, std::memory_order_relaxed);
    }
}

WavetableSynth::~WavetableSynth() = default;

void WavetableSynth::setTable (int osc, std::shared_ptr<const Wavetable> table)
{
    if (! juce::isPositiveAndBelow (osc, numOscillators))
        return;

    if (table == nullptr)
        table = getFactoryWavetable (0);

    freeRetired();
    tables[(size_t) osc].store (table.get(), std::memory_order_release);

    // The audio thread reads the one before until the end of its next block, when it takes the new one.
    if (auto& before = owned[(size_t) osc]; before != nullptr)
        retired.push_back ({ std::move (before), blocksDone.load (std::memory_order_acquire) + 2 });

    owned[(size_t) osc] = std::move (table);
}

std::shared_ptr<const Wavetable> WavetableSynth::getTable (int osc) const
{
    return juce::isPositiveAndBelow (osc, numOscillators) ? owned[(size_t) osc] : nullptr;
}

void WavetableSynth::freeRetired()
{
    const auto done = blocksDone.load (std::memory_order_acquire);
    retired.erase (std::remove_if (retired.begin(), retired.end(), [done] (const Retired& old) { return done >= old.safeAfter; }),
                   retired.end());
}

float WavetableSynth::getPlayedPosition (int osc) const noexcept
{
    return juce::isPositiveAndBelow (osc, numOscillators) ? playedPositions[(size_t) osc].load (std::memory_order_relaxed) : 0.0f;
}

void WavetableSynth::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;

    // A harmonic may play up to where its alias, folded back from the sample rate, is above 20 kHz.
    aliasLimit = std::clamp (1.0 - 20000.0 / sampleRate, 0.45, 0.9);
    const auto controlRate = sampleRate / controlStep;

    for (auto& voice : voices)
    {
        voice.amp.prepare (sampleRate);
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

void WavetableSynth::reset()
{
    for (size_t index = 0; index < voices.size(); ++index)
    {
        auto& voice = voices[index];
        voice.amp.reset();
        voice.envelope2.reset();
        voice.envelope3.reset();
        voice.note = -1;
        voice.held = voice.sustained = false;
        voice.noise.setSeed (noiseSeed + (std::uint32_t) index);
        voice.phases.setSeed (unisonSeed + (std::uint32_t) index);

        for (auto& filter : voice.filters)
            filter.reset();
    }

    for (size_t i = 0; i < freeLfos.size(); ++i)
        freeLfos[i].reset (0.0f, lfoSeed + (std::uint32_t) i);

    freeLfoValues.fill (0.0f);
    noteRandom.setSeed (randomSeed);
    numHeld = 0;
    notesPlayed = 0;
    lastNote = -1.0f;
    bend = modWheel = 0.0f;
    pedalDown = false;
    shownMode = -1;
}

double WavetableSynth::getTailSeconds() const
{
    return (double) tailSeconds.load (std::memory_order_relaxed);
}

//==============================================================================
void WavetableSynth::releaseVoice (Voice& voice)
{
    voice.held = voice.sustained = false;
    voice.amp.noteOff();
    voice.envelope2.noteOff();
    voice.envelope3.noteOff();
}

WavetableSynth::Voice& WavetableSynth::findVoiceFor (int note)
{
    // The voice playing this note already, then a free one, then the one ringing out longest, then the oldest.
    for (auto& voice : voices)
        if (voice.note == note && voice.amp.isActive())
            return voice;

    for (auto& voice : voices)
        if (! voice.amp.isActive())
            return voice;

    Voice* oldest = nullptr;

    for (auto& voice : voices)
        if (! voice.held && ! voice.sustained && (oldest == nullptr || voice.started < oldest->started))
            oldest = &voice;

    if (oldest != nullptr)
        return *oldest;

    for (auto& voice : voices)
        if (oldest == nullptr || voice.started < oldest->started)
            oldest = &voice;

    return *oldest;
}

// A voice that was silent starts its waves from scratch: the first unison voice of each oscillator from the start of its
// wave, the others anywhere, so they don't start in phase and sweep; its LFOs that start with the note from theirs.
void WavetableSynth::startVoice (Voice& voice)
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

    for (auto& filter : voice.filters)
        filter.reset();

    for (size_t i = 0; i < voice.lfos.size(); ++i)
        voice.lfos[i].reset (0.0f, lfoSeed + (std::uint32_t) (notesPlayed * 2 + i));
}

void WavetableSynth::startNote (int note, float velocityValue, std::span<const float> values)
{
    const auto sensitivity = values[velocity];
    const auto gain = 1.0f - sensitivity + sensitivity * velocityValue * velocityValue;
    const auto from = values[glide] > 0.0f && lastNote >= 0.0f ? lastNote : (float) note;
    lastNote = (float) note;

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
        const auto legato = voice.amp.isActive() && voice.held;
        voice.note = note;
        voice.target = (float) note;
        voice.held = true;
        voice.sustained = false;

        if (! legato)
        {
            if (! voice.amp.isActive())
                startVoice (voice);

            voice.velocity = velocityValue;
            voice.velocityGain = gain;
            voice.random = noteRandom.nextUnipolar();
            voice.pitch = from;
            voice.started = ++notesPlayed;
            voice.amp.noteOn();
            voice.envelope2.noteOn();
            voice.envelope3.noteOn();
        }

        return;
    }

    auto& voice = findVoiceFor (note);

    if (! voice.amp.isActive())
        startVoice (voice);

    voice.note = note;
    voice.held = true;
    voice.sustained = false;
    voice.velocity = velocityValue;
    voice.velocityGain = gain;
    voice.random = noteRandom.nextUnipolar();
    voice.target = (float) note;
    voice.pitch = from;
    voice.started = ++notesPlayed;
    voice.amp.noteOn();
    voice.envelope2.noteOn();
    voice.envelope3.noteOn();
}

void WavetableSynth::stopNote (int note, std::span<const float> values)
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

void WavetableSynth::handle (const NoteEvent& event, std::span<const float> values)
{
    switch (event.type)
    {
        case NoteEvent::Type::noteOn:
            startNote (event.number, event.value, values);
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
            numHeld = 0;
            pedalDown = false;

            for (auto& voice : voices)
                if (voice.amp.isActive())
                    releaseVoice (voice);

            break;
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

void WavetableSynth::render (float* left, float* right, int numSamples, std::span<const float> values)
{
    const auto settings = readSettings (values, bend, right != nullptr);
    const auto glideSamples = (double) values[glide] * 0.001 * sampleRate;
    const auto glideStep = glideSamples <= 1.0 ? 1.0f : (float) (1.0 - std::exp (-controlStep / glideSamples));
    const std::array<const Wavetable*, numOscillators> playing { tables[0].load (std::memory_order_acquire),
                                                                 tables[1].load (std::memory_order_acquire) };
    const auto nyquistLimit = 0.45 * sampleRate;

    for (int start = 0; start < numSamples; start += controlStep)
    {
        const auto length = std::min (controlStep, numSamples - start);

        // The LFOs that run free, a step each control step.
        for (size_t i = 0; i < freeLfos.size(); ++i)
            freeLfoValues[i] = freeLfos[i].next (settings.lfoShapes[i]);

        for (auto& voice : voices)
        {
            if (! voice.amp.isActive())
                continue;

            //==================================================================
            // Once a control step: the pitch, the modulation, the oscillators' frames and levels, the filter and gains.
            voice.pitch += (voice.target - voice.pitch) * glideStep;

            std::array<float, (size_t) ModSource::random + 1> sources {};
            sources[(size_t) ModSource::envelope2] = voice.envelope2.next();
            sources[(size_t) ModSource::envelope3] = voice.envelope3.next();

            for (size_t i = 0; i < voice.lfos.size(); ++i)
                sources[(size_t) ModSource::lfo1 + i] =
                    settings.lfoRetrigger[i] ? voice.lfos[i].next (settings.lfoShapes[i]) : freeLfoValues[i];

            sources[(size_t) ModSource::velocity] = voice.velocity;
            sources[(size_t) ModSource::note] = (voice.pitch - 60.0f) / 48.0f;
            sources[(size_t) ModSource::modWheel] = modWheel;
            sources[(size_t) ModSource::random] = voice.random;

            Moves moves {};

            for (size_t slot = 0; slot < (size_t) numModSlots; ++slot)
                if (settings.sources[slot] != ModSource::none && settings.targets[slot] != ModTarget::none)
                    moves[(size_t) settings.targets[slot]] += sources[(size_t) settings.sources[slot]] * settings.amounts[slot];

            const auto basePitch = voice.pitch + settings.bent + pitchRange * moveOf (moves, ModTarget::pitch);
            const auto detune = std::max (0.0f, settings.detune + detuneRange * moveOf (moves, ModTarget::unisonDetune));

            for (int osc = 0; osc < numOscillators; ++osc)
            {
                auto& run = voice.oscillators[(size_t) osc];
                const auto* table = playing[(size_t) osc];
                run.on = settings.on[(size_t) osc] && table != nullptr;

                if (! run.on)
                    continue;

                const auto targetOf = [osc] (ModTarget forFirst) { return (ModTarget) ((int) forFirst + osc * 4); };
                run.warp = settings.warp[(size_t) osc];
                run.position =
                    std::clamp (settings.position[(size_t) osc] + moveOf (moves, targetOf (ModTarget::osc1Position)), 0.0f, 1.0f);
                run.warpAmount =
                    std::clamp (settings.warpAmount[(size_t) osc] + moveOf (moves, targetOf (ModTarget::osc1Warp)), 0.0f, 1.0f);
                run.level = std::clamp (settings.level[(size_t) osc] + moveOf (moves, targetOf (ModTarget::osc1Level)), 0.0f, 1.0f);
                run.syncRatio = std::exp2 (3.0f * run.warpAmount);
                run.bend = run.warp == Warp::bendUp ? 1.0f + 7.0f * run.warpAmount : 1.0f / (1.0f + 7.0f * run.warpAmount);
                run.pulseWidth = 1.0f - 0.95f * run.warpAmount;

                const auto semitones =
                    (double) (basePitch + settings.pitch[(size_t) osc] + pitchRange * moveOf (moves, targetOf (ModTarget::osc1Pitch)));
                const auto other = 1 - osc;
                const auto otherRatio = std::exp2 ((settings.pitch[(size_t) other] - settings.pitch[(size_t) osc]) / 12.0f);
                const auto speed = getWarpSpeed (run.warp, run.warpAmount, otherRatio);

                // Each unison voice's pitch, then the level that its fastest allows.
                auto fastest = 0.0;

                for (int u = 0; u < settings.unison; ++u)
                {
                    const auto cents = (double) (settings.places[(size_t) u] * detune);
                    const auto increment = std::min (0.49, dsp::noteToFrequency (semitones + 0.01 * cents) / sampleRate);
                    run.readers[(size_t) u].increment = (float) increment;
                    fastest = std::max (fastest, increment);
                }

                const auto mipLevel = Wavetable::chooseLevel (aliasLimit / std::max (1.0e-9, fastest * (double) speed));
                const auto lastFrame = table->getNumFrames() - 1;

                for (int u = 0; u < settings.unison; ++u)
                {
                    auto& reader = run.readers[(size_t) u];
                    const auto spread = settings.places[(size_t) u] * settings.positionSpread * 0.5f;
                    const auto frame = std::clamp (run.position + spread, 0.0f, 1.0f) * (float) lastFrame;
                    const auto firstFrame = std::min ((int) frame, lastFrame);
                    reader.from = table->getLevel (firstFrame, mipLevel);
                    reader.to = table->getLevel (std::min (firstFrame + 1, lastFrame), mipLevel);
                    reader.morph = frame - (float) firstFrame;
                    reader.left = settings.lefts[(size_t) u];
                    reader.right = settings.rights[(size_t) u];
                }
            }

            voice.subGain = std::clamp (settings.subLevel + moveOf (moves, ModTarget::subLevel), 0.0f, 1.0f);
            voice.noiseGain = std::clamp (settings.noiseLevel + moveOf (moves, ModTarget::noiseLevel), 0.0f, 1.0f);

            if (voice.subGain > 0.0f)
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
                                                                .q = dsp::getResonanceQ (filterResonance) },
                                                              sampleRate);

                for (auto& side : voice.filters)
                    side.set (coefficients, length);

                voice.driveGain = settings.driveGain;
            }

            const auto volume = std::clamp (1.0f + moveOf (moves, ModTarget::volume), 0.0f, 2.0f);
            const auto angle = 0.78539816f * (1.0f + std::clamp (moveOf (moves, ModTarget::pan), -1.0f, 1.0f));
            voice.gainLeft = volume * 1.41421356f * std::cos (angle);
            voice.gainRight = volume * 1.41421356f * std::sin (angle);

            //==================================================================
            // Each sample.
            auto& osc1Run = voice.oscillators[0];
            auto& osc2Run = voice.oscillators[1];

            for (int i = 0; i < length; ++i)
            {
                auto soundLeft = 0.0f, soundRight = 0.0f;

                for (int u = 0; u < settings.unison; ++u)
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
                        const auto t = reader.phase, dt = reader.increment, amount = run.warpAmount;
                        auto place = t;

                        switch (run.warp)
                        {
                            case Warp::sync:
                                place = wrap (t * run.syncRatio);
                                break;

                            case Warp::bendUp:
                            case Warp::bendDown:
                                place = wrap (t * run.bend / (1.0f + (run.bend - 1.0f) * t));
                                break;

                            case Warp::pulseWidth:
                                place = t < run.pulseWidth ? wrap (t / run.pulseWidth) : 0.0f;
                                break;

                            case Warp::fm:
                                place = wrap (t + amount * otherLast);
                                break;

                            case Warp::none:
                            case Warp::mirror:
                            case Warp::ringMod:
                                break;
                        }

                        auto value = readAt (reader.from, reader.to, reader.morph, place);

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
                                              - readAt (reader.from, reader.to, reader.morph, wrap (run.syncRatio));
                            value += 0.5f * jump * dsp::polyBlep (t, dt);
                        }

                        raw[(size_t) osc] = value;
                        reader.phase = t + dt < 1.0f ? t + dt : t + dt - 1.0f;
                    }

                    // Ring modulation: by the other oscillator as it is now.
                    auto out1 = raw[0], out2 = raw[1];

                    if (osc1Run.on && osc1Run.warp == Warp::ringMod && osc2Run.on)
                        out1 *= 1.0f - osc1Run.warpAmount + osc1Run.warpAmount * raw[1];

                    if (osc2Run.on && osc2Run.warp == Warp::ringMod && osc1Run.on)
                        out2 *= 1.0f - osc2Run.warpAmount + osc2Run.warpAmount * raw[0];

                    osc1Run.readers[(size_t) u].last = raw[0];
                    osc2Run.readers[(size_t) u].last = raw[1];

                    const auto sound = (osc1Run.on ? out1 * osc1Run.level : 0.0f) + (osc2Run.on ? out2 * osc2Run.level : 0.0f);
                    const auto& panned = osc1Run.on ? osc1Run.readers[(size_t) u] : osc2Run.readers[(size_t) u];
                    soundLeft += sound * panned.left;
                    soundRight += sound * panned.right;
                }

                // Without unison, one sound in the middle.
                if (! settings.stereo)
                    soundRight = soundLeft;

                // The sub and the noise, in the middle.
                auto centre = 0.0f;

                if (voice.subGain > 0.0f)
                    centre += voice.subGain * voice.sub.next (settings.subWave);

                if (voice.noiseGain > 0.0f)
                    centre += voice.noiseGain * voice.noise.nextBipolar();

                soundLeft += centre;
                soundRight += centre;

                // The filter, its drive saturating what goes in (its quiet parts keeping their level): both sides through
                // it while unison spreads the voice, otherwise one.
                const auto channels = settings.stereo ? 2 : 1;
                std::array<float, 2> sides { soundLeft, soundRight };

                for (int channel = 0; channel < channels; ++channel)
                {
                    auto x = sides[(size_t) channel];

                    if (voice.filtering)
                    {
                        if (voice.driveGain > 1.0001f)
                            x = dsp::softClip (x * voice.driveGain) / voice.driveGain;

                        x = voice.filters[(size_t) channel].process (x);
                    }

                    sides[(size_t) channel] = x;
                }

                if (! settings.stereo)
                    sides[1] = sides[0];

                const auto gain = voice.amp.next() * voice.velocityGain * settings.outputGain;
                left[start + i] += sides[0] * gain * voice.gainLeft;

                if (right != nullptr)
                    right[start + i] += sides[1] * gain * voice.gainRight;
            }

            if (! voice.amp.isActive())
                voice.note = -1;
        }
    }
}

void WavetableSynth::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;

    if (block.numChannels == 0)
        return;

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

    const auto setEnvelope = [values] (dsp::Adsr& envelope, int attack)
    {
        envelope.setParameters (values[(size_t) attack] * 0.001f, values[(size_t) attack + 1] * 0.001f, values[(size_t) attack + 2],
                                values[(size_t) attack + 3] * 0.001f);
    };

    // The LFOs' rates: in hertz, or a note value at the song's tempo.
    std::array<double, 2> rates {};

    for (size_t i = 0; i < rates.size(); ++i)
    {
        const auto lfo = (int) i;
        const auto synced = values[(size_t) getLfoParameter (lfo, lfoSync)] > 0.5f;
        const auto division =
            std::clamp (choiceOf<int> (values[(size_t) getLfoParameter (lfo, lfoDivision)]), 0, (int) std::size (divisionBeats) - 1);
        rates[i] =
            synced ? block.tempo / 60.0 / divisionBeats[(size_t) division] : (double) values[(size_t) getLfoParameter (lfo, lfoRate)];
        freeLfos[i].setRate (rates[i]);
    }

    for (auto& voice : voices)
    {
        setEnvelope (voice.amp, env1Attack);
        setEnvelope (voice.envelope2, env2Attack);
        setEnvelope (voice.envelope3, env3Attack);

        for (size_t i = 0; i < voice.lfos.size(); ++i)
            voice.lfos[i].setRate (rates[i]);
    }

    tailSeconds.store (values[env1Release] * 0.001f, std::memory_order_relaxed);

    auto* left = block.channels[0];
    auto* right = block.numChannels > 1 ? block.channels[1] : nullptr;
    size_t next = 0;

    // A stretch at a time, between the events.
    for (int done = 0; done < block.numSamples;)
    {
        while (next < block.events.size() && block.events[next].sample <= done)
            handle (block.events[next++], values);

        const auto until = next < block.events.size() ? std::min (block.numSamples, block.events[next].sample) : block.numSamples;
        render (left + done, right != nullptr ? right + done : nullptr, until - done, values);
        done = until;
    }

    // Events at the block's end, for the next one.
    while (next < block.events.size())
        handle (block.events[next++], values);

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

    activeVoices.store (active, std::memory_order_relaxed);
    blocksDone.fetch_add (1, std::memory_order_release);
}
} // namespace phazite::devices
