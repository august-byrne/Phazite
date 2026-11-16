// SPDX-License-Identifier: AGPL-3.0-or-later
// The synths' control steps on the song's own grid (src/devices/dsp/ControlGrid.h): a passage plays the same however it was
// reached, from the song's start in a render, after a count-in, as a loop's second pass, after the device slept on silence,
// and in blocks of any size with notes at any sample of a step. Each synth played as the engine plays it, on its runner
// (DspProbe.h), at its defaults, its filter envelope or its glide moving at each step.
#include "DspProbe.h"
#include "TestHelpers.h"
#include "devices/dsp/ControlGrid.h"
#include "devices/fm/FmSynth.h"
#include "devices/sampler/Sampler.h"
#include "devices/synth/Synth.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/devices/internal/Devices.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace
{
namespace devices = phazite::devices;
namespace probe = phazite::test::probe;

constexpr double rate = 44100.0;
const char* const synths[] = { devices::Synth::typeID, devices::WavetableSynth::typeID, devices::FmSynth::typeID,
                               devices::Sampler::typeID };

// Notes that end well before the stretch does, their tails gone by its end: a chord, a note gliding in, a note on its own.
std::vector<devices::NoteEvent> getNotes()
{
    return { probe::noteOn (3, 48, 0.8f),    probe::noteOn (3, 55, 0.7f),
             probe::noteOn (1003, 60, 0.9f), { devices::NoteEvent::Type::portamentoOn, 4007, 67, 0.8f },
             probe::noteOff (9001, 48),      probe::noteOff (9001, 55),
             probe::noteOff (9005, 60),      probe::noteOff (9013, 67) };
}

// A device whose values move at each step: its pitch gliding, or, where the test needs a glide that remembers nothing of
// the notes before (a loop's second pass glides from the first's last note), its filter's envelope sweeping its cutoff.
std::unique_ptr<probe::Device> makeDevice (const char* typeID, int blockSize, bool glides = true)
{
    const auto* info = phazite::findOwnDevice (typeID);
    REQUIRE (info != nullptr);
    auto device = std::make_unique<probe::Device> (*info);

    if (glides && device->has ("glide"))
        device->set ("glide", 80.0f);

    if (! glides)
    {
        for (const auto* id : { "filterOn", "filterEnvelopeOn" })
            if (device->has (id))
                device->set (id, 1.0f);

        if (device->has ("filterEnvelope"))
            device->set ("filterEnvelope", 24.0f);
    }

    device->prepare (rate, blockSize);
    return device;
}

double differenceInDecibels (const probe::Sound& a, const probe::Sound& b, int from, int numSamples)
{
    const auto peak = probe::getPeak (b, from, numSamples);
    REQUIRE (peak > 1.0e-3);
    return probe::inDecibels (probe::getLargestDifference (a, b, from, numSamples) / peak);
}
} // namespace

TEST_CASE ("The control grid counts from the song's place while it plays, and on its own while the song is stopped")
{
    // The reviewer's finding 4 (2026-11-15). The stretches a synth plays (ControlGrid::next()), block by block.
    devices::dsp::ControlGrid grid (16);
    grid.startBlock (true, 0);
    auto stretch = grid.next (40);
    CHECK ((stretch.newStep && stretch.at == 0 && stretch.length == 16));
    stretch = grid.next (5);
    CHECK ((stretch.newStep && stretch.length == 5 && stretch.untilNextStep == 16));
    grid.startBlock (true, 21);
    stretch = grid.next (100);
    CHECK ((! stretch.newStep && stretch.at == 5 && stretch.length == 11 && stretch.untilNextStep == 11));

    // A jump to the middle of a step: a step begins there, to the grid's next line.
    grid.startBlock (true, 992 + 3);
    stretch = grid.next (100);
    CHECK ((stretch.newStep && stretch.at == 0 && stretch.length == 13 && stretch.untilNextStep == 13));

    // Stopped: on from where it was, whatever the song's place says.
    grid.startBlock (false, 5);
    stretch = grid.next (4);
    CHECK ((stretch.newStep && stretch.length == 4));
    grid.startBlock (false, 5);
    stretch = grid.next (100);
    CHECK ((! stretch.newStep && stretch.at == 4 && stretch.length == 12));
}

TEST_CASE ("Each synth plays a passage after a count-in as a render from the song's start plays it")
{
    // The reviewer's finding 4 (2026-11-15).
    // A bar of count-in at 120 beats a minute is 88200 samples at 44.1 kHz, 8 past a multiple of 16: the device starts over as
    // the count-in starts (the engine's flush), plays the count-in with the song stopped, then the song from its start.
    constexpr int length = 16384, countIn = 88200;
    const auto notes = getNotes();

    for (const auto* typeID : synths)
    {
        CAPTURE (std::string (typeID));
        auto rendered = makeDevice (typeID, 512);
        const auto straight = rendered->played (probe::makeSilence (length), 512, notes);

        auto live = makeDevice (typeID, 512);
        auto counting = probe::makeSilence (countIn);
        live->playStopped (counting, 512);
        const auto afterCountIn = live->played (probe::makeSilence (length), 512, notes);
        const auto apart = differenceInDecibels (afterCountIn, straight, 0, length);
        CAPTURE (apart);
        CHECK (apart < -120.0);
    }
}

TEST_CASE ("Each synth plays a loop's every pass alike, the loop no whole number of control steps long")
{
    // The reviewer's finding 4 (2026-11-15).
    // The loop is 60009 samples, 9 past a multiple of 16; what a pass played has ended by its end. The second and the third
    // passes against each other: each starts with what the one before left (a voice's filter gliding from where it was), which
    // the first, from silence, doesn't, so that only where the steps fall could set them apart.
    constexpr int loopLength = 60009;
    auto notes = getNotes();
    std::erase_if (notes, [] (const auto& note) { return note.type == devices::NoteEvent::Type::portamentoOn; });

    for (const auto* typeID : synths)
    {
        CAPTURE (std::string (typeID));
        auto device = makeDevice (typeID, 441, false);
        const auto first = device->played (probe::makeSilence (loopLength), 441, notes);
        REQUIRE (probe::getPeak (first, loopLength - 1000, 1000) < 1.0e-4); // its tails have gone
        device->moveTo (0);
        const auto second = device->played (probe::makeSilence (loopLength), 441, notes);
        device->moveTo (0);
        const auto third = device->played (probe::makeSilence (loopLength), 441, notes);
        const auto apart = differenceInDecibels (third, second, 0, loopLength);
        CAPTURE (apart);
        CHECK (apart < -100.0);
    }
}

TEST_CASE ("Each synth woken from sleeping on silence plays what a render, which never sleeps, plays")
{
    // The reviewer's finding 4 (2026-11-15).
    // Notes, five seconds of silence that its runner lets it sleep through (live: not a render), longer than any of their
    // tails, then the notes again. What its DC blocker still held as it fell asleep is all that may differ.
    constexpr int gap = 5 * 44100 + 7, length = 13001;
    auto notes = getNotes();

    for (const auto& note : getNotes())
    {
        auto again = note;
        again.sample += length + gap;
        notes.push_back (again);
    }

    for (const auto* typeID : synths)
    {
        CAPTURE (std::string (typeID));
        const auto play = [&] (bool rendering)
        {
            auto device = makeDevice (typeID, 512);
            device->isRendering = rendering;
            return device->played (probe::makeSilence (2 * length + gap), 512, notes);
        };

        const auto rendered = play (true), live = play (false);
        const auto apart = differenceInDecibels (live, rendered, length + gap, length);
        CAPTURE (apart);
        CHECK (apart < -80.0);
    }
}

TEST_CASE ("Each synth plays the same in blocks of one sample, and with a note at every sample of a control step")
{
    // The reviewer's finding 8 (2026-11-15).
    // Sixteen notes, one at each of a step's samples, against the same in blocks of 512.
    std::vector<devices::NoteEvent> notes;

    for (int i = 0; i < 16; ++i)
        notes.push_back (probe::noteOn (160 + 17 * i, 50 + i, 0.8f)); // 17 apart: each at the next sample of its step

    for (const auto* typeID : synths)
    {
        CAPTURE (std::string (typeID));
        auto whole = makeDevice (typeID, 512), single = makeDevice (typeID, 512);
        constexpr int length = 2048;
        const auto inWhole = whole->played (probe::makeSilence (length), 512, notes);
        const auto inSingles = single->played (probe::makeSilence (length), 1, notes);
        const auto apart = differenceInDecibels (inSingles, inWhole, 0, length);
        CAPTURE (apart);
        CHECK (apart < -120.0);
    }
}
