// SPDX-License-Identifier: AGPL-3.0-or-later
// Random edits of a song through the model, each drawn from a seed, so that any song they make can be made again: every
// kind the model has (tracks, folders, buses and returns with sends, Phazite's own instruments and effects with values set
// by hand, containers with chains, sidechains, a drum sampler's pads on tracks of their own, MIDI clips and notes, audio
// clips with their gain, pitch and fades, aliases, clips moved, automation, modulators and macros with what they modulate,
// sections, chords and keys, tempo changes and the time signature, mute, names and colours). For the randomised tests:
// the versions' merges (VersionsMergeTests.cpp) and the song's text, pieces and apply-song (RandomSongTests.cpp).
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
