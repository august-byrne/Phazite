// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Channel.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::engine
{
namespace
{
    constexpr double easeSeconds = 0.015; // a new volume or pan, as Tracktion's fader eased to one
    constexpr double fadeSeconds = 0.005; // a channel muted or heard again

    float toGain (float decibels) noexcept
    {
        return decibels <= Channel::silenceDb ? 0.0f : juce::Decibels::decibelsToGain (decibels, Channel::silenceDb);
    }
} // namespace

PanGains getPanGains (float pan) noexcept
{
    // In double, so that the middle is exactly at its level in float.
    const auto angle = ((double) std::clamp (pan, -1.0f, 1.0f) + 1.0) * juce::MathConstants<double>::pi / 4.0;
    return { (float) (juce::MathConstants<double>::sqrt2 * std::cos (angle)),
             (float) (juce::MathConstants<double>::sqrt2 * std::sin (angle)) };
}

float faderPositionToDecibels (float position) noexcept
{
    return position > 0.0f ? 20.0f * std::log (position) + 6.0f : Channel::silenceDb;
}

float decibelsToFaderPosition (float decibels) noexcept
{
    return decibels > Channel::silenceDb ? std::exp ((decibels - 6.0f) / 20.0f) : 0.0f;
}

void Channel::prepare (double rate) noexcept
{
    sampleRate = rate;
    started = false;
    easing = 0;
    fading = 0;
}

void Channel::process (float* const* channels, int numChannels, int numSamples, const Automation& automation,
                       float* const* beforeMute) noexcept
{
    // Where the fader is set: by hand, or by its automation where the song is (but what is written by hand now); then moved
    // by its modulation on a control point, as parts of its range (the volume's travel, the pan's from left to right).
    const auto* volumeCurve = volumeWriting.load (std::memory_order_relaxed) ? nullptr : automation.volume;
    const auto* panCurve = panWriting.load (std::memory_order_relaxed) ? nullptr : automation.pan;
    const auto modulated = ! automation.volumeModulation.empty() || ! automation.panModulation.empty();
    const auto follows = modulated || (automation.beatsPerSample > 0.0 && (volumeCurve != nullptr || panCurve != nullptr));
    const auto volumeAt = [this, volumeCurve] (double beat)
    { return volumeCurve != nullptr ? faderPositionToDecibels (volumeCurve->getValueAt (beat)) : getVolume(); };
    const auto panAt = [this, panCurve] (double beat)
    { return panCurve != nullptr ? std::clamp (panCurve->getValueAt (beat), -1.0f, 1.0f) : getPan(); };
    const auto modulate = [&automation] (float& decibels, float& panned, size_t point)
    {
        if (const auto& offsets = automation.volumeModulation; ! offsets.empty())
            decibels = faderPositionToDecibels (
                std::clamp (decibelsToFaderPosition (decibels) + offsets[std::min (point, offsets.size() - 1)], 0.0f, 1.0f));

        if (const auto& offsets = automation.panModulation; ! offsets.empty())
            panned = std::clamp (panned + 2.0f * offsets[std::min (point, offsets.size() - 1)], -1.0f, 1.0f);
    };
    const auto getGains = [] (float decibels, float panned, float* sides)
    {
        const auto gain = toGain (decibels);
        const auto pans = getPanGains (panned);
        sides[0] = gain * pans.left;
        sides[1] = gain * pans.right;
    };

    // Set: from where it is, eased there over 15 ms; at once as the channel first plays.
    const auto volumeNow = volumeAt (automation.beat), panNow = panAt (automation.beat);
    baseVolume = volumeNow;
    basePan = panNow;

    if (! started)
    {
        targetVolume = volumeNow;
        targetPan = panNow;
        modulate (targetVolume, targetPan, 0);
        getGains (targetVolume, targetPan, gains);
        easing = 0;
        heard = heardTarget = isAudible() ? 1.0f : 0.0f;
        fading = 0;
        started = true;
    }
    else if (! follows && (! juce::exactlyEqual (volumeNow, targetVolume) || ! juce::exactlyEqual (panNow, targetPan)))
    {
        targetVolume = volumeNow;
        targetPan = panNow;
        float targets[2];
        getGains (volumeNow, panNow, targets);
        easing = std::max (1, (int) (easeSeconds * sampleRate));

        for (int side = 0; side < 2; ++side)
            steps[side] = (targets[side] - gains[side]) / (float) easing;
    }

    // Muted or heard again: a fade over 5 ms, from where it is.
    if (const auto target = isAudible() ? 1.0f : 0.0f; ! juce::exactlyEqual (target, heardTarget))
    {
        heardTarget = target;
        fading = std::max (1, (int) (fadeSeconds * sampleRate));
        heardStep = (target - heard) / (float) fading;
    }

    float* sides[2] = { channels[0], numChannels > 1 ? channels[1] : nullptr };

    // A sample by the fader, and its sound before its mute, for a sidechain.
    const auto apply = [&sides, beforeMute, this] (int side, int i)
    {
        if (beforeMute != nullptr)
            beforeMute[side][i] = sides[side][i] * gains[side];

        sides[side][i] *= gains[side] * heard;
    };

    if (follows)
    {
        // Its automation and modulation: a straight line to its value on each control point, the first the song's next,
        // from where the line was as the block started, whatever the block's size.
        easing = 0;
        const auto toControlPoint =
            controlPointSpacing - (int) (((automation.sample % controlPointSpacing) + controlPointSpacing) % controlPointSpacing);

        for (int done = 0, stretch = 0; done < numSamples; ++stretch)
        {
            const auto toPoint = done == 0 ? toControlPoint : controlPointSpacing;
            const auto length = std::min (numSamples - done, toPoint);
            const auto beat = automation.beat + (done + toPoint) * automation.beatsPerSample;
            targetVolume = baseVolume = volumeAt (beat);
            targetPan = basePan = panAt (beat);
            modulate (targetVolume, targetPan, (size_t) stretch + 1);
            float ends[2];
            getGains (targetVolume, targetPan, ends);

            for (int side = 0; side < 2; ++side)
                steps[side] = (ends[side] - gains[side]) / (float) toPoint;

            for (int i = done; i < done + length; ++i)
            {
                gains[0] += steps[0];
                gains[1] += steps[1];

                if (fading > 0)
                {
                    heard += heardStep;

                    if (--fading == 0)
                        heard = heardTarget;
                }

                for (int side = 0; side < 2; ++side)
                    if (sides[side] != nullptr)
                        apply (side, i);
            }

            if (length == toPoint)
            {
                gains[0] = ends[0];
                gains[1] = ends[1];
            }

            done += length;
        }
    }
    else if (easing == 0 && fading == 0)
    {
        for (int side = 0; side < 2; ++side)
        {
            if (sides[side] == nullptr)
                continue;

            if (beforeMute != nullptr)
                juce::FloatVectorOperations::multiply (beforeMute[side], sides[side], gains[side], numSamples);

            juce::FloatVectorOperations::multiply (sides[side], gains[side] * heard, numSamples);
        }
    }
    else
    {
        for (int i = 0; i < numSamples; ++i)
        {
            if (easing > 0)
            {
                gains[0] += steps[0];
                gains[1] += steps[1];

                if (--easing == 0)
                {
                    const auto gain = toGain (targetVolume);
                    const auto pans = getPanGains (targetPan);
                    gains[0] = gain * pans.left;
                    gains[1] = gain * pans.right;
                }
            }

            if (fading > 0)
            {
                heard += heardStep;

                if (--fading == 0)
                    heard = heardTarget;
            }

            for (int side = 0; side < 2; ++side)
                if (sides[side] != nullptr)
                    apply (side, i);
        }
    }

    playedVolume.store (targetVolume, std::memory_order_relaxed);
    playedPan.store (targetPan, std::memory_order_relaxed);
    playedBaseVolume.store (baseVolume, std::memory_order_relaxed);
    playedBasePan.store (basePan, std::memory_order_relaxed);
    meter.measure (channels, std::min (numChannels, 2), numSamples);
}
} // namespace phazite::engine
