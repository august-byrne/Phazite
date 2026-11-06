// SPDX-License-Identifier: AGPL-3.0-or-later
// The sound Phazite makes once it has opened (src/model/engine/StartupSound.h): synthesized, its tail from Phazite's reverb,
// and played by the previews' engine (src/model/engine/Preview.h).
#include "EnginePlayback.h"
#include "TestHelpers.h"
#include "model/engine/Preview.h"
#include "model/engine/StartupSound.h"
#include "model/engine/internal/Preview.h"
#include "model/mixer/MixerModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
constexpr double rate = phazite::test::EnginePlayback::sampleRate;

// The loudest a stretch of a sound gets, in decibels against its peak.
float getLevelAt (const juce::AudioBuffer<float>& sound, double fromSeconds, double toSeconds)
{
    const auto start = (int) (fromSeconds * rate), end = std::min (sound.getNumSamples(), (int) (toSeconds * rate));
    return juce::Decibels::gainToDecibels (sound.getMagnitude (start, end - start) / sound.getMagnitude (0, sound.getNumSamples()),
                                           -200.0f);
}
} // namespace

TEST_CASE ("Phazite's startup sound is a quick blip whose echoes bounce between the sides, peaking at -6 dBFS and ending in "
           "silence, the same every time")
{
    const auto sound = phazite::makeStartupSound (rate);
    REQUIRE (sound.getNumChannels() == 2);
    const auto length = sound.getNumSamples();
    CHECK (length / rate > 1.0);
    CHECK (length / rate < 2.0);

    auto finite = true;

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < length; ++i)
            finite = finite && std::isfinite (sound.getSample (channel, i));

    REQUIRE (finite);
    CHECK (sound.getMagnitude (0, length) == doctest::Approx (0.5f).epsilon (0.001));

    // It starts without a click, and is loudest in its blip.
    CHECK (std::abs (sound.getSample (0, 0)) < 0.01f);
    CHECK (getLevelAt (sound, 0.0, 0.1) == doctest::Approx (0.0f));

    // Its echoes bounce between the sides: until the first comes back (44 ms in) the sides are the same, and once the blip
    // has gone they differ about as much as they agree.
    juce::AudioBuffer<float> side (1, length), middle (1, length);
    side.copyFrom (0, 0, sound, 0, 0, length);
    side.addFrom (0, 0, sound, 1, 0, length, -1.0f);
    middle.copyFrom (0, 0, sound, 0, 0, length);
    middle.addFrom (0, 0, sound, 1, 0, length);
    const auto firstEcho = (int) (0.044 * rate), afterBlip = (int) (0.13 * rate), later = (int) (0.5 * rate);
    CHECK (side.getMagnitude (0, firstEcho - 1) < 1.0e-6f);
    CHECK (side.getMagnitude (afterBlip, later - afterBlip) > 0.3f * middle.getMagnitude (afterBlip, later - afterBlip));

    // The echoes ring on after the blip, quiet beside it, and fade as they go: each 2 dB under the one before, so some 18 dB
    // in the 0.4 s after them.
    const auto echoes = getLevelAt (sound, 0.15, 0.25), laterOn = getLevelAt (sound, 0.55, 0.65);
    CAPTURE (echoes);
    CAPTURE (laterOn);
    CHECK (echoes > -32.0f);
    CHECK (echoes < -10.0f);
    CHECK (laterOn < echoes - 12.0f);
    CHECK (laterOn > echoes - 26.0f);

    // It ends in silence, and comes out the same every time.
    CHECK (sound.getMagnitude (length - (int) (0.01 * rate), (int) (0.01 * rate)) < 1.0e-4f);
    const auto again = phazite::makeStartupSound (rate);
    REQUIRE (again.getNumSamples() == length);

    for (int channel = 0; channel < 2; ++channel)
        CHECK (std::memcmp (again.getReadPointer (channel), sound.getReadPointer (channel), (size_t) length * sizeof (float)) == 0);
}

TEST_CASE ("Phazite's startup sound plays through the previews once, as it was made, at the previews' volume")
{
    phazite::test::EnginePlayback playback; // the previews' engine at 48 kHz too
    auto& previews = phazite::getPlaybackHost (phazite::test::getEngineContext()).getPreviewEngine();
    const auto previewer = phazite::createPreviewer (phazite::test::getEngineContext());
    CHECK (previewer->getSampleRate() == doctest::Approx (rate));

    // At 0 dB, so that it plays as it was made.
    previewer->setVolume (phazite::decibelsToFaderPosition (0.0f));
    phazite::playStartupSound (*previewer);
    CHECK (previewer->isPlaying());
    CHECK (previewer->getFile() == juce::File()); // no file: the browser shows none of its sounds playing
    REQUIRE (previews.isPlaying());

    const auto made = phazite::makeStartupSound (rate);
    const auto played = previews.render (made.getNumSamples() + (int) (0.5 * rate));
    auto difference = 0.0f;

    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < made.getNumSamples(); ++i)
            difference = std::max (difference, std::abs (played.getSample (channel, i) - made.getSample (channel, i)));

    CHECK (difference < 1.0e-4f);
    CHECK (played.getMagnitude (made.getNumSamples(), played.getNumSamples() - made.getNumSamples()) < 1.0e-6f);

    // Once it has played to its end, the previewer is done with it; and a sound without samples doesn't play.
    CHECK_FALSE (previewer->isPlaying());
    CHECK_FALSE (previewer->playSound (juce::AudioBuffer<float>(), rate));
    CHECK_FALSE (previewer->isPlaying());
    previewer->stop();
}
