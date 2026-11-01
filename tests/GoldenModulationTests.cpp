// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden renders of modulation (tests/golden/renders, RenderHarness.h; docs/tracktion-migration.md, T0): each modulator, a
// track's macro and a container's, moving a parameter of a song made through the model (TestSong.h). Each song is rendered
// first without its modulation, and its golden render's check asks that the modulation change the sound as it should.
// Tracktion's random source has no golden render: it isn't seeded, so it moves differently each time.
#include "RenderHarness.h"
#include "TestSong.h"
#include "devices/filter/FilterEffect.h"
#include "devices/saturation/Saturator.h"
#include "devices/synth/Synth.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"

#include <algorithm>
#include <cmath>

using phazite::ModulatorType;
using phazite::test::Fingerprint;
using phazite::test::TestSong;

namespace
{
using phazite::test::changesLevel;
using phazite::test::isLouder;
using phazite::test::swings;

phazite::test::GoldenRender golden (const char* name, const char* about)
{
    return { name,
             juce::String ("Golden render (tests/GoldenModulationTests.cpp): ") + about
                 + " Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).",
             4.0,
             {} };
}

// Renders a song as it is, then with a change, as a golden render (RenderHarness.h: checkGoldenChange()).
void checkChange (TestSong& song, const std::function<void()>& change, const char* name, const char* about,
                  const phazite::test::ChangeCheck& check)
{
    phazite::test::checkGoldenChange (song.context, change, golden (name, about), check);
}

// A tone on the first track, from the song's start for 4 s at 120 BPM.
void addTone (TestSong& song, double frequency = 440.0)
{
    song.setTempo (120.0);
    song.addAudioClip (song.firstTrack, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 4.0, frequency, 0.25f),
                       0.0);
}

// A high chord on Phazite's synth, as it is made, on the first track for 4 s at 120 BPM, through Phazite's filter, closed
// down to 200 Hz. Returns the filter.
phazite::DeviceID addChordThroughClosedFilter (TestSong& song)
{
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.setInstrument (track, phazite::devices::Synth::typeID);
    const auto filter = song.addEffect (track, phazite::devices::FilterEffect::typeID);
    song.setParameter (filter, "cutoff", 200.0f);
    const auto clip = song.addMidiClip (track, 0.0, 8.0);
    song.addChord (clip, { 72, 76, 79 }, 0.0, 8.0);
    return filter;
}
} // namespace

TEST_SUITE ("golden")
{
    TEST_CASE ("Golden render: an LFO swings a tone's volume")
    {
        TestSong song;
        addTone (song);
        checkChange (
            song,
            [&song]
            {
                const auto lfo = song.addModulator (song.firstTrack, ModulatorType::lfo);
                song.modulate (lfo, song.getVolumeParameter (song.firstTrack), 0.5f);
            },
            "modulation-lfo", "an LFO, as it is made, swinging a tone's volume by half its range.",
            [] (const Fingerprint&, const Fingerprint& after) { return swings (after, 2, 38, 10.0f, "the LFO swings the volume"); });
    }

    TEST_CASE ("Golden render: an envelope follower pans a tone as loud as its track is")
    {
        TestSong song;
        addTone (song);
        checkChange (
            song,
            [&song]
            {
                const auto follower = song.addModulator (song.firstTrack, ModulatorType::envelopeFollower);
                song.modulate (follower, song.getPanParameter (song.firstTrack), -0.8f);
            },
            "modulation-envelope-follower", "an envelope follower panning a tone left as loud as the tone is.",
            [] (const Fingerprint& before, const Fingerprint& after)
            {
                const auto middle = std::abs (before[20].rms[0] - before[20].rms[1]) < 0.1f;
                const auto left = after[20].rms[0] > after[20].rms[1] + 3.0f;
                CHECK_MESSAGE (middle, "before, the tone is in the middle");
                CHECK_MESSAGE (
                    left, ("2.0-2.1 s: the follower should pan the tone left; the RMS is " + phazite::test::describeLevels (after[20].rms))
                              .toStdString());
                return middle && left;
            });
    }

    TEST_CASE ("Golden render: a step sequencer steps a tone's volume")
    {
        TestSong song;
        addTone (song);
        checkChange (
            song,
            [&song]
            {
                const auto steps = song.addModulator (song.firstTrack, ModulatorType::steps);
                song.modulate (steps, song.getVolumeParameter (song.firstTrack), 0.5f);
            },
            "modulation-steps", "a step sequencer, its steps rising, stepping a tone's volume by half its range.",
            [] (const Fingerprint&, const Fingerprint& after) { return swings (after, 2, 38, 6.0f, "the steps move the volume"); });
    }

    TEST_CASE ("Golden render: a breakpoint envelope moves a tone's volume")
    {
        TestSong song;
        addTone (song);
        checkChange (
            song,
            [&song]
            {
                const auto envelope = song.addModulator (song.firstTrack, ModulatorType::breakpoints);
                song.modulate (envelope, song.getVolumeParameter (song.firstTrack), 0.5f);
            },
            "modulation-breakpoints", "a breakpoint envelope, as it is made, moving a tone's volume by half its range.",
            [] (const Fingerprint& before, const Fingerprint& after)
            {
                return phazite::test::differSomewhere (before, after, 3.0f) && swings (after, 2, 38, 3.0f, "the envelope moves the volume");
            });
    }

    TEST_CASE ("Golden render: a note follower moves a filter with the notes played")
    {
        TestSong song;
        song.setTempo (120.0);
        const auto track = song.firstTrack;
        song.setInstrument (track, phazite::devices::Synth::typeID);
        const auto filter = song.addEffect (track, phazite::devices::FilterEffect::typeID);
        song.setParameter (filter, "cutoff", 400.0f);
        const auto clip = song.addMidiClip (track, 0.0, 8.0);

        for (int note = 0; note < 4; ++note)
            song.addNote (clip, 48 + 12 * note, note * 2.0, 1.5);

        checkChange (
            song,
            [&song, filter]
            {
                const auto follower = song.addModulator (song.firstTrack, ModulatorType::noteFollower);
                song.modulate (follower, { filter, "cutoff" }, 0.8f);
            },
            "modulation-note-follower", "a note follower moving a filter's cutoff with four notes an octave apart on Phazite's synth.",
            [] (const Fingerprint& before, const Fingerprint& after)
            {
                return phazite::test::differSomewhere (before, after, 3.0f)
                       && isLouder (after, 35, 5, 3.0f, "the higher notes open the filter more");
            });
    }

    TEST_CASE ("Golden render: a track's macro turns a tone down")
    {
        TestSong song;
        addTone (song);
        checkChange (
            song,
            [&song]
            {
                const auto macro = song.addModulator (song.firstTrack, ModulatorType::macro);
                song.modulate (macro, song.getVolumeParameter (song.firstTrack), -0.5f);
                song.setMacroValue (macro, 1.0f);
            },
            "modulation-macro", "a track's macro, turned all the way up, turning a tone's volume down by half its range.",
            [] (const Fingerprint& before, const Fingerprint& after)
            { return changesLevel (before, after, 20, -10.0f, "the macro turns the tone down"); });
    }

    TEST_CASE ("Golden render: a container's macro opens a filter in it")
    {
        TestSong song;
        const auto filter = addChordThroughClosedFilter (song);
        const auto saturator = song.addEffect (song.firstTrack, phazite::devices::Saturator::typeID);
        checkChange (
            song,
            [&song, filter, saturator]
            {
                const auto container = phazite::groupIntoContainer (song.context, { filter, saturator });
                REQUIRE (container.isValid());
                const auto macros = phazite::getContainerMacros (song.context, container);
                REQUIRE (! macros.empty());
                song.modulate (macros.front().id, { filter, "cutoff" }, 0.8f);
                song.setMacroValue (macros.front().id, 1.0f);
            },
            "modulation-container-macro",
            "a container of a filter, closed at 200 Hz, and a saturator on a high chord, its first macro opening the filter.",
            [] (const Fingerprint& before, const Fingerprint& after)
            { return changesLevel (before, after, 20, 10.0f, "the macro opens the filter on the chord"); });
    }
}

TEST_CASE ("A random source moves what it modulates, differently each time, so it has no golden render")
{
    TestSong song;
    addTone (song);
    const auto random = song.addModulator (song.firstTrack, ModulatorType::random);
    song.modulate (random, song.getVolumeParameter (song.firstTrack), 0.5f);
    phazite::test::settle();

    // Over twelve seconds: over four, it stayed within 3 dB about one time in twelve (2026-11-01, CI run 41355268575).
    CHECK (swings (phazite::test::renderFingerprint (song.context, 12.0), 2, 118, 3.0f, "the random source moves the volume"));
}
