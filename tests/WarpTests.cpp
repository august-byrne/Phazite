// SPDX-License-Identifier: AGPL-3.0-or-later
// Warping audio clips (src/model/arrangement/AudioClipModel.h, internal/AudioClips.h; docs/recording-audit.md), as Ableton
// Live's: warp markers pin a time of a clip's file to a beat, the audio between them stretched to fit; the hits in a file,
// found in the background; and Quantize Audio, which moves the hits a clip plays to the grid through markers.
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Clips.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/AudioFiles.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <cmath>

namespace
{
constexpr double rate = 44100.0;

// Drum hits at times in a file of so many seconds, each noise falling away over 30 ms: what Rubber Band keeps on its beat.
juce::File writeHitsAt (const juce::File& file, const std::vector<double>& times, double seconds)
{
    juce::AudioBuffer<float> audio (1, (int) std::lround (seconds * rate));
    audio.clear();
    juce::Random random (5);

    for (const auto time : times)
        for (int i = 0, first = (int) std::lround (time * rate); i < (int) (0.2 * rate) && first + i < audio.getNumSamples(); ++i)
            audio.setSample (0, first + i, 0.5f * std::exp ((float) -i / (float) (0.03 * rate)) * (random.nextFloat() * 2.0f - 1.0f));

    file.getParentDirectory().createDirectory();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    auto writer = juce::WavAudioFormat().createWriterFor (
        stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
    REQUIRE (writer != nullptr);
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

// How far, in milliseconds, the hit at a time of the file plays from where it should in the song as Phazite's engine renders
// it: the lag within 10 ms of there that matches the hit's first 10 ms best (stretched, its loudest millisecond could be any).
double getHitError (const juce::AudioBuffer<float>& render, const juce::File& file, double hitTime, double expectedSeconds)
{
    const std::unique_ptr<juce::AudioFormatReader> reader (phazite::getAudioFormats().createReaderFor (file));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> sound (1, (int) reader->lengthInSamples);
    reader->read (&sound, 0, sound.getNumSamples(), 0, true, false);

    constexpr int length = 441, near = 441;
    const auto expected = (int) std::lround (expectedSeconds * rate);
    const auto* hit = sound.getReadPointer (0, (int) std::lround (hitTime * rate));
    const auto* out = render.getReadPointer (0);
    auto best = 0.0;
    auto at = expected;

    for (auto lag = std::max (0, expected - near); lag <= expected + near && lag + length <= render.getNumSamples(); ++lag)
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

    REQUIRE (best > 0.0);
    return (at - expected) / rate * 1000.0;
}

// A clip on the song's first track, from its start, playing a file of hits at a quarter of a second past each half second
// but the first, at 120 BPM (a beat is half a second), following the song's tempo.
struct WarpSong
{
    explicit WarpSong (std::vector<double> at = { 0.0, 0.5, 1.0, 1.5 })
        : hits (std::move (at))
    {
        song.setTempo (120.0);
        file = writeHitsAt (song.getSoundsFolder().getChildFile ("Hits.wav"), hits, 2.0);
        clip = phazite::test::addAudioClip (getSong(), song.firstTrack, "Hits", file, 0.0, 2.0);
        REQUIRE (clip.isValid());
        getSong().getUndoManager().clearUndoHistory();
    }

    phazite::Song& getSong() const { return *phazite::findSong (song.context); }
    juce::String getUndoName() const { return getSong().getUndoManager().getUndoDescription(); }
    double beatOf (double fileTime) const { return phazite::getBeatOfFileTime (getSong(), clip, fileTime).inBeats(); }

    // The song rendered through Phazite's engine from its start, its clips' renders made first.
    juce::AudioBuffer<float> render (double seconds) const
    {
        REQUIRE (phazite::renderMissingClipAudio (getSong()));
        return phazite::renderWithOwnEngine (getSong(), (int) std::lround (seconds * rate), rate, 512);
    }

    phazite::test::TestSong song;
    const std::vector<double> hits;
    juce::File file;
    phazite::ClipID clip;
};
} // namespace

TEST_CASE ("A warp marker added where its audio plays moves nothing; moved, the audio between it and the clip's start stretches to fit")
{
    WarpSong warp;
    auto& context = warp.song.context;

    // A marker on the hit at 1 s, which plays on beat 2: the clip follows the tempo from then on, and nothing moves.
    REQUIRE (phazite::addWarpMarker (context, warp.clip, 1.0));
    CHECK (warp.getUndoName() == juce::String ("Add Warp Marker"));
    CHECK (phazite::getClipTempo (warp.getSong(), warp.clip) == doctest::Approx (120.0));
    auto markers = phazite::getWarpMarkers (context, warp.clip);
    REQUIRE (markers.size() == 1);
    CHECK (markers[0].time == doctest::Approx (1.0));
    CHECK (markers[0].beat.inBeats() == doctest::Approx (2.0));
    CHECK (warp.beatOf (1.5) == doctest::Approx (3.0));
    CHECK (phazite::getClipRange (warp.getSong(), warp.clip).getEnd().inBeats() == doctest::Approx (4.0));
    CHECK_FALSE (phazite::addWarpMarker (context, warp.clip, 1.005)); // within 10 ms of it
    CHECK_FALSE (phazite::addWarpMarker (context, warp.clip, 2.5));   // past the file's end

    // Moved to beat 2.5: the clip's start holds a marker of its own, the hit at half a second stretches to beat 1.25, the one
    // after the marker plays straight on, a beat later, and the clip ends half a beat later.
    phazite::moveWarpMarker (context, warp.clip, 1.0, phazite::test::at (2.5));
    CHECK (warp.getUndoName() == juce::String ("Move Warp Marker"));
    markers = phazite::getWarpMarkers (context, warp.clip);
    REQUIRE (markers.size() == 2);
    CHECK (markers[0].time == doctest::Approx (0.0));
    CHECK (markers[0].beat.inBeats() == doctest::Approx (0.0));
    CHECK (markers[1].beat.inBeats() == doctest::Approx (2.5));
    CHECK (warp.beatOf (0.5) == doctest::Approx (1.25));
    CHECK (warp.beatOf (1.5) == doctest::Approx (3.5));
    CHECK (phazite::getClipRange (warp.getSong(), warp.clip).getStart().inBeats() == doctest::Approx (0.0));
    CHECK (phazite::getClipRange (warp.getSong(), warp.clip).getEnd().inBeats() == doctest::Approx (4.5));
    CHECK (phazite::isRendered (warp.getSong(), warp.clip));

    // Played through Phazite's engine, each hit where it plays now, give or take what Rubber Band moves an attack by.
    const auto render = warp.render (2.6);

    for (const auto& [time, beat] : { std::pair { 0.5, 1.25 }, std::pair { 1.0, 2.5 }, std::pair { 1.5, 3.5 } })
    {
        CAPTURE (time);
        CHECK (std::abs (getHitError (render, warp.file, time, beat * 0.5)) < 2.5);
    }

    // Undone, it plays straight again, and a marker can't move past its neighbours.
    phazite::undo (warp.getSong());
    CHECK (warp.beatOf (0.5) == doctest::Approx (1.0));
    phazite::moveWarpMarker (context, warp.clip, 1.0, phazite::test::at (-3.0));
    CHECK (warp.beatOf (1.0) > 0.0);
}

TEST_CASE ("A clip's warp markers go when it stops following the tempo, and stay when its tempo changes")
{
    WarpSong warp;
    auto& context = warp.song.context;
    REQUIRE (phazite::addWarpMarker (context, warp.clip, 1.0));
    phazite::moveWarpMarker (context, warp.clip, 1.0, phazite::test::at (2.5));

    // Played at half the tempo, its audio plays twice as fast at the song's: half as long, its markers half as far from its
    // start in beats.
    phazite::setClipTempo (context, warp.clip, 60.0);
    CHECK (warp.beatOf (1.0) == doctest::Approx (1.25));
    CHECK (phazite::getClipRange (warp.getSong(), warp.clip).getEnd().inBeats() == doctest::Approx (2.25));

    phazite::setClipFollowsTempo (context, warp.clip, false);
    CHECK (phazite::getWarpMarkers (context, warp.clip).empty());
    CHECK (warp.beatOf (1.0) == doctest::Approx (2.0)); // at its own speed
    phazite::undo (warp.getSong());
    CHECK (phazite::getWarpMarkers (context, warp.clip).size() == 2);

    phazite::removeWarpMarkers (context, warp.clip);
    CHECK (warp.getUndoName() == juce::String ("Remove Warp Markers"));
    CHECK (phazite::getWarpMarkers (context, warp.clip).empty());
    CHECK (warp.beatOf (1.0) == doctest::Approx (1.0)); // straight, at 60 BPM
}

TEST_CASE ("The hits in a clip's audio are found in the background, each with where it plays")
{
    WarpSong warp;
    std::optional<std::vector<phazite::WarpMarker>> hits;
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            hits = phazite::getTransients (warp.song.context, warp.clip);
            return hits.has_value();
        },
        30000));

    REQUIRE (hits->size() == 4);

    for (size_t i = 0; i < hits->size(); ++i)
    {
        CAPTURE (i);
        CHECK ((*hits)[i].time == doctest::Approx (warp.hits[i]).epsilon (0.002));
        CHECK ((*hits)[i].beat.inBeats() == doctest::Approx (warp.hits[i] * 2.0).epsilon (0.004));
    }
}

TEST_CASE ("Quantize Audio moves each hit a clip plays towards the grid line nearest it, through warp markers, as one undo step")
{
    // Hits a little early and late of beats 1, 2 and 3, on a clip that doesn't follow the tempo yet.
    WarpSong warp ({ 0.0, 0.53, 0.97, 1.54 });
    auto& context = warp.song.context;
    REQUIRE (phazite::canQuantizeAudio (context, { warp.clip }));

    phazite::quantizeAudio (context, { warp.clip }, phazite::test::lasting (1.0));
    CHECK (warp.getUndoName() == juce::String ("Quantize Audio"));
    CHECK (phazite::getClipTempo (warp.getSong(), warp.clip) == doctest::Approx (120.0));
    const auto markers = phazite::getWarpMarkers (context, warp.clip);
    REQUIRE (markers.size() == 4);

    for (size_t i = 0; i < markers.size(); ++i)
    {
        CAPTURE (i);
        CHECK (markers[i].beat.inBeats() == doctest::Approx ((double) i));
    }

    const auto render = warp.render (2.2);

    for (int beat = 1; beat <= 3; ++beat)
    {
        CAPTURE (beat);
        CHECK (std::abs (getHitError (render, warp.file, warp.hits[(size_t) beat], beat * 0.5)) < 2.5);
    }

    // Half way: each hit half as far from its line as it was.
    phazite::undo (warp.getSong());
    CHECK (phazite::getWarpMarkers (context, warp.clip).empty());
    phazite::quantizeAudio (context, { warp.clip }, phazite::test::lasting (1.0), 0.5);
    const auto halfway = phazite::getWarpMarkers (context, warp.clip);
    REQUIRE (halfway.size() == 4);
    CHECK (halfway[1].beat.inBeats() == doctest::Approx (1.03).epsilon (0.002)); // from 1.06
    CHECK (halfway[2].beat.inBeats() == doctest::Approx (1.97).epsilon (0.002)); // from 1.94
}
