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

void Channel::prepare (double rate) noexcept
{
    sampleRate = rate;
    started = false;
    easing = 0;
    fading = 0;
}

void Channel::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    // Where the fader is now: from where it is, eased there over 15 ms; at once as the channel first plays.
    const auto volumeNow = getVolume(), panNow = getPan();

    if (! started || ! juce::exactlyEqual (volumeNow, targetVolume) || ! juce::exactlyEqual (panNow, targetPan))
    {
        targetVolume = volumeNow;
        targetPan = panNow;
        const auto gain = toGain (volumeNow);
        const auto sides = getPanGains (panNow);
        const float targets[2] = { gain * sides.left, gain * sides.right };

        if (! started)
        {
            gains[0] = targets[0];
            gains[1] = targets[1];
            easing = 0;
            heard = heardTarget = isAudible() ? 1.0f : 0.0f;
            fading = 0;
            started = true;
        }
        else
        {
            easing = std::max (1, (int) (easeSeconds * sampleRate));

            for (int side = 0; side < 2; ++side)
                steps[side] = (targets[side] - gains[side]) / (float) easing;
        }
    }

    // Muted or heard again: a fade over 5 ms, from where it is.
    if (const auto target = isAudible() ? 1.0f : 0.0f; ! juce::exactlyEqual (target, heardTarget))
    {
        heardTarget = target;
        fading = std::max (1, (int) (fadeSeconds * sampleRate));
        heardStep = (target - heard) / (float) fading;
    }

    float* sides[2] = { channels[0], numChannels > 1 ? channels[1] : nullptr };

    if (easing == 0 && fading == 0)
    {
        for (int side = 0; side < 2; ++side)
            if (sides[side] != nullptr)
                juce::FloatVectorOperations::multiply (sides[side], gains[side] * heard, numSamples);
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
                    sides[side][i] *= gains[side] * heard;
        }
    }

    meter.measure (channels, std::min (numChannels, 2), numSamples);
}
} // namespace phazite::engine
