// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden renders (tests/golden/renders): songs made through the model (TestSong.h), each showing what plays in a song, are
// rendered offline through the reference engine and the candidate (RenderHarness.h; docs/tracktion-migration.md, T0), and
// the levels of each 100 ms block are compared with a committed fingerprint, so that a change to how Phazite plays a song
// fails a test. Each render is checked first, so that a broken one never becomes the golden file. GoldenFiles.h says how to
// regenerate them.
#include "RenderHarness.h"
#include "TestSong.h"
#include "devices/drums/DrumSampler.h"
#include "devices/filter/FilterEffect.h"
#include "devices/reverb/Reverb.h"
#include "devices/synth/Synth.h"
#include "devices/wavetable/FactoryWavetables.h"
#include "devices/wavetable/WavetableSynth.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/mixer/MixerModel.h"

#include <algorithm>
#include <cmath>

using phazite::test::Fingerprint;
using phazite::test::TestSong;

namespace
{
using phazite::test::describeLevels;
using phazite::test::floorDb;
using phazite::test::formatLevel;
using phazite::test::getPower;
using phazite::test::isBetween;
using phazite::test::isLouder;

phazite::BeatPosition beat (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

phazite::test::GoldenRender golden (const char* name, const char* about, double seconds,
                                    std::function<bool (const Fingerprint&, const juce::AudioBuffer<float>&)> check)
{
    return { name,
             juce::String ("Golden render (tests/GoldenRenderTests.cpp): ") + about
                 + " Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).",
             seconds, std::move (check) };
}

//==============================================================================
// The mix, at 120 BPM (a beat is 0.5 s): Low, a 220 Hz tone from 0 to 2 s whose volume falls from 0 to -12 dB, and
// High, a quieter 660 Hz tone from 1 to 2.5 s, panned halfway left, with a send after its fader to a reverb return; both
// in a bus at -3 dB. (No digits in the files' names: Phazite would read a tempo in them and stretch the clips.)
void addMix (TestSong& song)
{
    song.setTempo (120.0);
    const auto low = song.firstTrack;
    song.rename (low, "Low");
    const auto high = song.addTrack ("High");

    const auto sounds = song.getSoundsFolder();
    song.addAudioClip (low, phazite::test::writeSound (sounds.getChildFile ("Low Tone.wav"), 2.0, 220.0, 0.5f), 0.0);
    song.addAudioClip (high, phazite::test::writeSound (sounds.getChildFile ("High Tone.wav"), 1.5, 660.0, 0.25f), 2.0);

    song.addAutomation (song.getVolumeParameter (low), { { beat (0.0), phazite::decibelsToFaderPosition (0.0f) },
                                                         { beat (4.0), phazite::decibelsToFaderPosition (-12.0f) } });
    song.setPan (high, -0.5f);

    const auto bus = song.addBus ("Bus", { low, high });
    song.setVolume (bus, -3.0f);
    song.addReturn (high, phazite::devices::Reverb::typeID);
}

// Low alone in the middle, High panned left, the reverb's tail after both, and the tail fading.
bool soundsLikeTheMix (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    REQUIRE (fingerprint.size() == 40);

    // 0.5-0.6 s: Low alone, in the middle.
    const auto& lowAlone = fingerprint[5].rms;
    const auto lowInTheMiddle = lowAlone[0] > -30.0f && lowAlone[1] > -30.0f && std::abs (lowAlone[0] - lowAlone[1]) < 0.01f;
    CHECK_MESSAGE (lowInTheMiddle,
                   ("0.50-0.60 s: Low alone should be above -30 dBFS and as loud on each side; its RMS is " + describeLevels (lowAlone))
                       .toStdString());

    // 1.5-1.6 s: High too, panned left.
    const auto& withHigh = fingerprint[15].rms;
    const auto highOnTheLeft = withHigh[0] > withHigh[1] + 0.5f;
    CHECK_MESSAGE (
        highOnTheLeft,
        ("1.50-1.60 s: with High, panned left, the left side should be louder; the RMS is " + describeLevels (withHigh)).toStdString());

    // 2.6-2.7 s: both tones have ended, and the reverb's tail is left.
    const auto& tail = fingerprint[26].rms;
    const auto hasTail = std::max (tail[0], tail[1]) > floorDb + 10.0f;
    CHECK_MESSAGE (hasTail, ("2.60-2.70 s: the reverb's tail should be above " + formatLevel (floorDb + 10.0f) + " dBFS; its RMS is "
                             + describeLevels (tail))
                                .toStdString());

    // 3.9-4.0 s: the tail has faded, on each side.
    const auto& faded = fingerprint[39].rms;
    auto tailFades = true;

    for (size_t channel = 0; channel < faded.size(); ++channel)
        tailFades = tailFades && std::max (faded[channel], floorDb) < std::max (tail[channel], floorDb) - 6.0f;

    CHECK_MESSAGE (
        tailFades,
        ("3.90-4.00 s: the reverb's tail should have faded by 6 dB since 2.60-2.70 s; its RMS is " + describeLevels (faded)).toStdString());

    return lowInTheMiddle && highOnTheLeft && hasTail && tailFades;
}

//==============================================================================
constexpr int firstBeatSeconds = 8; // its 4 bars at 120 BPM

// Something in every block of its 8 seconds, the same on each side (the drums and the synth are in the middle), and the
// kick loud on the first beat.
bool soundsLikeFirstBeat (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    REQUIRE (fingerprint.size() == (size_t) firstBeatSeconds * 10);
    auto everywhere = true, inTheMiddle = true;

    for (const auto& block : fingerprint)
    {
        everywhere = everywhere && std::min (block.rms[0], block.rms[1]) > -40.0f;
        inTheMiddle = inTheMiddle && std::abs (block.rms[0] - block.rms[1]) < 0.01f;
    }

    CHECK_MESSAGE (everywhere, "every 100 ms of the First Beat example should be above -40 dBFS on each side");
    CHECK_MESSAGE (inTheMiddle, "First Beat's drums and synth should be as loud on each side");

    const auto kick = fingerprint.front().peak[0] > -18.0f;
    CHECK_MESSAGE (kick,
                   ("0.00-0.10 s: the kick should peak above -18 dBFS; it peaks at " + formatLevel (fingerprint.front().peak[0]) + " dBFS")
                       .toStdString());

    return everywhere && inTheMiddle && kick;
}

//==============================================================================
// The wavetable song, at 120 BPM: Phazite's wavetable synth with its first oscillator on Saw Build, low in the table, and
// its second on Vowels an octave down, each in unison four times over and spread across the stereo field, its third
// envelope lifting the first oscillator up its table at each note's start, through its filter: a chord from 0 to 1.5 s,
// another from 1.5 to 3 s, then the release.
void addWavetableSong (TestSong& song)
{
    using phazite::devices::WavetableSynth;
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    const auto synth = song.setInstrument (track, WavetableSynth::typeID);

    const auto device = phazite::getDeviceHandle (song.context, synth);
    device.setFactoryWavetable (0, phazite::devices::findFactoryWavetable ("Saw Build"));
    device.setFactoryWavetable (1, phazite::devices::findFactoryWavetable ("Vowels"));

    song.setParameter (synth, "osc1Position", 0.2f);
    song.setParameter (synth, "osc2On", 1.0f);
    song.setParameter (synth, "osc2Pitch", -12.0f);
    song.setParameter (synth, "osc2Position", 0.5f);
    song.setParameter (synth, "osc2Level", 0.5f);
    song.setParameter (synth, "unisonVoices", 3.0f); // four
    song.setParameter (synth, "unisonDetune", 15.0f);
    song.setParameter (synth, "mod2Source", (float) WavetableSynth::ModSource::envelope3);
    song.setParameter (synth, "mod2Target", (float) WavetableSynth::ModTarget::osc1Position);
    song.setParameter (synth, "mod2Amount", 0.6f);

    const auto clip = song.addMidiClip (track, 0.0, 6.0);
    song.addChord (clip, { 48, 51, 55, 58 }, 0.0, 3.0);
    song.addChord (clip, { 44, 48, 51, 55 }, 3.0, 3.0);
}

// Something on each side while the chords play, wider than the middle somewhere (its unison spreads it), and quiet once
// they have rung out.
bool soundsLikeTheWavetable (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    REQUIRE (fingerprint.size() == 40);
    auto playing = true, wide = false;

    for (size_t block = 1; block < 29; ++block)
    {
        const auto& rms = fingerprint[block].rms;
        playing = playing && std::min (rms[0], rms[1]) > -40.0f;
        wide = wide || std::abs (rms[0] - rms[1]) > 0.05f;
    }

    CHECK_MESSAGE (playing, "0.10-2.90 s: the chords should be above -40 dBFS on each side");
    CHECK_MESSAGE (wide, "0.10-2.90 s: the unison should make the sides differ somewhere");

    const auto& after = fingerprint[37].rms;
    const auto quiet = std::max (after[0], after[1]) < -60.0f;
    CHECK_MESSAGE (quiet, ("3.70-3.80 s: the chords should have rung out; the RMS is " + describeLevels (after)).toStdString());
    return playing && wide && quiet;
}

// A sine that starts loud and dies away (by 60 dB a second), for a clip played backwards: written as writeSound() writes
// tones, in mono.
juce::File writeDecay (const juce::File& file, double seconds, double frequency)
{
    constexpr double sampleRate = 44100.0;
    juce::AudioBuffer<float> buffer (1, (int) (seconds * sampleRate));

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const auto t = i / sampleRate;
        buffer.setSample (0, i, (float) (0.5 * std::pow (10.0, -3.0 * t) * std::sin (juce::MathConstants<double>::twoPi * frequency * t)));
    }

    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    return file;
}

//==============================================================================
// Phazite's synth as it is made: three chords of two beats each at 120 BPM, C, F and G major, then their release.
void addSynthSong (TestSong& song)
{
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.setInstrument (track, phazite::devices::Synth::typeID);
    const auto clip = song.addMidiClip (track, 0.0, 6.0);
    song.addChord (clip, { 48, 52, 55 }, 0.0, 2.0);
    song.addChord (clip, { 53, 57, 60 }, 2.0, 2.0);
    song.addChord (clip, { 55, 59, 62 }, 4.0, 2.0);
}

bool soundsLikeChords (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    auto playing = true;

    for (size_t block = 1; block < 29; ++block)
        playing = isBetween (fingerprint, block, -40.0f, 0.0f, "the chords play") && playing;

    return isLouder (fingerprint, 15, 39, 20.0f, "their release has rung out") && playing;
}

// Phazite's drum sampler as it is made, with its kit, for two bars at 120 BPM: a kick on the first and third beats, a snare
// on the second and fourth, and a closed hi-hat, quieter, on every half beat.
void addDrumSong (TestSong& song)
{
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.setInstrument (track, phazite::devices::DrumSampler::typeID);
    const auto clip = song.addMidiClip (track, 0.0, 8.0);

    for (int beatNumber = 0; beatNumber < 8; ++beatNumber)
        song.addNote (clip, beatNumber % 2 == 0 ? 36 : 38, beatNumber, 0.25);

    for (int half = 0; half < 16; ++half)
        song.addNote (clip, 42, half * 0.5, 0.25, 80);
}

// Each beat's hit is louder than the end of the beat before it, and the first kick is loud.
bool soundsLikeTheDrums (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    auto hits = true;

    for (size_t beatNumber = 1; beatNumber < 8; ++beatNumber)
        hits = isLouder (fingerprint, beatNumber * 5, beatNumber * 5 - 1, 3.0f, "each beat's hit") && hits;

    const auto kick = fingerprint.front().peak[0] > -18.0f;
    CHECK_MESSAGE (
        kick, ("0.00-0.10 s: the kick should peak above -18 dBFS; it peaks at " + formatLevel (fingerprint.front().peak[0])).toStdString());
    return hits && kick;
}

//==============================================================================
// A tone played plain from 0 to 1 s, then again from 2 to 3 s turned down 6 dB by its clip's gain, at 120 BPM.
void addClipGainSong (TestSong& song)
{
    song.setTempo (120.0);
    const auto tone = phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 1.0, 440.0, 0.25f);
    song.addAudioClip (song.firstTrack, tone, 0.0);
    const auto quieter = song.addAudioClip (song.firstTrack, tone, 4.0);
    phazite::setClipGain (song.context, quieter, -6.0f);
}

bool soundsLikeClipGain (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    const auto plain = getPower (fingerprint[5]);
    auto ok = isBetween (fingerprint, 5, -30.0f, -6.0f, "the tone plays");
    ok = isBetween (fingerprint, 25, plain - 6.5f, plain - 5.5f, "the second clip is 6 dB quieter") && ok;
    ok = isBetween (fingerprint, 15, -100.0f, floorDb, "nothing plays between the clips") && ok;
    return isBetween (fingerprint, 35, -100.0f, floorDb, "nothing plays after them") && ok;
}

// Four tones of 0.9 s, one each second at 120 BPM, each fading in over its first 0.3 s and out over its last 0.3 s, with
// linear, convex, concave and S-curve fades.
void addFadesSong (TestSong& song)
{
    song.setTempo (120.0);
    const auto tone = phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 0.9, 330.0, 0.25f);
    int i = 0;

    for (const auto* shape : { "clip.fadeLinear", "clip.fadeConvex", "clip.fadeConcave", "clip.fadeSCurve" })
    {
        const auto start = 2.0 * i++;
        const auto clip = song.addAudioClip (song.firstTrack, tone, start);
        phazite::fadeClipTo (song.context, clip, true, beat (start + 0.6));
        phazite::fadeClipTo (song.context, clip, false, beat (start + 1.2));
        song.select (clip);
        song.run (shape);
    }
}

bool soundsLikeFades (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    auto ok = true;

    for (size_t clip = 0; clip < 4; ++clip)
    {
        const auto start = clip * 10;
        ok = isLouder (fingerprint, start + 4, start, 6.0f, "each clip fades in") && ok;
        ok = isLouder (fingerprint, start + 4, start + 8, 3.0f, "each clip fades out") && ok;
    }

    return ok;
}

// A loop of clicks played at 120 BPM, which its name says, in a song at 90 BPM: it follows the song's tempo, a click on
// every beat, 0.667 s apart.
void addStretchedSong (TestSong& song)
{
    song.setTempo (90.0);
    const auto clicks = phazite::test::writeClicks (song.getSoundsFolder(), 8, 120.0);
    const auto clip = song.addAudioClip (song.firstTrack, clicks, 0.0);
    const auto info = phazite::getAudioClip (song.context, clip);
    REQUIRE (info.has_value());
    CHECK (info->followsTempo);
    song.waitForClipRenders();
}

bool soundsLikeClicksAt90 (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    auto ok = true;

    for (int click = 1; click < 6; ++click)
    {
        const auto onset = (size_t) (click * 20 / 3); // the block of the click on the beat, 2/3 s each
        const auto between = (size_t) ((click * 20 - 10) / 3);
        ok = isLouder (fingerprint, onset, between, 15.0f, "each beat's click, 0.667 s apart") && ok;
    }

    return ok;
}

// A 220 Hz tone pitched up an octave by its clip, at 120 BPM: it plays at 440 Hz, as long as it was.
void addPitchedSong (TestSong& song)
{
    song.setTempo (120.0);
    const auto tone = phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Low Tone.wav"), 3.0, 220.0, 0.25f);
    const auto clip = song.addAudioClip (song.firstTrack, tone, 0.0);
    phazite::setClipPitch (song.context, clip, 12.0f);
    song.waitForClipRenders();
}

bool soundsLikeAnOctaveUp (const Fingerprint& fingerprint, const juce::AudioBuffer<float>& audio)
{
    const auto frequency = phazite::test::estimateFrequency (audio, 0, phazite::test::renderSampleRate, 1.0, 1.0);
    const auto octaveUp = std::abs (frequency - 440.0) < 5.0;
    CHECK_MESSAGE (octaveUp, ("1-2 s: the tone should be at 440 Hz; it is at " + juce::String (frequency, 1) + " Hz").toStdString());
    auto ok = isBetween (fingerprint, 15, -30.0f, -6.0f, "the tone plays");
    return isBetween (fingerprint, 35, -100.0f, floorDb, "it lasts as long as it did") && ok && octaveUp;
}

// A tone that dies away, played backwards by its clip at 120 BPM: it swells instead.
void addReversedSong (TestSong& song)
{
    song.setTempo (120.0);
    const auto decay = writeDecay (song.getSoundsFolder().getChildFile ("Decay.wav"), 3.0, 330.0);
    const auto clip = song.addAudioClip (song.firstTrack, decay, 0.0);
    phazite::setClipReversed (song.context, clip, true);
    song.waitForClipRenders();
}

bool soundsLikeASwell (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    return isLouder (fingerprint, 28, 5, 20.0f, "it swells");
}

//==============================================================================
// A chord on the synth from 0 to 0.5 s and a higher note from 0.5 to 1 s at 120 BPM, and an alias of their clip from 2 to
// 3 s; the higher note is added to the original after the alias was made, and plays in both.
void addAliasSong (TestSong& song)
{
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.setInstrument (track, phazite::devices::Synth::typeID);
    const auto clip = song.addMidiClip (track, 0.0, 2.0);
    song.addChord (clip, { 60, 64, 67 }, 0.0, 1.0);
    song.copyClip (clip, 4.0, true);
    song.addNote (clip, 72, 1.0, 1.0);
}

bool soundsLikeAnAlias (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    auto ok = true;

    for (size_t block = 0; block < 10; ++block)
    {
        const auto original = getPower (fingerprint[block]);
        ok = isBetween (fingerprint, block + 20, original - 1.0f, original + 1.0f, "the alias plays what its original does") && ok;
    }

    return ok;
}

//==============================================================================
// A tone at 120 BPM whose volume is automated through each kind of line: from 0 dB down to -24 dB at 1 s bent as far as a
// line bends (staying high, then falling), back up to 0 dB at 2 s bent the other way (rising at once), holding there until
// 3 s and jumping down to -12 dB. Played as the lane draws it.
void addAutomationSong (TestSong& song)
{
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addAudioClip (track, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 4.0, 440.0, 0.25f), 0.0);

    const auto db = [] (float decibels) { return phazite::decibelsToFaderPosition (decibels); };
    song.addAutomation (song.getVolumeParameter (track), { { beat (0.0), db (0.0f), 0.5f },
                                                           { beat (2.0), db (-24.0f), -0.5f },
                                                           { beat (4.0), db (0.0f), 1.0f },
                                                           { beat (6.0), db (-12.0f) },
                                                           { beat (8.0), db (-12.0f) } });
}

bool soundsLikeTheAutomation (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    const auto held = getPower (fingerprint[22]);
    auto ok = isBetween (fingerprint, 5, held - 2.5f, held + 0.5f, "bent, the volume stays near 0 dB half way to 1 s");
    ok = isLouder (fingerprint, 0, 9, 9.0f, "then it falls toward -24 dB at 1 s") && ok;
    ok = isLouder (fingerprint, 19, 10, 9.0f, "it comes back up to 0 dB at 2 s") && ok;
    ok = isBetween (fingerprint, 14, held - 2.5f, held + 0.5f, "bent the other way, it is near 0 dB again half way back") && ok;
    ok = isBetween (fingerprint, 28, held - 0.5f, held + 0.5f, "it holds at 0 dB until 3 s") && ok;
    return isBetween (fingerprint, 33, held - 12.5f, held - 11.5f, "then it jumps down to -12 dB") && ok;
}

// A tone at 120 BPM whose pan is automated from left to right over 4 s.
void addPanAutomationSong (TestSong& song)
{
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.addAudioClip (track, phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Tone.wav"), 4.0, 440.0, 0.25f), 0.0);
    song.addAutomation (song.getPanParameter (track), { { beat (0.0), -1.0f }, { beat (8.0), 1.0f } });
}

bool soundsLikeAPanSweep (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    const auto left = fingerprint[1].rms[0] > fingerprint[1].rms[1] + 6.0f;
    const auto middle = std::abs (fingerprint[20].rms[0] - fingerprint[20].rms[1]) < 1.0f;
    const auto right = fingerprint[38].rms[1] > fingerprint[38].rms[0] + 6.0f;
    CHECK_MESSAGE (left, ("0.1-0.2 s: panned left; the RMS is " + describeLevels (fingerprint[1].rms)).toStdString());
    CHECK_MESSAGE (middle, ("2.0-2.1 s: in the middle; the RMS is " + describeLevels (fingerprint[20].rms)).toStdString());
    CHECK_MESSAGE (right, ("3.8-3.9 s: panned right; the RMS is " + describeLevels (fingerprint[38].rms)).toStdString());
    return left && middle && right;
}

// A high chord on the synth at 120 BPM through Phazite's filter, its cutoff automated to stay at 100 Hz for 2 s, then go up to
// 10 kHz by 3 s (a line in hertz, so most of the way it is above the chord): the chord comes up as the filter opens.
void addDeviceAutomationSong (TestSong& song)
{
    song.setTempo (120.0);
    const auto track = song.firstTrack;
    song.setInstrument (track, phazite::devices::Synth::typeID);
    const auto filter = song.addEffect (track, phazite::devices::FilterEffect::typeID);
    song.addAutomation ({ filter, "cutoff" }, { { beat (0.0), 100.0f }, { beat (4.0), 100.0f }, { beat (6.0), 10000.0f } });
    const auto clip = song.addMidiClip (track, 0.0, 8.0);
    song.addChord (clip, { 72, 76, 79 }, 0.0, 8.0);
}

bool soundsLikeAnOpeningFilter (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    return isLouder (fingerprint, 30, 10, 20.0f, "the chord comes up as the filter opens");
}

//==============================================================================
// At 90 BPM in 3/4: a short note on every beat on the synth, a beat lasting 0.667 s.
void addTempoSong (TestSong& song)
{
    song.setTempo (90.0);
    song.setTimeSignature (3, 4);
    const auto track = song.firstTrack;
    song.setInstrument (track, phazite::devices::Synth::typeID);
    const auto clip = song.addMidiClip (track, 0.0, 6.0);

    for (int beatNumber = 0; beatNumber < 6; ++beatNumber)
        song.addNote (clip, 60, beatNumber, 0.25);
}

bool soundsLikeBeatsAt90 (const Fingerprint& fingerprint, const juce::AudioBuffer<float>&)
{
    auto ok = true;

    for (int note = 1; note < 6; ++note)
    {
        const auto onset = (size_t) std::ceil (note * 20.0 / 3.0); // the first whole block after the note starts
        ok = isLouder (fingerprint, onset, onset - 2, 3.0f, "a note on each beat, 0.667 s apart") && ok;
    }

    return ok;
}

// A song made by a function, rendered as a golden render (checkGoldenRender()).
void renders (void (*make) (TestSong&), const phazite::test::GoldenRender& render)
{
    TestSong song;
    make (song);
    phazite::test::settle();
    phazite::test::checkGoldenRender (song.context, render);
}

//==============================================================================
// A fingerprint as long as a 4-second render's, with the same levels in every block and on both sides.
Fingerprint makeFingerprint (float rms, float peak)
{
    phazite::test::BlockLevels levels;
    levels.rms.fill (rms);
    levels.peak.fill (peak);
    return Fingerprint (40, levels);
}
} // namespace

//==============================================================================
TEST_SUITE ("golden")
{
    TEST_CASE ("Golden files: render fingerprints match within a tolerance, above a floor")
    {
        using phazite::test::compareFingerprints;
        using phazite::test::writeFingerprint;

        const auto golden = makeFingerprint (-20.0f, -17.0f);
        const auto goldenText = writeFingerprint (golden, "about");

        // One block on each line, with two decimals; a fingerprint matches itself.
        CHECK (goldenText.contains ("\n    { \"start\": 1.2, \"rms\": [-20.00, -20.00], \"peak\": [-17.00, -17.00] },\n"));
        CHECK (compareFingerprints (goldenText, goldenText).isEmpty());

        // Within the tolerance: a level 0.04 dB louder.
        auto louder = golden;
        louder[12].rms[0] = -19.96f;
        CHECK (compareFingerprints (goldenText, writeFingerprint (louder, "about")).isEmpty());

        // Beyond it: 0.1 dB louder is a difference, listed with its block, side and levels.
        louder[12].rms[0] = -19.9f;
        const auto differences = compareFingerprints (goldenText, writeFingerprint (louder, "about"));
        REQUIRE (differences.size() == 1);
        CHECK (differences[0] == juce::String ("1.20-1.30 s, left RMS: expected -20.00 dBFS, got -19.90 dBFS"));

        // Below the floor, levels are all silence: -80 and -95 dBFS are the same.
        auto quiet = golden;
        auto quieter = golden;
        quiet[30].peak.fill (-80.0f);
        quieter[30].peak.fill (-95.0f);
        CHECK (compareFingerprints (writeFingerprint (quiet, "about"), writeFingerprint (quieter, "about")).isEmpty());

        // A render of another length differs in its number of blocks, which aren't compared one by one.
        auto shorter = golden;
        shorter.pop_back();
        const auto lengthChange = compareFingerprints (goldenText, writeFingerprint (shorter, "about"));
        REQUIRE (lengthChange.size() == 1);
        CHECK (lengthChange[0] == juce::String ("blocks: expected 40, got 39"));

        // Another rate or block size isn't compared block by block either.
        const auto otherRate = compareFingerprints (goldenText, writeFingerprint (golden, "about", 48000, 4800));
        REQUIRE (otherRate.size() == 2);
        CHECK (otherRate[0] == juce::String ("sampleRate: expected 44100, got 48000"));

        // A render that differs everywhere lists only the first differences.
        CHECK (compareFingerprints (goldenText, writeFingerprint (makeFingerprint (-10.0f, -7.0f), "about")).size()
               == phazite::test::maxListedDifferences);
    }

    TEST_CASE ("Golden render: two tones through a bus, a send to a reverb return, automation and pan")
    {
        TestSong song;
        addMix (song);
        phazite::test::settle();
        phazite::test::checkGoldenRender (
            song.context,
            golden ("mix", "two tones through a bus, a send to a reverb return, volume automation and pan.", 4.0, soundsLikeTheMix));
    }

    TEST_CASE ("Golden render: the First Beat example song, a beat on Phazite's drum sampler and an arpeggio on its synth")
    {
        TestSong song (TestSong::Start::firstBeat);
        phazite::test::settle();
        phazite::test::checkGoldenRender (song.context, golden ("first-beat",
                                                                "the First Beat example song, a beat on Phazite's drum sampler and "
                                                                "an arpeggio on its synth.",
                                                                firstBeatSeconds, soundsLikeFirstBeat));
    }

    TEST_CASE ("Golden render: two chords on Phazite's wavetable synth, in unison, an envelope sweeping each note's table")
    {
        TestSong song;
        addWavetableSong (song);
        phazite::test::settle();
        phazite::test::checkGoldenRender (song.context, golden ("wavetable",
                                                                "two chords on Phazite's wavetable synth, its two oscillators in "
                                                                "unison, an envelope sweeping each note's table.",
                                                                4.0, soundsLikeTheWavetable));
    }

    TEST_CASE ("Golden render: three chords on Phazite's synth as it is made")
    {
        renders (addSynthSong,
                 golden ("synth", "three chords on Phazite's synth as it is made, then their release.", 4.0, soundsLikeChords));
    }

    TEST_CASE ("Golden render: two bars of a beat on Phazite's drum sampler with its kit")
    {
        renders (addDrumSong,
                 golden ("drums", "two bars of kick, snare and hi-hat on Phazite's drum sampler with its kit.", 4.0, soundsLikeTheDrums));
    }

    TEST_CASE ("Golden render: a tone played plain, then 6 dB quieter by its clip's gain")
    {
        renders (addClipGainSong,
                 golden ("audio-gain", "a tone in an audio clip, then in another turned down 6 dB by its gain.", 4.0, soundsLikeClipGain));
    }

    TEST_CASE ("Golden render: audio clips fading in and out, linear, convex, concave and S-curve")
    {
        renders (addFadesSong,
                 golden ("audio-fades", "four audio clips fading in and out over 0.3 s: linear, convex, concave and S-curve fades.", 4.0,
                         soundsLikeFades));
    }

    TEST_CASE ("Golden render: a loop of clicks made at 120 BPM follows a song at 90 BPM")
    {
        renders (addStretchedSong,
                 golden ("audio-stretched", "a loop of clicks made at 120 BPM in a song at 90 BPM, stretched to follow its tempo.", 4.0,
                         soundsLikeClicksAt90));
    }

    TEST_CASE ("Golden render: a tone pitched up an octave by its clip, as long as it was")
    {
        renders (addPitchedSong,
                 golden ("audio-pitched", "a 220 Hz tone pitched up an octave by its audio clip.", 4.0, soundsLikeAnOctaveUp));
    }

    TEST_CASE ("Golden render: a tone that dies away, played backwards by its clip")
    {
        renders (addReversedSong,
                 golden ("audio-reversed", "a tone that dies away, played backwards by its audio clip.", 4.0, soundsLikeASwell));
    }

    TEST_CASE ("Golden render: an alias plays what its original does, a note added later too")
    {
        renders (addAliasSong,
                 golden ("aliases", "a chord and a note on Phazite's synth, and an alias of their clip.", 4.0, soundsLikeAnAlias));
    }

    TEST_CASE ("Golden render: volume automation through bent lines, a hold and a jump")
    {
        renders (addAutomationSong, golden ("automation-volume", "a tone whose volume automation bends down and up, holds, then jumps.",
                                            4.0, soundsLikeTheAutomation));
    }

    TEST_CASE ("Golden render: pan automation from left to right")
    {
        renders (addPanAutomationSong,
                 golden ("automation-pan", "a tone whose pan automation sweeps from left to right.", 4.0, soundsLikeAPanSweep));
    }

    TEST_CASE ("Golden render: a device's parameter automated, a filter opening on a chord")
    {
        renders (addDeviceAutomationSong,
                 golden ("automation-device", "a chord on Phazite's synth through its filter, whose cutoff automation opens it.", 4.0,
                         soundsLikeAnOpeningFilter));
    }

    TEST_CASE ("Golden render: notes on the beats of a song at 90 BPM in 3/4")
    {
        renders (addTempoSong, golden ("tempo", "a short note on every beat of a song at 90 BPM in 3/4.", 4.0, soundsLikeBeatsAt90));
    }
}
