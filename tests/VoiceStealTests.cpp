// SPDX-License-Identifier: AGPL-3.0-or-later
// What Phazite's synths do when a note needs a voice and every voice sounds (src/devices/synth/, wavetable/, fm/, sampler/):
// each takes a voice and fades out what it played over a few milliseconds, however many it takes at once, as a chord does.
// Each played as the engine plays it, on its runner (DspProbe.h).
#include "DspProbe.h"
#include "TestHelpers.h"
#include "devices/fm/FmSynth.h"
#include "devices/sampler/Sampler.h"
#include "devices/synth/Synth.h"
#include "model/devices/internal/Devices.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
namespace devices = phazite::devices;
namespace probe = phazite::test::probe;

// The sound some notes make on a new device of a type, at its defaults: the sampler's 808 looped, so that it holds on as the
// synths' notes do rather than dying away.
probe::Sound playNotes (const devices::DeviceInfo& info, double rate, int numSamples, const std::vector<devices::NoteEvent>& notes)
{
    probe::Device device (info);

    if (info.typeID == std::string_view (devices::Sampler::typeID))
        device.set ("loopOn", 1.0f);

    device.prepare (rate, 512);
    return device.played (probe::makeSilence (numSamples), 512, notes);
}
} // namespace

TEST_CASE ("Each synth fades out every voice that a chord takes on one sample, rather than all but one cut dead")
{
    // Sixteen notes held, every voice of each synth, then a chord of eight on one sample, which takes the eight oldest. What
    // the chord took away is what the sixteen and the chord play apart, less what they play together; and the eight oldest
    // alone are what it took. Each faded out over the 3 ms of a voice taken, what was taken away is what they played, faded
    // in by a ramp from nothing over those 3 ms: measured as how far it is from that, its power against theirs. Cut dead,
    // all of a taken voice is taken away at once.
    constexpr double rate = 44100.0;
    constexpr int chordAt = 4410, length = chordAt + 441;
    const auto fadeStep = 1.0 / (0.003 * rate);

    std::vector<devices::NoteEvent> held, oldest, chord;

    for (int i = 0; i < 16; ++i)
        held.push_back (probe::noteOn (i, 40 + i, 0.8f));

    oldest.assign (held.begin(), held.begin() + 8);

    for (int i = 0; i < 8; ++i)
        chord.push_back (probe::noteOn (chordAt, 60 + i, 0.8f));

    auto together = held;
    together.insert (together.end(), chord.begin(), chord.end());

    for (const auto* typeID : { devices::Synth::typeID, devices::FmSynth::typeID, devices::Sampler::typeID })
    {
        CAPTURE (std::string (typeID));
        const auto* info = phazite::findOwnDevice (typeID);
        REQUIRE (info != nullptr);

        const auto all = playNotes (*info, rate, length, together);
        const auto heldAlone = playNotes (*info, rate, length, held);
        const auto chordAlone = playNotes (*info, rate, length, chord);
        const auto taken = playNotes (*info, rate, length, oldest);

        auto before = 0.0, away = 0.0, notes = 0.0;

        for (int channel = 0; channel < 2; ++channel)
        {
            for (int i = 0; i < length; ++i)
            {
                const auto gone =
                    (double) heldAlone.getSample (channel, i) + chordAlone.getSample (channel, i) - all.getSample (channel, i);

                if (i < chordAt)
                {
                    before = std::max (before, std::abs (gone));
                    continue;
                }

                const auto faded = (double) taken.getSample (channel, i) * std::min (1.0, (i - chordAt) * fadeStep);
                away += (gone - faded) * (gone - faded);
                notes += (double) taken.getSample (channel, i) * taken.getSample (channel, i);
            }
        }

        const auto unlikeFade = away / notes;
        CAPTURE (unlikeFade);
        CHECK (before < 1.0e-5); // the same, sample for sample, before the chord
        REQUIRE (notes > 1.0e-3);
        CHECK (unlikeFade < 0.001);
    }
}
