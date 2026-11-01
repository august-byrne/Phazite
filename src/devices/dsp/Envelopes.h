// SPDX-License-Identifier: AGPL-3.0-or-later
// Envelopes for Phazite's devices: the ADSR that shapes a note's level or a filter, and the level follower that
// compressors and limiters measure with. Real-time safe once prepared.
#pragma once

#include <cmath>

namespace phazite::devices::dsp
{
// Attack, decay, sustain and release. Each stage takes exactly its time along a curve of its own, and lands on its target
// when its time is up: an exponential that bends either way, or a straight line. A note that starts or ends mid-stage
// moves on from the level it has reached, so retriggering a voice doesn't click.
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

    // How each stage bends, from -1 to 1: 0 is a straight line; below 0 it moves fast at first and slows into its target,
    // as analog envelopes do, and above 0 it starts slowly and speeds up. A change applies from the next stage on.
    void setCurves (float attackCurve, float decayCurve, float releaseCurve) noexcept
    {
        attackBend = getBend (attackCurve);
        decayBend = getBend (decayCurve);
        releaseBend = getBend (releaseCurve);
    }

    // The curves stages have until set: an analog envelope's, each stage aiming a little past its target (the attack 30 %
    // of its way past it, so that it rises nearly straight, the others 1 %, settling gently), as ln (o / (1 + o)) / 10.
    static constexpr float analogAttackCurve = -0.146633707f, analogSettleCurve = -0.461512052f;

    // Where a stage that bends by a curve is, from 0 (its start) to 1 (its target), a fraction of its time in: for drawing it.
    static float getShape (float curve, float fraction) noexcept
    {
        const auto bend = getBend (curve);

        if (isStraightBend (bend))
            return fraction;

        return (float) (std::expm1 (bend * (double) fraction) / std::expm1 (bend));
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
                level = isStraight ? level + step : aim + (level - aim) * coefficient;

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

    static constexpr float sustainFollow = 0.002f; // a few milliseconds
    static constexpr double steepestBend = 10.0;   // at a curve of 1, a stage's last tenth goes 63 % of its way (at -1, its first)

    // A curve as the exponent of its shape, (e^(bend t) - 1) / (e^bend - 1) over the stage's time t from 0 to 1; nearly
    // straight curves are straight, which keeps the exponent away from 0.
    static double getBend (float curve) noexcept
    {
        const auto bend = (curve < -1.0f ? -1.0f : (curve > 1.0f ? 1.0f : curve)) * steepestBend;
        return isStraightBend (bend) ? 0.0 : bend;
    }

    static bool isStraightBend (double bend) noexcept { return std::abs (bend) < 1.0e-4; }

    double sampleRate = 44100.0;
    float attack = 0.005f, decay = 0.1f, sustain = 1.0f, release = 0.1f;
    double attackBend = getBend (analogAttackCurve), decayBend = getBend (analogSettleCurve), releaseBend = getBend (analogSettleCurve);
    Stage stage = Stage::idle;
    float level = 0.0f, target = 0.0f, aim = 0.0f, coefficient = 0.0f, step = 0.0f;
    bool isStraight = false;
    int samplesLeft = 0;

    void startStage (Stage newStage) noexcept
    {
        stage = newStage;

        switch (newStage)
        {
            case Stage::attack:
                startSegment (1.0f, attack, attackBend);
                break;
            case Stage::decay:
                startSegment (sustain, decay, decayBend);
                break;
            case Stage::release:
                startSegment (0.0f, release, releaseBend);
                break;
            case Stage::sustain:
            case Stage::idle:
                break;
        }
    }

    // Toward newTarget in seconds, from whatever level it starts at, along its shape: the distance to an aim beyond the
    // target (bending below 0) or behind the start (above 0) changes by the same factor each sample, e^(bend / samples),
    // so that it lands on the target as its time is up. A straight stage steps the same amount each sample.
    void startSegment (float newTarget, float seconds, double bend) noexcept
    {
        target = newTarget;
        samplesLeft = (int) std::lround ((double) seconds * sampleRate);

        if (samplesLeft <= 0)
        {
            finishStage();
            return;
        }

        isStraight = isStraightBend (bend);
        step = (newTarget - level) / (float) samplesLeft;
        aim = isStraight ? newTarget : (float) (level - (double) (newTarget - level) / std::expm1 (bend));
        coefficient = (float) std::exp (bend / samplesLeft);
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
