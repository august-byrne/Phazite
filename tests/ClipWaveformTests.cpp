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
