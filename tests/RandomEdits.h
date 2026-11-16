// SPDX-License-Identifier: AGPL-3.0-or-later
// Random edits of a song through the model, each drawn from a seed, so that any song they make can be made again: every
// kind the model has (tracks, folders, buses and returns with sends, Phazite's own instruments and effects with values set
// by hand, switched off, moved, removed and given a factory preset or sound, containers with chains muted, soloed, split
// and mixed, sidechains added and removed, a drum sampler's pads on tracks of their own, MIDI clips and notes, audio clips
// with their gain, pitch (mostly none) and fades, warped, reversed and at a tempo of their own, clips muted, cut and split,
// aliases, clips moved, automation points added, moved and removed, modulators and macros with what they modulate and how
// much, sends and their levels, sections, chords and keys, tempo changes and the time signature, freezes (where asked),
// mute, names and colours). For the randomised tests: the versions' merges (VersionsMergeTests.cpp) and the song's text,
// pieces, apply-song and plans (RandomSongTests.cpp).
//
// Each number is drawn into a variable of its own: the order two calls in one expression are made in isn't the same
// everywhere, and the same seed must make the same song with every compiler.
#pragma once

#include "TestHelpers.h"
#include "model/base/Song.h"

namespace phazite::test
{
class RandomEdits
{
public:
    // Edits of a song from a seed. An audio clip plays sound, a short sound file written into soundsFolder once; without
    // one, no audio clips are made.
    RandomEdits (Song&, juce::int64 seed, const juce::File& soundsFolder = {});

    // One edit, of a kind drawn at random among those that can be made (an edit of a clip needs a clip).
    void edit();
    void edit (int count);

    juce::Random random;

    // Whether tracks are frozen and unfrozen too (from a copy of the sound, into the song's cache folder): off for the tests
    // of a song's text and pieces, whose copies have no cache folder, where a frozen track without its file isn't frozen.
    bool freezes = false;

private:
    Song& song;
    juce::File sound;

    int draw (int below) { return below > 0 ? random.nextInt (below) : 0; }
    template <typename T>
    T pick (const std::vector<T>& among)
    {
        return among[(size_t) draw ((int) among.size())];
    }
};
} // namespace phazite::test
