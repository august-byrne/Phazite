// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's tempo and time signature (src/model/transport/internal/Tempo.h), and what a change of tempo stretches:
// samples that follow it, which Phazite renders (roadmap step 2.18), and patterns unless they keep their speed.
#include "EnginePlayback.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/engine/Preview.h"
#include "model/engine/internal/Preview.h"
#include "model/mirror/internal/Mirror.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>

namespace
{
using phazite::test::writeClicks;

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

// What an audio clip plays, its file or Phazite's render of it; and how long it lasts, in the song's seconds.
juce::File playedFileOf (const phazite::Song& song, phazite::ClipID clip)
{
    return phazite::getPlayedFile (song, phazite::findClipState (song, clip));
}

double playedSecondsOf (const phazite::Song& song, phazite::ClipID clip)
{
    const auto position = phazite::getPlayedPosition (song, phazite::findClipState (song, clip));
    return position.end - position.start;
}

bool isAudioClip (const phazite::Song& song, phazite::ClipID clip)
{
    return phazite::isAudioState (phazite::findClipState (song, clip));
}
} // namespace

TEST_CASE ("The tempo changes as one undo step, within Tracktion's limits, and clips stay on their beats")
{
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    auto& song = phazite::getSong (*edit);
    const auto track = phazite::getTrackID (*te::getAudioTracks (*edit)[0]);
    const auto clip = phazite::insertMidiClip (song, track, { phazite::test::at (8.0), phazite::test::at (12.0) }, nullptr);
    REQUIRE (clip.isValid());
    phazite::getSong (*edit).getUndoManager().clearUndoHistory();
    CHECK (phazite::getTempo (phazite::getSong (*edit)) == doctest::Approx (120.0));

    phazite::setTempo (song, 90.004);
    CHECK (phazite::getTempo (phazite::getSong (*edit)) == doctest::Approx (90.0));
    CHECK (phazite::getSong (*edit).getUndoManager().getUndoDescription() == juce::String ("Change Tempo"));
    CHECK (phazite::getClipRange (song, clip).getStart().inBeats() == doctest::Approx (8.0));
    CHECK (phazite::toSeconds (song, phazite::getClipRange (song, clip).getStart()) == doctest::Approx (8.0 * 60.0 / 90.0));

    phazite::setTempo (song, 1000.0);
    CHECK (phazite::getTempo (phazite::getSong (*edit)) == doctest::Approx (phazite::maxTempo));
    phazite::setTempo (song, 1.0);
    CHECK (phazite::getTempo (phazite::getSong (*edit)) == doctest::Approx (phazite::minTempo));

    phazite::undo (*edit);
    phazite::undo (*edit);
    CHECK (phazite::getTempo (phazite::getSong (*edit)) == doctest::Approx (90.0));

    // The same tempo again changes nothing.
    phazite::getSong (*edit).getUndoManager().clearUndoHistory();
    phazite::setTempo (song, 90.0);
    CHECK_FALSE (phazite::getSong (*edit).getUndoManager().canUndo());
}

TEST_CASE ("The time signature changes as one undo step, to one Phazite takes")
{
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    phazite::getSong (*edit).getUndoManager().clearUndoHistory();
    CHECK (phazite::getTimeSignature (phazite::getSong (*edit)) == phazite::TimeSignature { 4, 4 });

    phazite::setTimeSignature (phazite::getSong (*edit), { 6, 8 });
    CHECK (phazite::getTimeSignature (phazite::getSong (*edit)) == phazite::TimeSignature { 6, 8 });
    CHECK (phazite::getSong (*edit).getUndoManager().getUndoDescription() == juce::String ("Change Time Signature"));

    // Not 0 beats, nor a beat of a third of a note.
    for (const auto invalid : { phazite::TimeSignature { 0, 4 }, phazite::TimeSignature { 4, 3 }, phazite::TimeSignature { 33, 4 } })
    {
        CHECK_FALSE (phazite::isValid (invalid));
        phazite::setTimeSignature (phazite::getSong (*edit), invalid);
        CHECK (phazite::getTimeSignature (phazite::getSong (*edit)) == phazite::TimeSignature { 6, 8 });
    }

    phazite::undo (*edit);
    CHECK (phazite::getTimeSignature (phazite::getSong (*edit)) == phazite::TimeSignature { 4, 4 });
}

TEST_CASE ("The tempo map is Phazite's own, which Tracktion's tempo sequence mirrors one way, undo and redo too")
{
    phazite::test::TestSong song;
    auto& edit = *phazite::getEdit (song.context);

    // Every project has one, at 120 BPM in 4/4 from the song's start.
    const auto map = phazite::findPhaziteState (edit).getChildWithName (phazite::schema::TEMPOMAP);
    REQUIRE (map.isValid());
    CHECK ((double) map.getChildWithName (phazite::schema::TEMPO)[phazite::schema::bpm] == doctest::Approx (120.0));
    CHECK ((int) map.getChildWithName (phazite::schema::TIMESIGNATURE)[phazite::schema::numerator] == 4);

    // A change goes into the map, and Tracktion follows; an undo puts both back, and a redo both again.
    phazite::setTempo (phazite::getSong (edit), 96.5);
    phazite::setTimeSignature (phazite::getSong (edit), { 7, 8 });
    CHECK ((double) map.getChildWithName (phazite::schema::TEMPO)[phazite::schema::bpm] == doctest::Approx (96.5));
    CHECK (edit.tempoSequence.getTempo (0)->getBpm() == doctest::Approx (96.5));
    CHECK (edit.tempoSequence.getTimeSig (0)->numerator.get() == 7);
    CHECK (edit.tempoSequence.getTimeSig (0)->denominator.get() == 8);

    phazite::undo (edit);
    CHECK (phazite::getTimeSignature (phazite::getSong (edit)) == phazite::TimeSignature { 4, 4 });
    CHECK (edit.tempoSequence.getTimeSig (0)->numerator.get() == 4);
    phazite::undo (edit);
    CHECK (phazite::getTempo (phazite::getSong (edit)) == doctest::Approx (120.0));
    CHECK (edit.tempoSequence.getTempo (0)->getBpm() == doctest::Approx (120.0));
    phazite::redo (edit);
    CHECK (phazite::getTempo (phazite::getSong (edit)) == doctest::Approx (96.5));
    CHECK (edit.tempoSequence.getTempo (0)->getBpm() == doctest::Approx (96.5));

    // One way: what Tracktion holds is never the song's tempo; the mirror puts it back as the map says, and once they
    // agree, it changes nothing.
    edit.tempoSequence.getTempo (0)->setBpm (150.0);
    CHECK (phazite::getTempo (phazite::getSong (edit)) == doctest::Approx (96.5));
    phazite::mirror::reconcileTempoMap (edit);
    CHECK (edit.tempoSequence.getTempo (0)->getBpm() == doctest::Approx (96.5));

    phazite::getSong (edit).getUndoManager().clearUndoHistory();
    phazite::mirror::reconcileTempoMap (edit);
    CHECK_FALSE (phazite::getSong (edit).getUndoManager().canUndo());
}

TEST_CASE ("A tempo change stretches the samples that follow it, which Phazite renders, and leaves the others at their speed")
{
    phazite::test::TemporaryFolder temp;
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    auto& song = phazite::getSong (*edit);
    const auto track = phazite::getTrackID (*te::getAudioTracks (*edit)[0]);

    // A loop that knows its tempo follows the song's; a sound that doesn't plays at its own speed.
    const auto loop = phazite::importFile (song, track, writeClicks (temp.folder, 8, 120.0), phazite::BeatPosition());
    const auto sound = phazite::importFile (song, track, phazite::test::writeSound (temp.folder.getChildFile ("One-second hit.wav"), 1.0),
                                            phazite::test::at (10.0));
    REQUIRE (isAudioClip (song, loop));
    REQUIRE (isAudioClip (song, sound));
    CHECK (phazite::getClipTempo (song, loop) == doctest::Approx (120.0));
    CHECK_FALSE (phazite::isRendered (song, loop)); // at its own tempo it plays its file
    CHECK (phazite::getClipTempo (song, sound) == doctest::Approx (0.0));
    phazite::getSong (*edit).getUndoManager().clearUndoHistory();

    phazite::setTempo (song, 100.0);
    CHECK (phazite::getSong (*edit).getUndoManager().getUndoDescription() == juce::String ("Change Tempo"));

    // The loop keeps its beats: eight of them, stretched from its file's four seconds, played from a render.
    CHECK (phazite::getClipRange (song, loop).getStart().inBeats() == doctest::Approx (0.0));
    CHECK (phazite::getClipRange (song, loop).getLength().inBeats() == doctest::Approx (8.0));
    CHECK (phazite::getClipStretch (song, loop).timeRatio == doctest::Approx (1.2));
    REQUIRE (phazite::isRendered (song, loop));
    CHECK (phazite::getClipFile (song, phazite::findClipState (song, loop)).getFileName() == juce::String ("Clicks 120 BPM.wav"));
    const auto render = playedFileOf (song, loop);
    CHECK (render.getFileName().startsWith (phazite::renderFilePrefix));
    CHECK_FALSE (phazite::getAudioFileInfo (render).isValid()); // until it is rendered: silent

    // The sound starts on its beat, at its own speed.
    CHECK (phazite::getClipRange (song, sound).getStart().inBeats() == doctest::Approx (10.0));
    CHECK (playedSecondsOf (song, sound) == doctest::Approx (1.0));
    CHECK_FALSE (phazite::isRendered (song, sound));

    REQUIRE (phazite::renderMissingClipAudio (song));
    CHECK (phazite::getAudioFileInfo (playedFileOf (song, loop)).isValid());
    CHECK (phazite::getAudioFileInfo (playedFileOf (song, loop)).getSeconds() == doctest::Approx (4.8).epsilon (0.001));

    // Undone, it plays its file again; redone, its render, at once.
    phazite::undo (*edit);
    CHECK (phazite::getTempo (phazite::getSong (*edit)) == doctest::Approx (120.0));
    CHECK_FALSE (phazite::isRendered (song, loop));
    CHECK (playedSecondsOf (song, loop) == doctest::Approx (4.0));
    phazite::redo (*edit);
    CHECK (playedFileOf (song, loop) == render);
    CHECK (phazite::getAudioFileInfo (playedFileOf (song, loop)).isValid());

    // A render no clip plays goes when the cache is purged.
    phazite::undo (*edit);
    phazite::purgeClipRenders (song);
    CHECK_FALSE (render.existsAsFile());
}

TEST_CASE ("A project's renders are made in the background, and one that no clip needs any more stops and leaves nothing")
{
    phazite::test::TemporaryFolder temp;
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    auto& song = phazite::getSong (*edit);
    const auto track = phazite::getTrackID (*te::getAudioTracks (*edit)[0]);

    // A loop rendered in no time, and one so long that its render is still going when the tempo changes back (Tracktion
    // takes a tempo from a name for files of up to a minute).
    const auto shortLoop =
        phazite::importFile (song, track, writeClicks (temp.folder.getChildFile ("Short"), 4, 120.0), phazite::BeatPosition());
    const auto longLoop =
        phazite::importFile (song, track, writeClicks (temp.folder.getChildFile ("Long"), 112, 120.0), phazite::test::at (8.0));
    REQUIRE (isAudioClip (song, shortLoop));
    REQUIRE (isAudioClip (song, longLoop));
    phazite::ClipRenders renders (song);
    CHECK_FALSE (renders.isRendering());

    // A short while after the tempo last changed, both renders begin, each written beside where it goes until it is
    // whole; once the short one is made, its clip plays it.
    phazite::setTempo (song, 100.0);
    const auto longRender = playedFileOf (song, longLoop);
    REQUIRE (phazite::isRendered (song, longLoop));
    const auto isWritingLongRender = [&longRender]
    {
        return ! longRender.getParentDirectory()
                     .findChildFiles (juce::File::findFiles, false, longRender.getFileNameWithoutExtension() + "_*.part")
                     .isEmpty();
    };
    CHECK (renders.isRendering());
    CHECK (renders.getProgress().renders == 0); // not before the tempo has stayed put a moment
    REQUIRE (phazite::test::dispatchMessagesUntil ([&renders] { return renders.getProgress().renders > 0; }, 10000));
    CHECK (renders.getProgress().done < 1.0);

    // Made, and played: no longer among the renders being made, which the message thread hands to what plays them.
    const auto shortRender = playedFileOf (song, shortLoop);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&renders, &shortRender]
        { return phazite::getAudioFileInfo (shortRender).isValid() && ! renders.getProgressOf (shortRender).has_value(); }, 60000));
    REQUIRE (isWritingLongRender());

    // The short one counts as made, the long one as far as it has come.
    const auto halfway = renders.getProgress();
    CHECK (halfway.renders == 1);
    CHECK (halfway.done > 0.0);
    CHECK (halfway.done < 1.0);

    // Back at their own tempo the loops play their files, and the long render stops, leaving nothing, and no progress.
    phazite::setTempo (song, 120.0);
    CHECK_FALSE (renders.isRendering());
    CHECK (phazite::test::dispatchMessagesUntil ([&renders] { return renders.getProgress().renders == 0; }, 10000));
    CHECK (phazite::test::dispatchMessagesUntil ([&isWritingLongRender] { return ! isWritingLongRender(); }, 10000));
    CHECK_FALSE (longRender.existsAsFile());
    phazite::purgeClipRenders (song); // the short one's, which no clip plays now
}

TEST_CASE ("A loop stretched to the song's tempo plays every hit once and on its beat, as export renders it")
{
    // Known bug 1, which Tracktion's renders had: clips stretched by SoundTouch played 20 to 28 ms early, and doubled hits.
    phazite::test::TemporaryFolder temp;
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    auto& song = phazite::getSong (*edit);
    phazite::setTempo (song, 100.0);
    const auto loop = phazite::importFile (song, phazite::getTrackID (*te::getAudioTracks (*edit)[0]), writeClicks (temp.folder, 8, 120.0),
                                           phazite::BeatPosition());
    REQUIRE (isAudioClip (song, loop));
    REQUIRE (phazite::renderMissingClipAudio (song));

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

TEST_CASE ("A loop previewed in time plays stretched as its clip would be, every hit once and on its beat, round and round")
{
    // The browser's preview (model/engine/Preview.cpp), played by Phazite's engine for previews and stretched by Rubber
    // Band into memory as it starts (internal/PreviewLoop.h), which Tracktion stretched with SoundTouch as it played. A bar
    // at 44.1 kHz, played at 48: stretched and brought to the output's rate in one go. The tests before this one wrote two
    // bars at the same path, which Tracktion's knowledge of the file still had: the preview reads it again.
    phazite::test::EnginePlayback playback; // the previews' engine at 48 kHz too
    phazite::test::TemporaryFolder temp;
    const auto file = writeClicks (temp.folder, 4, 120.0);
    phazite::test::TestSong song;
    song.setTempo (100.0);

    auto& previews = phazite::getPlaybackHost (phazite::test::getEngineContext()).getPreviewEngine();
    const auto previewer = phazite::createPreviewer (phazite::test::getEngineContext());
    REQUIRE (previewer->play (file, song.context));
    REQUIRE (previewer->isInTime());
    REQUIRE (phazite::test::runTimersUntil ([&previews] { return previews.isPlaying(); }, 60000));

    // Played as a render plays, which waits for each part of the loop the render hasn't reached.
    const auto rate = phazite::test::EnginePlayback::sampleRate;
    const auto played = previews.render ((int) (6.0 * rate));
    previewer->stop();
    const std::vector<float> out (played.getReadPointer (0), played.getReadPointer (0) + played.getNumSamples());
    // From the first click, one on each beat at 100 BPM, twice round the bar, and nothing between.
    const auto start = std::find_if (out.begin(), out.end(), [] (float sample) { return std::abs (sample) > 0.01f; });
    REQUIRE (start != out.end());
    const auto beat = (int) std::lround (0.6 * rate), near = (int) (0.01 * rate);
    const auto first = findLoudestMillisecond (out.data(), (int) (start - out.begin()), (int) (start - out.begin()) + near).second;
    REQUIRE (first + 8 * beat < (int) out.size());

    for (int k = 1; k < 8; ++k)
    {
        CAPTURE (k);
        const auto expected = first + k * beat;
        const auto [level, at] = findLoudestMillisecond (out.data(), expected - near, expected + near);
        CHECK (std::abs ((at - expected) / rate * 1000.0) < 2.5);

        const auto between =
            findLoudestMillisecond (out.data(), expected - beat + (int) (0.02 * rate), expected - (int) (0.02 * rate)).first;
        CHECK (juce::Decibels::gainToDecibels (between / level) < -20.0);
    }
}

TEST_CASE ("Patterns follow the tempo, their notes on their beats, unless the song says they keep their speed")
{
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    auto& song = phazite::getSong (*edit);
    const auto clip = phazite::insertMidiClip (song, phazite::getTrackID (*te::getAudioTracks (*edit)[0]),
                                               { phazite::test::at (4.0), phazite::test::at (8.0) }, nullptr);
    REQUIRE (clip.isValid());
    std::vector<phazite::NoteRef> notes;

    for (int i = 0; i < 4; ++i)
        notes.push_back (phazite::test::addMidiNote (song, clip, 60, phazite::test::at (i), phazite::test::lasting (0.5), 100));

    phazite::getSong (*edit).getUndoManager().clearUndoHistory();

    // A note as the clip holds it, and its beat counted from the clip's content start, where its notes count from.
    const auto note = [&song, &notes] (int i)
    {
        const auto data = phazite::getNoteData (song, notes[(size_t) i]);
        REQUIRE (data.has_value());
        return *data;
    };
    const auto noteBeat = [&song, &note, clip] (int i)
    { return (note (i).start - phazite::getContentStart (phazite::findClipState (song, clip))).inBeats(); };

    // Following, they play twice as slowly at half the tempo.
    CHECK (phazite::doPatternsFollowTempo (phazite::getSong (*edit)));
    phazite::setTempo (song, 60.0);
    CHECK (noteBeat (3) == doctest::Approx (3.0));
    CHECK (phazite::getClipRange (song, clip).getLength().inBeats() == doctest::Approx (4.0));
    phazite::undo (*edit);

    // Keeping their speed, they keep their times: a note a second and a half into the clip stays there, half as many beats
    // in, and the clip, starting on the same beat, lasts as many seconds.
    {
        const phazite::UndoStep step (*edit, "Patterns Follow Tempo");
        phazite::togglePatternsFollowTempo (phazite::getSong (*edit));
    }

    CHECK_FALSE (phazite::doPatternsFollowTempo (phazite::getSong (*edit)));
    phazite::setTempo (song, 60.0);
    CHECK (noteBeat (3) == doctest::Approx (1.5));
    CHECK (note (3).length.inBeats() == doctest::Approx (0.25));
    const auto range = phazite::getClipRange (song, clip);
    CHECK (range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (range.getLength().inBeats() == doctest::Approx (2.0));
    CHECK (phazite::toSeconds (song, range.getEnd()) - phazite::toSeconds (song, range.getStart()) == doctest::Approx (2.0));

    phazite::undo (*edit);
    CHECK (noteBeat (3) == doctest::Approx (3.0));
    CHECK (phazite::getClipRange (song, clip).getLength().inBeats() == doctest::Approx (4.0));
}

TEST_CASE ("What a tempo change stretches: every sample follows it or none does, and patterns follow or keep their speed")
{
    phazite::test::TemporaryFolder temp;
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    auto& song = phazite::getSong (*edit);
    const auto track = phazite::getTrackID (*te::getAudioTracks (*edit)[0]);
    REQUIRE (phazite::importFile (song, track, writeClicks (temp.folder, 8, 120.0), phazite::BeatPosition()).isValid());
    REQUIRE (phazite::importFile (song, track, phazite::test::writeSound (temp.folder.getChildFile ("One-second hit.wav"), 1.0),
                                  phazite::test::at (10.0))
                 .isValid());
    const auto other = phazite::insertTrack (song, track, nullptr);
    REQUIRE (other.isValid());
    REQUIRE (phazite::insertMidiClip (song, other, { phazite::BeatPosition(), phazite::test::at (4.0) }, nullptr).isValid());

    const auto stretch = phazite::getTempoStretch (song);
    CHECK (stretch.samples == 2);
    CHECK (stretch.samplesFollowing == 1);
    CHECK (stretch.patterns == 1);
    CHECK (stretch.patternsFollow);
    CHECK_FALSE (phazite::doAllSamplesFollowTempo (song));

    // Some follow: all of them do.
    phazite::toggleSamplesFollowTempo (song);
    CHECK (phazite::getTempoStretch (song).samplesFollowing == 2);
    CHECK (phazite::doAllSamplesFollowTempo (song));

    // All of them did: none does.
    phazite::toggleSamplesFollowTempo (song);
    CHECK (phazite::getTempoStretch (song).samplesFollowing == 0);

    phazite::togglePatternsFollowTempo (phazite::getSong (*edit));
    CHECK_FALSE (phazite::getTempoStretch (song).patternsFollow);
}
