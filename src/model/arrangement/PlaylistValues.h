// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist's edits as plain values (no Tracktion here), for PlaylistModel.h and the model's own arrangement code: where
// tracks dragged in the playlist go, and the shapes of audio clips' fades.
#pragma once

#include <juce_core/juce_core.h>

namespace phazite
{
// Where tracks dragged in the playlist go: before or after a track, among the tracks of the folder it is in (or the
// top-level tracks), or into a folder, after its last track.
enum class TrackDrop
{
    before,
    after,
    into
};

// An audio clip's fade's shape, as Phazite's engine plays it (engine/audio/AudioRegion.h): a curve or an S-curve, bent from
// -1 to 1. A curve bent to 1 is convex: quick at first; to -1, concave: slow at first. An S-curve bent to 1 starts and ends
// slowly; to -1, quickly. Either is straight at 0. The fade shapes' commands (clip.fadeLinear...) give the fades these.
enum class FadeShape
{
    curve,
    sCurve
};

struct FadeCurve
{
    FadeShape shape = FadeShape::curve;
    float bend = 0.0f;

    bool operator== (const FadeCurve& other) const noexcept { return shape == other.shape && juce::exactlyEqual (bend, other.bend); }
};

inline constexpr FadeCurve linearFade {}, convexFade { FadeShape::curve, 1.0f }, concaveFade { FadeShape::curve, -1.0f },
    sCurveFade { FadeShape::sCurve, 1.0f };
} // namespace phazite
