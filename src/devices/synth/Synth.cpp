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
    constexpr std::string_view subOctaves[] = { "-1 Octave", "-2 Octaves" }; // the wavetable synth's sub's
    constexpr std::string_view lfoShapes[] = { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random" };
    constexpr std::string_view lfoTargets[] = { "Pitch", "Cutoff", "Level", "Pulse Width" };
    constexpr std::string_view voiceModes[] = { "Poly", "Mono" };
    constexpr std::string_view glideModes[] = { "Always", "Legato" };
    constexpr std::string_view unisonCounts[] = { "1", "2", "3", "4", "5", "6", "7" };

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

    constexpr ParameterInfo makeToggle (std::string_view id, std::string_view name, float defaultValue)
    {
        return { .id = id, .name = name, .maximum = 1.0f, .defaultValue = defaultValue, .unit = Unit::toggle };
    }

    // How an envelope's stage bends (dsp::Adsr::setCurves()): from -100 % (fast at first) through 0 (straight) to 100 % (slow at
    // first), an analog envelope's curve at first.
    constexpr ParameterInfo makeCurve (std::string_view id, std::string_view name, float defaultValue)
    {
        return { .id = id, .name = name, .minimum = -1.0f, .maximum = 1.0f, .defaultValue = defaultValue, .unit = Unit::percent };
    }

    // In the order of Synth::Parameter. The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        makeChoice ("osc1Wave", "Osc 1 Wave", waves, 2.0f),
        makeChoice ("osc2Wave", "Osc 2 Wave", waves, 2.0f),
        { .id = "osc2Pitch", .name = "Osc 2 Pitch", .minimum = -24.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::semitones },
        { .id = "osc2Detune", .name = "Osc 2 Detune", .minimum = -100.0f, .maximum = 100.0f, .defaultValue = 8.0f, .unit = Unit::cents },
        makeToggle ("osc2Sync", "Osc 2 Sync", 0.0f),
        { .id = "osc2Envelope",
          .name = "Osc 2 Envelope",
          .minimum = -48.0f,
          .maximum = 48.0f,
          .defaultValue = 0.0f,
          .unit = Unit::semitones },
        makePercent ("oscMix", "Osc Mix", 0.5f),
        { .id = "pulseWidth", .name = "Pulse Width", .minimum = 0.05f, .maximum = 0.95f, .defaultValue = 0.5f, .unit = Unit::percent },
        makePercent ("subLevel", "Sub", 0.0f),
        makeChoice ("subOctave", "Sub Octave", subOctaves, 0.0f),
        makeChoice ("subShape", "Sub Shape", waves, 0.0f), // a sine
        makePercent ("noiseLevel", "Noise", 0.0f),
        makePercent ("drift", "Drift", 0.0f),
        makeChoice ("unisonVoices", "Unison", unisonCounts, 2.0f), // three, heard at once, as the unison is switched on
        { .id = "unisonDetune", .name = "Unison Detune", .maximum = 100.0f, .defaultValue = 25.0f, .unit = Unit::cents },
        { .id = "unisonSpread", .name = "Unison Spread", .defaultValue = 0.8f, .unit = Unit::percent },
        makeChoice ("filterType", "Filter Type", dsp::filterModeNames, 0.0f),
        { .id = "cutoff",
          .name = "Cutoff",
          .minimum = 20.0f,
          .maximum = 20000.0f,
          .defaultValue = 3000.0f,
          .unit = Unit::hertz,
          .midpoint = 632.456f }, // the middle of the range, on a log scale
        makePercent ("resonance", "Resonance", 0.15f),
        makeChoice ("filterSlope", "Filter Slope", dsp::filterSlopeNames, 0.0f), // 12 dB, the synth's gentler sound
        { .id = "drive", .name = "Filter Drive", .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::decibels }, // the wavetable's
        { .id = "filterEnvelope",
          .name = "Filter Envelope",
          .minimum = -48.0f,
          .maximum = 48.0f,
          .defaultValue = 24.0f,
          .unit = Unit::semitones },
        makePercent ("keyTracking", "Key Tracking", 0.5f),
        makePercent ("filterVelocity", "Filter Velocity", 0.0f),
        makeTime ("filterAttack", "Filter Attack", 0.0f, 1.0f),
        makeTime ("filterDecay", "Filter Decay", 0.0f, 400.0f),
        makePercent ("filterSustain", "Filter Sustain", 0.2f),
        makeTime ("filterRelease", "Filter Release", 0.0f, 300.0f),
        makeCurve ("filterAttackCurve", "Filter Attack Curve", dsp::Adsr::analogAttackCurve),
        makeCurve ("filterDecayCurve", "Filter Decay Curve", dsp::Adsr::analogSettleCurve),
        makeCurve ("filterReleaseCurve", "Filter Release Curve", dsp::Adsr::analogSettleCurve),
        makeTime ("attack", "Attack", 0.5f, 2.0f), // never instant, so a note doesn't click
        makeTime ("decay", "Decay", 0.0f, 300.0f),
        makePercent ("sustain", "Sustain", 0.8f),
        makeTime ("release", "Release", 0.5f, 200.0f),
        makeCurve ("attackCurve", "Attack Curve", dsp::Adsr::analogAttackCurve),
        makeCurve ("decayCurve", "Decay Curve", dsp::Adsr::analogSettleCurve),
        makeCurve ("releaseCurve", "Release Curve", dsp::Adsr::analogSettleCurve),
        makeChoice ("lfoShape", "LFO Shape", lfoShapes, 0.0f),
        makeChoice ("lfoTarget", "LFO Target", lfoTargets, 0.0f),
        { .id = "lfoRate",
          .name = "LFO Rate",
          .minimum = 0.05f,
          .maximum = 20.0f,
          .defaultValue = 5.0f,
          .unit = Unit::hertz,
          .midpoint = 1.0f },
        makePercent ("lfoDepth", "LFO Depth", 0.25f), // heard at once, as the LFO is switched on
        makeTime ("lfoFade", "LFO Fade In", 0.0f, 0.0f),
        { .id = "pitchAmount", // the FM synth's pitch envelope
          .name = "Pitch Amount",
          .minimum = -48.0f,
          .maximum = 48.0f,
          .defaultValue = 0.0f,
          .unit = Unit::semitones },
        { .id = "pitchDecay",
          .name = "Pitch Decay",
          .minimum = 1.0f,
          .maximum = 2000.0f,
          .defaultValue = 60.0f,
          .unit = Unit::milliseconds,
          .midpoint = 100.0f },
        makeChoice ("voiceMode", "Voices", voiceModes, 0.0f),
        { .id = "glide",
          .name = "Glide",
          .minimum = 0.0f,
          .maximum = 2000.0f,
          .defaultValue = 0.0f,
          .unit = Unit::milliseconds,
          .midpoint = 200.0f },
        makeChoice ("glideMode", "Glide Mode", glideModes, 0.0f),
        { .id = "bendRange", .name = "Bend Range", .minimum = 0.0f, .maximum = 24.0f, .defaultValue = 2.0f, .unit = Unit::semitones },
        makePercent ("velocity", "Velocity", 0.6f),
        { .id = "level", .name = "Level", .minimum = -36.0f, .maximum = 6.0f, .defaultValue = -6.0f, .unit = Unit::decibels },
        makeToggle ("filterOn", "Filter On", 1.0f),
        makeToggle ("filterEnvelopeOn", "Filter Envelope On", 1.0f),
        makeToggle ("lfoOn", "LFO On", 0.0f),
        makeToggle ("unisonOn", "Unison On", 0.0f),
    };

    static_assert (std::size (parameters) == (size_t) Synth::unisonOn + 1);

    constexpr int sustainPedal = 64, modulationWheel = 1; // MIDI controller numbers
    constexpr std::uint32_t noiseSeed = 0x51a7e5u, unisonSeed = 0x0715e5u, driftSeed = 0xd71f75u;
    constexpr double lowestCutoff = 20.0; // where the filter's envelope, keys and LFO can take it, at the lowest

    // Drift at full: how far each oscillator wanders (in cents) and the cutoff (in semitones), each toward a new place
    // every so often, gliding there, as an analog voice's tuning and filter wander with its temperature.
    constexpr float driftCents = 8.0f, driftSemitones = 2.0f;
    constexpr double driftWanderSeconds = 0.45, driftGlideSeconds = 0.3;

    float wrap (float phase) noexcept
    {
        return phase - std::floor (phase);
    }

    // Osc 2 hard-synced to osc 1: its wave read ratio times each of osc 1's cycles, starting again with each, the jump back
    // to its start smoothed (PolyBLEP) as the wavetable synth's sync smooths it. t and dt are osc 1's phase and its step.
    float readSynced (const dsp::Oscillator& osc, dsp::Waveform wave, float width, float t, float dt, float ratio) noexcept
    {
        auto value = osc.valueAt (wave, wrap (t * ratio), width);

        if (t < dt || t > 1.0f - dt)
        {
            const auto jump = osc.valueAt (wave, 0.0f, width) - osc.valueAt (wave, wrap (ratio), width);
            value += 0.5f * jump * dsp::polyBlep (t, dt);
        }

        return value;
    }

    template <typename Enum>
    Enum choiceOf (float value)
    {
        return static_cast<Enum> (std::lround (value));
    }
} // namespace

const DeviceInfo Synth::info { .typeID = typeID, .name = "Synth", .kind = DeviceKind::instrument, .parameters = parameters };

void Synth::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;

    for (auto& voice : voices)
    {
        voice.amp.prepare (sampleRate);
        voice.filterEnvelope.prepare (sampleRate);
        for (auto& filter : voice.filters)
            filter.prepare (dsp::getLongestCombDelay (lowestCutoff, sampleRate));
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
        voice.note = -1;
        voice.held = voice.sustained = false;
        voice.fade = 1.0f;
        voice.fadeStep = 0.0f;
        voice.noise.setSeed (noiseSeed + (std::uint32_t) index);
        voice.unisonPhases.setSeed (unisonSeed + (std::uint32_t) index);
        voice.drifting.setSeed (driftSeed + (std::uint32_t) index);
        voice.driftNow = voice.driftTo = {};
        voice.driftStepsLeft = 0;
        voice.pitchEnvelope = 0.0f;

        for (auto& filter : voice.filters)
            filter.reset();

        for (auto& shaper : voice.drives)
            shaper.reset();

        voice.driveBlend = 0.0f;
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

void Synth::letGoOf (int note)
{
    // A key played again while its key or the pedal still holds its voice (two keyboards, or the song and a player, can
    // send that) lets the voice go as its key coming up would, as JUCE's synthesiser and Serum 2 under the pedal do: it rings
    // out over its release while the new note takes a voice of its own, and the key's first note-off lets go of that. So a
    // key never holds two voices, which would double it, or pile up under the pedal and take the voices it holds for others.
    for (size_t i = 0; i < (size_t) maxVoices; ++i)
        if (auto& voice = voices[i]; voice.note == note && (voice.held || voice.sustained))
            releaseVoice (voice);
}

Synth::Voice& Synth::findVoice()
{
    // A free voice, then the one ringing out longest, then the oldest: among those that play notes, not the last, which
    // fades out a voice taken (stealVoice()). A key played again as it rings out takes one too, as LMMS's notes and most
    // polyphonic synths' do: its tail rings on untouched, and the new note starts from its waves' starts and its envelopes'.
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
    // no jump to the new note at the old one's level. The copy reuses the last voice's memory (every voice is prepared
    // alike), so this doesn't allocate.
    auto& fading = voices[(size_t) maxVoices];
    fading = voice;
    fading.note = -1;
    fading.held = fading.sustained = false;
    fading.fadeStep = std::max (fading.fadeStep, (float) (1.0 / (stealFadeSeconds * sampleRate)));
    voice.amp.reset();
    voice.filterEnvelope.reset();
    voice.fade = 1.0f;
    voice.fadeStep = 0.0f;
}

bool Synth::isAnotherKeyHeld (int note) const noexcept
{
    for (size_t i = 0; i < (size_t) maxVoices; ++i)
        if (const auto& voice = voices[i]; voice.held && voice.note != note && voice.amp.isActive())
            return true;

    return false;
}

void Synth::startDrift (Voice& voice) noexcept
{
    // Each note from somewhere of its own, as an analog voice is never quite where the last note left it.
    for (size_t i = 0; i < voice.driftNow.size(); ++i)
    {
        voice.driftNow[i] = voice.drifting.nextBipolar();
        voice.driftTo[i] = voice.drifting.nextBipolar();
    }

    voice.driftStepsLeft = std::max (1, (int) (driftWanderSeconds * sampleRate / controlStep));
}

void Synth::moveDrift (Voice& voice) noexcept
{
    if (--voice.driftStepsLeft <= 0)
    {
        for (auto& to : voice.driftTo)
            to = voice.drifting.nextBipolar();

        voice.driftStepsLeft = std::max (1, (int) (driftWanderSeconds * sampleRate / controlStep));
    }

    const auto follow = (float) (1.0 - std::exp (-controlStep / (driftGlideSeconds * sampleRate)));

    for (size_t i = 0; i < voice.driftNow.size(); ++i)
        voice.driftNow[i] += (voice.driftTo[i] - voice.driftNow[i]) * follow;
}

void Synth::startNote (int note, float velocityValue, std::span<const float> values, bool portamento)
{
    const auto sensitivity = values[velocity];
    const auto gain = 1.0f - sensitivity + sensitivity * velocityValue * velocityValue;
    const auto glidesIn = portamento && lastNote >= 0.0f;

    // Glide from the note before: always, or only for a note played over a held one (legato).
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
            voice.velocityGain = gain;
            voice.velocity = velocityValue;
            voice.pitch = from;
            voice.pitchEnvelope = 1.0f;
            voice.age = 0;
            voice.amp.noteOn();
            voice.filterEnvelope.noteOn();
            startDrift (voice);
        }

        voice.slidePerSample = slideFrom (voice.pitch);
        return;
    }

    letGoOf (note);
    auto& voice = findVoice();

    if (voice.amp.isActive())
        stealVoice (voice);

    // Every note from silence: the first oscillators from the start of their waves, as always; the unison's others anywhere,
    // so they don't start in phase and sweep.
    voice.osc1[0].setPhase (0.0f);
    voice.osc2[0].setPhase (0.0f);

    for (size_t u = 1; u < (size_t) maxUnison; ++u)
    {
        voice.osc1[u].setPhase (voice.unisonPhases.nextUnipolar());
        voice.osc2[u].setPhase (voice.unisonPhases.nextUnipolar());
    }

    voice.sub.setPhase (0.0f);

    for (auto& filter : voice.filters)
        filter.reset();

    for (auto& shaper : voice.drives)
        shaper.reset();

    voice.driveBlend = 0.0f;
    voice.filterIsSet = false;
    voice.note = note;
    voice.held = true;
    voice.sustained = false;
    voice.velocityGain = gain;
    voice.velocity = velocityValue;
    voice.target = (float) note;
    voice.pitch = from;
    voice.pitchEnvelope = 1.0f;
    voice.slidePerSample = slideFrom (from);
    voice.started = ++notesPlayed;
    voice.age = 0;
    voice.amp.noteOn();
    voice.filterEnvelope.noteOn();
    startDrift (voice);
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

double Synth::getFilterMagnitude (std::span<const float> values, double cutoffHertz, double frequency, double rate)
{
    // As render() sets its voices' filters up.
    const dsp::FilterSettings settings { .mode = dsp::toFilterMode (values[filterType]),
                                         .frequency = cutoffHertz,
                                         .resonance = values[resonance],
                                         .steep = dsp::isSteep (values[filterSlope]),
                                         .q = 0.5 * std::pow (50.0, (double) values[resonance]) };
    return dsp::getMagnitude (dsp::makeMultimode (settings, rate), frequency, rate);
}

void Synth::render (float* left, float* right, int numSamples, std::span<const float> values)
{
    const auto wave1 = choiceOf<dsp::Waveform> (values[osc1Wave]), wave2 = choiceOf<dsp::Waveform> (values[osc2Wave]);
    const auto mix2 = values[oscMix], width = values[pulseWidth], subGain = values[subLevel], noiseGain = values[noiseLevel];
    const auto subWave = choiceOf<dsp::Waveform> (values[subShape]);
    const auto subRatio = choiceOf<int> (values[subOctave]) == 1 ? 0.25 : 0.5;
    const auto mode = dsp::toFilterMode (values[filterType]);
    const auto steep = dsp::isSteep (values[filterSlope]);
    const auto q = 0.5 * std::pow (50.0, (double) values[resonance]); // a 12 dB section's, the synth's own: 0.5, soft, to 25
    const auto outputGain = dsp::decibelsToGain (values[level]);
    const auto lfoAim = choiceOf<LfoTarget> (values[lfoTarget]);
    // The parts switched off do nothing: the LFO (the mod wheel's depth too), the filter envelope (on the cutoff and on osc
    // 2's pitch), the unison, the filter.
    const auto depth = values[lfoOn] > 0.5f ? std::clamp (values[lfoDepth] + modWheel, 0.0f, 1.0f) : 0.0f;
    const auto envelopeOn = values[filterEnvelopeOn] > 0.5f;
    const auto envelopeAmount = envelopeOn ? values[filterEnvelope] : 0.0f;
    const auto osc2Sweep = envelopeOn ? values[osc2Envelope] : 0.0f;
    const auto filtered = values[filterOn] > 0.5f;
    const auto lfoWave = choiceOf<dsp::LfoShape> (values[lfoShape]);
    const auto fadeSamples = (float) (values[lfoFade] * 0.001 * sampleRate);
    const auto glideSamples = (double) values[glide] * 0.001 * sampleRate;
    const auto bent = bend * values[bendRange];
    const auto osc2Offset = values[osc2Pitch] + values[osc2Detune] * 0.01f;
    const auto synced = values[osc2Sync] > 0.5f;
    const auto pitchSweep = values[pitchAmount];
    const auto pitchFallPerSample = 1.0 / (std::max (1.0f, values[pitchDecay]) * 0.001 * sampleRate);
    const auto driftAmount = values[drift];
    const auto drifting = driftAmount > 0.0f;
    const auto velocityRange = values[filterVelocity] * dsp::filterVelocityRange;
    const auto driveGain = dsp::decibelsToGain (values[drive]);
    const auto driveWanted = filtered && driveGain > 1.0001f ? 1.0f : 0.0f;
    const auto nyquistLimit = 0.45 * sampleRate;

    // Unison: each oscillator's detune (in semitones) and its place between the speakers, spread evenly from one side to
    // the other, at a level that keeps the voice's loudness (each the square root of their number down).
    const auto numUnison = values[unisonOn] > 0.5f ? std::clamp (choiceOf<int> (values[unisonVoices]) + 1, 1, maxUnison) : 1;
    const auto stereo = numUnison > 1 && right != nullptr;
    const auto unisonNorm = 1.0f / std::sqrt ((float) numUnison);
    std::array<float, maxUnison> detunes {}, lefts {}, rights {};

    // Each oscillator's pitch as a ratio of the voice's, worked out once a block rather than at each voice's every step
    // (docs/performance-audit.md, A4).
    std::array<double, maxUnison> ratios1 {}, ratios2 {};
    const auto osc2Ratio = std::exp2 ((double) osc2Offset / 12.0);

    for (int u = 0; u < numUnison; ++u)
    {
        const auto place = numUnison > 1 ? 2.0f * (float) u / (float) (numUnison - 1) - 1.0f : 0.0f; // -1 to 1
        const auto angle = 0.78539816f * (1.0f + place * values[unisonSpread]);                      // 0 to pi/2
        detunes[(size_t) u] = place * values[unisonDetune] * 0.01f;
        lefts[(size_t) u] = unisonNorm * 1.41421356f * std::cos (angle);
        rights[(size_t) u] = unisonNorm * 1.41421356f * std::sin (angle);
        ratios1[(size_t) u] = std::exp2 ((double) detunes[(size_t) u] / 12.0);
        ratios2[(size_t) u] = ratios1[(size_t) u] * osc2Ratio;
    }

    for (int start = 0; start < numSamples; start += controlStep)
    {
        const auto length = std::min (controlStep, numSamples - start);
        std::array<float, controlStep> moved {};

        // The glide moves as far as this stretch's length takes it: a step that a block's end or an event cuts short moves
        // less, so the glide keeps its time however the samples come; and so does the pitch envelope's fall.
        const auto glideStep = glideSamples <= 1.0 ? 1.0f : (float) (1.0 - std::exp (-length / glideSamples));
        const auto pitchFall = ! juce::exactlyEqual (pitchSweep, 0.0f) ? (float) std::exp (-length * pitchFallPerSample) : 0.0f;

        for (int i = 0; i < length; ++i)
            moved[(size_t) i] = lfo.next (lfoWave);

        for (auto& voice : voices)
        {
            if (! voice.amp.isActive())
                continue;

            voice.age = std::min (voice.age + length, 1 << 30);

            // The LFO comes in over its fade after each note starts.
            const auto fadeIn = fadeSamples > 0.0f ? std::min (1.0f, (float) voice.age / fadeSamples) : 1.0f;
            const auto voiceDepth = depth * fadeIn;
            const auto pitchMove = lfoAim == LfoTarget::pitch ? depth * moved[0] * fadeIn : 0.0f;
            const auto cutoffMove = lfoAim == LfoTarget::cutoff ? depth * 24.0f * moved[0] * fadeIn : 0.0f;
            const auto widthMove = lfoAim == LfoTarget::pulseWidth ? voiceDepth * 0.45f : 0.0f;

            // Pitch and filter, once per step: a slide moves evenly until it gets there, a glide part of the way each step.
            if (voice.slidePerSample > 0.0f)
            {
                const auto step = voice.slidePerSample * (float) length;
                const auto gap = voice.target - voice.pitch;
                voice.pitch = std::abs (gap) <= step ? voice.target : voice.pitch + std::copysign (step, gap);
                voice.slidePerSample = std::abs (gap) <= step ? 0.0f : voice.slidePerSample;
            }
            else
            {
                voice.pitch += (voice.target - voice.pitch) * glideStep;
            }

            const auto sweep = pitchSweep * voice.pitchEnvelope;
            voice.pitchEnvelope *= pitchFall;
            const auto semitones = (double) (voice.pitch + bent + pitchMove + sweep);
            const auto frequency = dsp::noteToFrequency (semitones);

            // Each oscillator's own drift, and osc 2's sweep by the filter envelope, from where the envelope is now: at its
            // top as a note with no attack starts.
            if (drifting)
                moveDrift (voice);

            const auto envelopeLevel = voice.filterEnvelope.getLevel();

            const auto frequency1 = drifting ? frequency * std::exp2 (driftAmount * driftCents / 1200.0f * voice.driftNow[0]) : frequency;
            auto frequency2 = drifting ? frequency * std::exp2 (driftAmount * driftCents / 1200.0f * voice.driftNow[1]) : frequency;

            if (! juce::exactlyEqual (osc2Sweep, 0.0f))
                frequency2 *= std::exp2 (osc2Sweep * envelopeLevel / 12.0f);

            for (int u = 0; u < numUnison; ++u)
            {
                voice.osc1[(size_t) u].setFrequency (frequency1 * ratios1[(size_t) u], sampleRate);
                voice.osc2[(size_t) u].setFrequency (frequency2 * ratios2[(size_t) u], sampleRate);
            }

            // Synced, osc 2 plays this many of its cycles in each of osc 1's (for every unison voice alike).
            const auto syncRatio = synced ? (float) (osc2Ratio * frequency2 / frequency1) : 1.0f;
            voice.sub.setFrequency (subRatio * frequency1, sampleRate);

            auto moves = envelopeAmount * envelopeLevel + values[keyTracking] * (voice.pitch - 60.0f) + cutoffMove;

            if (velocityRange > 0.0f)
                moves += velocityRange * (voice.velocity - 1.0f);

            if (drifting)
                moves += driftAmount * driftSemitones * voice.driftNow[2];

            const auto filterFrequency = std::clamp ((double) values[cutoff] * std::pow (2.0, moves / 12.0), lowestCutoff, nyquistLimit);

            // The same filter on both sides; a comb's delay glides to its new length over the step. Made again only when its
            // settings change: a held note's often don't.
            const dsp::FilterSettings filterSettings { .mode = mode,
                                                       .frequency = filterFrequency,
                                                       .resonance = values[resonance],
                                                       .steep = steep,
                                                       .q = q };

            if (! voice.filterIsSet || ! dsp::areSame (filterSettings, voice.filterSettings))
            {
                const auto coefficients = dsp::makeMultimode (filterSettings, sampleRate);

                for (auto& filter : voice.filters)
                    filter.set (coefficients, length);

                voice.filterSettings = filterSettings;
                voice.filterIsSet = true;
            }

            // One unison voice's two oscillators, osc 2 read in step with osc 1 while it is synced.
            const auto play = [&voice, synced, mix2, wave1, wave2, syncRatio] (size_t u, float pulse)
            {
                auto& osc1 = voice.osc1[u];
                auto& osc2 = voice.osc2[u];

                if (! synced)
                    return (1.0f - mix2) * osc1.next (wave1, pulse) + mix2 * osc2.next (wave2, pulse);

                const auto t = osc1.getPhase(), dt = osc1.getIncrement();
                const auto first = osc1.next (wave1, pulse);
                return (1.0f - mix2) * first + mix2 * readSynced (osc2, wave2, pulse, t, dt, syncRatio);
            };

            for (int i = 0; i < length; ++i)
            {
                // The oscillators (spread across the sides while unison is stereo), then the sub and the noise in the middle.
                auto mono = 0.0f, soundLeft = 0.0f, soundRight = 0.0f;
                const auto pulse = widthMove > 0.0f ? width + widthMove * moved[(size_t) i] : width;

                if (numUnison == 1)
                {
                    mono = play (0, pulse);
                }
                else
                {
                    for (int u = 0; u < numUnison; ++u)
                    {
                        const auto sound = play ((size_t) u, pulse);
                        mono += sound * unisonNorm;
                        soundLeft += sound * lefts[(size_t) u];
                        soundRight += sound * rights[(size_t) u];
                    }
                }

                if (subGain > 0.0f)
                {
                    const auto sub = subGain * voice.sub.next (subWave);
                    mono += sub;
                    soundLeft += sub;
                    soundRight += sub;
                }

                if (noiseGain > 0.0f)
                {
                    const auto noise = noiseGain * voice.noise.nextBipolar();
                    mono += noise;
                    soundLeft += noise;
                    soundRight += noise;
                }

                voice.filterEnvelope.next();
                auto gain = voice.amp.next() * voice.velocityGain * outputGain * voice.fade;

                // A voice taken for another note, or cut as playback stopped, fades out; faded, it is free.
                if (voice.fadeStep > 0.0f && (voice.fade -= voice.fadeStep) <= 0.0f)
                {
                    voice.fade = 1.0f;
                    voice.fadeStep = 0.0f;
                    voice.amp.reset();
                }

                if (lfoAim == LfoTarget::level)
                    gain *= 1.0f - voiceDepth * 0.5f * (1.0f + moved[(size_t) i]);

                // The filter, its drive saturating what goes in (its quiet parts keeping their level) at twice the rate so
                // that it doesn't alias, faded in and out over 32 samples as it comes on or off, as the wavetable synth's.
                voice.driveBlend += std::clamp (driveWanted - voice.driveBlend, -1.0f / 32.0f, 1.0f / 32.0f);

                const auto through = [&voice, filtered, driveGain] (size_t side, float x)
                {
                    if (! filtered)
                        return x;

                    if (voice.driveBlend > 0.0f)
                    {
                        const auto boost = std::max (1.0f, driveGain);
                        const auto driven = voice.drives[side].process (x * boost, dsp::SoftCurve {}) / boost;
                        x += voice.driveBlend * (driven - x);
                    }

                    return voice.filters[side].process (x);
                };

                if (! stereo)
                {
                    // One filter, the same sound on both sides, as before unison.
                    const auto out = through (0, mono) * gain;
                    left[start + i] += out;

                    if (right != nullptr)
                        right[start + i] += out;

                    continue;
                }

                left[start + i] += through (0, soundLeft) * gain;
                right[start + i] += through (1, soundRight) * gain;
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

    for (auto& voice : voices)
    {
        voice.amp.setParameters (values[attack] * 0.001f, values[decay] * 0.001f, values[sustain], values[release] * 0.001f);
        voice.amp.setCurves (values[attackCurve], values[decayCurve], values[releaseCurve]);
        voice.filterEnvelope.setParameters (values[filterAttack] * 0.001f, values[filterDecay] * 0.001f, values[filterSustain],
                                            values[filterRelease] * 0.001f);
        voice.filterEnvelope.setCurves (values[filterAttackCurve], values[filterDecayCurve], values[filterReleaseCurve]);
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

    // For the editor: the newest voice, and the LFO.
    const Voice* newest = nullptr;

    for (const auto& voice : voices)
        if (voice.amp.isActive() && (newest == nullptr || voice.started > newest->started))
            newest = &voice;

    shown.amp.set (newest != nullptr ? newest->amp.getLevel() : 0.0f);
    shown.filterEnvelope.set (newest != nullptr ? newest->filterEnvelope.getLevel() : 0.0f);
    shown.cutoff.set (newest != nullptr && newest->filterIsSet ? (float) newest->filterSettings.frequency : 0.0f);
    shown.lfoPhase.set (lfo.getPhase());
}
} // namespace phazite::devices
