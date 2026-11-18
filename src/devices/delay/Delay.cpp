// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Delay.h"
#include "devices/dsp/Math.h"
#include "devices/dsp/NoteValues.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace phazite::devices
{
namespace
{
    // The note values a synced echo takes: the shared list's (NoteValues.h), from a 32nd note to a bar of the song's.
    constexpr int numNoteValues = 14;
    constexpr std::span<const std::string_view> noteValues { dsp::noteValueNames, (size_t) numNoteValues };
    static_assert (dsp::noteValueLengths[numNoteValues - 1].bars > 0.0); // the last, a bar

    constexpr std::string_view characters[] = { "Tape", "Clean" };
    constexpr std::string_view transitions[] = { "Repitch", "Fade" };

    // The IDs are saved in projects.
    constexpr ParameterInfo parameters[] = {
        { .id = "sync", .name = "Sync", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
        { .id = "note",
          .name = "Note",
          .maximum = (float) numNoteValues - 1.0f,
          .defaultValue = 6.0f, // a dotted eighth
          .unit = Unit::choice,
          .choices = noteValues },
        { .id = "time",
          .name = "Time",
          .minimum = 1.0f,
          .maximum = 2000.0f,
          .defaultValue = 375.0f,
          .unit = Unit::milliseconds,
          .midpoint = 250.0f },
        { .id = "link", .name = "Link", .maximum = 1.0f, .defaultValue = 1.0f, .unit = Unit::toggle },
        { .id = "noteRight",
          .name = "Note Right",
          .maximum = (float) numNoteValues - 1.0f,
          .defaultValue = 6.0f,
          .unit = Unit::choice,
          .choices = noteValues },
        { .id = "timeRight",
          .name = "Time Right",
          .minimum = 1.0f,
          .maximum = 2000.0f,
          .defaultValue = 375.0f,
          .unit = Unit::milliseconds,
          .midpoint = 250.0f },
        { .id = "feedback", .name = "Feedback", .maximum = 1.1f, .defaultValue = 0.35f, .unit = Unit::percent },
        { .id = "pingPong", .name = "Ping-Pong", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "character", .name = "Character", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::choice, .choices = characters },
        { .id = "modRate",
          .name = "Mod Rate",
          .minimum = 0.05f,
          .maximum = 10.0f,
          .defaultValue = 0.5f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "modDepth", .name = "Mod Depth", .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "duck", .name = "Duck", .defaultValue = 0.0f, .unit = Unit::percent },
        { .id = "freeze", .name = "Freeze", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::toggle },
        { .id = "transition", .name = "Transition", .maximum = 1.0f, .defaultValue = 0.0f, .unit = Unit::choice, .choices = transitions },
        { .id = "lowCut",
          .name = "Low Cut",
          .minimum = 20.0f,
          .maximum = 2000.0f,
          .defaultValue = 100.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = "highCut",
          .name = "High Cut",
          .minimum = 1000.0f,
          .maximum = 20000.0f,
          .defaultValue = 8000.0f,
          .unit = Unit::hertz,
          .scale = Scale::logarithmic },
        { .id = mixParameterID, .name = "Mix", .defaultValue = 0.25f, .unit = Unit::percent },
    };

    constexpr double glideSeconds = 0.05;      // how long a change of time takes to glide (Repitch), about
    constexpr double fadeSeconds = 0.05;       // and to fade (Fade)
    constexpr double fadeAfterSeconds = 0.001; // with Fade, a time read this far behind the time set fades to it
    constexpr double switchSeconds = 0.02;     // a change of character, and the freeze switched, crossfade
    constexpr double depthGlideSeconds = 0.02; // the modulation's depth glides
    constexpr double shortestSeconds = 0.001;  // the shortest time, which the modulation never takes a line under
    constexpr double butterworthQ = 0.70710678;
    constexpr double halfPi = juce::MathConstants<double>::halfPi;

    // With Fade, how fast the time read may follow the time set: as far as bends the pitch 10 cents, 0.58 % of real time.
    constexpr float fadeGlideStep = 0.00579294f; // 2 to the power 10/1200, less 1

    // The duck's follower rises 60 dB in 5 ms and falls 60 dB in 250 ms, as a tail's 60 dB are counted (a sound coming back
    // from loud to silence is out of the duck's range in that time), from the loudest of the last 20 ms; its gain glides
    // with a time constant of 5 ms.
    constexpr double duckRiseSeconds = 0.005, duckFallSeconds = 0.25, duckGlideSeconds = 0.005;

    // A share a step nearer 0 or 1.
    float fadeShare (float share, bool on, float step) noexcept
    {
        return on ? std::min (1.0f, share + step) : std::max (0.0f, share - step);
    }

    // One of two values, or a share of the way from the first to the second, exactly either at the ends.
    float blend (float first, float second, float share) noexcept
    {
        return share <= 0.0f ? first : share >= 1.0f ? second : first + share * (second - first);
    }

    // Two reads crossfaded at equal power, a share of the way from the first to the second: a sound unrelated between the
    // two keeps its level through the fade.
    float crossfade (float first, float second, float share) noexcept
    {
        const auto angle = halfPi * (double) share;
        return (float) std::cos (angle) * first + (float) std::sin (angle) * second;
    }
} // namespace

const DeviceInfo Delay::info { .typeID = typeID, .name = "Delay", .kind = DeviceKind::effect, .parameters = parameters };

double Delay::getDelaySeconds (std::span<const float> values, double tempo, double barLength, int side)
{
    const auto own = side != 0 && values[link] < 0.5f;
    auto seconds = (double) values[own ? timeRight : time] * 0.001;

    if (values[sync] > 0.5f)
    {
        const auto index = (size_t) juce::jlimit (0, numNoteValues - 1, (int) std::lround (values[own ? noteRight : note]));
        seconds = dsp::noteValueLengths[index].inBeats (barLength) * 60.0 / std::max (tempo, 1.0);
    }

    return std::min (seconds, maximumSeconds);
}

float Delay::bendClean (float sample) noexcept
{
    const auto size = std::abs (sample);

    if (size <= 1.0f)
        return sample;

    const auto bent = 1.0f + std::tanh (size - 1.0f);
    return sample < 0.0f ? -bent : bent;
}

float Delay::getDuckDecibels (float amount, float levelDecibels) noexcept
{
    const auto share = std::clamp ((levelDecibels - duckNoneDecibels) / (duckAllDecibels - duckNoneDecibels), 0.0f, 1.0f);
    return mostDuckDecibels * std::clamp (amount, 0.0f, 1.0f) * share;
}

int Delay::getFadesUnderWay() const noexcept
{
    return (int) std::count_if (sides.begin(), sides.end(), [] (const Side& side) { return side.fadeLeft > 0; });
}

void Delay::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    timeSmoothing = (float) (1.0 - std::exp (-1.0 / (glideSeconds * sampleRate)));
    fadeLength = std::max (1, (int) std::lround (fadeSeconds * sampleRate));
    fadeAfter = (float) (fadeAfterSeconds * sampleRate);
    shareStep = (float) (1.0 / std::max (1.0, switchSeconds * sampleRate));
    depthSmoothing = (float) (1.0 - std::exp (-1.0 / (depthGlideSeconds * sampleRate)));
    samplesPerDuckPeak = std::max (1, (int) std::lround (sampleRate * 0.001));

    const auto sixtyDecibels = std::log (1000.0);
    duckRise = (float) std::exp (-sixtyDecibels / (duckRiseSeconds * sampleRate));
    duckFall = (float) std::exp (-sixtyDecibels / (duckFallSeconds * sampleRate));
    duckSmoothing = (float) (1.0 - std::exp (-1.0 / (duckGlideSeconds * sampleRate)));

    // Room for the longest time and the modulation's reach beyond it.
    for (auto& side : sides)
        side.line.prepare ((int) std::ceil ((maximumSeconds + mostModulationSeconds) * sampleRate) + 1);

    reset();
}

void Delay::reset()
{
    for (auto& side : sides)
    {
        side.line.reset();
        side.lowFilter.reset();
        side.highFilter.reset();
        side.fadeLeft = 0;
        side.looping = false;
    }

    modPhase = 0.0;
    nextSongSample = 0;
    wasPlaying = false;
    restDuck();
    duckGain = 1.0f;
    shownDuck.set (0.0f);
    fadesStarted = 0;
    isFirstBlock = true;
}

// The modulation's phase runs on a sample at a time from its rate, as an LFO in hertz does (a move of the rate, by hand or
// by automation, changes how fast it runs, never where it is). It counts the song's samples, not its beats: a change of
// tempo moves nothing. As playback starts (the engine resets the device at every start and every jump of the playhead, and
// a render starts so too) it is set at once from where the song is, its sample times the rate; while the song plays on, it
// runs on from there. A block that comes later than the last one ended (the runner let the device sleep on silence, its
// lines empty) moves it on as far as the samples it missed, at the rate it has, as if it had played them, so that live
// playback and a render, which never sleeps, swing alike after a sleep. A loop going round (a block earlier than the last
// one ended: the engine doesn't reset the device at a loop's wrap) leaves it running on, in a render of the loop as live.
// Stopped, it runs free.
void Delay::startModulation (const ProcessContext& block, float rate) noexcept
{
    modIncrement = std::max (0.0, (double) rate) / sampleRate;

    if (! block.isPlaying)
    {
        wasPlaying = false;
        return;
    }

    if (! wasPlaying)
    {
        const auto wanted = (double) block.songSample * modIncrement;
        modPhase = wanted - std::floor (wanted);
    }
    else if (block.songSample > nextSongSample)
    {
        modPhase += (double) (block.songSample - nextSongSample) * modIncrement;
        modPhase -= std::floor (modPhase);
    }

    wasPlaying = true;
    nextSongSample = block.songSample + block.numSamples;
}

void Delay::advanceModulation() noexcept
{
    modPhase += modIncrement;

    if (modPhase >= 1.0)
        modPhase -= std::floor (modPhase);
}
// A side's time a sample nearer its target: gliding there (Repitch); or, with Fade, no faster than bends the pitch 10 cents,
// and once the time read is more than 1 ms behind the time set, read at the time set at once while the read as it is fades
// out. A change while a fade is under way waits for it to end, then glides or fades on from what is heard. So a nudge glides
// in without a bend anyone hears, a jump fades, and a time that moves faster than 10 cents (a steep ramp of the tempo) fades
// again and again.
bool Delay::moveTime (Side& side, float target, bool fades) noexcept
{
    if (side.fadeLeft > 0)
    {
        --side.fadeLeft;
        return false;
    }

    const auto lag = target - side.delaySamples;

    if (! fades)
    {
        side.delaySamples += lag * timeSmoothing;
        return false;
    }

    if (std::abs (lag) <= fadeAfter)
    {
        side.delaySamples += std::clamp (lag, -fadeGlideStep, fadeGlideStep);
        return false;
    }

    side.fadingFrom = side.delaySamples;
    side.fadeLeft = fadeLength;
    side.delaySamples = target;
    ++fadesStarted;
    return true;
}
// How far the modulation moves a read at a time, in samples: its depth now, but never under the shortest time.
float Delay::getSwing (float delay) const noexcept
{
    return std::clamp (delay - (float) (shortestSeconds * sampleRate), 0.0f, depthNow);
}

// What went in the delay ago, the modulation's swing on it: read before this sample goes in, so one less back from the last
// one in; while it fades, the read at the old time faded out and the new one in, each read swung by its own time's swing.
float Delay::readEcho (const Side& side, float wave) const noexcept
{
    const auto modulated = depthNow > 0.0f;
    const auto now = side.line.read (side.delaySamples + (modulated ? getSwing (side.delaySamples) * wave : 0.0f) - 1.0f);

    if (side.fadeLeft <= 0)
        return now;

    const auto before = side.line.read (side.fadingFrom + (modulated ? getSwing (side.fadingFrom) * wave : 0.0f) - 1.0f);
    return crossfade (before, now, 1.0f - (float) side.fadeLeft / (float) fadeLength);
}

// What goes round while frozen, unmodulated: while the time rests, whole samples, so that the line's content comes back
// unchanged pass after pass (a read between samples, pushed back, would dull it a little each time); while it moves, read
// between samples at the time, so that the loop is stretched with it rather than cut by a sample here and there, which
// dulls only what goes round during the move, once. While it fades, the two reads crossfaded.
float Delay::readLoop (const Side& side) const noexcept
{
    const auto now = side.line.read (side.loopNow - 1.0f);

    if (side.fadeLeft <= 0)
        return now;

    return crossfade (side.line.read (side.loopFrom - 1.0f), now, 1.0f - (float) side.fadeLeft / (float) fadeLength);
}

// Where what goes round is read, a sample on: as the freeze comes on, where the time is; as a fade starts, from where it was
// to the time set; otherwise the time while it moves, and the whole sample nearest it while it rests, which it moves to no
// faster than the time moves and 10 cents more, so that it never steps.
void Delay::moveLoop (Side& side, float moved, bool fadeStarted) noexcept
{
    if (! side.looping)
    {
        side.loopNow = side.delaySamples;
        side.loopFrom = side.fadingFrom;
        side.looping = true;
        return;
    }

    if (fadeStarted)
    {
        side.loopFrom = side.loopNow;
        side.loopNow = side.delaySamples;
        return;
    }

    if (side.fadeLeft > 0)
        return;

    // Resting: the time moved less this sample than half of what 10 cents move it (Repitch's glide ends so, its last steps
    // too small to tell from its float's, two samples or so from the time set at a long time, where it stays).
    const auto rests = std::abs (moved) < 0.5f * fadeGlideStep;
    const auto goal = rests ? std::round (side.delaySamples) : side.delaySamples;
    const auto reach = fadeGlideStep + std::abs (moved);
    side.loopNow += std::clamp (goal - side.loopNow, -reach, reach);
}
// The duck's follower a sample on: the loudest of the last 20 ms (a running maximum of each millisecond's peak, as the
// compressor's Duck holds its key's, dynamics/Compressor.cpp), which it rises to and falls to.
float Delay::followDuck (float level) noexcept
{
    duckRests = false;
    duckPeakNow = std::max (duckPeakNow, level);

    if (++duckPeakSamples >= samplesPerDuckPeak)
    {
        duckPeaks[(size_t) duckPeakIndex] = duckPeakNow;
        duckPeakIndex = (duckPeakIndex + 1) % (int) duckPeaks.size();
        duckPeakSamples = 0;
        duckPeakNow = 0.0f;
        duckPeakBefore = *std::max_element (duckPeaks.begin(), duckPeaks.end());
    }

    const auto held = std::max (duckPeakBefore, duckPeakNow);
    duckLevel = held + (held > duckLevel ? duckRise : duckFall) * (duckLevel - held);
    return duckLevel;
}

void Delay::restDuck() noexcept
{
    duckPeaks.fill (0.0f);
    duckPeakIndex = duckPeakSamples = 0;
    duckPeakNow = duckPeakBefore = duckLevel = 0.0f;
    duckRests = true;
}

void Delay::process (const ProcessContext& block)
{
    const juce::ScopedNoDenormals noDenormals;
    silenceWhatIsNotSound (block);
    const auto& values = block.parameters;
    const auto numChannels = std::min (block.numChannels, 2);

    if (numChannels <= 0)
        return;

    const auto seconds = getDelaySeconds (values, block.tempo, block.barLength, 0);
    const auto secondsRight = getDelaySeconds (values, block.tempo, block.barLength, 1);
    const auto longest = maximumSeconds * sampleRate;
    const float targets[] = { (float) juce::jlimit (1.0, longest, seconds * sampleRate),
                              (float) juce::jlimit (1.0, longest, secondsRight * sampleRate) };

    const auto amount = values[feedback];
    const auto bounce = values[pingPong] > 0.5f && numChannels == 2;
    const auto onTape = values[character] < 0.5f;
    const auto frozen = values[freeze] > 0.5f;
    const auto fades = values[transition] > 0.5f;
    const auto depthTarget = (float) ((double) std::clamp (values[modDepth], 0.0f, 1.0f) * mostModulationSeconds * sampleRate);
    const auto duckAmount = std::clamp (values[duck], 0.0f, 1.0f);
    const auto lowCutFilter = dsp::makeFilter (dsp::FilterShape::highPass, values[lowCut], sampleRate, butterworthQ);
    const auto highCutFilter = dsp::makeFilter (dsp::FilterShape::lowPass, values[highCut], sampleRate, butterworthQ);

    if (isFirstBlock)
    {
        for (size_t s = 0; s < sides.size(); ++s)
            sides[s].delaySamples = targets[s];

        tapeShare = onTape ? 1.0f : 0.0f;
        frozenShare = frozen ? 1.0f : 0.0f;
        depthNow = depthTarget;
        isFirstBlock = false;
    }

    // How long the echoes ring: until the feedback has taken them 60 dB down, at the longer side's time; held (100 % and
    // over, or frozen), its longest.
    const auto sideSeconds = std::max (seconds, numChannels > 1 ? secondsRight : seconds) + values[modDepth] * mostModulationSeconds;
    const auto holds = amount >= 1.0f || frozen || frozenShare > 0.0f;
    const auto repeats = amount > 0.001f && ! holds ? std::log (0.001) / std::log ((double) amount) : 1.0;
    tailSeconds.store (holds ? longestTailSeconds : std::min (longestTailSeconds, sideSeconds * (repeats + 1.0)),
                       std::memory_order_relaxed);
    shownTempo.set ((float) block.tempo);
    shownBarLength.set ((float) block.barLength);
    startModulation (block, values[modRate]);

    auto* left = block.channels[0];
    auto* right = numChannels > 1 ? block.channels[1] : nullptr;
    const auto numSides = right != nullptr ? 2 : 1;

    for (int i = 0; i < block.numSamples; ++i)
    {
        // The switches crossfade, and the modulation's depth glides.
        tapeShare = fadeShare (tapeShare, onTape, shareStep);
        frozenShare = fadeShare (frozenShare, frozen, shareStep);
        depthNow += (depthTarget - depthNow) * depthSmoothing;

        if (depthTarget <= 0.0f && depthNow < 1.0e-4f)
            depthNow = 0.0f;

        const auto inLeft = left[i], inRight = right != nullptr ? right[i] : 0.0f;
        float echo[2] {}, back[2] {}, held[2] {};

        for (int s = 0; s < numSides; ++s)
        {
            auto& side = sides[(size_t) s];
            const auto before = side.delaySamples;
            const auto fadeStarted = moveTime (side, targets[s], fades);

            // The modulation: a sine, the right a quarter of a cycle ahead.
            const auto wave = depthNow > 0.0f ? (float) std::sin (juce::MathConstants<double>::twoPi * (modPhase + 0.25 * s)) : 0.0f;

            // Each echo darker and thinner than the one before; the feedback bent by the character's curve, which holds it.
            echo[s] = side.highFilter.process (side.lowFilter.process (readEcho (side, wave), lowCutFilter), highCutFilter);
            const auto x = echo[s] * amount;
            back[s] = tapeShare >= 1.0f ? dsp::softClip (x) : blend (bendClean (x), dsp::softClip (x), tapeShare);

            if (frozenShare > 0.0f)
            {
                moveLoop (side, side.delaySamples - before, fadeStarted);
                held[s] = readLoop (side);
            }
            else
            {
                side.looping = false;
            }
        }

        // What goes back into the lines; frozen, what they hold goes round unchanged, and nothing new goes in.
        if (bounce)
        {
            // The sound goes in on the left, and each echo crosses to the other side.
            sides[0].line.push (blend (0.5f * (inLeft + inRight) + back[1], held[1], frozenShare));
            sides[1].line.push (blend (back[0], held[0], frozenShare));
        }
        else
        {
            sides[0].line.push (blend (inLeft + back[0], held[0], frozenShare));

            if (right != nullptr)
                sides[1].line.push (blend (inRight + back[1], held[1], frozenShare));
        }

        // The duck: a follower of the louder side coming in, which turns what it puts out down; at rest while the Duck is at 0
        // and its gain back at 1, from where it rises in 5 ms as it is turned up.
        auto gain = 1.0f;

        if (duckAmount > 0.0f || duckGain < 1.0f)
        {
            const auto level = followDuck (std::max (std::abs (inLeft), std::abs (inRight)));
            const auto wanted = dsp::decibelsToGain (-getDuckDecibels (duckAmount, dsp::gainToDecibels (level)));
            duckGain += (wanted - duckGain) * duckSmoothing;

            if (wanted >= 1.0f && duckGain > 0.999999f)
                duckGain = 1.0f;

            gain = duckGain;
        }
        else if (! duckRests)
        {
            restDuck();
        }

        // The echoes, Clean's held by its curve too, which is a straight line to full scale: the cuts can ring a loop held
        // at its curve's top past it, as far as +9 dBFS with a feedback of 110 %. Frozen too, what is heard is the echo as
        // the cuts read it, which goes on as it was as the freeze comes on.
        for (int s = 0; s < numSides; ++s)
            if (tapeShare < 1.0f)
                echo[s] = blend (bendClean (echo[s]), echo[s], tapeShare);

        left[i] = echo[0];

        if (right != nullptr)
            right[i] = echo[1];

        if (gain < 1.0f)
        {
            left[i] *= gain;

            if (right != nullptr)
                right[i] *= gain;
        }

        advanceModulation();
    }

    shownDuck.set (-dsp::gainToDecibels (duckGain));
}
} // namespace phazite::devices
