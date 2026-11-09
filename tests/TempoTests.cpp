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
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/Preview.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <cmath>

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

// A loop of drum hits, one a beat, each noise falling away over 30 ms, its tempo in its name: what Rubber Band keeps on its
// beat when it stretches it to twice as long, where it puts writeClicks()'s 2 ms clicks up to 8 ms early, at one tempo as
// along tempo changes.
juce::File writeHits (const juce::File& folder, int beats, double bpm)
{
    constexpr double sampleRate = 44100.0;
    const auto beatSamples = (int) std::lround (60.0 / bpm * sampleRate);
    juce::AudioBuffer<float> audio (1, beats * beatSamples);
    audio.clear();
    juce::Random random (5);

    for (int beat = 0; beat < beats; ++beat)
        for (int i = 0; i < std::min (beatSamples, (int) (0.24 * sampleRate)); ++i)
            audio.setSample (0, beat * beatSamples + i,
                             0.5f * std::exp ((float) -i / (float) (0.03 * sampleRate)) * (random.nextFloat() * 2.0f - 1.0f));

    const auto file = folder.getChildFile ("Hits " + juce::String (bpm, 0) + " BPM.wav");
    folder.createDirectory();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}
} // namespace

TEST_CASE ("The tempo changes as one undo step, within the song's limits, and clips stay on their beats")
{
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    const auto track = phazite::getTrackIDs (song)[0];
    const auto clip = phazite::insertMidiClip (song, track, { phazite::test::at (8.0), phazite::test::at (12.0) }, nullptr);
    REQUIRE (clip.isValid());
    song.getUndoManager().clearUndoHistory();
    CHECK (phazite::getTempo (song) == doctest::Approx (120.0));

    phazite::setTempo (song, 90.004);
    CHECK (phazite::getTempo (song) == doctest::Approx (90.0));
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Change Tempo"));
    CHECK (phazite::getClipRange (song, clip).getStart().inBeats() == doctest::Approx (8.0));
    CHECK (phazite::toSeconds (song, phazite::getClipRange (song, clip).getStart()) == doctest::Approx (8.0 * 60.0 / 90.0));

    phazite::setTempo (song, 1000.0);
    CHECK (phazite::getTempo (song) == doctest::Approx (phazite::maxTempo));
    phazite::setTempo (song, 1.0);
    CHECK (phazite::getTempo (song) == doctest::Approx (phazite::minTempo));

    phazite::undo (song);
    phazite::undo (song);
    CHECK (phazite::getTempo (song) == doctest::Approx (90.0));

    // The same tempo again changes nothing.
    song.getUndoManager().clearUndoHistory();
    phazite::setTempo (song, 90.0);
    CHECK_FALSE (song.getUndoManager().canUndo());
}

TEST_CASE ("The time signature changes as one undo step, to one Phazite takes")
{
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    song.getUndoManager().clearUndoHistory();
    CHECK (phazite::getTimeSignature (song) == phazite::TimeSignature { 4, 4 });

    phazite::setTimeSignature (song, { 6, 8 });
    CHECK (phazite::getTimeSignature (song) == phazite::TimeSignature { 6, 8 });
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Change Time Signature"));

    // Not 0 beats, nor a beat of a third of a note.
    for (const auto invalid : { phazite::TimeSignature { 0, 4 }, phazite::TimeSignature { 4, 3 }, phazite::TimeSignature { 33, 4 } })
    {
        CHECK_FALSE (phazite::isValid (invalid));
        phazite::setTimeSignature (song, invalid);
        CHECK (phazite::getTimeSignature (song) == phazite::TimeSignature { 6, 8 });
    }

    phazite::undo (song);
    CHECK (phazite::getTimeSignature (song) == phazite::TimeSignature { 4, 4 });
}

TEST_CASE ("Every project's song has a tempo map, whose tempo and time signature undo and redo put back")
{
    phazite::test::TestSong song;
    auto& state = *phazite::findSong (song.context);

    // Every project has one, at 120 BPM in 4/4 from the song's start.
    const auto map = state.getState().getChildWithName (phazite::schema::TEMPOMAP);
    REQUIRE (map.isValid());
    CHECK ((double) map.getChildWithName (phazite::schema::TEMPO)[phazite::schema::bpm] == doctest::Approx (120.0));
    CHECK ((int) map.getChildWithName (phazite::schema::TIMESIGNATURE)[phazite::schema::numerator] == 4);

    // A change goes into the map; an undo puts it back, and a redo makes it again.
    phazite::setTempo (state, 96.5);
    phazite::setTimeSignature (state, { 7, 8 });
    CHECK ((double) map.getChildWithName (phazite::schema::TEMPO)[phazite::schema::bpm] == doctest::Approx (96.5));
    CHECK ((int) map.getChildWithName (phazite::schema::TIMESIGNATURE)[phazite::schema::numerator] == 7);
    CHECK (phazite::getTimeSignature (state) == phazite::TimeSignature { 7, 8 });

    phazite::undo (state);
    CHECK (phazite::getTimeSignature (state) == phazite::TimeSignature { 4, 4 });
    phazite::undo (state);
    CHECK (phazite::getTempo (state) == doctest::Approx (120.0));
    phazite::redo (state);
    CHECK (phazite::getTempo (state) == doctest::Approx (96.5));
}

TEST_CASE ("A tempo change stretches the samples that follow it, which Phazite renders, and leaves the others at their speed")
{
    phazite::test::TemporaryFolder temp;
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    const auto track = phazite::getTrackIDs (song)[0];

    // A loop that knows its tempo follows the song's; a sound that doesn't plays at its own speed.
    const auto loop = phazite::importFile (song, track, writeClicks (temp.folder, 8, 120.0), phazite::BeatPosition());
    const auto sound = phazite::importFile (song, track, phazite::test::writeSound (temp.folder.getChildFile ("One-second hit.wav"), 1.0),
                                            phazite::test::at (10.0));
    REQUIRE (isAudioClip (song, loop));
    REQUIRE (isAudioClip (song, sound));
    CHECK (phazite::getClipTempo (song, loop) == doctest::Approx (120.0));
    CHECK_FALSE (phazite::isRendered (song, loop)); // at its own tempo it plays its file
    CHECK (phazite::getClipTempo (song, sound) == doctest::Approx (0.0));
    song.getUndoManager().clearUndoHistory();

    phazite::setTempo (song, 100.0);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Change Tempo"));

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
    phazite::undo (song);
    CHECK (phazite::getTempo (song) == doctest::Approx (120.0));
    CHECK_FALSE (phazite::isRendered (song, loop));
    CHECK (playedSecondsOf (song, loop) == doctest::Approx (4.0));
    phazite::redo (song);
    CHECK (playedFileOf (song, loop) == render);
    CHECK (phazite::getAudioFileInfo (playedFileOf (song, loop)).isValid());

    // A render no clip plays goes when the cache is purged.
    phazite::undo (song);
    phazite::purgeClipRenders (song);
    CHECK_FALSE (render.existsAsFile());
}

TEST_CASE ("A project's renders are made in the background, and one that no clip needs any more stops and leaves nothing")
{
    phazite::test::TemporaryFolder temp;
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    const auto track = phazite::getTrackIDs (song)[0];

    // A loop rendered in no time, and one so long that its render is still going when the tempo changes back (a tempo in
    // a file's name counts for files of up to a minute: model/engine/AudioFiles.h).
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
    // Stretched clips must neither play early nor double their hits.
    phazite::test::TemporaryFolder temp;
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    phazite::setTempo (song, 100.0);
    const auto loop =
        phazite::importFile (song, phazite::getTrackIDs (song)[0], writeClicks (temp.folder, 8, 120.0), phazite::BeatPosition());
    REQUIRE (isAudioClip (song, loop));
    REQUIRE (phazite::renderMissingClipAudio (song));

    // The song from its start to its end, as an export renders it.
    constexpr double rate = 44100.0;
    const auto seconds = phazite::toSeconds (song, phazite::getSongEnd (song));
    REQUIRE (seconds == doctest::Approx (4.8));
    const auto audio = phazite::renderWithOwnEngine (song, (int) std::lround (seconds * rate), rate, 512);
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
    // Band into memory as it starts (internal/PreviewLoop.h). A bar at 44.1 kHz, played at 48: stretched and brought to the
    // output's rate in one go. The tests before this one wrote two bars at the same path: the preview reads it again.
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
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    const auto clip =
        phazite::insertMidiClip (song, phazite::getTrackIDs (song)[0], { phazite::test::at (4.0), phazite::test::at (8.0) }, nullptr);
    REQUIRE (clip.isValid());
    std::vector<phazite::NoteRef> notes;

    for (int i = 0; i < 4; ++i)
        notes.push_back (phazite::test::addMidiNote (song, clip, 60, phazite::test::at (i), phazite::test::lasting (0.5), 100));

    song.getUndoManager().clearUndoHistory();

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
    CHECK (phazite::doPatternsFollowTempo (song));
    phazite::setTempo (song, 60.0);
    CHECK (noteBeat (3) == doctest::Approx (3.0));
    CHECK (phazite::getClipRange (song, clip).getLength().inBeats() == doctest::Approx (4.0));
    phazite::undo (song);

    // Keeping their speed, they keep their times: a note a second and a half into the clip stays there, half as many beats
    // in, and the clip, starting on the same beat, lasts as many seconds.
    {
        const phazite::UndoStep step (song, "Patterns Follow Tempo");
        phazite::togglePatternsFollowTempo (song);
    }

    CHECK_FALSE (phazite::doPatternsFollowTempo (song));
    phazite::setTempo (song, 60.0);
    CHECK (noteBeat (3) == doctest::Approx (1.5));
    CHECK (note (3).length.inBeats() == doctest::Approx (0.25));
    const auto range = phazite::getClipRange (song, clip);
    CHECK (range.getStart().inBeats() == doctest::Approx (4.0));
    CHECK (range.getLength().inBeats() == doctest::Approx (2.0));
    CHECK (phazite::toSeconds (song, range.getEnd()) - phazite::toSeconds (song, range.getStart()) == doctest::Approx (2.0));

    phazite::undo (song);
    CHECK (noteBeat (3) == doctest::Approx (3.0));
    CHECK (phazite::getClipRange (song, clip).getLength().inBeats() == doctest::Approx (4.0));
}

TEST_CASE ("What a tempo change stretches: every sample follows it or none does, and patterns follow or keep their speed")
{
    phazite::test::TemporaryFolder temp;
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    const auto track = phazite::getTrackIDs (song)[0];
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

    phazite::togglePatternsFollowTempo (song);
    CHECK_FALSE (phazite::getTempoStretch (song).patternsFollow);
}

TEST_CASE ("Tempo changes along the song: added, moved, ramped, bent and deleted, each a named undo step, the first at the start")
{
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    phazite::setTempo (song, 100.0);
    song.getUndoManager().clearUndoHistory();
    CHECK_FALSE (phazite::hasTempoChanges (song));

    // Added on the line where it goes, nothing changes until it moves: a step, as tempo changes start.
    CHECK (phazite::addTempoChange (song, phazite::test::at (8.0)) == 1);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Add Tempo Change"));
    CHECK (phazite::hasTempoChanges (song));
    CHECK (phazite::getTempoChanges (song)[1].bpm == doctest::Approx (100.0));
    CHECK (phazite::getTempoChanges (song)[1].curve == doctest::Approx (phazite::tempoJump));

    // Moved, it changes the tempo from there; the first stays at the song's start, its tempo changed.
    CHECK (phazite::moveTempoChange (song, 1, phazite::test::at (4.0), 140.0) == 1);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Move Tempo Change"));
    CHECK (phazite::getTempoAt (song, phazite::test::at (3.9)) == doctest::Approx (100.0));
    CHECK (phazite::getTempoAt (song, phazite::test::at (4.0)) == doctest::Approx (140.0));
    CHECK (phazite::moveTempoChange (song, 0, phazite::test::at (2.0), 90.0) == 0);
    CHECK (phazite::getTempoChanges (song)[0].beat.inBeats() == doctest::Approx (0.0));
    CHECK (phazite::getTempo (song) == doctest::Approx (90.0));
    CHECK (phazite::toSeconds (song, phazite::test::at (4.0)) == doctest::Approx (4.0 * 60.0 / 90.0));

    // Ramped, the tempo goes straight from one to the next, and the beats take the time it gives them.
    phazite::setTempoCurve (song, 0, phazite::tempoRamp);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Change Tempo Ramp"));
    CHECK (phazite::getTempoAt (song, phazite::test::at (2.0)) == doctest::Approx (115.0));
    CHECK (phazite::toSeconds (song, phazite::test::at (4.0)) == doctest::Approx (4.0 * 60.0 / 50.0 * std::log (140.0 / 90.0)));
    const auto values = phazite::getTempoValues (song, { phazite::BeatPosition(), phazite::test::at (8.0) }, 5);
    REQUIRE (values.size() == 5);
    CHECK (values[0] == doctest::Approx (90.0));
    CHECK (values[1] == doctest::Approx (115.0));
    CHECK (values[4] == doctest::Approx (140.0));

    // One added on a ramp ramps on from it, at the ramp's tempo there.
    CHECK (phazite::addTempoChange (song, phazite::test::at (1.0)) == 1);
    CHECK (phazite::getTempoChanges (song)[1].bpm == doctest::Approx (102.5));
    CHECK (phazite::getTempoChanges (song)[1].curve == doctest::Approx (phazite::tempoRamp));
    CHECK (phazite::getTempoAt (song, phazite::test::at (2.0)) == doctest::Approx (115.0));

    // A tempo already at a beat takes the one given; the top bar's change sets the tempo in effect where it is.
    CHECK (phazite::addTempoChange (song, phazite::test::at (1.0), 110.0) == 1);
    CHECK (phazite::getTempoChanges (song).size() == 3);
    CHECK (phazite::getTempoChanges (song)[1].bpm == doctest::Approx (110.0));
    phazite::setTempoAt (song, phazite::test::at (6.0), 150.0);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Change Tempo"));
    CHECK (phazite::getTempoChanges (song)[2].bpm == doctest::Approx (150.0));
    CHECK (phazite::getTempo (song) == doctest::Approx (90.0));

    // Within the limits, to a hundredth, and curves within -1 and 1.
    phazite::moveTempoChange (song, 2, phazite::test::at (4.0), 1000.0);
    CHECK (phazite::getTempoChanges (song)[2].bpm == doctest::Approx (phazite::maxTempo));
    phazite::setTempoCurve (song, 1, 0.3f);
    CHECK (phazite::getTempoChanges (song)[1].curve == doctest::Approx (0.3f));
    phazite::setTempoCurve (song, 1, 5.0f);
    CHECK (phazite::getTempoChanges (song)[1].curve == doctest::Approx (1.0f));

    // Deleted: never the first; and all of them removed, the first tempo goes on all the way, a step again.
    phazite::deleteTempoChange (song, 0);
    CHECK (phazite::getTempoChanges (song).size() == 3);
    phazite::deleteTempoChange (song, 1);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Delete Tempo Change"));
    CHECK (phazite::getTempoChanges (song).size() == 2);
    phazite::removeTempoChanges (song);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Remove Tempo Changes"));
    CHECK_FALSE (phazite::hasTempoChanges (song));
    CHECK (phazite::getTempoChanges (song)[0].curve == doctest::Approx (phazite::tempoJump));
    CHECK (phazite::getTempoAt (song, phazite::test::at (100.0)) == doctest::Approx (90.0));

    // Undone, they come back.
    phazite::undo (song);
    CHECK (phazite::getTempoChanges (song).size() == 2);
    CHECK (phazite::getTempoChanges (song)[0].curve == doctest::Approx (phazite::tempoRamp));
}

TEST_CASE ("The tempo commands add a tempo change where the playhead is, and take them all away")
{
    phazite::test::TestSong song;
    auto& state = *phazite::findSong (song.context);
    song.setTempo (100.0);
    CHECK (phazite::CommandRegistry::isEnabled (*song.commands.find ("transport.addTempoChange")));
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*song.commands.find ("transport.removeTempoChanges")));

    phazite::movePlayhead (state, phazite::test::at (16.0));
    song.run ("transport.addTempoChange");
    REQUIRE (phazite::getTempoChanges (state).size() == 2);
    CHECK (phazite::getTempoChanges (state)[1].beat.inBeats() == doctest::Approx (16.0));
    CHECK (phazite::getTempoChanges (state)[1].bpm == doctest::Approx (100.0));
    CHECK (phazite::CommandRegistry::isEnabled (*song.commands.find ("transport.removeTempoChanges")));

    // The top bar's tempo is the one where the playhead is, and changing it changes that one.
    auto tempo = phazite::getTempoHandle (song.context);
    tempo.setValue (128.0);
    CHECK (phazite::getTempoChanges (state)[1].bpm == doctest::Approx (128.0));
    CHECK (phazite::getTempo (state) == doctest::Approx (100.0));
    CHECK (tempo.getValue() == doctest::Approx (128.0));
    phazite::movePlayhead (state, phazite::test::at (4.0));
    CHECK (tempo.getValue() == doctest::Approx (100.0));

    song.run ("transport.removeTempoChanges");
    CHECK_FALSE (phazite::hasTempoChanges (state));
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*song.commands.find ("transport.removeTempoChanges")));
}

TEST_CASE ("A loop that follows the tempo plays along the song's tempo changes, every hit on its beat, through a ramp and a step")
{
    phazite::test::TemporaryFolder temp;
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    const auto loop =
        phazite::importFile (song, phazite::getTrackIDs (song)[0], writeHits (temp.folder, 8, 120.0), phazite::BeatPosition());
    REQUIRE (isAudioClip (song, loop));
    CHECK (phazite::getClipTempo (song, loop) == doctest::Approx (120.0));
    CHECK_FALSE (phazite::followsTempoChanges (song, phazite::findClipState (song, loop)));

    // A ramp from 120 down to 100 BPM over its first four beats (a ritardando), 100 held, and a step down to 90 at beat 6.
    // (StretchTests.cpp holds a ramp that doubles a stretch, with hits close together; on its own after silence, a hit
    // stretched to twice as long comes out 5 to 8 ms early from Rubber Band, at one tempo as along changes.)
    phazite::takeTempoChanges (
        song,
        { { phazite::BeatPosition(), 120.0, phazite::tempoRamp }, { phazite::test::at (4.0), 100.0 }, { phazite::test::at (6.0), 90.0 } },
        phazite::BeatPosition());
    const auto clipState = phazite::findClipState (song, loop);
    REQUIRE (phazite::followsTempoChanges (song, clipState));

    // It keeps its eight beats, which last as long as the tempo says (the ramp's four 12 ln 1.2 seconds), and each of its
    // file's seconds is two of its beats.
    CHECK (phazite::getClipRange (song, loop).getLength().inBeats() == doctest::Approx (8.0));
    const auto position = phazite::getPlayedPosition (song, clipState);
    const auto end = phazite::toSeconds (song, phazite::test::at (8.0));
    CHECK (end == doctest::Approx (12.0 * std::log (1.2) + 2.0 * 0.6 + 2.0 * 60.0 / 90.0));
    CHECK (position.start == doctest::Approx (0.0));
    CHECK (position.end == doctest::Approx (end));
    CHECK (position.offset == doctest::Approx (0.0));
    CHECK (phazite::getFileTimeAt (song, loop, phazite::test::at (4.0)) == doctest::Approx (2.0));
    CHECK (phazite::getFileTimeAt (song, loop, phazite::test::at (7.0)) == doctest::Approx (3.5));

    const auto settings = phazite::getClipStretch (song, clipState);
    REQUIRE (settings.timeMap.size() >= 8);
    CHECK (settings.timeMap.back().first == doctest::Approx (4.0));
    CHECK (settings.timeMap.back().second == doctest::Approx (end));
    CHECK (settings.timeRatio == doctest::Approx (end / 4.0));
    REQUIRE (phazite::isRendered (song, loop));

    // Trimmed, it plays the file from its beat, and back.
    phazite::trimClipStart (song, loop, phazite::test::at (2.0));
    CHECK (phazite::getFilePart (phazite::findClipState (song, loop)).start == doctest::Approx (1.0));
    CHECK (phazite::getClipRange (song, loop).getEnd().inBeats() == doctest::Approx (8.0));
    phazite::trimClipStart (song, loop, phazite::BeatPosition());
    CHECK (phazite::getFilePart (phazite::findClipState (song, loop)).start == doctest::Approx (0.0));

    // Rendered through Phazite's engine, as an export plays it: each hit where its beat is, give or take what Rubber Band
    // moves an attack by, as at one tempo. Each is found by its first 10 ms, the lag that matches them best within 10 ms of
    // its beat: stretched, a hit's loudest millisecond could be any of them.
    REQUIRE (phazite::renderMissingClipAudio (song));
    constexpr double rate = 44100.0;
    const auto audio = phazite::renderWithOwnEngine (song, (int) std::lround ((end + 0.2) * rate), rate, 512);
    const auto* out = audio.getReadPointer (0);
    const std::unique_ptr<juce::AudioFormatReader> reader (
        phazite::getAudioFormats().createReaderFor (phazite::getClipFile (song, phazite::findClipState (song, loop))));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> hits (1, (int) reader->lengthInSamples);
    reader->read (&hits, 0, hits.getNumSamples(), 0, true, false);
    constexpr int length = 441, near = 441;

    for (int k = 1; k < 8; ++k)
    {
        CAPTURE (k);
        const auto expected = (int) std::lround (phazite::toSeconds (song, phazite::test::at (k)) * rate);
        const auto* hit = hits.getReadPointer (0, k * 22050);
        auto best = 0.0;
        auto at = expected;

        for (auto lag = expected - near; lag <= expected + near && lag + length <= audio.getNumSamples(); ++lag)
        {
            auto dot = 0.0;

            for (int i = 0; i < length; ++i)
                dot += (double) out[lag + i] * hit[i];

            if (dot > best)
            {
                best = dot;
                at = lag;
            }
        }

        CHECK (best > 0.0);
        CHECK (std::abs ((at - expected) / rate * 1000.0) < 1.5);
    }

    // Back to one tempo, it plays at one ratio again.
    phazite::removeTempoChanges (song);
    CHECK_FALSE (phazite::followsTempoChanges (song, phazite::findClipState (song, loop)));
    CHECK (phazite::getClipStretch (song, loop).timeMap.empty());
    CHECK_FALSE (phazite::isRendered (song, loop));
}

TEST_CASE ("A MIDI file's tempo changes come into a song without clips; a song with clips keeps its tempo")
{
    phazite::test::TemporaryFolder temp;

    // 120 BPM, then 60 from beat 4, then 90 from beat 6; a note over eight beats.
    juce::MidiMessageSequence events;
    events.addEvent (juce::MidiMessage::tempoMetaEvent (500000), 0.0);
    events.addEvent (juce::MidiMessage::tempoMetaEvent (1000000), 4.0 * 960.0);
    events.addEvent (juce::MidiMessage::tempoMetaEvent (666667), 6.0 * 960.0);
    events.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0.0);
    events.addEvent (juce::MidiMessage::noteOff (1, 60), 8.0 * 960.0);
    events.updateMatchedPairs();
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (960);
    midi.addTrack (events);
    const auto file = temp.folder.getChildFile ("Tempo changes.mid");

    {
        juce::FileOutputStream out (file);
        REQUIRE (out.openedOk());
        REQUIRE (midi.writeTo (out));
    }

    // Into a song without clips, from where it is dropped.
    const auto emptyPtr = phazite::createSong();
    auto& empty = *emptyPtr;
    REQUIRE (phazite::importFile (empty, phazite::getTrackIDs (empty)[0], file, phazite::test::at (4.0)).isValid());
    const auto changes = phazite::getTempoChanges (empty);
    REQUIRE (changes.size() == 3);
    CHECK (changes[0].bpm == doctest::Approx (120.0));
    CHECK (changes[1].beat.inBeats() == doctest::Approx (8.0));
    CHECK (changes[1].bpm == doctest::Approx (60.0));
    CHECK (changes[2].beat.inBeats() == doctest::Approx (10.0));
    CHECK (changes[2].bpm == doctest::Approx (90.0).epsilon (0.001));
    CHECK (changes[1].curve == doctest::Approx (phazite::tempoJump));

    // A song that has clips already keeps its tempo.
    const auto songPtr = phazite::createSong();
    auto& song = *songPtr;
    const auto track = phazite::getTrackIDs (song)[0];
    REQUIRE (phazite::insertMidiClip (song, track, { phazite::BeatPosition(), phazite::test::at (4.0) }, nullptr).isValid());
    REQUIRE (phazite::importFile (song, track, file, phazite::test::at (8.0)).isValid());
    CHECK_FALSE (phazite::hasTempoChanges (song));
}
