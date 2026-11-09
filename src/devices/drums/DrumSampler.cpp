// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumSampler.h"
#include "FactoryDrums.h"
#include "devices/dsp/Math.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace phazite::devices
{
namespace
{
    using Field = DrumSampler::Field;
    using FilterType = DrumSampler::FilterType;
    constexpr int numPads = DrumSampler::numPads;
    constexpr int fieldsPerPad = DrumSampler::fieldsPerPad;

    constexpr std::string_view chokeGroups[] = { "Off", "1", "2", "3", "4", "5", "6", "7", "8" };
    constexpr std::string_view modes[] = { "One Shot", "Gate" };
    constexpr std::string_view filterTypes[] = { "Off", "Low Pass", "High Pass", "Band Pass" };
    constexpr std::string_view layerModes[] = { "Stack", "Velocity", "Round Robin", "Random" };

    // Each field's ID and name, after its pad's note: saved in projects ("pad36Level") and shown ("Pad 36 Level").
    constexpr std::string_view fieldIDs[] = {
        "Level",     "Pan",         "Tune",        "Fine",        "Start",      "Reverse",  "Attack",           "Hold",  "Decay", "Mode",
        "Sweep",     "SweepTime",   "Filter",      "Cutoff",      "Resonance",  "Velocity", "VelocityToCutoff", "Choke", "Mute",  "Solo",
        "LayerMode", "Layer1Level", "Layer2Level", "Layer3Level", "Layer4Level"
    };
    constexpr std::string_view fieldNames[] = { "Level",
                                                "Pan",
                                                "Tune",
                                                "Fine",
                                                "Start",
                                                "Reverse",
                                                "Attack",
                                                "Hold",
                                                "Decay",
                                                "Mode",
                                                "Sweep",
                                                "Sweep Time",
                                                "Filter",
                                                "Cutoff",
                                                "Resonance",
                                                "Velocity",
                                                "Velocity to Cutoff",
                                                "Choke",
                                                "Mute",
                                                "Solo",
                                                "Layer Mode",
                                                "Layer 1 Level",
                                                "Layer 2 Level",
                                                "Layer 3 Level",
                                                "Layer 4 Level" };
    static_assert (std::size (fieldIDs) == fieldsPerPad && std::size (fieldNames) == fieldsPerPad);

    // A short text made as the program compiles, so that the parameters' IDs and names have static storage.
    struct Text
    {
        std::array<char, 32> chars {};
        std::size_t size = 0;

        constexpr void append (std::string_view text)
        {
            for (const auto c : text)
                chars[size++] = c;
        }

        constexpr void append (int number)
        {
            std::array<char, 4> digits {};
            auto count = 0;

            do
            {
                digits[(size_t) count++] = (char) ('0' + number % 10);
                number /= 10;
            } while (number > 0);

            while (count > 0)
                chars[size++] = digits[(size_t) --count];
        }

        constexpr std::string_view view() const { return { chars.data(), size }; }
    };

    constexpr auto makeTexts (std::string_view before, std::string_view between, const std::string_view (&fields)[fieldsPerPad])
    {
        std::array<Text, (size_t) numPads * fieldsPerPad> made {};

        for (int pad = 0; pad < numPads; ++pad)
        {
            for (int field = 0; field < fieldsPerPad; ++field)
            {
                auto& text = made[(size_t) (pad * fieldsPerPad + field)];
                text.append (before);
                text.append (pad);
                text.append (between);
                text.append (fields[field]);
            }
        }

        return made;
    }

    constexpr auto ids = makeTexts ("pad", "", fieldIDs);
    constexpr auto names = makeTexts ("Pad ", " ", fieldNames);

    constexpr double fadeSeconds = 0.01;     // how long a cut or choked sound takes to stop
    constexpr double justBegunSeconds = 0.1; // a hit this young when playback stops is cut
    constexpr float silent = 0.001f;         // -60 dB: where a decay ends
    constexpr int kitChokeGroup = 1;         // the hats, which stop each other

    constexpr bool isHat (int pad)
    {
        return pad == 42 || pad == 44 || pad == 46; // closed, pedal and open, on their General MIDI notes
    }

    constexpr auto makeParameters()
    {
        std::array<ParameterInfo, DrumSampler::outputParameter + 1> made {};

        for (int pad = 0; pad < numPads; ++pad)
        {
            const auto set = [&made, pad] (Field field, ParameterInfo parameter)
            {
                const auto index = (size_t) DrumSampler::getParameterIndex (pad, field);
                parameter.id = ids[index].view();
                parameter.name = names[index].view();
                made[index] = parameter;
            };

            set (Field::level, { .minimum = -48.0f, .maximum = 12.0f, .defaultValue = 0.0f, .unit = Unit::decibels });
            set (Field::pan, { .minimum = -1.0f, .maximum = 1.0f, .unit = Unit::pan });
            set (Field::tune, { .minimum = -24.0f, .maximum = 24.0f, .defaultValue = 0.0f, .unit = Unit::semitones });
            set (Field::fine, { .minimum = -50.0f, .maximum = 50.0f, .defaultValue = 0.0f, .unit = Unit::cents });
            set (Field::start, { .minimum = 0.0f, .maximum = 0.95f, .defaultValue = 0.0f, .unit = Unit::percent });
            set (Field::reverse, { .maximum = 1.0f, .unit = Unit::toggle });
            set (Field::attack,
                 { .minimum = 0.0f, .maximum = 200.0f, .defaultValue = 0.0f, .unit = Unit::milliseconds, .midpoint = 20.0f });
            set (Field::hold, { .minimum = 0.0f,
                                .maximum = DrumSampler::wholeSound,
                                .defaultValue = DrumSampler::wholeSound,
                                .unit = Unit::milliseconds,
                                .midpoint = 300.0f });
            set (Field::decay,
                 { .minimum = 1.0f, .maximum = 5000.0f, .defaultValue = 200.0f, .unit = Unit::milliseconds, .midpoint = 200.0f });
            set (Field::mode, { .maximum = 1.0f, .unit = Unit::choice, .choices = modes });
            set (Field::sweep, { .minimum = -48.0f, .maximum = 48.0f, .defaultValue = 0.0f, .unit = Unit::semitones });
            set (Field::sweepTime,
                 { .minimum = 1.0f, .maximum = 2000.0f, .defaultValue = 50.0f, .unit = Unit::milliseconds, .midpoint = 100.0f });
            set (Field::filter, { .maximum = 3.0f, .unit = Unit::choice, .choices = filterTypes });
            set (Field::cutoff,
                 { .minimum = 20.0f, .maximum = 20000.0f, .defaultValue = 1000.0f, .unit = Unit::hertz, .midpoint = 1000.0f });
            set (Field::resonance, { .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::percent });
            set (Field::velocity, { .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::percent });
            set (Field::velocityToCutoff, { .minimum = 0.0f, .maximum = 48.0f, .defaultValue = 0.0f, .unit = Unit::semitones });
            set (Field::choke, { .maximum = 8.0f,
                                 .defaultValue = isHat (pad) ? (float) kitChokeGroup : 0.0f,
                                 .unit = Unit::choice,
                                 .choices = chokeGroups });
            set (Field::mute, { .maximum = 1.0f, .unit = Unit::toggle });
            set (Field::solo, { .maximum = 1.0f, .unit = Unit::toggle });
            set (Field::layerMode, { .maximum = 3.0f, .unit = Unit::choice, .choices = layerModes });

            for (int layer = 0; layer < DrumSampler::numLayers; ++layer)
                set ((Field) (Field::layer1Level + layer),
                     { .minimum = -48.0f, .maximum = 12.0f, .defaultValue = 0.0f, .unit = Unit::decibels });
        }

        made[DrumSampler::outputParameter] = { .id = "output",
                                               .name = "Output",
                                               .minimum = -24.0f,
                                               .maximum = 6.0f,
                                               .defaultValue = 0.0f,
                                               .unit = Unit::decibels };
        return made;
    }

    constexpr auto parameters = makeParameters();

    // "pad", a pad's note (without a 0 before it), then a field's ID; or the output's.
    int findByID (std::string_view id)
    {
        if (id == parameters[DrumSampler::outputParameter].id)
            return DrumSampler::outputParameter;

        if (! id.starts_with ("pad") || id.size() < 4 || id[3] < '0' || id[3] > '9'
            || (id[3] == '0' && id.size() > 4 && id[4] >= '0' && id[4] <= '9'))
            return -1;

        auto pad = 0;
        size_t at = 3;

        for (; at < id.size() && at < 6 && id[at] >= '0' && id[at] <= '9'; ++at)
            pad = pad * 10 + (id[at] - '0');

        if (pad >= numPads)
            return -1;

        const auto field = id.substr (at);

        for (int each = 0; each < fieldsPerPad; ++each)
            if (fieldIDs[each] == field)
                return DrumSampler::getParameterIndex (pad, (Field) each);

        return -1;
    }

    bool isSoloing (std::span<const float> values)
    {
        for (int pad = 0; pad < numPads; ++pad)
            if (values[(size_t) DrumSampler::getParameterIndex (pad, Field::solo)] > 0.5f)
                return true;

        return false;
    }

    bool isSilenced (std::span<const float> values, int pad, bool soloing)
    {
        return values[(size_t) DrumSampler::getParameterIndex (pad, Field::mute)] > 0.5f
               || (soloing && values[(size_t) DrumSampler::getParameterIndex (pad, Field::solo)] < 0.5f);
    }
} // namespace

const DeviceInfo DrumSampler::info { .typeID = typeID,
                                     .name = "Drum Sampler",
                                     .kind = DeviceKind::instrument,
                                     .parameters = parameters,
                                     .findByID = findByID };

DrumSampler::DrumSampler()
{
    for (int pad = 0; pad < numPads; ++pad)
    {
        playing[(size_t) pad][0] = getDefaultSound (pad);
        sounds[(size_t) pad][0].store (playing[(size_t) pad][0].get(), std::memory_order_release);
        playheads[(size_t) pad].set (-1.0f);
    }
}

DrumSampler::~DrumSampler() = default;

std::shared_ptr<const DrumSampler::Sound> DrumSampler::getDefaultSound (int pad)
{
    return pad >= kitFirstNote && pad < kitFirstNote + kitPads ? getFactoryDrum (pad - kitFirstNote) : nullptr;
}

void DrumSampler::setSound (int pad, int layer, std::shared_ptr<const Sound> sound)
{
    if (! juce::isPositiveAndBelow (pad, numPads) || ! juce::isPositiveAndBelow (layer, numLayers))
        return;

    freeRetired();
    auto& played = playing[(size_t) pad][(size_t) layer];
    given[(size_t) pad][(size_t) layer] = sound != nullptr;

    if (sound == nullptr && layer == 0)
        sound = getDefaultSound (pad);

    if (sound == played)
        return;

    sounds[(size_t) pad][(size_t) layer].store (sound.get(), std::memory_order_release);

    // The audio thread may play the one before until the end of its next block, when its voices notice the change.
    retired.push_back ({ std::move (played), blocksDone.load (std::memory_order_acquire) + 2 });
    played = std::move (sound);
}

bool DrumSampler::hasOwnSound (int pad, int layer) const noexcept
{
    return juce::isPositiveAndBelow (pad, numPads) && juce::isPositiveAndBelow (layer, numLayers) && given[(size_t) pad][(size_t) layer];
}

void DrumSampler::freeRetired()
{
    const auto done = blocksDone.load (std::memory_order_acquire);
    retired.erase (std::remove_if (retired.begin(), retired.end(), [done] (const Retired& old) { return done >= old.safeAfter; }),
                   retired.end());
}

void DrumSampler::audition (int pad, float hitVelocity)
{
    const auto scope = auditionFifo.write (1);

    if (scope.blockSize1 > 0)
        auditions[(size_t) scope.startIndex1] = { pad, hitVelocity, juce::Time::getMillisecondCounter() };
}

bool DrumSampler::isWaitingToPlay() const noexcept
{
    return auditionFifo.getNumReady() > 0;
}

void DrumSampler::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    reset();
}

void DrumSampler::reset()
{
    for (auto& voice : voices)
        voice = Voice {};
}

double DrumSampler::getTailSeconds() const
{
    // The longest sound a pad plays.
    auto longest = 0.0;

    for (const auto& layers : sounds)
        for (const auto& sound : layers)
            if (const auto* each = sound.load (std::memory_order_acquire))
                longest = std::max (longest, each->audio.getNumSamples() / each->sampleRate);

    return longest;
}

void DrumSampler::hit (int pad, float hitVelocity, std::span<const float> values)
{
    const auto at = [&values, pad] (Field field) { return values[(size_t) getParameterIndex (pad, field)]; };

    // Its layers that have a sound, in their order.
    std::array<int, numLayers> layers {};
    std::array<const Sound*, numLayers> layerSounds {};
    auto found = 0;

    for (int layer = 0; layer < numLayers; ++layer)
    {
        if (const auto* sound = sounds[(size_t) pad][(size_t) layer].load (std::memory_order_acquire);
            sound != nullptr && sound->audio.getNumSamples() > 0)
        {
            layers[(size_t) found] = layer;
            layerSounds[(size_t) found++] = sound;
        }
    }

    if (found == 0 || isSilenced (values, pad, isSoloing (values)))
        return;

    // Its choke group stops the others in it.
    if (const auto group = (int) std::lround (at (Field::choke)); group > 0)
        for (auto& voice : voices)
            if (voice.pad >= 0 && voice.pad != pad
                && (int) std::lround (values[(size_t) getParameterIndex (voice.pad, Field::choke)]) == group)
                voice.fadeStep = std::max (voice.fadeStep, (float) (1.0 / (fadeSeconds * sampleRate)));

    // Its layers share the hit as its mode says: all at once, or one of them.
    hitVelocity = std::clamp (hitVelocity, 0.0f, 1.0f);
    auto chosen = 0;

    switch ((LayerMode) std::clamp ((int) std::lround (at (Field::layerMode)), 0, (int) LayerMode::random))
    {
        case LayerMode::stack:
            for (int each = 0; each < found; ++each)
                startVoice (pad, layers[(size_t) each], *layerSounds[(size_t) each], hitVelocity, values);

            padMeters[(size_t) pad].raise (hitVelocity);
            return;

        case LayerMode::velocity:
            chosen = std::min (found - 1, (int) (hitVelocity * (float) found));
            break;

        case LayerMode::roundRobin:
            chosen = nextLayer[(size_t) pad] % found;
            nextLayer[(size_t) pad] = (std::uint8_t) ((chosen + 1) % found);
            break;

        case LayerMode::random:
            chosen = (int) (random.nextInt() % (std::uint32_t) std::max (1, found - 1));

            if (found > 1 && chosen >= lastLayer[(size_t) pad] % found)
                ++chosen; // never the one before

            lastLayer[(size_t) pad] = (std::uint8_t) chosen;
            break;
    }

    startVoice (pad, layers[(size_t) chosen], *layerSounds[(size_t) chosen], hitVelocity, values);
    padMeters[(size_t) pad].raise (hitVelocity);
}

void DrumSampler::startVoice (int pad, int layer, const Sound& sound, float hitVelocity, std::span<const float> values)
{
    const auto at = [&values, pad] (Field field) { return values[(size_t) getParameterIndex (pad, field)]; };

    // A free voice, or the oldest.
    Voice* chosen = nullptr;

    for (auto& voice : voices)
        if (voice.pad < 0 || chosen == nullptr || (chosen->pad >= 0 && voice.started < chosen->started))
            chosen = &voice;

    auto& voice = *chosen;
    const auto toSamples = [this] (float milliseconds) { return (int) std::lround ((double) milliseconds * 0.001 * sampleRate); };

    voice.pad = pad;
    voice.layer = layer;
    voice.sound = &sound;
    voice.velocity = hitVelocity;
    voice.reversed = at (Field::reverse) > 0.5f;
    voice.position = std::floor ((double) at (Field::start) * sound.audio.getNumSamples());
    voice.step = sound.sampleRate / sampleRate * std::exp2 (((double) at (Field::tune) + at (Field::fine) / 100.0) / 12.0);
    voice.played = 0;
    voice.attackSamples = toSamples (at (Field::attack));
    voice.holdSamples = at (Field::hold) >= wholeSound ? std::numeric_limits<int>::max() / 2 : toSamples (at (Field::hold));
    voice.decayStep = (float) std::exp (std::log ((double) silent) / std::max (1.0, (double) at (Field::decay) * 0.001 * sampleRate));
    voice.level = 1.0f;
    voice.decaying = false;
    voice.released = false;
    voice.fade = 1.0f;
    voice.fadeStep = 0.0f;
    voice.pitchOffset = at (Field::sweep);
    voice.pitchFall = std::exp (-1.0 / std::max (1.0, (double) at (Field::sweepTime) * 0.001 * sampleRate));

    voice.filterType = FilterType::off;
    voice.ic1 = {};
    voice.ic2 = {};
    applyValues (voice, values);
    voice.started = ++hits;
}

void DrumSampler::applyValues (Voice& voice, std::span<const float> values)
{
    const auto at = [&values, &voice] (Field field) { return values[(size_t) getParameterIndex (voice.pad, field)]; };

    // Its level and its layer's, as much quieter as its velocity is softer as the pad's velocity says, and its pan.
    const auto gain = dsp::decibelsToGain (at (Field::level) + at ((Field) (Field::layer1Level + voice.layer)))
                      * (1.0f - at (Field::velocity) * (1.0f - voice.velocity));
    const auto panning = at (Field::pan);
    voice.gainLeft = gain * std::min (1.0f, 1.0f - panning);
    voice.gainRight = gain * std::min (1.0f, 1.0f + panning);

    // Its filter, its cutoff closed by as many semitones as velocity takes off it; a filter switched on as it plays starts
    // from silence.
    const auto type = (FilterType) std::clamp ((int) std::lround (at (Field::filter)), 0, (int) FilterType::bandPass);

    if (type != voice.filterType)
    {
        voice.filterType = type;
        voice.ic1 = {};
        voice.ic2 = {};
    }

    if (type == FilterType::off)
        return;

    const auto closed = (double) at (Field::velocityToCutoff) * (1.0 - voice.velocity);
    const auto hertz = std::clamp ((double) at (Field::cutoff) * std::exp2 (-closed / 12.0), 20.0, 0.45 * sampleRate);
    voice.g = (float) std::tan (juce::MathConstants<double>::pi * hertz / sampleRate);
    voice.k = 2.0f - 1.9f * at (Field::resonance); // 1 / Q, from 0.5 to 10
    voice.a1 = 1.0f / (1.0f + voice.g * (voice.g + voice.k));
    voice.a2 = voice.g * voice.a1;
    voice.a3 = voice.g * voice.a2;
}

void DrumSampler::release (int pad)
{
    // A gated pad's note ended: its hits decay from where they are.
    for (auto& voice : voices)
    {
        if (voice.pad == pad && ! voice.released)
        {
            voice.released = true;
            voice.decaying = true;
        }
    }
}

void DrumSampler::follow (std::span<const float> values)
{
    const auto soloing = isSoloing (values);

    for (auto& voice : voices)
    {
        if (voice.pad < 0)
            continue;

        if (isSilenced (values, voice.pad, soloing))
            voice.fadeStep = std::max (voice.fadeStep, (float) (1.0 / (fadeSeconds * sampleRate)));

        applyValues (voice, values);
    }
}

void DrumSampler::render (float* left, float* right, int numSamples, std::span<const PartOutput> outputs, int outputStart)
{
    for (auto& voice : voices)
    {
        if (voice.pad < 0)
            continue;

        // A pad on a channel of its own plays into its output, the others into the sampler's channels.
        auto* intoLeft = left;
        auto* intoRight = right;

        for (const auto& output : outputs)
        {
            if (output.part == voice.pad)
            {
                intoLeft = output.channels[0] + outputStart;
                intoRight = output.channels[1] + outputStart;
                break;
            }
        }

        const auto& audio = voice.sound->audio;
        const auto frames = audio.getNumSamples();
        const auto* first = audio.getReadPointer (0);
        const auto* second = audio.getReadPointer (std::min (1, audio.getNumChannels() - 1));

        for (int i = 0; i < numSamples; ++i)
        {
            if (voice.position >= frames || voice.fade <= 0.0f)
            {
                voice.pad = -1;
                break;
            }

            // Its attack, then its hold at full level, then its decay (a gated hit's from its note's end, even in its attack);
            // and a choke's fade.
            if (! voice.decaying)
            {
                voice.level = voice.played < voice.attackSamples ? (float) voice.played / (float) voice.attackSamples : 1.0f;
                voice.decaying = voice.played >= voice.attackSamples + voice.holdSamples;
            }

            if (voice.decaying)
            {
                voice.level *= voice.decayStep;

                if (voice.level < silent)
                {
                    voice.pad = -1;
                    break;
                }
            }

            const auto envelope = voice.level * voice.fade;

            // Swept from above (or below) its pitch back to it.
            auto step = voice.step;

            if (! juce::exactlyEqual (voice.pitchOffset, 0.0))
            {
                step *= std::exp2 (voice.pitchOffset / 12.0);
                voice.pitchOffset = std::abs (voice.pitchOffset) < 0.001 ? 0.0 : voice.pitchOffset * voice.pitchFall;
            }

            // At its own rate and untuned, its samples as they are, forwards or back; otherwise read between them by a windowed
            // sinc.
            auto sampleLeft = 0.0f, sampleRight = 0.0f;
            const auto at = voice.reversed ? frames - 1 - voice.position : voice.position;

            if (juce::exactlyEqual (step, 1.0))
            {
                const auto index = (int) at;
                sampleLeft = first[index];
                sampleRight = second[index];
            }
            else
            {
                reader.read (first, second, frames, at, step, sampleLeft, sampleRight);
            }

            // Its filter (a state-variable filter, as Cytomic's), each side through its own state.
            if (voice.filterType != FilterType::off)
            {
                const auto throughFilter = [&voice] (float in, size_t side)
                {
                    const auto v3 = in - voice.ic2[side];
                    const auto v1 = voice.a1 * voice.ic1[side] + voice.a2 * v3;
                    const auto v2 = voice.ic2[side] + voice.a2 * voice.ic1[side] + voice.a3 * v3;
                    voice.ic1[side] = 2.0f * v1 - voice.ic1[side];
                    voice.ic2[side] = 2.0f * v2 - voice.ic2[side];

                    if (voice.filterType == FilterType::lowPass)
                        return v2;

                    if (voice.filterType == FilterType::bandPass)
                        return voice.k * v1; // as loud as it was in its middle, however resonant

                    return in - voice.k * v1 - v2;
                };

                sampleLeft = throughFilter (sampleLeft, 0);
                sampleRight = throughFilter (sampleRight, 1);
            }

            intoLeft[i] += sampleLeft * voice.gainLeft * envelope;

            if (intoRight != nullptr)
                intoRight[i] += sampleRight * voice.gainRight * envelope;

            voice.position += step;
            voice.fade -= voice.fadeStep;
            ++voice.played;
        }
    }
}

void DrumSampler::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    const auto& values = block.parameters;

    if (block.numChannels == 0)
        return;

    // A pad whose sound changed stops: its voice can't read the old one any more. The others follow their pads' values as
    // they are now (automation, a knob turned); those of pads muted, or left out by a solo, fade.
    for (auto& voice : voices)
        if (voice.pad >= 0 && sounds[(size_t) voice.pad][(size_t) voice.layer].load (std::memory_order_acquire) != voice.sound)
            voice.pad = -1;

    follow (values);

    // Pads the editor played, unless one waited too long for a block, as while the sampler was off. JUCE's approximate
    // clock (an atomic read) can lag behind, never run ahead: a lag only lets a pad play.
    const auto now = juce::Time::getApproximateMillisecondCounter();
    const auto playAudition = [this, now, &values] (const Audition& waiting)
    {
        if (juce::isPositiveAndBelow (waiting.pad, numPads) && (int) (now - waiting.playedAt) <= longestAuditionWait)
            hit (waiting.pad, waiting.velocity, values);
    };
    const auto scope = auditionFifo.read (auditionFifo.getNumReady());

    for (int i = 0; i < scope.blockSize1; ++i)
        playAudition (auditions[(size_t) (scope.startIndex1 + i)]);

    for (int i = 0; i < scope.blockSize2; ++i)
        playAudition (auditions[(size_t) (scope.startIndex2 + i)]);

    auto* left = block.channels[0];
    auto* right = block.numChannels > 1 ? block.channels[1] : nullptr;
    size_t next = 0;

    // A stretch at a time, between the notes; a one-shot plays on whole whatever its note, a gated pad decays at its end.
    for (int done = 0; done < block.numSamples;)
    {
        for (; next < block.events.size() && block.events[next].sample <= done; ++next)
        {
            const auto& event = block.events[next];
            const auto pad = event.number - firstNote;

            const auto isNoteOn = event.type == NoteEvent::Type::noteOn || event.type == NoteEvent::Type::portamentoOn;

            if (isNoteOn && juce::isPositiveAndBelow (pad, numPads))
                hit (pad, event.value, values);

            if (event.type == NoteEvent::Type::noteOff && juce::isPositiveAndBelow (pad, numPads)
                && (Mode) std::lround (values[(size_t) getParameterIndex (pad, Field::mode)]) == Mode::gate)
                release (pad);

            // Playback stopped: hits that had only just begun fade out, so that nothing the song began just before Stop
            // sounds after it (the loop's first hits, when Stop came as it went round). Older hits ring on.
            if (event.type == NoteEvent::Type::allNotesOff && ! block.isPlaying)
                for (auto& voice : voices)
                    if (voice.pad >= 0 && voice.played < justBegunSeconds * sampleRate)
                        voice.fadeStep = std::max (voice.fadeStep, (float) (1.0 / (fadeSeconds * sampleRate)));
        }

        const auto until = next < block.events.size() ? std::min (block.numSamples, block.events[next].sample) : block.numSamples;
        render (left + done, right != nullptr ? right + done : nullptr, until - done, block.partOutputs, block.outputOffset + done);
        done = until;
    }

    // The output's level, over the whole block.
    const auto output = dsp::decibelsToGain (values[outputParameter]);

    if (! juce::exactlyEqual (output, 1.0f))
        for (int channel = 0; channel < std::min (block.numChannels, 2); ++channel)
            juce::FloatVectorOperations::multiply (block.channels[channel], output, block.numSamples);

    // Where each pad's newest hit is, for the editor.
    std::array<const Voice*, numPads> newest {};

    for (const auto& voice : voices)
        if (voice.pad >= 0 && (newest[(size_t) voice.pad] == nullptr || voice.started > newest[(size_t) voice.pad]->started))
            newest[(size_t) voice.pad] = &voice;

    for (size_t pad = 0; pad < numPads; ++pad)
    {
        const auto* voice = newest[pad];
        const auto frames = voice != nullptr ? voice->sound->audio.getNumSamples() : 0;
        playheads[pad].set (frames > 0 ? (float) (voice->position / frames) : -1.0f);
    }

    blocksDone.fetch_add (1, std::memory_order_release);
}
} // namespace phazite::devices
