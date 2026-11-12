// SPDX-License-Identifier: AGPL-3.0-or-later
// The waveforms of audio clips (src/app/ClipWaveforms.h), from Phazite's own cache of their files' levels: one loads in
// the background and draws where the clip's file has audio, nothing past it; a second clip of the same file draws at once.
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "app/ClipWaveforms.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

namespace
{
// Writes a sound of silence but for a burst at full scale from one time to another, in seconds.
juce::File writeBurst (const juce::File& file, double seconds, double from, double to)
{
    constexpr double sampleRate = 44100.0;
    juce::AudioBuffer<float> buffer (1, (int) (seconds * sampleRate));
    buffer.clear();

    for (auto i = (int) (from * sampleRate); i < (int) (to * sampleRate); ++i)
        buffer.setSample (0, i, (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / sampleRate));

    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (16)))
        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());

    return file;
}

// How many pixels of an image's columns, from one to another, a waveform lit.
int countLit (const juce::Image& image, int fromX, int toX)
{
    auto lit = 0;

    for (int x = fromX; x < toX; ++x)
        for (int y = 0; y < image.getHeight(); ++y)
            if (image.getPixelAt (x, y).getAlpha() > 0)
                ++lit;

    return lit;
}

// A waveform painted over an image, its span reaching a clip's length past the clip's end.
juce::Image paint (phazite::ui::ClipWaveform& waveform, phazite::BeatRange span)
{
    juce::Image image (juce::Image::ARGB, 400, 60, true);
    juce::Graphics g (image);
    g.setColour (juce::Colours::white);
    waveform.paintSpan (g, image.getBounds(), span);
    return image;
}
} // namespace

TEST_CASE ("A clip's waveform loads in the background and draws where its file has audio; another clip of the file draws at once")
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Waves", error);
    REQUIRE (project != nullptr);
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, &project->getSong());

    // A second of sound at 120 BPM: two beats, from the song's start.
    const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 1.0);
    const auto track = phazite::getTrackIDs (project->getSong()).front();
    const auto clips = phazite::importFiles (context, { file.getFullPathName() }, track, phazite::BeatPosition());
    REQUIRE (clips.size() == 1);

    // What shows it is told as its levels load, to paint it again.
    auto loads = 0;
    const auto make = phazite::getClipWaveformMaker();
    auto waveform = make (context, clips[0], [&loads] { ++loads; });
    REQUIRE (waveform != nullptr);

    // The clip's two beats, then two more past its end, where its file has no audio.
    const phazite::BeatRange span { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0) };
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return countLit (paint (*waveform, span), 0, 190) > 190 * 10; }, 10000));
    CHECK (phazite::test::dispatchMessagesUntil ([&loads] { return loads > 0; })); // its message can come after the levels
    const auto image = paint (*waveform, span);
    CHECK (countLit (image, 210, 400) == 0);

    // Another waveform of the same file has its levels at once, from the cache.
    auto again = make (context, clips[0], {});
    CHECK (countLit (paint (*again, span), 0, 190) > 190 * 10);

    setSong (context, nullptr);
}

TEST_CASE ("A clip that doesn't follow the tempo draws its sound where it plays it, across the song's tempo changes")
{
    // Known bug 16 until 2026-11-11: such a clip plays the song's seconds, which a change of tempo puts at other beats, but its
    // waveform was spread over the clip as if the tempo held, its sound drawn away from where it plays.
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Waves", error);
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, &song);

    // Three seconds of silence but for a burst at a second and a half, at its own speed, under 120 BPM for a beat and then 60.
    const auto file = writeBurst (temp.folder.getChildFile ("Burst.wav"), 3.0, 1.45, 1.55);
    const auto track = phazite::getTrackIDs (song).front();
    const auto clips = phazite::importFiles (context, { file.getFullPathName() }, track, phazite::BeatPosition());
    REQUIRE (clips.size() == 1);
    phazite::setClipFollowsTempo (song, clips[0], false);
    phazite::addTempoChange (song, phazite::BeatPosition::fromBeats (1.0), 60.0);
    REQUIRE_FALSE (phazite::followsTempoChanges (song, phazite::findClipState (song, clips[0])));

    // Painted as the playlist paints a clip, over all of it.
    constexpr int width = 400, height = 40;
    auto waveform = phazite::getClipWaveformMaker() (context, clips[0], {});
    REQUIRE (waveform != nullptr);
    const auto paintAll = [&waveform]
    {
        juce::Image image (juce::Image::ARGB, width, height, true);
        juce::Graphics g (image);
        g.setColour (juce::Colours::white);
        waveform->paint (g, { 0, 0, width, height }, width);
        return image;
    };

    // The columns where the clip plays the burst's middle and the last of it, as the model places them.
    const auto range = phazite::getClipRange (song, clips[0]);
    const auto columnPlaying = [&] (double fileSeconds)
    {
        for (int x = 0; x < width; ++x)
            if (const auto beat = range.getStart() + phazite::BeatDuration::fromBeats (range.getLength().inBeats() * (x + 0.5) / width);
                phazite::getFileTimeAt (song, clips[0], beat) >= fileSeconds)
                return x;

        return -1;
    };

    const auto expected = columnPlaying (1.5), last = columnPlaying (1.55) - 1;
    REQUIRE (expected > 0);
    REQUIRE (last > expected);

    // Where it draws the burst: the middle of the columns it lights more than a few pixels tall.
    const auto burstAt = [] (const juce::Image& image)
    {
        auto weighed = 0.0, total = 0.0;

        for (int x = 0; x < image.getWidth(); ++x)
            if (const auto lit = countLit (image, x, x + 1); lit > height / 4)
            {
                weighed += (x + 0.5) * lit;
                total += lit;
            }

        return total > 0.0 ? weighed / total : -1.0;
    };

    // JUCE's thumbnail reads the file's levels in pieces from its start, here a second and a half of it at a time, and each
    // is drawn as it comes: the burst straddles the first two, so its start can show before its end (CI's windows-debug
    // measured it half drawn, 2026-11-12). Once the column that plays the last of it lights, all of it is drawn.
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return countLit (paintAll(), last, last + 1) > height / 4; }, 10000));
    CAPTURE (expected);
    CHECK (std::abs (burstAt (paintAll()) - (expected + 0.5)) <= 2.0);

    setSong (context, nullptr);
}

TEST_CASE ("A clip's waveform draws the same at every part of a pixel the playlist moves it by")
{
    // The owner, 2026-11-09: near the lo-fi example's end, the rain sample's waveform blinked. The view's left edge was
    // crossing its clip, which the playlist then paints afresh at each frame while the song glides under the playhead by
    // parts of a pixel (TimelineEditor::setSubPixel()). JUCE's Direct2D renderer drew the waveform's columns, a list of
    // rectangles a pixel wide, sharp at most of those parts and soft at some, as their edges' rounding fell: about one frame
    // in four looked fainter. On Linux, JUCE's software renderer draws the same at every part, before and after the fix.
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Waves", error);
    REQUIRE (project != nullptr);
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, &project->getSong());

    // Two seconds of a quiet tone, as thin a waveform as the rain's.
    const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Rain.wav"), 2.0, 220.0, 0.05f);
    const auto track = phazite::getTrackIDs (project->getSong()).front();
    const auto clips = phazite::importFiles (context, { file.getFullPathName() }, track, phazite::BeatPosition());
    REQUIRE (clips.size() == 1);
    auto waveform = phazite::getClipWaveformMaker() (context, clips[0], {});
    REQUIRE (waveform != nullptr);

    // Painted as the playlist paints a clip: the lanes placed in the window, moved by a part of a pixel, and the clip in them,
    // across x = 512, where a column's edges round differently either side.
    const auto paintAt = [&waveform] (float part)
    {
        juce::Image image (juce::Image::ARGB, 1100, 40, true);
        juce::Graphics g (image);
        g.setOrigin ({ 3, 0 });
        g.addTransform (juce::AffineTransform::translation (-part, 0.0f));
        g.setOrigin ({ 487, 0 });
        g.setColour (juce::Colours::white);
        waveform->paint (g, { 0, 0, 400, 40 }, 400);
        return image;
    };

    const auto drawn = [] (const juce::Image& image)
    {
        const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);
        auto total = 0.0;

        for (int y = 0; y < data.height; ++y)
            for (int x = 0; x < data.width; ++x)
                total += data.getPixelColour (x, y).getFloatAlpha();

        return total;
    };

    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return countLit (paintAt (0.0f), 490, 890) > 400; }, 10000));
    phazite::test::dispatchPendingMessages(); // every level loaded

    // How much it draws in all is the same at every part of a pixel.
    const auto whole = drawn (paintAt (0.0f));
    juce::Random random (7);
    juce::String report;
    auto most = 0.0;

    for (int i = 0; i < 48; ++i)
    {
        const auto part = random.nextFloat();
        const auto total = drawn (paintAt (part));
        report << juce::String (part, 4) << ": " << juce::String (total, 1) << "\n";
        most = std::max (most, std::abs (total - whole));
    }

    CAPTURE (whole);
    CAPTURE (report.toStdString());
    CHECK (whole > 400.0);
    CHECK (most < whole * 0.002);

    setSong (context, nullptr);
}
