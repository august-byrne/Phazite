// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Synth.h"
#include "devices/dsp/Math.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    constexpr std::string_view waves[] = { "Sine", "Triangle", "Saw", "Square" };
    constexpr std::string_view filterTypes[] = { "Low Pass", "Band Pass", "High Pass" };
    constexpr std::string_view lfoShapes[] = { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random" };
    constexpr std::string_view lfoTargets[] = { "Pitch", "Cutoff", "Level" };
    constexpr std::string_view voiceModes[] = { "Poly", "Mono" };

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

    constexpr ParameterInfo makePercent (std::string_view id, std::string_view name, float defaultValue)
    {
        return { .id = id, .name = name, .defaultValue = defaultValue, .unit = Unit::percent };
    }

    // In the order of Synth::Parameter. The IDs are saved in projects: never change them.
    constexpr ParameterInfo parameters[] = {
        makeChoice ("osc1Wave", "Osc 1 Wave", waves, 2.0f),
        makeChoice ("osc2Wave", "Osc 2 Wave", waves, 2.0f),
        { .id = "osc2Pitch", .name = "Osc 2 Pitch", .minimum = -24.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::semitones },
        { .id = "osc2Detune", .name = "Osc 2 Detune", .minimum = -100.0f, .maximum = 100.0f, .defaultValue = 8.0f, .unit = Unit::cents },
        makePercent ("oscMix", "Osc Mix", 0.5f),
        { .id = "pulseWidth", .name = "Pulse Width", .minimum = 0.05f, .maximum = 0.95f, .defaultValue = 0.5f, .unit = Unit::percent },
        makePercent ("subLevel", "Sub", 0.0f),
        makePercent ("noiseLevel", "Noise", 0.0f),
        makeChoice ("filterType", "Filter Type", filterTypes, 0.0f),
        { .id = "cutoff",
          .name = "Cutoff",
          .minimum = 20.0f,
          .maximum = 20000.0f,
          .defaultValue = 3000.0f,
          .unit = Unit::hertz,
          .midpoint = 632.456f }, // the middle of the range, on a log scale
        makePercent ("resonance", "Resonance", 0.15f),
        { .id = "filterEnvelope",
          .name = "Filter Envelope",
          .minimum = -48.0f,
          .maximum = 48.0f,
          .defaultValue = 24.0f,
          .unit = Unit::semitones },
        makePercent ("keyTracking", "Key Tracking", 0.5f),
        makeTime ("filterAttack", "Filter Attack", 0.0f, 1.0f),
        makeTime ("filterDecay", "Filter Decay", 0.0f, 400.0f),
        makePercent ("filterSustain", "Filter Sustain", 0.2f),
        makeTime ("filterRelease", "Filter Release", 0.0f, 300.0f),
        makeTime ("attack", "Attack", 0.5f, 2.0f), // never instant, so a note doesn't click
        makeTime ("decay", "Decay", 0.0f, 300.0f),
        makePercent ("sustain", "Sustain", 0.8f),
        makeTime ("release", "Release", 0.5f, 200.0f),
        makePercent ("velocity", "Velocity", 0.6f),
        makeChoice ("lfoShape", "LFO Shape", lfoShapes, 0.0f),
        { .id = "lfoRate",
          .name = "LFO Rate",
          .minimum = 0.05f,
          .maximum = 20.0f,
          .defaultValue = 5.0f,
          .unit = Unit::hertz,
          .midpoint = 1.0f },
        makeChoice ("lfoTarget", "LFO Target", lfoTargets, 0.0f),
        makePercent ("lfoDepth", "LFO Depth", 0.0f),
        makeChoice ("voiceMode", "Voices", voiceModes, 0.0f),
        { .id = "glide",
          .name = "Glide",
          .minimum = 0.0f,
          .maximum = 2000.0f,
          .defaultValue = 0.0f,
          .unit = Unit::milliseconds,
          .midpoint = 200.0f },
        { .id = "bendRange", .name = "Bend Range", .minimum = 0.0f, .maximum = 24.0f, .defaultValue = 2.0f, .unit = Unit::semitones },
        { .id = "level", .name = "Level", .minimum = -36.0f, .maximum = 6.0f, .defaultValue = -6.0f, .unit = Unit::decibels },
    };

    // In a mixer slot: the filter, the synth's most played-with controls.
    constexpr std::string_view keyControls[] = { "cutoff", "resonance" };

    constexpr int sustainPedal = 64, modulationWheel = 1; // MIDI controller numbers
    constexpr std::uint32_t noiseSeed = 0x51a7e5u;

    template <typename Enum>
    Enum choiceOf (float value)
    {
        return static_cast<Enum> (std::lround (value));
    }
} // namespace

const DeviceInfo Synth::info { .typeID = typeID,
                               .name = "Synth",
                               .kind = DeviceKind::instrument,
                               .parameters = parameters,
                               .keyControls = keyControls };

void Synth::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;

    for (auto& voice : voices)
    {
        voice.amp.prepare (sampleRate);
        voice.filterEnvelope.prepare (sampleRate);
    }

    lfo.prepare (sampleRate);
    reset();
}

void Synth::reset()
{
    for (size_t index = 0; index < voices.size(); ++index)
    {
        auto& voice = voices[index];
        voice.amp.reset();
        voice.filterEnvelope.reset();
        voice.filter.reset();
        voice.note = -1;
        voice.held = voice.sustained = false;
        voice.filterEnvelopeLevel = 0.0f;
        voice.noise.setSeed (noiseSeed + (std::uint32_t) index);
    }

    lfo.reset();
    numHeld = 0;
    notesPlayed = 0;
    lastNote = -1.0f;
    bend = modWheel = 0.0f;
    pedalDown = false;
    shownMode = -1;
}

double Synth::getTailSeconds() const
{
    return (double) tailSeconds.load (std::memory_order_relaxed);
}

void Synth::releaseVoice (Voice& voice)
{
    voice.held = voice.sustained = false;
    voice.amp.noteOff();
    voice.filterEnvelope.noteOff();
}

Synth::Voice& Synth::findVoiceFor (int note)
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

void Synth::startNote (int note, float velocityValue, std::span<const float> values)
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
            voice.velocityGain = gain;
            voice.pitch = from;
            voice.amp.noteOn();
            voice.filterEnvelope.noteOn();
        }

        return;
    }

    auto& voice = findVoiceFor (note);

    if (! voice.amp.isActive())
    {
        voice.osc1.setPhase (0.0f);
        voice.osc2.setPhase (0.0f);
        voice.sub.setPhase (0.0f);
        voice.filter.reset();
        voice.filterEnvelopeLevel = 0.0f;
    }

    voice.note = note;
    voice.held = true;
    voice.sustained = false;
    voice.velocityGain = gain;
    voice.target = (float) note;
    voice.pitch = from;
    voice.started = ++notesPlayed;
    voice.amp.noteOn();
    voice.filterEnvelope.noteOn();
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

void Synth::handle (const NoteEvent& event, std::span<const float> values)
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

void Synth::render (float* left, float* right, int numSamples, std::span<const float> values)
{
    const auto wave1 = choiceOf<dsp::Waveform> (values[osc1Wave]), wave2 = choiceOf<dsp::Waveform> (values[osc2Wave]);
    const auto mix2 = values[oscMix], width = values[pulseWidth], subGain = values[subLevel], noiseGain = values[noiseLevel];
    const auto type = choiceOf<FilterType> (values[filterType]);
    const auto q = 0.5 * std::pow (50.0, (double) values[resonance]); // from 0.5, flat, to 25, ringing
    const auto outputGain = dsp::decibelsToGain (values[level]);
    const auto lfoAim = choiceOf<LfoTarget> (values[lfoTarget]);
    const auto depth = std::clamp (values[lfoDepth] + modWheel, 0.0f, 1.0f);
    const auto lfoWave = choiceOf<dsp::LfoShape> (values[lfoShape]);
    const auto glideSamples = (double) values[glide] * 0.001 * sampleRate;
    const auto glideStep = glideSamples <= 1.0 ? 1.0f : (float) (1.0 - std::exp (-controlStep / glideSamples));
    const auto bent = bend * values[bendRange];
    const auto osc2Offset = values[osc2Pitch] + values[osc2Detune] * 0.01f;
    const auto nyquistLimit = 0.45 * sampleRate;

    for (int start = 0; start < numSamples; start += controlStep)
    {
        const auto length = std::min (controlStep, numSamples - start);
        std::array<float, controlStep> moved {};

        for (int i = 0; i < length; ++i)
            moved[(size_t) i] = lfo.next (lfoWave);

        const auto pitchMove = lfoAim == LfoTarget::pitch ? depth * moved[0] : 0.0f;
        const auto cutoffMove = lfoAim == LfoTarget::cutoff ? depth * 24.0f * moved[0] : 0.0f;

        for (auto& voice : voices)
        {
            if (! voice.amp.isActive())
                continue;

            // Pitch and filter, once per step.
            voice.pitch += (voice.target - voice.pitch) * glideStep;
            const auto semitones = (double) (voice.pitch + bent + pitchMove);
            const auto frequency = dsp::noteToFrequency (semitones);
            voice.osc1.setFrequency (frequency, sampleRate);
            voice.osc2.setFrequency (dsp::noteToFrequency (semitones + osc2Offset), sampleRate);
            voice.sub.setFrequency (0.5 * frequency, sampleRate);

            const auto moves =
                values[filterEnvelope] * voice.filterEnvelopeLevel + values[keyTracking] * (voice.pitch - 60.0f) + cutoffMove;
            const auto filterFrequency = std::clamp ((double) values[cutoff] * std::pow (2.0, moves / 12.0), 20.0, nyquistLimit);
            voice.coefficients = dsp::makeFilter (dsp::FilterShape::lowPass, filterFrequency, sampleRate, q);

            for (int i = 0; i < length; ++i)
            {
                auto sound = (1.0f - mix2) * voice.osc1.next (wave1, width) + mix2 * voice.osc2.next (wave2, width);

                if (subGain > 0.0f)
                    sound += subGain * voice.sub.next (dsp::Waveform::sine);

                if (noiseGain > 0.0f)
                    sound += noiseGain * voice.noise.nextBipolar();

                const auto outputs = voice.filter.processAll (sound, voice.coefficients);
                const auto filtered = type == FilterType::lowPass
                                          ? outputs.low
                                          : (type == FilterType::bandPass ? outputs.band * voice.coefficients.k : outputs.high);
                voice.filterEnvelopeLevel = voice.filterEnvelope.next();
                auto gain = voice.amp.next() * voice.velocityGain * outputGain;

                if (lfoAim == LfoTarget::level)
                    gain *= 1.0f - depth * 0.5f * (1.0f + moved[(size_t) i]);

                const auto out = filtered * gain;
                left[start + i] += out;

                if (right != nullptr)
                    right[start + i] += out;
            }

            if (! voice.amp.isActive())
                voice.note = -1;
        }
    }
}

void Synth::process (const ProcessContext& block)
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

    for (auto& voice : voices)
    {
        voice.amp.setParameters (values[attack] * 0.001f, values[decay] * 0.001f, values[sustain], values[release] * 0.001f);
        voice.filterEnvelope.setParameters (values[filterAttack] * 0.001f, values[filterDecay] * 0.001f, values[filterSustain],
                                            values[filterRelease] * 0.001f);
    }

    lfo.setRate (values[lfoRate]);
    tailSeconds.store (values[release] * 0.001f, std::memory_order_relaxed);

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

    activeVoices.store ((int) std::count_if (voices.begin(), voices.end(), [] (const Voice& each) { return each.amp.isActive(); }),
                        std::memory_order_relaxed);
}
} // namespace phazite::devices
