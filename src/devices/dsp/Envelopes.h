// SPDX-License-Identifier: AGPL-3.0-or-later
// Envelopes for Phazite's devices: the ADSR that shapes a note's level or a filter, and the level follower that
// compressors and limiters measure with. Real-time safe once prepared.
#pragma once

#include <cmath>

namespace phazite::devices::dsp
{
// Attack, decay, sustain and release. Each stage takes exactly its time: it curves exponentially toward a point a
// little past its target, as analog envelopes do, and lands on the target when its time is up. A note that starts
// or ends mid-stage moves on from the level it has reached, so retriggering a voice doesn't click.
class Adsr
{
public:
    void prepare (double newSampleRate) noexcept { sampleRate = newSampleRate; }

    // Times in seconds (0 for none), the sustain level from 0 to 1. A change applies from the next stage on, except
    // the sustain level, which the sustaining stage follows smoothly.
    void setParameters (float attackSeconds, float decaySeconds, float sustainLevel, float releaseSeconds) noexcept
    {
        attack = attackSeconds;
        decay = decaySeconds;
        sustain = sustainLevel < 0.0f ? 0.0f : (sustainLevel > 1.0f ? 1.0f : sustainLevel);
        release = releaseSeconds;
    }

    void noteOn() noexcept { startStage (Stage::attack); }

    void noteOff() noexcept
    {
        if (stage != Stage::idle)
            startStage (Stage::release);
    }

    void reset() noexcept
    {
        stage = Stage::idle;
        level = 0.0f;
    }

    bool isActive() const noexcept { return stage != Stage::idle; }
    bool isReleasing() const noexcept { return stage == Stage::release; }
    float getLevel() const noexcept { return level; }

    float next() noexcept
    {
        switch (stage)
        {
            case Stage::idle:
                return 0.0f;

            case Stage::sustain:
                level += (sustain - level) * sustainFollow;
                return level;

            case Stage::attack:
            case Stage::decay:
            case Stage::release:
                level = aim + (level - aim) * coefficient;

                if (--samplesLeft <= 0)
                    finishStage();

                return level;
        }

        return level;
    }

private:
    enum class Stage
    {
        idle,
        attack,
        decay,
        sustain,
        release
    };

    // How far past its target a stage aims, as a fraction of the way it goes: more is a straighter curve.
    static constexpr double attackOvershoot = 0.3, settleOvershoot = 0.01;
    static constexpr float sustainFollow = 0.002f; // a few milliseconds

    double sampleRate = 44100.0;
    float attack = 0.005f, decay = 0.1f, sustain = 1.0f, release = 0.1f;
    Stage stage = Stage::idle;
    float level = 0.0f, target = 0.0f, aim = 0.0f, coefficient = 0.0f;
    int samplesLeft = 0;

    void startStage (Stage newStage) noexcept
    {
        stage = newStage;

        switch (newStage)
        {
            case Stage::attack:
                startSegment (1.0f, attack, attackOvershoot);
                break;
            case Stage::decay:
                startSegment (sustain, decay, settleOvershoot);
                break;
            case Stage::release:
                startSegment (0.0f, release, settleOvershoot);
                break;
            case Stage::sustain:
            case Stage::idle:
                break;
        }
    }

    // Toward newTarget in seconds: the distance to the aim shrinks by the same factor each sample, from
    // (1 + overshoot) to overshoot times the way to go, whatever the level it starts from.
    void startSegment (float newTarget, float seconds, double overshoot) noexcept
    {
        target = newTarget;
        samplesLeft = (int) std::lround ((double) seconds * sampleRate);

        if (samplesLeft <= 0)
        {
            finishStage();
            return;
        }

        aim = (float) (newTarget + (newTarget - level) * overshoot);
        coefficient = (float) std::pow (overshoot / (1.0 + overshoot), 1.0 / samplesLeft);
    }

    void finishStage() noexcept
    {
        level = target;

        switch (stage)
        {
            case Stage::attack:
                startStage (Stage::decay);
                break;
            case Stage::decay:
                stage = Stage::sustain;
                break;
            case Stage::release:
                stage = Stage::idle;
                level = 0.0f;
                break;
            case Stage::sustain:
            case Stage::idle:
                break;
        }
    }
};

//==============================================================================
// Follows a level, such as a signal's absolute value or its square: rising with the attack time constant and falling
// with the release one (in each, it gets 63 % of the way in that time).
class LevelFollower
{
public:
    void prepare (double newSampleRate) noexcept { sampleRate = newSampleRate; }

    void setTimes (float attackSeconds, float releaseSeconds) noexcept
    {
        attackCoefficient = getCoefficient (attackSeconds);
        releaseCoefficient = getCoefficient (releaseSeconds);
    }

    void reset (float newValue = 0.0f) noexcept { value = newValue; }
    float getValue() const noexcept { return value; }

    float process (float input) noexcept
    {
        const auto c = input > value ? attackCoefficient : releaseCoefficient;
        value = input + c * (value - input);
        return value;
    }

private:
    double sampleRate = 44100.0;
    float attackCoefficient = 0.0f, releaseCoefficient = 0.0f, value = 0.0f;

    float getCoefficient (float seconds) const noexcept
    {
        return seconds <= 0.0f ? 0.0f : (float) std::exp (-1.0 / ((double) seconds * sampleRate));
    }
};
} // namespace phazite::devices::dsp
