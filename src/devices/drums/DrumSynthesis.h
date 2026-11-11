// SPDX-License-Identifier: AGPL-3.0-or-later
// Drums that Phazite synthesizes, after Sound On Sound's "Synth Secrets" on drums (2002) and the analysis of the 808's
// circuits by Werner, Abel and Smith (DAFx-14, ICMC 2014): the drum sampler's factory sounds (FactoryDrums.h), and some of
// Phazite Sounds (tools/soundkit/). Each is seeded, so it comes out the same every time, whatever compiler built it: their
// noise is drawn from JUCE's generator or from Phazite's own (dsp/Random.h), never from the standard library's
// distributions, which each library works out its own way. They allocate: never on the audio thread.
#pragma once

#include "devices/dsp/Random.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace phazite::devices::drums
{
using Sound = juce::AudioBuffer<float>;

// White noise from -1 to 1: from JUCE's generator, as the sounds made before the natural kits draw theirs (and so stay as
// they were), or from Phazite's own, as what is newer draws it.
float noise (juce::Random&);
float noise (dsp::Random&);

// A generator of Phazite's for a seed and one of the streams a sound draws from (a side, a layer): each stream's noise is
// its own.
dsp::Random makeRandom (int seed, int stream = 0);

// tanh saturation, scaled so that 1 stays 1.
float softClip (double x, double drive);

// A kick, a tom, an 808, a rim or a clave: a sine whose pitch falls from startHz to endHz, a click, and saturation.
Sound makeKick (double sampleRate, double startHz, double endHz, double pitchSeconds, double decaySeconds, double length, double click,
                double drive);

// A rung resonator, as a bridged-T or a twin-T circuit rings when struck, or one mode of a shell, a bar or a bell: a sine
// whose frequency starts `bend` times as high and settles over bendTau, its level swelling in over swell, held for hold and
// then falling with tau.
struct Ring
{
    double hz = 100.0, tau = 0.1, bend = 1.0, bendTau = 0.01, swell = 0.0, level = 1.0;
    double hold = 0.0;
};

// A ring as it sounds: at() gives its next sample, asked for at each sample's time in turn.
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
        const auto fall = ring.hold > 0.0 ? std::exp (-std::max (0.0, t - ring.hold) / ring.tau) : std::exp (-t / ring.tau);
        const auto value = std::sin (phase) * fall * (ring.swell > 0.0 ? 1.0 - std::exp (-t / ring.swell) : 1.0);
        phase += juce::MathConstants<double>::twoPi * hz / rate;
        return ring.level * value;
    }
};

// A kick or a drum shaped more closely, as a sampled one measures: its pitch falling in two stages, a chirp from chirpHz
// in its first milliseconds and then from startHz to endHz; swelling in over attackSeconds and held at full level until
// holdSeconds before it decays, as a gated drum is; and a click around clickHz, as wide as clickQ says: band-passed noise,
// or (clickKnock, from 0 to 1, its share) that band struck once, a knock that is the same whatever the seed. The body can
// go through an equalizer of its own before the click and the drive, a bell (boost) and a low shelf, as Phazite's EQ makes
// them: the boom an engineer's low boost gives a kick, which also lets a body whose pitch falls into the boost swell as it
// lands. Boosted, the body is brought back to a peak of 1, so that the click, the breath and the drive stay what they say
// against it.
struct KickShape
{
    double chirpHz = 0.0, chirpSeconds = 0.0015; // no chirp at 0
    double startHz = 160.0, endHz = 50.0, pitchSeconds = 0.03;
    double attackSeconds = 0.0005;
    double holdSeconds = 0.0, decaySeconds = 0.3;
    double boostHz = 80.0, boostQ = 1.0, boostDecibels = 0.0; // no bell at 0 dB
    double shelfHz = 60.0, shelfDecibels = 0.0;               // no shelf at 0 dB
    double click = 0.5, clickHz = 1000.0, clickSeconds = 0.003;
    double clickQ = 0.8, clickKnock = 0.0;
    double noise = 0.0, noiseHz = 4000.0, noiseSeconds = 0.05; // a breath of band-passed noise under the body
    double length = 0.6, drive = 1.6;
    int seed = 1;
};

Sound makeKick (double sampleRate, const KickShape&);

// A snare: its shell, two tuned sines that fade each at its own pace (and any further modes of it, each a ring at its own
// level), and its snares, band-passed noise that rings for snaresSeconds. The defaults are the default kit's snare.
struct SnareShape
{
    double lowHz = 185.0, highHz = 330.0;
    double lowSeconds = 0.07, highSeconds = 0.045;
    double snaresSeconds = 0.11;
    double snaresLevel = 0.55;                           // against the shell's tones, at most 1
    double snaresLowCut = 800.0, snaresHighCut = 9000.0; // Hz
    double length = 0.45, drive = 1.6;
    int seed = 2;
    double lowLevel = 0.6, highLevel = 0.4; // the shell's tones' levels
    std::vector<Ring> modes {};             // more of the shell's modes
};

Sound makeSnare (double sampleRate, const SnareShape& = {});

// A clap: band-passed noise struck four times about 10 ms apart (times spread), then a tail: several hands, as the 808's
// clap does it. More hands strike more often (strikes, from 1 to 6, each later one a little sooner after the last); and
// with a width, each side hears noise of its own beside what both hear, at that level against it, so that the clap is in
// stereo, its hands a little apart (0.3 leaves the sides' difference about 14 dB under what they share).
struct ClapShape
{
    double centreHz = 1100.0, q = 1.3, lowCut = 500.0;
    double spread = 1.0; // the strikes' spacing, against the 808's
    double tailSeconds = 0.13;
    double length = 0.55;
    int seed = 3;
    double tailLevel = 1.0; // the tail's level against the strikes', as a room or a plate leaves it
    int strikes = 4;
    double width = 0.0; // mono at 0
};

Sound makeClap (double sampleRate, const ClapShape& = {});

// A hat, closed or open, or a cymbal: six square waves at the 808's cymbal frequencies (times pitch), with some noise,
// band-passed and high-passed (lowCutQ: 0.5 for the gentlest knee). It rings out over decaySeconds; with a fade, it is cut
// instead, as a sampled or a choked hat stops: still falling by its decay until holdSeconds, then down to nothing over
// fadeSeconds (a long decay holds it flat until then). With an attack it comes in over that long, as a pedal closes a hat.
struct HatShape
{
    double decaySeconds = 0.05, length = 0.25;
    double noise = 0.15; // against the square waves' metal
    double centreHz = 7100.0, lowCut = 6000.0;
    double pitch = 1.0;
    int seed = 4;
    double lowCutQ = 0.707;
    double holdSeconds = 0.0, fadeSeconds = 0.0; // not cut without a fade
    double attackSeconds = 0.0;
};

Sound makeHat (double sampleRate, const HatShape&);

// Two square waves, 540 and 800 Hz, through a band-pass: a strike, then a ringing tail.
Sound makeCowbell (double sampleRate);

// What rings when it is struck, metal or wood, a bell, a bar, a block or a pipe, by its modes, as modal synthesis makes it
// (a sum of decaying sines, Perry Cook's "Real Sound Synthesis for Interactive Applications", 2002): its partials, each a
// ring of its own (inharmonic for metal; a partial with a hold stays at its level that long before it falls, as a hard
// strike or a gate holds it), and the stick's click, noise through a band (clickHz, clickQ) and under clickHighCut (24 dB
// an octave), held for clickHoldSeconds and then falling with clickSeconds. Saturated where it has a drive.
struct StruckShape
{
    std::vector<Ring> partials {};
    double click = 0.5, clickHz = 3000.0, clickQ = 0.7, clickHighCut = 20000.0;
    double clickHoldSeconds = 0.0, clickSeconds = 0.004;
    double length = 0.3, drive = 0.0; // clean at 0
    int seed = 1;
};

Sound makeStruck (double sampleRate, const StruckShape&);

// Two mono sounds as one stereo sound, the left and the right, as long as the longer.
Sound makeStereo (const Sound& left, const Sound& right);

// A gated reverb, as the 80s gave their snares, claps and toms (Hugh Padgham's compressed room on Phil Collins's drums, and
// the AMS RMX16's Nonlin programs after it): the drum played into a dense, bright room, whose sound a gate the hit opens
// lets through, coming in over onsetSeconds (so that the drum is heard first), falling only by fallDecibels while it holds
// for gateSeconds, then cut in closeSeconds, so that the drum sounds huge yet stops dead, whatever its own decay. The room
// is at its level against the drum's peak, through its band (the RMX16 keeps it under 8 kHz), each side a room of its own
// so that it spreads wide. A mono drum comes back stereo, long enough for its fade (FactoryDrums.h) to come after the gate.
struct GatedReverbShape
{
    double onsetSeconds = 0.012, gateSeconds = 0.25, closeSeconds = 0.05;
    double fallDecibels = 3.0;
    double level = 0.5; // the room's peak against the drum's
    double lowCut = 250.0, highCut = 8000.0;
    int seed = 1;
};

Sound addGatedReverb (const Sound& drum, double sampleRate, const GatedReverbShape&);

// A room that isn't gated, a hall or a plate left to ring: the drum played into dense noise that answers after
// preDelaySeconds, comes in over onsetSeconds (so that the hit is heard dry first) and then falls smoothly, 60 dB in
// decaySeconds at its low end and faster the higher, as rooms do: its top (far above dampingHz) takes highDecay times as
// long, and at dampingHz the fall is halfway between the two rates, so that the tail darkens as it falls. A room's answer
// as exponentially decaying noise is James Moorer's ("About This Reverberation Business", 1979); each frequency falling
// at its own rate, as Jean-Marc Jot's absorption filters make a feedback delay network's fall (1992). Nothing splits
// the room into bands: each frequency of one noise falls at its own rate. The room is at its level against the drum's
// peak, through its band, each side a room of its own at width 1 (so that it spreads wide once it has come in, the hit
// staying in the middle), the same room on both at 0. A mono drum comes back stereo, long enough for the tail to be 60 dB
// down before a kit's fade (FactoryDrums.h).
struct RoomShape
{
    double preDelaySeconds = 0.0, onsetSeconds = 0.01;
    double decaySeconds = 0.8;
    double highDecay = 0.5, dampingHz = 3000.0;
    double level = 0.3; // the room's peak against the drum's
    double lowCut = 200.0, highCut = 8000.0;
    double width = 1.0;
    int seed = 1;
};

Sound addRoom (const Sound& drum, double sampleRate, const RoomShape&);

// A drum as the Simmons SDS-V makes its toms and snares: a triangle wave whose pitch bends down only, from startHz to endHz
// (bendSeconds its time constant), fading over toneSeconds; noise through a four-pole low-pass without resonance, as the
// SDS-V's toms have none, its cutoff sweeping from noiseFromHz down to noiseToHz (sweepSeconds) as it fades over
// noiseSeconds; and a millisecond's click, as its Click control adds. With a hold, the tone stays at its level that long
// before it fades, as a compressed drum's does, while its noise fades from the start.
struct SimmonsShape
{
    double startHz = 200.0, endHz = 100.0, bendSeconds = 0.15;
    double toneSeconds = 0.15;
    double noise = 0.15; // the noise's share, against the tone's
    double noiseFromHz = 4000.0, noiseToHz = 600.0, sweepSeconds = 0.15, noiseSeconds = 0.1;
    double click = 0.3;
    double length = 1.0, drive = 1.2;
    int seed = 1;
    double holdSeconds = 0.0;
};

Sound makeSimmons (double sampleRate, const SimmonsShape&);

// A drum as an old sampler played it back: read at heldRate with bits of resolution, its top cut at cutoffHz, and
// saturated with drive. In place. Companded (muLaw), its steps grow with its level, as the LinnDrum's and the Oberheim
// DMX's 8-bit samples' did, so that a quiet tail stays clean while the hit is gritty.
struct DustShape
{
    double heldRate = 22050.0;
    int bits = 12;
    double cutoffHz = 6000.0;
    double drive = 1.5;
    bool muLaw = false;
};

void makeDusty (Sound&, double sampleRate, const DustShape&);
} // namespace phazite::devices::drums
