// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumMachines.h"
#include "devices/dsp/Oscillators.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace phazite::devices::drums
{
namespace
{
    constexpr double twoPi = juce::MathConstants<double>::twoPi;
    constexpr double t20ToTau = 1.0 / 2.302585; // a decay's time to fall 20 dB, as its time constant

    int samplesOf (double seconds, double rate)
    {
        return std::max (1, (int) std::lround (seconds * rate));
    }

    // How long a sound whose loudest part falls with this time constant takes to be 60 dB down, and a little more.
    double lengthFor (double tau, double extra = 0.02)
    {
        return 6.9 * tau + extra;
    }

    // One-pole filters.
    struct LowPass1
    {
        double a = 0.0, z = 0.0;
        LowPass1 (double rate, double hz)
            : a (std::exp (-twoPi * hz / rate))
        {
        }
        double process (double x) noexcept { return z = (1.0 - a) * x + a * z; }
    };

    struct HighPass1
    {
        LowPass1 low;
        HighPass1 (double rate, double hz)
            : low (rate, hz)
        {
        }
        double process (double x) noexcept { return x - low.process (x); }
    };

    // Two-pole filters, by JUCE's biquads.
    juce::IIRFilter makeFilter (const juce::IIRCoefficients& coefficients)
    {
        juce::IIRFilter filter;
        filter.setCoefficients (coefficients);
        return filter;
    }

    juce::IIRFilter lowPass (double rate, double hz, double q = 0.7071)
    {
        return makeFilter (juce::IIRCoefficients::makeLowPass (rate, std::min (hz, 0.45 * rate), q));
    }

    juce::IIRFilter highPass (double rate, double hz, double q = 0.7071)
    {
        return makeFilter (juce::IIRCoefficients::makeHighPass (rate, std::min (hz, 0.45 * rate), q));
    }

    juce::IIRFilter bandPass (double rate, double hz, double q)
    {
        return makeFilter (juce::IIRCoefficients::makeBandPass (rate, std::min (hz, 0.45 * rate), q));
    }

    float run (juce::IIRFilter& filter, double x)
    {
        return filter.processSingleSampleRaw ((float) x);
    }

    // A square wave whose edges are smoothed (PolyBLEP), so that its harmonics don't fold back: the 808's oscillators run at
    // 48 % of their cycle high.
    struct Square
    {
        double phase = 0.0, step = 0.0, duty = 0.48;

        Square (double rate, double hz, double start = 0.0, double high = 0.48)
            : phase (start),
              step (hz / rate),
              duty (high)
        {
        }

        double next() noexcept
        {
            const auto t = (float) phase, dt = (float) step;
            auto value = phase < duty ? 1.0 : -1.0;
            value += dsp::polyBlep (t, dt);
            value -= dsp::polyBlep ((float) (phase >= duty ? phase - duty : phase - duty + 1.0), dt);
            phase += step;

            if (phase >= 1.0)
                phase -= 1.0;

            return value;
        }
    };

    // The 808's six square oscillators, which its cymbal, hats and cowbell share. Each starts somewhere of its own, as
    // free-running oscillators are when a hit comes.
    constexpr std::array<double, 6> metal808 { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };

    std::vector<Square> makeMetal (double rate, const std::array<double, 6>& frequencies, juce::Random& random)
    {
        std::vector<Square> squares;

        for (const auto hz : frequencies)
            squares.emplace_back (rate, hz, random.nextDouble());

        return squares;
    }

    double nextMetal (std::vector<Square>& squares) noexcept
    {
        auto sum = 0.0;

        for (auto& square : squares)
            sum += square.next();

        return sum / (double) squares.size();
    }

    // A rung resonator, as the bridged-T and twin-T circuits ring when struck: a sine whose frequency starts `bend` times as
    // high and settles over bendTau, its level falling with tau and swelling in over swell.
    struct Ring
    {
        double hz = 100.0, tau = 0.1, bend = 1.0, bendTau = 0.01, swell = 0.0, level = 1.0;
    };

    struct Ringing
    {
        Ring ring;
        double rate, phase = 0.0;

        Ringing (const Ring& r, double sampleRate)
            : ring (r),
              rate (sampleRate)
        {
        }

        double at (double t) noexcept
        {
            const auto hz = ring.hz * (1.0 + (ring.bend - 1.0) * std::exp (-t / ring.bendTau));
            const auto value = std::sin (phase) * std::exp (-t / ring.tau) * (ring.swell > 0.0 ? 1.0 - std::exp (-t / ring.swell) : 1.0);
            phase += twoPi * hz / rate;
            return ring.level * value;
        }
    };

    // A gate that closes over fall seconds from `at` on, as the 808's mutes cut its claves and its rim shot.
    double gate (double t, double at, double fall)
    {
        return t < at ? 1.0 : std::exp (-(t - at) / fall);
    }

    // A level held for a while, then falling as fast as it likes: the 909's VCAs.
    double holdThenFall (double t, double hold, double tau)
    {
        return t < hold ? 1.0 : std::exp (-(t - hold) / tau);
    }

    // A rounded triangle, as the 909's diodes round its oscillators' triangles into near sines with odd harmonics.
    double roundedTriangle (double phase)
    {
        const auto p = phase - std::floor (phase);
        const auto triangle = p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0);
        return std::tanh (1.6 * triangle) / std::tanh (1.6);
    }

    // Read back as a six-bit converter reads a sample at 30 kHz, through the filters after it: the 909's cymbals.
    void playAsSixBits (Sound& sound, double rate)
    {
        const auto step = 30000.0 / rate;
        constexpr auto levels = 32.0;
        auto* samples = sound.getWritePointer (0);
        auto filters = std::array { lowPass (rate, 13000.0), lowPass (rate, 13000.0) };
        auto phase = 1.0, held = 0.0;

        // The 909 compresses each sound before its converter and expands it after, so that its quiet tail keeps its steps:
        // here, the steps are taken against the sound's envelope.
        const auto peak = std::max (1.0e-9f, sound.getMagnitude (0, 0, sound.getNumSamples()));
        LowPass1 follower (rate, 30.0);

        for (int i = 0; i < sound.getNumSamples(); ++i)
        {
            const auto x = samples[i] / peak;
            const auto envelope = std::max (0.02, follower.process (std::abs (x)) * 2.0);

            if ((phase += step) >= 1.0)
            {
                phase -= 1.0;
                held = std::round (x / envelope * levels) / levels * envelope;
            }

            auto y = held;

            for (auto& filter : filters)
                y = run (filter, y);

            samples[i] = (float) (y * peak);
        }
    }

    // A sound made sample by sample, mono.
    template <typename Make>
    Sound render (double rate, double seconds, Make&& make)
    {
        Sound sound (1, samplesOf (seconds, rate));

        for (int i = 0; i < sound.getNumSamples(); ++i)
            sound.setSample (0, i, (float) make (i / rate));

        return sound;
    }
} // namespace

//==============================================================================
Sound make808Kick (double rate, double decaySeconds, double toneHz)
{
    // About 50 Hz (49.5 by its parts, 56 by Roland's chart): for its first 3.7 ms more than an octave up (3.2 times), then a
    // sigh from 23 % above down onto it over a fifth of the decay. Its trigger, 1 ms long, leaks through high-passed at
    // 400 Hz; Tone low-passes the lot. No noise, no drive.
    constexpr double hz = 50.0, crisp = 0.0037;
    const auto tau = decaySeconds * t20ToTau, sighTau = 0.21 * decaySeconds;
    HighPass1 click (rate, 400.0);
    LowPass1 tone (rate, toneHz);
    auto phase = 0.0;

    return render (rate, lengthFor (tau),
                   [&] (double t)
                   {
                       const auto frequency = t < crisp ? 3.2 * hz : hz * (1.0 + 0.23 * std::exp (-(t - crisp) / sighTau));
                       const auto body = std::sin (phase) * std::exp (-t / tau);
                       phase += twoPi * frequency / rate;
                       const auto trigger = click.process (t < 0.001 ? 1.0 : 0.0);
                       return tone.process (body + 0.35 * trigger);
                   });
}

Sound make808Snare (double rate, double tone, double snappy)
{
    // Its two resonators (Q 16 and 10), Tone crossfading them, struck at once; its snares, noise rising over 1.4 ms and
    // falling over 28, high-passed at 2.75 kHz (and 590 Hz) and rolled off above 5 kHz.
    Ringing low ({ .hz = 238.0, .tau = 0.029 }, rate), high ({ .hz = 476.0, .tau = 0.010 }, rate);
    juce::Random random (808);
    auto shelf = highPass (rate, 2750.0, 0.73), roll = lowPass (rate, 5000.0);
    HighPass1 lowCut (rate, 590.0);
    const auto lowLevel = 1.0 - 0.6 * tone, highLevel = 0.4 + 0.6 * tone;

    return render (rate, 0.3,
                   [&] (double t)
                   {
                       const auto shell = lowLevel * low.at (t) + highLevel * high.at (t);
                       const auto hiss = run (roll, run (shelf, lowCut.process (drums::noise (random))));
                       const auto snares = 1.6 * snappy * hiss * (1.0 - std::exp (-t / 0.0014)) * std::exp (-t / 0.028);
                       return 0.7 * shell + snares;
                   });
}

Sound make808Tom (double rate, double hz, double decaySeconds)
{
    // A resonator swelling in over a few milliseconds, a little sharp as it is struck, and a faint low noise behind it,
    // ringing a little longer: the toms' "reverb".
    const auto tau = decaySeconds * t20ToTau;
    Ringing body ({ .hz = hz, .tau = tau, .bend = 1.04, .bendTau = 0.02, .swell = 0.003 }, rate);
    juce::Random random ((int) hz);
    auto rumble = lowPass (rate, 271.0);

    return render (
        rate, lengthFor (1.3 * tau), [&] (double t)
        { return body.at (t) + 0.5 * run (rumble, drums::noise (random)) * std::exp (-t / (1.3 * tau)) * (1.0 - std::exp (-t / 0.004)); });
}

Sound make808Conga (double rate, double hz, double decaySeconds)
{
    const auto tau = decaySeconds * t20ToTau;
    Ringing body ({ .hz = hz, .tau = tau, .bend = 1.03, .bendTau = 0.012, .swell = 0.0015 }, rate);
    return render (rate, lengthFor (tau), [&] (double t) { return body.at (t); });
}

Sound make808Rim (double rate)
{
    // Two resonators (455 Hz and 1.67 kHz, Q 11) through its "swing" VCA, which chops them short (5 ms) and clips them: the
    // wood in it; then a resonant high-pass at 590 Hz, and a mute 12 ms in.
    Ringing low ({ .hz = 455.0, .tau = 11.0 / (juce::MathConstants<double>::pi * 455.0) }, rate);
    Ringing high ({ .hz = 1667.0, .tau = 11.0 / (juce::MathConstants<double>::pi * 1667.0) }, rate);
    auto cut = highPass (rate, 590.0, 2.9);

    return render (rate, 0.04,
                   [&] (double t)
                   {
                       const auto swung = (low.at (t) + high.at (t)) * std::exp (-t / 0.005);
                       return run (cut, softClip (1.4 * swung, 3.0)) * gate (t, 0.012, 0.001);
                   });
}

Sound make808Claves (double rate)
{
    Ringing ring ({ .hz = 2500.0, .tau = 0.035 }, rate);
    return render (rate, 0.04, [&] (double t) { return ring.at (t) * gate (t, 0.021, 0.001); });
}

Sound make808Clap (double rate)
{
    // Noise band-passed at 1.1 kHz (Q 1.9), struck four times 11.5 ms apart, the first three cut off by the next, the fourth
    // falling over 20 ms, saturated a little; and its reverb, the same noise low-passed at 1.2 kHz, rising slowly behind them
    // so that their gaps stay clean, then falling over 150 ms.
    juce::Random random (3);
    auto band = bandPass (rate, 1100.0, 1.9), room = lowPass (rate, 1200.0);
    LowPass1 top (rate, 5000.0); // the gentle roll-off of what is above it, as its circuit has
    constexpr double spacing = 0.0115;

    return render (rate, 0.9,
                   [&] (double t)
                   {
                       const auto white = drums::noise (random);
                       const auto strike = std::min (3, (int) (t / spacing));
                       const auto since = t - strike * spacing;
                       const auto hands = std::exp (-since / (strike < 3 ? 0.0035 : 0.02));
                       const auto tail = (1.0 - std::exp (-t / 0.035)) * std::exp (-t / 0.15);
                       return softClip (1.6 * hands * top.process (run (band, white)) + 0.45 * tail * run (room, white), 1.3);
                   });
}

Sound make808Maracas (double rate)
{
    // Noise high-passed at 1.1 kHz and resonantly at 10 kHz, opening over 9 ms, open for 32 ms, then cut.
    juce::Random random (6);
    HighPass1 lowCut (rate, 1100.0);
    auto air = highPass (rate, 10000.0, 2.0);

    return render (rate, 0.06, [&] (double t)
                   { return run (air, lowCut.process (drums::noise (random))) * (1.0 - std::exp (-t / 0.009)) * gate (t, 0.032, 0.002); });
}

Sound make808Cowbell (double rate, double drive)
{
    // Its 540 and 800 Hz squares, high-passed at 1.4 kHz into a band-pass at 890 Hz (Q 3) and high-passed again at 330 Hz,
    // through two decays: a strike over 8.7 ms, and a ring over 130 ms.
    juce::Random random (11);
    Square first (rate, 540.0, random.nextDouble()), second (rate, 800.0, random.nextDouble());
    HighPass1 before (rate, 1400.0), after (rate, 330.0);
    auto band = bandPass (rate, 890.0, 3.0);

    return render (rate, 0.7,
                   [&] (double t)
                   {
                       const auto tone = 0.5 * (first.next() + second.next());
                       const auto envelope = 0.82 * std::exp (-t / 0.0087) + 0.18 * std::exp (-t / 0.13);
                       const auto bell = 2.2 * after.process (run (band, before.process (tone))) * envelope;
                       return drive > 0.0 ? softClip (bell, drive) : bell;
                   });
}

Sound make808Cymbal (double rate, double decaySeconds, double tone)
{
    // Its six squares into two bands, a broad one at 3.44 kHz and one at 7.1 kHz; three VCAs, each with an envelope of its
    // own: the high band's strike (through a resonant high-pass at 11 kHz), the high band's decay, and the low band's longer
    // one (high-passed at 4.8 kHz); all swelling in over 2.6 ms; Tone balancing them; then rising 6 dB an octave. No noise.
    juce::Random random (12);
    auto squares = makeMetal (rate, metal808, random);
    auto lowBand = bandPass (rate, 3440.0, 0.7), highBand = bandPass (rate, 7100.0, 1.4);
    auto strikeCut = highPass (rate, 11000.0, 2.0), lowCut = highPass (rate, 4800.0, 0.7);
    const auto tau = decaySeconds * t20ToTau;
    auto last = 0.0;

    return render (rate, lengthFor (1.4 * tau, 0.05),
                   [&] (double t)
                   {
                       const auto metal = nextMetal (squares);
                       const auto low = run (lowBand, metal), high = run (highBand, metal);
                       const auto swell = 1.0 - std::exp (-t / 0.0009);
                       const auto strike = run (strikeCut, high) * std::exp (-t / 0.012);
                       const auto body = high * std::exp (-t / tau);
                       const auto ring = run (lowCut, low) * std::exp (-t / (1.4 * tau));
                       const auto mixed = swell * softClip (0.9 * strike + tone * body + (1.0 - 0.6 * tone) * ring, 1.2);
                       const auto rising = mixed - 0.6 * last;
                       last = mixed;
                       return rising;
                   });
}

namespace
{
    // The 808's hats: its six squares' 7.1 kHz band through its "swing" VCA (which clips), a resonant high-pass, and a
    // gentle roll-off of the top.
    Sound make808Hat (double rate, double tau, double cutAt, double highPassHz, double seconds)
    {
        juce::Random random (13);
        auto squares = makeMetal (rate, metal808, random);
        auto band = bandPass (rate, 7100.0, 1.4), cut = highPass (rate, highPassHz, 2.5);
        LowPass1 top (rate, 14000.0);

        return render (rate, seconds,
                       [&] (double t)
                       {
                           const auto swung = softClip (2.0 * run (band, nextMetal (squares)) * std::exp (-t / tau), 1.5);
                           return top.process (run (cut, swung)) * gate (t, cutAt, 0.012);
                       });
    }
} // namespace

Sound make808ClosedHat (double rate)
{
    return make808Hat (rate, 0.015, 1.0, 11700.0, 0.12);
}

Sound make808OpenHat (double rate, double decaySeconds)
{
    return make808Hat (rate, 0.17, decaySeconds, 7800.0, decaySeconds + 0.08);
}

//==============================================================================
Sound make909Kick (double rate, double sweepSeconds, double decaySeconds, double attack)
{
    // Its triangle, reset as it is struck and rounded by diodes, swept down from four times its 52 Hz; its level held for
    // 12 ms, then falling (Decay); and its click, a short pulse and noise low-passed at 5 kHz, over 0.7 ms (Attack).
    constexpr double hz = 52.0, hold = 0.012;
    const auto tau = std::max (0.01, decaySeconds - hold) * t20ToTau;
    juce::Random random (909);
    auto clickTone = lowPass (rate, 5000.0);
    auto phase = 0.0;

    return render (rate, hold + lengthFor (tau),
                   [&] (double t)
                   {
                       const auto frequency = hz * (1.0 + 3.0 * std::exp (-t / sweepSeconds));
                       const auto body = roundedTriangle (phase) * holdThenFall (t, hold, tau);
                       phase += frequency / rate;
                       const auto pulse = t < 0.0004 ? 1.0 : 0.0;
                       const auto click = attack * (0.6 * pulse + run (clickTone, drums::noise (random))) * std::exp (-t / 0.0007);
                       return 0.85 * body + 0.5 * click;
                   });
}

Sound make909Snare (double rate, double tone, double snappy, double hz)
{
    // Two rounded triangles a ratio of 1.585 apart, bumped up a fifth as they start and settling over 7 ms, each fading at its
    // own pace; and noise, low-passed, then high-passed, holding for 24 ms and then falling faster and faster (Tone: for how
    // long), with a short, quiet second branch.
    juce::Random random (9090);
    auto smooth = lowPass (rate, 9000.0), bright = highPass (rate, 1200.0);
    auto phaseA = 0.0, phaseB = 0.0;
    const auto noiseTau = 0.03 + 0.09 * tone;

    return render (rate, 0.45,
                   [&] (double t)
                   {
                       const auto bump = 1.0 + 0.5 * std::exp (-t / 0.007);
                       const auto shell =
                           0.6 * roundedTriangle (phaseA) * std::exp (-t / 0.045) + 0.4 * roundedTriangle (phaseB) * std::exp (-t / 0.028);
                       phaseA += hz * bump / rate;
                       phaseB += 1.585 * hz * bump / rate;
                       const auto white = run (smooth, drums::noise (random));
                       const auto held = t < 0.024 ? 1.0 : std::exp (-std::pow ((t - 0.024) / noiseTau, 1.6));
                       const auto snares = snappy * (run (bright, white) * held + 0.3 * white * std::exp (-t / 0.008));
                       return 0.75 * shell + 0.9 * snares;
                   });
}

Sound make909Tom (double rate, double hz, double decaySeconds)
{
    // Three partials, at 1, 1.5 and 2.75 times its pitch, swept down together from 60 % above over 40 ms; the lowest rings for
    // the decay, the others only through the attack, the top one with noise.
    const auto tau = decaySeconds * t20ToTau;
    juce::Random random ((int) hz + 9);
    auto noiseTone = lowPass (rate, 4000.0);
    auto phase = 0.0;

    return render (rate, lengthFor (tau),
                   [&] (double t)
                   {
                       const auto sweep = 1.0 + 0.6 * std::exp (-t / 0.04);
                       const auto p = phase;
                       phase += hz * sweep / rate;
                       const auto first = roundedTriangle (p) * std::exp (-t / tau);
                       const auto second = std::sin (twoPi * 1.5 * p) * std::exp (-t / 0.02);
                       const auto third =
                           (std::sin (twoPi * 2.75 * p) + 0.5 * run (noiseTone, drums::noise (random))) * std::exp (-t / 0.012);
                       return 0.75 * first + 0.3 * second + 0.2 * third;
                   });
}

Sound make909Rim (double rate)
{
    // Three resonators (220, 500 and 1000 Hz, ringing for 45, 20 and 5 ms), clipped by diodes, cut short and high-passed.
    Ringing low ({ .hz = 220.0, .tau = 0.045 * t20ToTau, .level = 0.5 }, rate);
    Ringing mid ({ .hz = 500.0, .tau = 0.02 * t20ToTau }, rate);
    Ringing high ({ .hz = 1000.0, .tau = 0.005 * t20ToTau }, rate);
    auto cut = highPass (rate, 400.0, 1.0);

    return render (rate, 0.06, [&] (double t)
                   { return run (cut, softClip (1.5 * (low.at (t) + mid.at (t) + high.at (t)), 2.5)) * std::exp (-t / 0.008); });
}

Sound make909Clap (double rate)
{
    // The 808's design: four strikes 12 ms apart, cleaner and brighter, with a longer tail.
    juce::Random random (99);
    auto band = bandPass (rate, 1250.0, 2.0), room = lowPass (rate, 1500.0);
    LowPass1 top (rate, 7000.0);
    constexpr double spacing = 0.012;

    return render (rate, 1.0,
                   [&] (double t)
                   {
                       const auto white = drums::noise (random);
                       const auto strike = std::min (3, (int) (t / spacing));
                       const auto hands = std::exp (-(t - strike * spacing) / (strike < 3 ? 0.003 : 0.025));
                       const auto tail = (1.0 - std::exp (-t / 0.03)) * std::exp (-t / 0.22);
                       return 1.5 * hands * top.process (run (band, white)) + 0.4 * tail * run (room, white);
                   });
}

namespace
{
    // The 909's sampled metal: dense, inharmonic squares and their products (what a cymbal's modes make), with noise,
    // high-passed, at six bits.
    Sound make909Metal (double rate, double seconds, double tau, double highPassHz, double noiseLevel, double bell, int seed)
    {
        static constexpr std::array<double, 6> lows { 263.0, 400.0, 421.0, 474.0, 587.0, 845.0 };
        static constexpr std::array<double, 3> bells { 3150.0, 4230.0, 5610.0 };
        juce::Random random (seed);
        auto squares = makeMetal (rate, lows, random);
        auto squaresHigh = makeMetal (rate, { 1047.0, 1356.0, 1520.0, 1730.0, 2210.0, 2620.0 }, random);
        auto cut = highPass (rate, highPassHz, 0.9), cut2 = highPass (rate, highPassHz, 0.6);
        std::array<double, 3> bellPhases {};

        auto sound = render (rate, seconds,
                             [&] (double t)
                             {
                                 const auto metal =
                                     nextMetal (squares) * nextMetal (squaresHigh) * 4.0 + noiseLevel * drums::noise (random);
                                 auto ring = 0.0;

                                 for (size_t b = 0; b < bells.size(); ++b)
                                 {
                                     ring += std::sin (bellPhases[b]);
                                     bellPhases[b] += twoPi * bells[b] / rate;
                                 }

                                 const auto bellPart = bell * ring / 3.0 * std::exp (-t / (0.45 * tau));
                                 return (run (cut2, run (cut, metal)) + bellPart) * std::exp (-t / tau) * (1.0 - std::exp (-t / 0.0005));
                             });

        playAsSixBits (sound, rate);
        return sound;
    }
} // namespace

Sound make909ClosedHat (double rate)
{
    return make909Metal (rate, 0.2, 0.028, 7500.0, 0.35, 0.0, 41);
}

Sound make909OpenHat (double rate, double decaySeconds)
{
    const auto tau = decaySeconds * t20ToTau;
    return make909Metal (rate, lengthFor (tau, 0.05), tau, 6500.0, 0.4, 0.0, 42);
}

Sound make909Crash (double rate)
{
    return make909Metal (rate, 2.6, 0.55, 3200.0, 0.6, 0.15, 43);
}

Sound make909Ride (double rate)
{
    return make909Metal (rate, 3.0, 0.8, 4500.0, 0.25, 0.6, 44);
}

//==============================================================================
Sound makeBoxKick (double rate)
{
    // Small: a resonator at 62 Hz and a click, without the 808's long boom.
    Ringing body ({ .hz = 62.0, .tau = 0.07, .bend = 1.12, .bendTau = 0.012 }, rate);
    HighPass1 click (rate, 600.0);
    return render (rate, 0.5, [&] (double t) { return body.at (t) + 0.3 * click.process (t < 0.0008 ? 1.0 : 0.0); });
}

Sound makeBoxSnare (double rate)
{
    // Noise with a tuned body under it.
    Ringing body ({ .hz = 205.0, .tau = 0.025, .bend = 1.08, .bendTau = 0.006 }, rate);
    juce::Random random (78);
    auto band = bandPass (rate, 3200.0, 0.8);
    return render (rate, 0.3,
                   [&] (double t) { return 0.6 * body.at (t) + 1.2 * run (band, drums::noise (random)) * std::exp (-t / 0.045); });
}

Sound makeBoxRim (double rate)
{
    Ringing ring ({ .hz = 1650.0, .tau = 0.006 }, rate), low ({ .hz = 520.0, .tau = 0.008, .level = 0.6 }, rate);
    auto cut = highPass (rate, 500.0, 1.5);
    return render (rate, 0.05, [&] (double t) { return run (cut, softClip (1.3 * (ring.at (t) + low.at (t)), 2.0)); });
}

Sound makeBoxHat (double rate, double decaySeconds)
{
    // White noise through a ringing band-pass, its tick on top.
    juce::Random random (781);
    auto ring = bandPass (rate, 8200.0, 4.0), air = highPass (rate, 6000.0);
    const auto tau = decaySeconds * t20ToTau;
    return render (rate, lengthFor (tau),
                   [&] (double t)
                   {
                       const auto white = drums::noise (random);
                       return (2.2 * run (ring, white) + 0.4 * run (air, white)) * std::exp (-t / tau);
                   });
}

Sound makeBoxCymbal (double rate)
{
    juce::Random random (782);
    auto ring = bandPass (rate, 6200.0, 2.5), air = highPass (rate, 4500.0);
    return render (rate, 1.6,
                   [&] (double t)
                   {
                       const auto white = drums::noise (random);
                       return (1.6 * run (ring, white) + 0.5 * run (air, white)) * std::exp (-t / 0.32);
                   });
}

Sound makeBoxMetal (double rate)
{
    // Its metallic beat: three squares out of tune with each other through a resonant band-pass at 5.6 kHz, a clink.
    juce::Random random (783);
    Square a (rate, 4080.0, random.nextDouble()), b (rate, 5620.0, random.nextDouble()), c (rate, 6170.0, random.nextDouble());
    auto ring = bandPass (rate, 5600.0, 6.0);
    return render (rate, 0.12, [&] (double t) { return 2.0 * run (ring, (a.next() + b.next() + c.next()) / 3.0) * std::exp (-t / 0.018); });
}

Sound makeBoxMaracas (double rate)
{
    juce::Random random (784);
    auto air = highPass (rate, 7000.0, 1.2);
    return render (rate, 0.05,
                   [&] (double t) { return run (air, drums::noise (random)) * (1.0 - std::exp (-t / 0.003)) * std::exp (-t / 0.012); });
}

Sound makeBoxTambourine (double rate)
{
    // High-passed noise with the jingles' ring in it, shaken: a burst, then a second, softer one.
    juce::Random random (785);
    auto air = highPass (rate, 5000.0, 0.9), jingle = bandPass (rate, 9000.0, 3.0);
    return render (rate, 0.3,
                   [&] (double t)
                   {
                       const auto white = drums::noise (random);
                       const auto shake = std::exp (-t / 0.03) + 0.4 * (t > 0.045 ? std::exp (-(t - 0.045) / 0.05) : 0.0);
                       return (run (air, white) + 1.5 * run (jingle, white)) * shake;
                   });
}

Sound makeBoxGuiro (double rate)
{
    // A scrape: ten short bursts of noise through a band-pass that rises as it goes, over a step.
    juce::Random random (786);
    auto band = bandPass (rate, 2500.0, 3.0);
    constexpr int teeth = 10;
    constexpr double scrape = 0.16;
    auto sound = render (rate, scrape + 0.02,
                         [&] (double t)
                         {
                             const auto tooth = std::fmod (t, scrape / teeth) / (scrape / teeth);
                             const auto burst = t < scrape ? std::exp (-tooth * 6.0) : 0.0;
                             return drums::noise (random) * burst * (0.6 + 0.4 * t / scrape);
                         });

    // Its band-pass sweeping up across the scrape.
    for (int i = 0; i < sound.getNumSamples(); ++i)
    {
        if (i % 32 == 0)
            band.setCoefficients (juce::IIRCoefficients::makeBandPass (rate, 1800.0 + 2500.0 * (i / rate) / scrape, 3.0));

        sound.setSample (0, i, 1.6f * band.processSingleSampleRaw (sound.getSample (0, i)));
    }

    return sound;
}

Sound makeBoxCowbell (double rate)
{
    // The CR-78's: two squares close together (587 and 845 Hz) through a band-pass, as a CR-8000's cowbell is made.
    juce::Random random (787);
    Square first (rate, 587.0, random.nextDouble()), second (rate, 845.0, random.nextDouble());
    auto band = bandPass (rate, 2640.0, 1.5);
    return render (
        rate, 0.5, [&] (double t)
        { return 1.8 * run (band, 0.5 * (first.next() + second.next())) * (0.7 * std::exp (-t / 0.006) + 0.3 * std::exp (-t / 0.1)); });
}

Sound makeBoxClaves (double rate, double hz, double decaySeconds)
{
    const auto tau = decaySeconds * t20ToTau;
    Ringing ring ({ .hz = hz, .tau = tau }, rate);
    return render (rate, lengthFor (tau), [&] (double t) { return ring.at (t); });
}

Sound makeBoxDrum (double rate, double hz, double decaySeconds)
{
    const auto tau = decaySeconds * t20ToTau;
    Ringing body ({ .hz = hz, .tau = tau, .bend = 1.06, .bendTau = 0.008, .swell = 0.001 }, rate);
    return render (rate, lengthFor (tau), [&] (double t) { return body.at (t); });
}

//==============================================================================
Sound makeLongBass808 (double rate, double hz, double decaySeconds, double drive)
{
    // A sine falling an octave onto its pitch over 22 ms, for seconds, saturated: the harmonics carry it on small speakers.
    const auto tau = decaySeconds * t20ToTau;
    auto phase = 0.0;
    return render (rate, std::min (4.0, lengthFor (tau)),
                   [&] (double t)
                   {
                       const auto frequency = hz * (1.0 + std::exp (-t / 0.022));
                       const auto body = std::sin (phase) * std::exp (-t / tau) * (1.0 - std::exp (-t / 0.0006));
                       phase += twoPi * frequency / rate;
                       return softClip (body, drive);
                   });
}

Sound makePunchKick (double rate)
{
    // A short kick that cuts through over an 808: a fast sweep from 330 Hz, a bright click and a tight body.
    return makeKick (rate, 330.0, 57.0, 0.009, 0.1, 0.4, 1.0, 2.6);
}

Sound makeCrispSnare (double rate)
{
    return makeSnare (rate, { .lowHz = 200.0,
                              .highHz = 340.0,
                              .lowSeconds = 0.04,
                              .highSeconds = 0.025,
                              .snaresSeconds = 0.07,
                              .snaresLevel = 0.9,
                              .snaresLowCut = 1800.0,
                              .snaresHighCut = 9000.0,
                              .length = 0.3,
                              .drive = 2.4,
                              .seed = 31 });
}

Sound makeLayeredClap (double rate)
{
    // Two claps a few milliseconds apart, one higher, as producers stack them: wider and denser than one.
    auto first = make808Clap (rate);
    juce::Random random (32);
    auto band = bandPass (rate, 1700.0, 1.6);
    constexpr double spacing = 0.009, offset = 0.004;

    for (int i = 0; i < first.getNumSamples(); ++i)
    {
        const auto t = i / rate - offset;

        if (t < 0.0)
            continue;

        const auto strike = std::min (3, (int) (t / spacing));
        const auto hands = std::exp (-(t - strike * spacing) / (strike < 3 ? 0.003 : 0.03));
        first.setSample (0, i, first.getSample (0, i) + (float) (0.9 * hands * run (band, drums::noise (random))));
    }

    return first;
}

Sound makeSnap (double rate)
{
    // A finger snap: a click of noise around 2.5 kHz and a little ring.
    juce::Random random (33);
    auto band = bandPass (rate, 2600.0, 2.2);
    Ringing ring ({ .hz = 1900.0, .tau = 0.012, .level = 0.3 }, rate);
    return render (rate, 0.12, [&] (double t) { return 2.0 * run (band, drums::noise (random)) * std::exp (-t / 0.011) + ring.at (t); });
}

Sound makeTightHat (double rate, double decaySeconds)
{
    // Bright metal for rolls: the 808's squares and noise, high above 9 kHz, short.
    juce::Random random (34);
    auto squares = makeMetal (rate, metal808, random);
    auto cut = highPass (rate, 9000.0, 1.2), cut2 = highPass (rate, 7000.0);
    const auto tau = decaySeconds * t20ToTau;
    return render (rate, lengthFor (tau),
                   [&] (double t)
                   {
                       const auto metal = 0.7 * nextMetal (squares) + 0.6 * drums::noise (random);
                       return 1.5 * run (cut2, run (cut, metal)) * std::exp (-t / tau);
                   });
}

//==============================================================================
Sound makeHandDrum (double rate, double hz, double decaySeconds, bool slapped)
{
    // A skin: its pitch, its first overtone (1.59 times, a membrane's), and the hand's slap, a burst of band-passed noise.
    const auto tau = decaySeconds * t20ToTau;
    Ringing body ({ .hz = hz, .tau = tau, .bend = 1.05, .bendTau = 0.01, .swell = 0.0008 }, rate);
    Ringing overtone ({ .hz = 1.59 * hz, .tau = 0.5 * tau, .level = slapped ? 0.5 : 0.25 }, rate);
    juce::Random random ((int) hz);
    auto band = bandPass (rate, slapped ? 2500.0 : 1200.0, 1.5);
    return render (rate, lengthFor (tau),
                   [&] (double t)
                   {
                       const auto hand =
                           (slapped ? 1.4 : 0.5) * run (band, drums::noise (random)) * std::exp (-t / (slapped ? 0.012 : 0.006));
                       return body.at (t) + overtone.at (t) + hand;
                   });
}

Sound makeTimbale (double rate, double hz)
{
    // A shell that rings: its pitch and two metallic overtones, with a rim click.
    Ringing body ({ .hz = hz, .tau = 0.12, .bend = 1.02, .bendTau = 0.01 }, rate);
    Ringing ring1 ({ .hz = 2.31 * hz, .tau = 0.09, .level = 0.4 }, rate), ring2 ({ .hz = 3.73 * hz, .tau = 0.07, .level = 0.25 }, rate);
    juce::Random random ((int) hz + 1);
    auto band = bandPass (rate, 3000.0, 1.2);
    return render (rate, 0.9, [&] (double t)
                   { return body.at (t) + ring1.at (t) + ring2.at (t) + run (band, drums::noise (random)) * std::exp (-t / 0.004); });
}

Sound makeAgogo (double rate, double hz)
{
    // A bell: inharmonic partials (1, 2.76, 5.4 times), the higher shorter.
    Ringing a ({ .hz = hz, .tau = 0.18 }, rate), b ({ .hz = 2.76 * hz, .tau = 0.08, .level = 0.45 }, rate),
        c ({ .hz = 5.4 * hz, .tau = 0.04, .level = 0.25 }, rate);
    return render (rate, 1.0, [&] (double t) { return a.at (t) + b.at (t) + c.at (t); });
}

Sound makeWoodblock (double rate, double hz)
{
    Ringing a ({ .hz = hz, .tau = 0.025, .bend = 1.03, .bendTau = 0.003 }, rate), b ({ .hz = 2.6 * hz, .tau = 0.012, .level = 0.3 }, rate);
    return render (rate, 0.15, [&] (double t) { return a.at (t) + b.at (t); });
}

Sound makeShaker (double rate)
{
    // Grains of noise high above 5 kHz, swelling and falling over 80 ms.
    juce::Random random (41);
    auto air = highPass (rate, 5500.0, 1.0);
    return render (rate, 0.14,
                   [&] (double t)
                   {
                       const auto grains = 0.5 + 0.5 * random.nextFloat();
                       return run (air, drums::noise (random)) * grains * (t < 0.03 ? t / 0.03 : std::exp (-(t - 0.03) / 0.03));
                   });
}

Sound makeTambourine (double rate)
{
    // Jingles: metal (inharmonic squares above 6 kHz) and noise, with the frame's knock.
    juce::Random random (42);
    auto squares = makeMetal (rate, { 5300.0, 6100.0, 7400.0, 8100.0, 9300.0, 10200.0 }, random);
    auto air = highPass (rate, 6000.0, 0.8);
    Ringing knock ({ .hz = 380.0, .tau = 0.01, .level = 0.3 }, rate);
    return render (rate, 0.45, [&] (double t)
                   { return (run (air, 0.6 * nextMetal (squares) + 0.7 * drums::noise (random))) * std::exp (-t / 0.09) + knock.at (t); });
}

Sound makeTriangle (double rate, bool muted)
{
    // A steel bar's partials, nearly harmonic, ringing long (or damped).
    const auto tau = muted ? 0.05 : 1.1;
    Ringing a ({ .hz = 1240.0, .tau = tau }, rate), b ({ .hz = 3720.0, .tau = 0.7 * tau, .level = 0.5 }, rate),
        c ({ .hz = 6210.0, .tau = 0.5 * tau, .level = 0.3 }, rate), d ({ .hz = 8650.0, .tau = 0.4 * tau, .level = 0.2 }, rate);
    return render (rate, lengthFor (tau, 0.02), [&] (double t) { return a.at (t) + b.at (t) + c.at (t) + d.at (t); });
}

Sound makeCabasa (double rate)
{
    juce::Random random (43);
    auto air = highPass (rate, 4500.0, 0.8), band = bandPass (rate, 8000.0, 1.5);
    return render (rate, 0.12,
                   [&] (double t)
                   {
                       const auto white = drums::noise (random);
                       return (run (air, white) + run (band, white)) * (t < 0.01 ? t / 0.01 : std::exp (-(t - 0.01) / 0.025));
                   });
}
} // namespace phazite::devices::drums
