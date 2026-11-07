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
    constexpr int numPads = DrumSampler::numPads;

    constexpr std::string_view chokeGroups[] = { "Off", "1", "2", "3", "4" };

    // The pads' parameter IDs, saved in projects, and names.
    constexpr std::string_view levelIDs[] = { "pad1Level",  "pad2Level",  "pad3Level",  "pad4Level",  "pad5Level",  "pad6Level",
                                              "pad7Level",  "pad8Level",  "pad9Level",  "pad10Level", "pad11Level", "pad12Level",
                                              "pad13Level", "pad14Level", "pad15Level", "pad16Level" };
    constexpr std::string_view panIDs[] = { "pad1Pan", "pad2Pan",  "pad3Pan",  "pad4Pan",  "pad5Pan",  "pad6Pan",  "pad7Pan",  "pad8Pan",
                                            "pad9Pan", "pad10Pan", "pad11Pan", "pad12Pan", "pad13Pan", "pad14Pan", "pad15Pan", "pad16Pan" };
    constexpr std::string_view tuneIDs[] = { "pad1Tune",  "pad2Tune",  "pad3Tune",  "pad4Tune",  "pad5Tune",  "pad6Tune",
                                             "pad7Tune",  "pad8Tune",  "pad9Tune",  "pad10Tune", "pad11Tune", "pad12Tune",
                                             "pad13Tune", "pad14Tune", "pad15Tune", "pad16Tune" };
    constexpr std::string_view attackIDs[] = { "pad1Attack",  "pad2Attack",  "pad3Attack",  "pad4Attack",  "pad5Attack",  "pad6Attack",
                                               "pad7Attack",  "pad8Attack",  "pad9Attack",  "pad10Attack", "pad11Attack", "pad12Attack",
                                               "pad13Attack", "pad14Attack", "pad15Attack", "pad16Attack" };
    constexpr std::string_view lengthIDs[] = { "pad1Length",  "pad2Length",  "pad3Length",  "pad4Length",  "pad5Length",  "pad6Length",
                                               "pad7Length",  "pad8Length",  "pad9Length",  "pad10Length", "pad11Length", "pad12Length",
                                               "pad13Length", "pad14Length", "pad15Length", "pad16Length" };
    constexpr std::string_view chokeIDs[] = { "pad1Choke",  "pad2Choke",  "pad3Choke",  "pad4Choke",  "pad5Choke",  "pad6Choke",
                                              "pad7Choke",  "pad8Choke",  "pad9Choke",  "pad10Choke", "pad11Choke", "pad12Choke",
                                              "pad13Choke", "pad14Choke", "pad15Choke", "pad16Choke" };

    constexpr std::string_view levelNames[] = { "Pad 1 Level",  "Pad 2 Level",  "Pad 3 Level",  "Pad 4 Level",
                                                "Pad 5 Level",  "Pad 6 Level",  "Pad 7 Level",  "Pad 8 Level",
                                                "Pad 9 Level",  "Pad 10 Level", "Pad 11 Level", "Pad 12 Level",
                                                "Pad 13 Level", "Pad 14 Level", "Pad 15 Level", "Pad 16 Level" };
    constexpr std::string_view panNames[] = { "Pad 1 Pan",  "Pad 2 Pan",  "Pad 3 Pan",  "Pad 4 Pan",  "Pad 5 Pan",  "Pad 6 Pan",
                                              "Pad 7 Pan",  "Pad 8 Pan",  "Pad 9 Pan",  "Pad 10 Pan", "Pad 11 Pan", "Pad 12 Pan",
                                              "Pad 13 Pan", "Pad 14 Pan", "Pad 15 Pan", "Pad 16 Pan" };
    constexpr std::string_view tuneNames[] = { "Pad 1 Tune",  "Pad 2 Tune",  "Pad 3 Tune",  "Pad 4 Tune",  "Pad 5 Tune",  "Pad 6 Tune",
                                               "Pad 7 Tune",  "Pad 8 Tune",  "Pad 9 Tune",  "Pad 10 Tune", "Pad 11 Tune", "Pad 12 Tune",
                                               "Pad 13 Tune", "Pad 14 Tune", "Pad 15 Tune", "Pad 16 Tune" };
    constexpr std::string_view attackNames[] = { "Pad 1 Attack",  "Pad 2 Attack",  "Pad 3 Attack",  "Pad 4 Attack",
                                                 "Pad 5 Attack",  "Pad 6 Attack",  "Pad 7 Attack",  "Pad 8 Attack",
                                                 "Pad 9 Attack",  "Pad 10 Attack", "Pad 11 Attack", "Pad 12 Attack",
                                                 "Pad 13 Attack", "Pad 14 Attack", "Pad 15 Attack", "Pad 16 Attack" };
    constexpr std::string_view lengthNames[] = { "Pad 1 Length",  "Pad 2 Length",  "Pad 3 Length",  "Pad 4 Length",
                                                 "Pad 5 Length",  "Pad 6 Length",  "Pad 7 Length",  "Pad 8 Length",
                                                 "Pad 9 Length",  "Pad 10 Length", "Pad 11 Length", "Pad 12 Length",
                                                 "Pad 13 Length", "Pad 14 Length", "Pad 15 Length", "Pad 16 Length" };
    constexpr std::string_view chokeNames[] = { "Pad 1 Choke",  "Pad 2 Choke",  "Pad 3 Choke",  "Pad 4 Choke",
                                                "Pad 5 Choke",  "Pad 6 Choke",  "Pad 7 Choke",  "Pad 8 Choke",
                                                "Pad 9 Choke",  "Pad 10 Choke", "Pad 11 Choke", "Pad 12 Choke",
                                                "Pad 13 Choke", "Pad 14 Choke", "Pad 15 Choke", "Pad 16 Choke" };

    constexpr float longestLength = 10000.0f; // ms: a length this long plays the whole sound
    constexpr double fadeSeconds = 0.01;      // how long a cut or choked sound takes to stop
    constexpr double justBegunSeconds = 0.1;  // a hit this young when playback stops is cut
    constexpr int kitChokeGroup = 1;          // the hats, which stop each other

    constexpr bool isHat (int pad)
    {
        return pad == 6 || pad == 8 || pad == 10; // closed, pedal and open
    }

    constexpr auto makeParameters()
    {
        std::array<ParameterInfo, DrumSampler::outputParameter + 1> made {};

        for (int pad = 0; pad < numPads; ++pad)
        {
            const auto at = [pad] (Field field) { return (size_t) DrumSampler::getParameterIndex (pad, field); };
            made[at (Field::level)] = { .id = levelIDs[pad],
                                        .name = levelNames[pad],
                                        .minimum = -48.0f,
                                        .maximum = 12.0f,
                                        .defaultValue = 0.0f,
                                        .unit = Unit::decibels };
            made[at (Field::pan)] = { .id = panIDs[pad], .name = panNames[pad], .minimum = -1.0f, .maximum = 1.0f, .unit = Unit::pan };
            made[at (Field::tune)] = { .id = tuneIDs[pad],
                                       .name = tuneNames[pad],
                                       .minimum = -24.0f,
                                       .maximum = 24.0f,
                                       .defaultValue = 0.0f,
                                       .unit = Unit::semitones };
            made[at (Field::attack)] = { .id = attackIDs[pad],
                                         .name = attackNames[pad],
                                         .minimum = 0.0f,
                                         .maximum = 200.0f,
                                         .defaultValue = 0.0f,
                                         .unit = Unit::milliseconds,
                                         .midpoint = 20.0f };
            made[at (Field::length)] = { .id = lengthIDs[pad],
                                         .name = lengthNames[pad],
                                         .minimum = 10.0f,
                                         .maximum = longestLength,
                                         .defaultValue = longestLength,
                                         .unit = Unit::milliseconds,
                                         .midpoint = 300.0f };
            made[at (Field::choke)] = { .id = chokeIDs[pad],
                                        .name = chokeNames[pad],
                                        .minimum = 0.0f,
                                        .maximum = 4.0f,
                                        .defaultValue = isHat (pad) ? (float) kitChokeGroup : 0.0f,
                                        .unit = Unit::choice,
                                        .choices = chokeGroups };
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
} // namespace

const DeviceInfo DrumSampler::info { .typeID = typeID, .name = "Drum Sampler", .kind = DeviceKind::instrument, .parameters = parameters };

DrumSampler::DrumSampler()
{
    for (int pad = 0; pad < numPads; ++pad)
    {
        playing[(size_t) pad] = getFactoryDrum (pad);
        sounds[(size_t) pad].store (playing[(size_t) pad].get(), std::memory_order_release);
        playheads[(size_t) pad].set (-1.0f);
    }
}

DrumSampler::~DrumSampler() = default;

void DrumSampler::setSound (int pad, std::shared_ptr<const Sound> sound)
{
    if (! juce::isPositiveAndBelow (pad, numPads))
        return;

    freeRetired();
    given[(size_t) pad] = sound != nullptr;

    if (sound == nullptr)
        sound = getFactoryDrum (pad);

    if (sound == playing[(size_t) pad])
        return;

    sounds[(size_t) pad].store (sound.get(), std::memory_order_release);

    // The audio thread may play the one before until the end of its next block, when its voices notice the change.
    retired.push_back ({ std::move (playing[(size_t) pad]), blocksDone.load (std::memory_order_acquire) + 2 });
    playing[(size_t) pad] = std::move (sound);
}

bool DrumSampler::hasOwnSound (int pad) const noexcept
{
    return juce::isPositiveAndBelow (pad, numPads) && given[(size_t) pad];
}

void DrumSampler::freeRetired()
{
    const auto done = blocksDone.load (std::memory_order_acquire);
    retired.erase (std::remove_if (retired.begin(), retired.end(), [done] (const Retired& old) { return done >= old.safeAfter; }),
                   retired.end());
}

void DrumSampler::audition (int pad, float velocity)
{
    const auto scope = auditionFifo.write (1);

    if (scope.blockSize1 > 0)
        auditions[(size_t) scope.startIndex1] = { pad, velocity, juce::Time::getMillisecondCounter() };
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

    for (const auto& sound : sounds)
        if (const auto* each = sound.load (std::memory_order_acquire))
            longest = std::max (longest, each->audio.getNumSamples() / each->sampleRate);

    return longest;
}

void DrumSampler::hit (int pad, float velocity, std::span<const float> values)
{
    const auto at = [&values, pad] (Field field) { return values[(size_t) getParameterIndex (pad, field)]; };
    const auto* sound = sounds[(size_t) pad].load (std::memory_order_acquire);

    if (sound == nullptr || sound->audio.getNumSamples() == 0)
        return;

    // Its choke group stops the others in it.
    if (const auto group = (int) std::lround (at (Field::choke)); group > 0)
        for (auto& voice : voices)
            if (voice.pad >= 0 && voice.pad != pad
                && (int) std::lround (values[(size_t) getParameterIndex (voice.pad, Field::choke)]) == group)
                voice.fadeStep = std::max (voice.fadeStep, (float) (1.0 / (fadeSeconds * sampleRate)));

    // A free voice, or the oldest.
    Voice* chosen = nullptr;

    for (auto& voice : voices)
        if (voice.pad < 0 || chosen == nullptr || (chosen->pad >= 0 && voice.started < chosen->started))
            chosen = &voice;

    auto& voice = *chosen;
    const auto gain = dsp::decibelsToGain (at (Field::level)) * std::clamp (velocity, 0.0f, 1.0f);
    const auto panning = at (Field::pan);
    voice.pad = pad;
    voice.sound = sound;
    voice.position = 0.0;
    voice.step = sound->sampleRate / sampleRate * std::pow (2.0, (double) at (Field::tune) / 12.0);
    voice.gainLeft = gain * std::min (1.0f, 1.0f - panning);
    voice.gainRight = gain * std::min (1.0f, 1.0f + panning);
    voice.played = 0;
    voice.attackSamples = (int) std::lround ((double) at (Field::attack) * 0.001 * sampleRate);
    voice.lengthSamples = at (Field::length) >= longestLength ? std::numeric_limits<int>::max()
                                                              : (int) std::lround ((double) at (Field::length) * 0.001 * sampleRate);
    voice.fade = 1.0f;
    voice.fadeStep = 0.0f;
    voice.started = ++hits;
    padMeters[(size_t) pad].raise (velocity);
}

void DrumSampler::render (float* left, float* right, int numSamples)
{
    const auto cutStep = (float) (1.0 / (fadeSeconds * sampleRate));

    for (auto& voice : voices)
    {
        if (voice.pad < 0)
            continue;

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

            // Its attack, its length (a cut fades), a choke's fade.
            auto envelope = voice.played < voice.attackSamples ? (float) voice.played / (float) voice.attackSamples : 1.0f;

            if (voice.played >= voice.lengthSamples)
                voice.fadeStep = std::max (voice.fadeStep, cutStep);

            envelope *= voice.fade;

            // At its own rate and untuned, its samples as they are; otherwise read between them by a windowed sinc.
            auto sampleLeft = 0.0f, sampleRight = 0.0f;

            if (juce::exactlyEqual (voice.step, 1.0))
            {
                const auto index = (int) voice.position;
                sampleLeft = first[index];
                sampleRight = second[index];
            }
            else
            {
                reader.read (first, second, frames, voice.position, voice.step, sampleLeft, sampleRight);
            }

            left[i] += sampleLeft * voice.gainLeft * envelope;

            if (right != nullptr)
                right[i] += sampleRight * voice.gainRight * envelope;

            voice.position += voice.step;
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

    // A pad whose sound changed stops: its voice can't read the old one any more.
    for (auto& voice : voices)
        if (voice.pad >= 0 && sounds[(size_t) voice.pad].load (std::memory_order_acquire) != voice.sound)
            voice.pad = -1;

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

    // A stretch at a time, between the notes; a note's end doesn't stop its sound, which plays once, whole.
    for (int done = 0; done < block.numSamples;)
    {
        for (; next < block.events.size() && block.events[next].sample <= done; ++next)
        {
            const auto& event = block.events[next];
            const auto pad = event.number - firstNote;

            const auto isNoteOn = event.type == NoteEvent::Type::noteOn || event.type == NoteEvent::Type::portamentoOn;

            if (isNoteOn && juce::isPositiveAndBelow (pad, numPads))
                hit (pad, event.value, values);

            // Playback stopped: hits that had only just begun fade out, so that nothing the song began just before Stop
            // sounds after it (the loop's first hits, when Stop came as it went round). Older hits ring on.
            if (event.type == NoteEvent::Type::allNotesOff && ! block.isPlaying)
                for (auto& voice : voices)
                    if (voice.pad >= 0 && voice.played < justBegunSeconds * sampleRate)
                        voice.fadeStep = std::max (voice.fadeStep, (float) (1.0 / (fadeSeconds * sampleRate)));
        }

        const auto until = next < block.events.size() ? std::min (block.numSamples, block.events[next].sample) : block.numSamples;
        render (left + done, right != nullptr ? right + done : nullptr, until - done);
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
