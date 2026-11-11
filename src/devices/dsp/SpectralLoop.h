// SPDX-License-Identifier: AGPL-3.0-or-later
// A sound made from its spectrum by the PADsynth algorithm, as Paul Nasca published it for ZynAddSubFX ("PADsynth
// algorithm", zynaddsubfx.sourceforge.net/doc/PADsynth/PADsynth.htm): each harmonic of a tone is spread into a band of
// frequencies, a Gaussian so many cents wide, every frequency gets a random phase, and one inverse FFT makes the sound of
// them all: a loop that goes round without a seam, since the transform's period is the loop. A band is what many players of
// one note make together, each a little off the others' pitch: an ensemble, a choir, a pad. Where a few detuned oscillators
// beat in a pattern, a band holds every pitch in it, so each harmonic's level wanders and never repeats within the loop.
// The sampler's factory sounds are made with it (sampler/FactorySamples.h). Not for the audio thread: it allocates, and a
// loop of 2^18 samples takes some tens of milliseconds.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace phazite::devices::dsp
{
struct SpectralLoop
{
    // Each harmonic's level, from the first (the fundamental), as an additive sound would have them: a harmonic keeps its
    // power however wide its band is. Those at or past half the sample rate are left out.
    std::span<const float> harmonics;

    double fundamental = 220.0; // hertz

    // How wide the first harmonic's band is, in cents, as Nasca's bw: its level falls as exp (-x^2), x the distance from
    // the harmonic over half the bandwidth, so the band's power has a standard deviation of a quarter of it. About 10 is a
    // few players in tune, 20 to 40 an ensemble, 60 and more a choir or a wash.
    double bandwidth = 20.0;

    // How the bands widen up the harmonics, as Nasca's bwscale: harmonic n's is n^bandwidthScale times the first's in
    // hertz. At 1 every band is as wide in cents, as detuned players' harmonics are; below 1 the upper harmonics stay
    // clearer, and at 0 every band is as wide in hertz.
    double bandwidthScale = 1.0;

    double sampleRate = 48000.0;
    int order = 17;         // the loop is 2^order samples long
    std::uint32_t seed = 1; // of its phases (dsp/Random.h): the same loop from the same seed, on every machine
};

// The loop: one channel, 2^order samples, at an RMS of 1 (its peaks are about 12 dB above that), silent for a spectrum
// with nothing in it. It has no start and no end: any place in it is as good a start as another, and the same loop read
// from half-way round is as good as unrelated to it, which makes a wide stereo sound of the two.
std::vector<float> makeSpectralLoop (const SpectralLoop&);
} // namespace phazite::devices::dsp
