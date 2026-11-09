// SPDX-License-Identifier: AGPL-3.0-or-later
// The waveforms of audio clips (src/app/ClipWaveforms.h), from Phazite's own cache of their files' levels: one loads in
// the background and draws where the clip's file has audio, nothing past it; a second clip of the same file draws at once.
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "app/ClipWaveforms.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"

namespace
{
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
