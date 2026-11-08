// SPDX-License-Identifier: AGPL-3.0-or-later
#include "FactoryDrums.h"
#include "DrumMachines.h"
#include "DrumSynthesis.h"
#include "devices/dsp/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <mutex>

namespace phazite::devices
{
namespace
{
    using drums::ClapShape;
    using drums::DustShape;
    using drums::HatShape;
    using drums::makeClap;
    using drums::makeHat;
    using drums::makeKick;
    using drums::makeSnare;
    using drums::SnareShape;
    using Audio = drums::Sound;

    constexpr double rate = 48000.0;  // what they are made at: the sampler plays them at any rate
    constexpr double fadeShare = 0.3; // of a kit's sound, fading out at its end

    //==============================================================================
    // The default kit's toms and hats.
    Audio tom (double fromHz, double toHz)
    {
        return makeKick (rate, fromHz, toHz, 0.05, 0.25, 0.55, 0.15, 1.2);
    }

    Audio hat (double decaySeconds, double length)
    {
        return makeHat (rate, { .decaySeconds = decaySeconds, .length = length });
    }

    // Each kit's toms: one drum at six pitches, from General MIDI's low floor tom to its high tom.
    Audio houseTom (double hz)
    {
        return makeKick (rate, hz * 1.4, hz, 0.04, 0.26, 0.75, 0.2, 1.6);
    }

    Audio punchyTom (double hz)
    {
        return makeKick (rate, hz * 1.6, hz, 0.018, 0.17, 0.5, 0.6, 2.4);
    }

    Audio tightTom (double hz)
    {
        return makeKick (rate, hz * 1.3, hz, 0.02, 0.11, 0.35, 0.35, 1.8);
    }

    Audio wideTom (double hz)
    {
        return makeKick (rate, hz * 1.35, hz, 0.045, 0.28, 0.8, 0.2, 1.5);
    }

    // The lo-fi kit: each drum as an old sampler plays it back.
    Audio dusty (Audio sound, const DustShape& shape)
    {
        drums::makeDusty (sound, rate, shape);
        return sound;
    }

    Audio lofiTom (double hz)
    {
        return dusty (makeKick (rate, hz * 1.2, hz, 0.05, 0.22, 0.6, 0.1, 1.3),
                      { .heldRate = 22050.0, .bits = 12, .cutoffHz = 4000.0, .drive = 1.3 });
    }

    Audio lofiHat (const HatShape& shape, double cutoffHz)
    {
        return dusty (makeHat (rate, shape), { .heldRate = 22050.0, .bits = 10, .cutoffHz = cutoffHz, .drive = 1.3 });
    }

    // The wide kit: the same drum on each side, from other noise on the right (and its metal a little higher), so that
    // what is noise in it spreads wide and what is tone stays in the middle.
    Audio wideSnare (const SnareShape& shape)
    {
        auto right = shape;
        right.seed += 20;
        return drums::makeStereo (makeSnare (rate, shape), makeSnare (rate, right));
    }

    Audio wideClap (const ClapShape& shape)
    {
        auto right = shape;
        right.spread *= 1.2;
        right.seed += 30;
        return drums::makeStereo (makeClap (rate, shape), makeClap (rate, right));
    }

    Audio wideHat (const HatShape& shape)
    {
        auto right = shape;
        right.pitch *= 1.03;
        right.seed += 40;
        return drums::makeStereo (makeHat (rate, shape), makeHat (rate, right));
    }

    //==============================================================================
    struct Entry
    {
        std::string_view name {};
        Audio (*make)() = nullptr;
    };

    // Each kit in the pads' order: General MIDI's drums from note 36, a kick, up to 51, a ride.
    constexpr Entry entries[] = {
        // The default kit, as a new sampler has played it since the drum sampler was made (roadmap step 1.6).
        { "Kick", [] { return makeKick (rate, 160.0, 48.0, 0.035, 0.32, 0.7, 0.6, 1.6); } },
        { "Rim", [] { return makeKick (rate, 1700.0, 900.0, 0.004, 0.018, 0.12, 1.0, 2.2); } },
        { "Snare", [] { return makeSnare (rate); } },
        { "Clap", [] { return makeClap (rate); } },
        { "Snare 2", [] { return makeSnare (rate, { .lowHz = 230.0, .highHz = 410.0, .snaresSeconds = 0.08 }); } },
        { "Low Tom", [] { return tom (140.0, 85.0); } },
        { "Closed Hat", [] { return hat (0.035, 0.18); } },
        { "Floor Tom", [] { return tom (160.0, 100.0); } },
        { "Pedal Hat", [] { return hat (0.06, 0.25); } },
        { "Mid Tom", [] { return tom (190.0, 120.0); } },
        { "Open Hat", [] { return hat (0.32, 0.9); } },
        { "Mid Tom 2", [] { return tom (220.0, 140.0); } },
        { "High Tom", [] { return tom (250.0, 165.0); } },
        { "Crash", [] { return hat (0.9, 2.2); } },
        { "High Tom 2", [] { return tom (290.0, 190.0); } },
        { "Ride", [] { return hat (0.7, 1.8); } },

        // The 808's sixteen voices (DrumMachines.h), General MIDI's drums where it has them, its own where it hasn't: its
        // claves for the second snare, its maracas for the pedal hat, its three congas between its three toms, its cymbal
        // for the crash and its cowbell for the ride.
        { "808 Kick", [] { return drums::make808Kick (rate); } },
        { "808 Rim", [] { return drums::make808Rim (rate); } },
        { "808 Snare", [] { return drums::make808Snare (rate); } },
        { "808 Clap", [] { return drums::make808Clap (rate); } },
        { "808 Claves", [] { return drums::make808Claves (rate); } },
        { "808 Low Tom", [] { return drums::make808Tom (rate, 90.0, 0.2); } },
        { "808 Closed Hat", [] { return drums::make808ClosedHat (rate); } },
        { "808 Mid Tom", [] { return drums::make808Tom (rate, 135.0, 0.13); } },
        { "808 Maracas", [] { return drums::make808Maracas (rate); } },
        { "808 High Tom", [] { return drums::make808Tom (rate, 185.0, 0.1); } },
        { "808 Open Hat", [] { return drums::make808OpenHat (rate); } },
        { "808 Low Conga", [] { return drums::make808Conga (rate, 185.0, 0.18); } },
        { "808 Mid Conga", [] { return drums::make808Conga (rate, 280.0, 0.1); } },
        { "808 Cymbal", [] { return drums::make808Cymbal (rate); } },
        { "808 High Conga", [] { return drums::make808Conga (rate, 400.0, 0.08); } },
        { "808 Cowbell", [] { return drums::make808Cowbell (rate); } },

        // House: a round kick after the 909's, a big clap, crisp hats with a short open hat, and a bright ride.
        { "House Kick", [] { return makeKick (rate, 180.0, 54.0, 0.018, 0.34, 0.9, 0.35, 2.1); } },
        { "House Rim", [] { return makeKick (rate, 1500.0, 1350.0, 0.002, 0.016, 0.1, 0.5, 2.2); } },
        { "House Snare",
          []
          {
              return makeSnare (rate, { .lowHz = 205.0,
                                        .highHz = 375.0,
                                        .lowSeconds = 0.065,
                                        .highSeconds = 0.04,
                                        .snaresSeconds = 0.15,
                                        .snaresLevel = 0.65,
                                        .snaresLowCut = 1000.0,
                                        .snaresHighCut = 11000.0,
                                        .length = 0.5,
                                        .drive = 1.9 });
          } },
        { "House Clap",
          []
          {
              return makeClap (rate, { .centreHz = 1250.0, .q = 1.2, .lowCut = 550.0, .spread = 1.15, .tailSeconds = 0.22, .length = 0.8 });
          } },
        { "House Snare 2",
          []
          {
              return makeSnare (rate, { .lowHz = 245.0,
                                        .highHz = 445.0,
                                        .lowSeconds = 0.05,
                                        .highSeconds = 0.035,
                                        .snaresSeconds = 0.11,
                                        .snaresLevel = 0.75,
                                        .snaresLowCut = 1400.0,
                                        .snaresHighCut = 12000.0,
                                        .length = 0.4,
                                        .drive = 2.0 });
          } },
        { "House Low Tom", [] { return houseTom (88.0); } },
        { "House Closed Hat",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.035, .length = 0.18, .noise = 0.45, .centreHz = 8800.0, .lowCut = 7200.0, .pitch = 1.1 });
          } },
        { "House Floor Tom", [] { return houseTom (104.0); } },
        { "House Pedal Hat",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.065, .length = 0.24, .noise = 0.6, .centreHz = 8000.0, .lowCut = 6500.0, .pitch = 1.1 });
          } },
        { "House Mid Tom", [] { return houseTom (124.0); } },
        { "House Open Hat",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.22, .length = 0.75, .noise = 0.5, .centreHz = 8800.0, .lowCut = 7000.0, .pitch = 1.1 });
          } },
        { "House Mid Tom 2", [] { return houseTom (146.0); } },
        { "House High Tom", [] { return houseTom (172.0); } },
        { "House Crash",
          [] { return makeHat (rate, { .decaySeconds = 1.0, .length = 2.6, .noise = 0.55, .centreHz = 6000.0, .lowCut = 3800.0 }); } },
        { "House High Tom 2", [] { return houseTom (204.0); } },
        { "House Ride",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.65, .length = 1.9, .noise = 0.25, .centreHz = 9200.0, .lowCut = 6200.0, .pitch = 1.3 });
          } },

        // Lo-fi: softer drums as an old sampler plays them back, read at a low rate and few bits, dull and warm.
        { "Lo-Fi Kick",
          []
          {
              return dusty (makeKick (rate, 125.0, 52.0, 0.03, 0.28, 0.7, 0.15, 1.4),
                            { .heldRate = 22050.0, .bits = 12, .cutoffHz = 3500.0, .drive = 1.3 });
          } },
        { "Lo-Fi Rim",
          []
          {
              return dusty (makeKick (rate, 1450.0, 1300.0, 0.002, 0.02, 0.12, 0.4, 2.0),
                            { .heldRate = 16000.0, .bits = 10, .cutoffHz = 5000.0, .drive = 1.3 });
          } },
        { "Lo-Fi Snare",
          []
          {
              return dusty (makeSnare (rate, { .lowHz = 190.0,
                                               .highHz = 345.0,
                                               .lowSeconds = 0.07,
                                               .highSeconds = 0.045,
                                               .snaresSeconds = 0.17,
                                               .snaresLevel = 0.65,
                                               .snaresLowCut = 600.0,
                                               .snaresHighCut = 7000.0,
                                               .length = 0.5,
                                               .drive = 1.6 }),
                            { .heldRate = 22050.0, .bits = 11, .cutoffHz = 5500.0, .drive = 1.4 });
          } },
        { "Lo-Fi Clap",
          []
          {
              return dusty (
                  makeClap (rate, { .centreHz = 950.0, .q = 1.1, .lowCut = 400.0, .spread = 1.1, .tailSeconds = 0.14, .length = 0.55 }),
                  { .heldRate = 16000.0, .bits = 10, .cutoffHz = 4500.0, .drive = 1.4 });
          } },
        { "Lo-Fi Snare 2",
          []
          {
              return dusty (makeSnare (rate, { .lowHz = 225.0,
                                               .highHz = 400.0,
                                               .lowSeconds = 0.05,
                                               .highSeconds = 0.035,
                                               .snaresSeconds = 0.1,
                                               .snaresLevel = 0.7,
                                               .snaresLowCut = 900.0,
                                               .snaresHighCut = 6500.0,
                                               .length = 0.4,
                                               .drive = 1.7 }),
                            { .heldRate = 16000.0, .bits = 10, .cutoffHz = 5000.0, .drive = 1.4 });
          } },
        { "Lo-Fi Low Tom", [] { return lofiTom (85.0); } },
        { "Lo-Fi Closed Hat",
          []
          {
              return lofiHat ({ .decaySeconds = 0.04, .length = 0.2, .noise = 0.5, .centreHz = 6200.0, .lowCut = 4500.0, .pitch = 0.9 },
                              7000.0);
          } },
        { "Lo-Fi Floor Tom", [] { return lofiTom (100.0); } },
        { "Lo-Fi Pedal Hat",
          []
          {
              return lofiHat ({ .decaySeconds = 0.07, .length = 0.28, .noise = 0.5, .centreHz = 6200.0, .lowCut = 4500.0, .pitch = 0.9 },
                              7000.0);
          } },
        { "Lo-Fi Mid Tom", [] { return lofiTom (120.0); } },
        { "Lo-Fi Open Hat",
          []
          {
              return lofiHat ({ .decaySeconds = 0.3, .length = 0.9, .noise = 0.5, .centreHz = 6000.0, .lowCut = 4300.0, .pitch = 0.9 },
                              7000.0);
          } },
        { "Lo-Fi Mid Tom 2", [] { return lofiTom (140.0); } },
        { "Lo-Fi High Tom", [] { return lofiTom (165.0); } },
        { "Lo-Fi Crash",
          []
          {
              return lofiHat ({ .decaySeconds = 0.9, .length = 2.2, .noise = 0.5, .centreHz = 5200.0, .lowCut = 3200.0, .pitch = 0.9 },
                              6000.0);
          } },
        { "Lo-Fi High Tom 2", [] { return lofiTom (195.0); } },
        { "Lo-Fi Ride",
          []
          {
              return lofiHat ({ .decaySeconds = 0.7, .length = 1.8, .noise = 0.2, .centreHz = 6800.0, .lowCut = 4500.0, .pitch = 1.1 },
                              6500.0);
          } },

        // Punchy: hard attacks and short bodies, a kick whose click leads, snares driven hard.
        { "Punchy Kick", [] { return makeKick (rate, 320.0, 58.0, 0.008, 0.24, 0.6, 1.0, 2.4); } },
        { "Punchy Rim", [] { return makeKick (rate, 2000.0, 1600.0, 0.001, 0.01, 0.07, 1.0, 3.0); } },
        { "Punchy Snare",
          []
          {
              return makeSnare (rate, { .lowHz = 195.0,
                                        .highHz = 355.0,
                                        .lowSeconds = 0.055,
                                        .highSeconds = 0.035,
                                        .snaresSeconds = 0.1,
                                        .snaresLevel = 0.75,
                                        .snaresLowCut = 1100.0,
                                        .snaresHighCut = 11000.0,
                                        .length = 0.35,
                                        .drive = 2.8 });
          } },
        { "Punchy Clap",
          []
          {
              return makeClap (rate, { .centreHz = 1350.0, .q = 1.5, .lowCut = 700.0, .spread = 0.7, .tailSeconds = 0.08, .length = 0.35 });
          } },
        { "Punchy Snare 2",
          []
          {
              return makeSnare (rate, { .lowHz = 225.0,
                                        .highHz = 405.0,
                                        .lowSeconds = 0.045,
                                        .highSeconds = 0.03,
                                        .snaresSeconds = 0.08,
                                        .snaresLevel = 0.85,
                                        .snaresLowCut = 1500.0,
                                        .snaresHighCut = 12000.0,
                                        .length = 0.3,
                                        .drive = 3.2 });
          } },
        { "Punchy Low Tom", [] { return punchyTom (92.0); } },
        { "Punchy Closed Hat",
          []
          {
              return makeHat (
                  rate, { .decaySeconds = 0.025, .length = 0.12, .noise = 0.35, .centreHz = 8200.0, .lowCut = 7000.0, .pitch = 1.15 });
          } },
        { "Punchy Floor Tom", [] { return punchyTom (108.0); } },
        { "Punchy Pedal Hat",
          []
          {
              return makeHat (
                  rate, { .decaySeconds = 0.045, .length = 0.18, .noise = 0.35, .centreHz = 8200.0, .lowCut = 7000.0, .pitch = 1.15 });
          } },
        { "Punchy Mid Tom", [] { return punchyTom (128.0); } },
        { "Punchy Open Hat",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.2, .length = 0.65, .noise = 0.35, .centreHz = 8200.0, .lowCut = 6800.0, .pitch = 1.15 });
          } },
        { "Punchy Mid Tom 2", [] { return punchyTom (150.0); } },
        { "Punchy High Tom", [] { return punchyTom (176.0); } },
        { "Punchy Crash",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.75, .length = 2.0, .noise = 0.45, .centreHz = 6200.0, .lowCut = 3800.0, .pitch = 1.05 });
          } },
        { "Punchy High Tom 2", [] { return punchyTom (208.0); } },
        { "Punchy Ride",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.5, .length = 1.5, .noise = 0.2, .centreHz = 9000.0, .lowCut = 6200.0, .pitch = 1.25 });
          } },

        // The 909's (DrumMachines.h): its analog kick, snare, toms, rim shot and clap, and its sampled hats, crash and ride;
        // a snappier second snare, a shorter open hat for the pedal hat, and each tom tuned low and high, as its Tune knobs
        // tune them.
        { "909 Kick", [] { return drums::make909Kick (rate); } },
        { "909 Rim", [] { return drums::make909Rim (rate); } },
        { "909 Snare", [] { return drums::make909Snare (rate); } },
        { "909 Clap", [] { return drums::make909Clap (rate); } },
        { "909 Snare 2", [] { return drums::make909Snare (rate, 0.85, 0.95, 205.0); } },
        { "909 Low Tom", [] { return drums::make909Tom (rate, 82.0, 0.42); } },
        { "909 Closed Hat", [] { return drums::make909ClosedHat (rate); } },
        { "909 Low Tom 2", [] { return drums::make909Tom (rate, 96.0, 0.4); } },
        { "909 Pedal Hat", [] { return drums::make909OpenHat (rate, 0.12); } },
        { "909 Mid Tom", [] { return drums::make909Tom (rate, 120.0, 0.36); } },
        { "909 Open Hat", [] { return drums::make909OpenHat (rate, 0.6); } },
        { "909 Mid Tom 2", [] { return drums::make909Tom (rate, 138.0, 0.34); } },
        { "909 High Tom", [] { return drums::make909Tom (rate, 165.0, 0.3); } },
        { "909 Crash", [] { return drums::make909Crash (rate); } },
        { "909 High Tom 2", [] { return drums::make909Tom (rate, 190.0, 0.28); } },
        { "909 Ride", [] { return drums::make909Ride (rate); } },

        // Tight: everything short and dry, a small kick, a snap of a snare and hats that barely ring.
        { "Tight Kick", [] { return makeKick (rate, 190.0, 62.0, 0.012, 0.12, 0.35, 0.55, 2.0); } },
        { "Tight Rim", [] { return makeKick (rate, 1900.0, 1550.0, 0.001, 0.006, 0.05, 0.8, 2.6); } },
        { "Tight Snare",
          []
          {
              return makeSnare (rate, { .lowHz = 215.0,
                                        .highHz = 395.0,
                                        .lowSeconds = 0.035,
                                        .highSeconds = 0.025,
                                        .snaresSeconds = 0.055,
                                        .snaresLevel = 0.7,
                                        .snaresLowCut = 1300.0,
                                        .snaresHighCut = 11000.0,
                                        .length = 0.22,
                                        .drive = 2.2 });
          } },
        { "Tight Clap",
          []
          {
              return makeClap (rate,
                               { .centreHz = 1350.0, .q = 1.6, .lowCut = 700.0, .spread = 0.55, .tailSeconds = 0.045, .length = 0.2 });
          } },
        { "Tight Snare 2",
          []
          {
              return makeSnare (rate, { .lowHz = 255.0,
                                        .highHz = 465.0,
                                        .lowSeconds = 0.03,
                                        .highSeconds = 0.02,
                                        .snaresSeconds = 0.045,
                                        .snaresLevel = 0.75,
                                        .snaresLowCut = 1600.0,
                                        .snaresHighCut = 12000.0,
                                        .length = 0.18,
                                        .drive = 2.4 });
          } },
        { "Tight Low Tom", [] { return tightTom (95.0); } },
        { "Tight Closed Hat",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.016, .length = 0.08, .noise = 0.3, .centreHz = 8500.0, .lowCut = 7500.0, .pitch = 1.2 });
          } },
        { "Tight Floor Tom", [] { return tightTom (112.0); } },
        { "Tight Pedal Hat",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.03, .length = 0.12, .noise = 0.3, .centreHz = 8500.0, .lowCut = 7500.0, .pitch = 1.2 });
          } },
        { "Tight Mid Tom", [] { return tightTom (132.0); } },
        { "Tight Open Hat",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.12, .length = 0.4, .noise = 0.3, .centreHz = 8500.0, .lowCut = 7200.0, .pitch = 1.2 });
          } },
        { "Tight Mid Tom 2", [] { return tightTom (155.0); } },
        { "Tight High Tom", [] { return tightTom (182.0); } },
        { "Tight Crash",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.45, .length = 1.2, .noise = 0.4, .centreHz = 6500.0, .lowCut = 4200.0, .pitch = 1.1 });
          } },
        { "Tight High Tom 2", [] { return tightTom (215.0); } },
        { "Tight Ride",
          []
          {
              return makeHat (rate,
                              { .decaySeconds = 0.3, .length = 0.9, .noise = 0.2, .centreHz = 9500.0, .lowCut = 6800.0, .pitch = 1.3 });
          } },

        // Wide: in stereo, but for the kick and the toms, which stay where the pads' pans put them.
        { "Wide Kick", [] { return makeKick (rate, 210.0, 53.0, 0.017, 0.32, 0.85, 0.4, 2.0); } },
        { "Wide Rim",
          []
          {
              return drums::makeStereo (makeKick (rate, 1650.0, 1500.0, 0.002, 0.014, 0.09, 0.6, 2.4),
                                        makeKick (rate, 1720.0, 1560.0, 0.002, 0.014, 0.09, 0.6, 2.4));
          } },
        { "Wide Snare",
          []
          {
              return wideSnare ({ .lowHz = 200.0,
                                  .highHz = 365.0,
                                  .lowSeconds = 0.065,
                                  .highSeconds = 0.04,
                                  .snaresSeconds = 0.15,
                                  .snaresLevel = 0.7,
                                  .snaresLowCut = 1100.0,
                                  .snaresHighCut = 11500.0,
                                  .length = 0.5,
                                  .drive = 1.8 });
          } },
        { "Wide Clap",
          [] { return wideClap ({ .centreHz = 1150.0, .q = 1.2, .lowCut = 550.0, .spread = 1.0, .tailSeconds = 0.24, .length = 0.85 }); } },
        { "Wide Snare 2",
          []
          {
              return wideSnare ({ .lowHz = 240.0,
                                  .highHz = 440.0,
                                  .lowSeconds = 0.05,
                                  .highSeconds = 0.035,
                                  .snaresSeconds = 0.11,
                                  .snaresLevel = 0.8,
                                  .snaresLowCut = 1500.0,
                                  .snaresHighCut = 12000.0,
                                  .length = 0.4,
                                  .drive = 2.0,
                                  .seed = 5 });
          } },
        { "Wide Low Tom", [] { return wideTom (84.0); } },
        { "Wide Closed Hat",
          [] { return wideHat ({ .decaySeconds = 0.035, .length = 0.18, .noise = 0.4, .centreHz = 8600.0, .lowCut = 7000.0 }); } },
        { "Wide Floor Tom", [] { return wideTom (100.0); } },
        { "Wide Pedal Hat",
          [] { return wideHat ({ .decaySeconds = 0.06, .length = 0.25, .noise = 0.4, .centreHz = 8600.0, .lowCut = 7000.0 }); } },
        { "Wide Mid Tom", [] { return wideTom (120.0); } },
        { "Wide Open Hat",
          [] { return wideHat ({ .decaySeconds = 0.32, .length = 1.0, .noise = 0.4, .centreHz = 8600.0, .lowCut = 7000.0 }); } },
        { "Wide Mid Tom 2", [] { return wideTom (142.0); } },
        { "Wide High Tom", [] { return wideTom (168.0); } },
        { "Wide Crash",
          [] { return wideHat ({ .decaySeconds = 1.3, .length = 3.0, .noise = 0.5, .centreHz = 6000.0, .lowCut = 3600.0, .seed = 6 }); } },
        { "Wide High Tom 2", [] { return wideTom (198.0); } },
        { "Wide Ride",
          []
          {
              return wideHat (
                  { .decaySeconds = 0.8, .length = 2.2, .noise = 0.2, .centreHz = 9000.0, .lowCut = 6200.0, .pitch = 1.25, .seed = 7 });
          } },

        // Trap: a short kick over a long 808 (G1), a crisp snare and a layered clap on the backbeat, tight hats for rolls, a
        // snap, a rim, 808-style toms, a saturated cowbell as phonk plays it, and a shaker, tambourine and crash.
        { "Trap Kick", [] { return drums::makePunchKick (rate); } },
        { "Trap Rim", [] { return makeKick (rate, 1900.0, 1700.0, 0.001, 0.008, 0.06, 0.8, 3.0); } },
        { "Trap Snare", [] { return drums::makeCrispSnare (rate); } },
        { "Trap Clap", [] { return drums::makeLayeredClap (rate); } },
        { "Trap Snap", [] { return drums::makeSnap (rate); } },
        { "Trap 808", [] { return drums::makeLongBass808 (rate); } },
        { "Trap Closed Hat", [] { return drums::makeTightHat (rate, 0.05); } },
        { "Trap Low Tom", [] { return drums::make808Tom (rate, 100.0, 0.45); } },
        { "Trap Roll Hat", [] { return drums::makeTightHat (rate, 0.025); } },
        { "Trap High Tom", [] { return drums::make808Tom (rate, 150.0, 0.35); } },
        { "Trap Open Hat", [] { return drums::makeTightHat (rate, 0.4); } },
        { "Trap Cowbell", [] { return drums::make808Cowbell (rate, 2.5); } },
        { "Trap Shaker", [] { return drums::makeShaker (rate); } },
        { "Trap Crash", [] { return drums::make808Cymbal (rate, 1.2, 0.75); } },
        { "Trap Perc", [] { return drums::makeWoodblock (rate, 1150.0); } },
        { "Trap Tambourine", [] { return drums::makeTambourine (rate); } },

        // A rhythm box, as the CR-78: its fourteen voices and its open hat, a second snare, General MIDI's drums where it has
        // them and its own percussion between them (DrumMachines.h).
        { "Rhythm Box Kick", [] { return drums::makeBoxKick (rate); } },
        { "Rhythm Box Rim", [] { return drums::makeBoxRim (rate); } },
        { "Rhythm Box Snare", [] { return drums::makeBoxSnare (rate); } },
        { "Rhythm Box Claves", [] { return drums::makeBoxClaves (rate); } },
        { "Rhythm Box Guiro", [] { return drums::makeBoxGuiro (rate); } },
        { "Rhythm Box Conga", [] { return drums::makeBoxDrum (rate, 175.0, 0.25); } },
        { "Rhythm Box Closed Hat", [] { return drums::makeBoxHat (rate, 0.06); } },
        { "Rhythm Box Low Bongo", [] { return drums::makeBoxDrum (rate, 290.0, 0.14); } },
        { "Rhythm Box Metal", [] { return drums::makeBoxMetal (rate); } },
        { "Rhythm Box High Bongo", [] { return drums::makeBoxDrum (rate, 410.0, 0.1); } },
        { "Rhythm Box Open Hat", [] { return drums::makeBoxHat (rate, 0.45); } },
        { "Rhythm Box Tambourine", [] { return drums::makeBoxTambourine (rate); } },
        { "Rhythm Box Maracas", [] { return drums::makeBoxMaracas (rate); } },
        { "Rhythm Box Cymbal", [] { return drums::makeBoxCymbal (rate); } },
        { "Rhythm Box Cowbell", [] { return drums::makeBoxCowbell (rate); } },
        { "Rhythm Box Snare 2",
          []
          {
              return makeSnare (rate, { .lowHz = 215.0,
                                        .highHz = 390.0,
                                        .lowSeconds = 0.03,
                                        .highSeconds = 0.02,
                                        .snaresSeconds = 0.06,
                                        .snaresLevel = 0.9,
                                        .snaresLowCut = 2500.0,
                                        .snaresHighCut = 8000.0,
                                        .length = 0.25,
                                        .drive = 1.4,
                                        .seed = 78 });
          } },

        // Hand percussion (DrumMachines.h): congas, open and slapped, bongos, timbales, agogo bells, woodblocks and claves,
        // and what is shaken and struck: shaker, cabasa, tambourine and triangle.
        { "Percussion Conga", [] { return drums::makeHandDrum (rate, 185.0, 0.32, false); } },
        { "Percussion Slap", [] { return drums::makeHandDrum (rate, 245.0, 0.12, true); } },
        { "Percussion High Conga", [] { return drums::makeHandDrum (rate, 280.0, 0.26, false); } },
        { "Percussion Low Bongo", [] { return drums::makeHandDrum (rate, 340.0, 0.16, false); } },
        { "Percussion High Bongo", [] { return drums::makeHandDrum (rate, 470.0, 0.12, false); } },
        { "Percussion Low Timbale", [] { return drums::makeTimbale (rate, 330.0); } },
        { "Percussion Shaker", [] { return drums::makeShaker (rate); } },
        { "Percussion High Timbale", [] { return drums::makeTimbale (rate, 440.0); } },
        { "Percussion Cabasa", [] { return drums::makeCabasa (rate); } },
        { "Percussion Low Agogo", [] { return drums::makeAgogo (rate, 820.0); } },
        { "Percussion Tambourine", [] { return drums::makeTambourine (rate); } },
        { "Percussion High Agogo", [] { return drums::makeAgogo (rate, 1100.0); } },
        { "Percussion Claves", [] { return drums::makeBoxClaves (rate, 2500.0, 0.08); } },
        { "Percussion Low Woodblock", [] { return drums::makeWoodblock (rate, 700.0); } },
        { "Percussion Triangle", [] { return drums::makeTriangle (rate, false); } },
        { "Percussion High Woodblock", [] { return drums::makeWoodblock (rate, 1050.0); } },
    };

    constexpr auto numDrums = std::size (entries);

    constexpr auto names = []
    {
        std::array<std::string_view, numDrums> made {};

        for (size_t i = 0; i < numDrums; ++i)
            made[i] = entries[i].name;

        return made;
    }();

    // A kit's sound fades out over its last part, so that it stops without a click however loud it still is.
    void fadeOut (Audio& sound)
    {
        const auto length = sound.getNumSamples();
        const auto fade = std::max (1, (int) (fadeShare * length));

        for (int i = 0; i < fade; ++i)
        {
            const auto gain = (float) (0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * (i + 1) / fade)));

            for (int channel = 0; channel < sound.getNumChannels(); ++channel)
                sound.setSample (channel, length - fade + i, sound.getSample (channel, length - fade + i) * gain);
        }
    }

    std::shared_ptr<const DrumSampler::Sound> make (size_t index)
    {
        auto sound = std::make_shared<DrumSampler::Sound>();
        sound->audio = entries[index].make();
        sound->sampleRate = rate;

        // The default kit's as songs have played them since it was made; the kits' end in a fade.
        if (index >= (size_t) DrumSampler::numPads)
            fadeOut (sound->audio);

        if (const auto peak = sound->audio.getMagnitude (0, sound->audio.getNumSamples()); peak > 0.0f)
            sound->audio.applyGain (dsp::decibelsToGain (-6.0f) / peak);

        return sound;
    }
} // namespace

std::span<const std::string_view> getFactoryDrumNames()
{
    return names;
}

int findFactoryDrum (std::string_view name)
{
    const auto found = std::find (names.begin(), names.end(), name);
    return found != names.end() ? (int) (found - names.begin()) : -1;
}

std::shared_ptr<const DrumSampler::Sound> getFactoryDrum (int index)
{
    const auto at = juce::isPositiveAndBelow (index, (int) numDrums) ? (size_t) index : 0;

    // Each one made once, while something holds it; the default kit's for good, as every new sampler plays it.
    static std::mutex lock;
    static std::array<std::weak_ptr<const DrumSampler::Sound>, numDrums> made;
    static std::array<std::shared_ptr<const DrumSampler::Sound>, DrumSampler::numPads> defaultKit;
    const std::scoped_lock scope (lock);

    if (auto sound = made[at].lock())
        return sound;

    auto sound = make (at);
    made[at] = sound;

    if (at < defaultKit.size())
        defaultKit[at] = sound;

    return sound;
}
} // namespace phazite::devices
