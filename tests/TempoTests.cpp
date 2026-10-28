// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo and time signature (src/model/transport/internal/Tempo.h), and what a change of tempo stretches:
// samples that follow it, which Phazite renders (roadmap step 2.18), and patterns unless they keep their speed.
#include "TestHelpers.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>

namespace
{
constexpr double clickRate = 44100.0;

// A loop of clicks, one on each beat at a tempo, each a burst of noise of its own, named for its tempo so that it follows
// the song's ("Clicks 120 BPM.wav").
juce::File writeClicks (const juce::File& folder, int beats, double bpm)
{
    const auto file = folder.getChildFile ("Clicks " + juce::String (bpm, 0) + " BPM.wav");
    const auto beatSamples = (int) std::lround (60.0 / bpm * clickRate);
    juce::AudioBuffer<float> audio (1, beats * beatSamples);
    audio.clear();
    juce::Random random (3);

    for (int beat = 0; beat < beats; ++beat)
        for (int i = 0; i < 88; ++i)
            audio.setSample (0, beat * beatSamples + i, 0.5f * std::exp ((float) -i / 30.0f) * (random.nextFloat() * 2.0f - 1.0f));

    folder.createDirectory();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (clickRate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

// The loudest millisecond (RMS) of a channel between two samples, and where it starts.
std::pair<double, int> findLoudestMillisecond (const float* samples, int from, int to)
{
    constexpr int ms = 44;
    auto loudest = std::pair<double, int> { 0.0, from };

    for (int i = std::max (0, from); i + ms <= to; ++i)
    {
        double sum = 0.0;

        for (int j = 0; j < ms; ++j)
            sum += (double) samples[i + j] * samples[i + j];

        if (const auto level = std::sqrt (sum / ms); level > loudest.first)
            loudest = { level, i };
    }

    return loudest;
}
} // namespace

TEST_CASE ("The tempo changes as one undo step, within Tracktion's limits, and clips stay on their beats")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto* track = te::getAudioTracks (*edit)[0];
    auto* clip = phazite::insertMidiClip (*track, { te::BeatPosition::fromBeats (8.0), te::BeatPosition::fromBeats (12.0) }, nullptr);
    REQUIRE (clip != nullptr);
    edit->getUndoManager().clearUndoHistory();
    CHECK (phazite::getTempo (*edit) == doctest::Approx (120.0));

    phazite::setTempo (*edit, 90.004);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (90.0));
    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Change Tempo"));
    CHECK (clip->getStartBeat().inBeats() == doctest::Approx (8.0));
    CHECK (clip->getPosition().getStart().inSeconds() == doctest::Approx (8.0 * 60.0 / 90.0));

    phazite::setTempo (*edit, 1000.0);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (phazite::maxTempo));
    phazite::setTempo (*edit, 1.0);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (phazite::minTempo));

    phazite::undo (*edit);
    phazite::undo (*edit);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (90.0));

    // The same tempo again changes nothing.
    edit->getUndoManager().clearUndoHistory();
    phazite::setTempo (*edit, 90.0);
    CHECK_FALSE (edit->getUndoManager().canUndo());
}

TEST_CASE ("The time signature changes as one undo step, to one Phazite takes")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->getUndoManager().clearUndoHistory();
    CHECK (phazite::getTimeSignature (*edit) == phazite::TimeSignature { 4, 4 });

    phazite::setTimeSignature (*edit, { 6, 8 });
    CHECK (phazite::getTimeSignature (*edit) == phazite::TimeSignature { 6, 8 });
    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Change Time Signature"));

    // Not 0 beats, nor a beat of a third of a note.
    for (const auto invalid : { phazite::TimeSignature { 0, 4 }, phazite::TimeSignature { 4, 3 }, phazite::TimeSignature { 33, 4 } })
    {
        CHECK_FALSE (phazite::isValid (invalid));
        phazite::setTimeSignature (*edit, invalid);
        CHECK (phazite::getTimeSignature (*edit) == phazite::TimeSignature { 6, 8 });
    }

    phazite::undo (*edit);
    CHECK (phazite::getTimeSignature (*edit) == phazite::TimeSignature { 4, 4 });
}

TEST_CASE ("A tempo change stretches the samples that follow it, which Phazite renders, and leaves the others at their speed")
{
    phazite::test::TemporaryFolder temp;
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto& track = *te::getAudioTracks (*edit)[0];

    // A loop that knows its tempo follows the song's; a sound that doesn't plays at its own speed.
    auto* loop = dynamic_cast<te::AudioClipBase*> (phazite::importFile (track, writeClicks (temp.folder, 8, 120.0), te::BeatPosition()));
    auto* sound = dynamic_cast<te::AudioClipBase*> (phazite::importFile (
        track, phazite::test::writeSound (temp.folder.getChildFile ("One-second hit.wav"), 1.0), te::BeatPosition::fromBeats (10.0)));
    REQUIRE (loop != nullptr);
    REQUIRE (sound != nullptr);
    CHECK (phazite::getClipTempo (*loop) == doctest::Approx (120.0));
    CHECK_FALSE (phazite::isRendered (*loop)); // at its own tempo it plays its file
    CHECK (phazite::getClipTempo (*sound) == doctest::Approx (0.0));
    edit->getUndoManager().clearUndoHistory();

    phazite::setTempo (*edit, 100.0);
    CHECK (edit->getUndoManager().getUndoDescription() == juce::String ("Change Tempo"));

    // The loop keeps its beats: eight of them, stretched from its file's four seconds, played from a render.
    CHECK (loop->getStartBeat().inBeats() == doctest::Approx (0.0));
    CHECK (loop->getLengthInBeats().inBeats() == doctest::Approx (8.0));
    CHECK (phazite::getClipStretch (*loop).timeRatio == doctest::Approx (1.2));
    REQUIRE (phazite::isRendered (*loop));
    CHECK (phazite::getClipFile (*loop).getFileName() == juce::String ("Clicks 120 BPM.wav"));
    const auto render = loop->getSourceFileReference().getFile();
    CHECK (render.getFileName().startsWith (phazite::renderFilePrefix));
    CHECK_FALSE (loop->getAudioFile().isValid()); // until it is rendered: silent

    // The sound starts on its beat, at its own speed.
    CHECK (sound->getStartBeat().inBeats() == doctest::Approx (10.0));
    CHECK (sound->getPosition().getLength().inSeconds() == doctest::Approx (1.0));
    CHECK_FALSE (phazite::isRendered (*sound));

    REQUIRE (phazite::renderMissingClipAudio (*edit));
    CHECK (loop->getAudioFile().isValid());
    CHECK (loop->getAudioFile().getLength() == doctest::Approx (4.8).epsilon (0.001));

    // Undone, it plays its file again; redone, its render, at once.
    phazite::undo (*edit);
    CHECK (phazite::getTempo (*edit) == doctest::Approx (120.0));
    CHECK_FALSE (phazite::isRendered (*loop));
    CHECK (loop->getPosition().getLength().inSeconds() == doctest::Approx (4.0));
    phazite::redo (*edit);
    CHECK (loop->getSourceFileReference().getFile() == render);
    CHECK (loop->getAudioFile().isValid());

    // A render no clip plays goes when the cache is purged.
    phazite::undo (*edit);
    phazite::purgeClipRenders (*edit);
    CHECK_FALSE (render.existsAsFile());
}

TEST_CASE ("A loop stretched to the song's tempo plays every hit once and on its beat, as export renders it")
{
    // Known bug 1, which Tracktion's renders had: clips stretched by SoundTouch played 20 to 28 ms early, and doubled hits.
    phazite::test::TemporaryFolder temp;
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    edit->tempoSequence.getTempos()[0]->setBpm (100.0);
    auto* loop = dynamic_cast<te::AudioClipBase*> (
        phazite::importFile (*te::getAudioTracks (*edit)[0], writeClicks (temp.folder, 8, 120.0), te::BeatPosition()));
    REQUIRE (loop != nullptr);
    REQUIRE (phazite::renderMissingClipAudio (*edit));

    juce::TemporaryFile wav (".wav");
    REQUIRE (te::Renderer::renderToFile (*edit, wav.getFile(), false));
    const std::unique_ptr<juce::AudioFormatReader> reader (
        phazite::test::getEngine().getAudioFileFormatManager().readFormatManager.createReaderFor (wav.getFile()));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
    const auto rate = reader->sampleRate;
    const auto* out = audio.getReadPointer (0);

    // Each click where its beat is at 100 BPM, measured from the first, give or take what Rubber Band moves an attack by.
    const auto beat = (int) std::lround (0.6 * rate), near = (int) (0.01 * rate);
    const auto first = findLoudestMillisecond (out, 0, near).second;
    std::vector<double> offsets;

    for (int k = 1; k < 8; ++k)
    {
        CAPTURE (k);
        const auto expected = first + k * beat;
        const auto [level, at] = findLoudestMillisecond (out, expected - near, expected + near);
        offsets.push_back ((at - expected) / rate * 1000.0);
        CHECK (std::abs (offsets.back()) < 2.5);

        // And once: nothing between two clicks comes within 20 dB of one.
        const auto between = findLoudestMillisecond (out, expected - beat + (int) (0.02 * rate), expected - (int) (0.02 * rate)).first;
        CHECK (juce::Decibels::gainToDecibels (between / level) < -20.0);
    }

    std::sort (offsets.begin(), offsets.end());
    CHECK (std::abs (offsets[offsets.size() / 2]) < 1.0);
}

TEST_CASE ("Patterns follow the tempo, their notes on their beats, unless the song says they keep their speed")
{
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto* clip = phazite::insertMidiClip (*te::getAudioTracks (*edit)[0],
                                          { te::BeatPosition::fromBeats (4.0), te::BeatPosition::fromBeats (8.0) }, nullptr);
    REQUIRE (clip != nullptr);

    for (int i = 0; i < 4; ++i)
        clip->getSequence().addNote (60, te::BeatPosition::fromBeats (i), te::BeatDuration::fromBeats (0.5), 100, 0, nullptr);

    edit->getUndoManager().clearUndoHistory();
    const auto noteBeat = [&clip] (int i) { return clip->getSequence().getNotes()[i]->getStartBeat().inBeats(); };

    // Following, they play twice as slowly at half the tempo.
    CHECK (phazite::doPatternsFollowTempo (*edit));
    phazite::setTempo (*edit, 60.0);
    CHECK (noteBeat (3) == doctest::Approx (3.0));
    CHECK (clip->getLengthInBeats().inBeats() == doctest::Approx (4.0));
    phazite::undo (*edit);

    // Keeping their speed, they keep their times: a note a second and a half into the clip stays there, half as many beats
    // in, and the clip, starting on the same beat, lasts as many seconds.
    {
        const phazite::UndoStep step (*edit, "Patterns Follow Tempo");
        phazite::togglePatternsFollowTempo (*edit);
    }

    CHECK_FALSE (phazite::doPatternsFollowTempo (*edit));
    phazite::setTempo (*edit, 60.0);
    CHECK (noteBeat (3) == doctest::Approx (1.5));
    CHECK (clip->getSequence().getNotes()[3]->getLengthBeats().inBeats() == doctest::Approx (0.25));
    CHECK (clip->getStartBeat().inBeats() == doctest::Approx (4.0));
    CHECK (clip->getLengthInBeats().inBeats() == doctest::Approx (2.0));
    CHECK (clip->getPosition().getLength().inSeconds() == doctest::Approx (2.0));

    phazite::undo (*edit);
    CHECK (noteBeat (3) == doctest::Approx (3.0));
    CHECK (clip->getLengthInBeats().inBeats() == doctest::Approx (4.0));
}

TEST_CASE ("What a tempo change stretches: every sample follows it or none does, and patterns follow or keep their speed")
{
    phazite::test::TemporaryFolder temp;
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    auto& track = *te::getAudioTracks (*edit)[0];
    REQUIRE (phazite::importFile (track, writeClicks (temp.folder, 8, 120.0), te::BeatPosition()) != nullptr);
    REQUIRE (phazite::importFile (track, phazite::test::writeSound (temp.folder.getChildFile ("One-second hit.wav"), 1.0),
                                  te::BeatPosition::fromBeats (10.0))
             != nullptr);
    auto* other = phazite::insertTrack (*edit, &track, nullptr);
    REQUIRE (other != nullptr);
    REQUIRE (phazite::insertMidiClip (*other, { te::BeatPosition(), te::BeatPosition::fromBeats (4.0) }, nullptr) != nullptr);

    const auto stretch = phazite::getTempoStretch (*edit);
    CHECK (stretch.samples == 2);
    CHECK (stretch.samplesFollowing == 1);
    CHECK (stretch.patterns == 1);
    CHECK (stretch.patternsFollow);
    CHECK_FALSE (phazite::doAllSamplesFollowTempo (*edit));

    // Some follow: all of them do.
    phazite::toggleSamplesFollowTempo (*edit);
    CHECK (phazite::getTempoStretch (*edit).samplesFollowing == 2);
    CHECK (phazite::doAllSamplesFollowTempo (*edit));

    // All of them did: none does.
    phazite::toggleSamplesFollowTempo (*edit);
    CHECK (phazite::getTempoStretch (*edit).samplesFollowing == 0);

    phazite::togglePatternsFollowTempo (*edit);
    CHECK_FALSE (phazite::getTempoStretch (*edit).patternsFollow);
}
