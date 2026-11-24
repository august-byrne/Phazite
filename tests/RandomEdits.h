// SPDX-License-Identifier: AGPL-3.0-or-later
// Random edits of a song through the model, each drawn from a seed, so that any song they make can be made again: every
// kind the model has (tracks, folders, buses and returns with sends, Phazite's own instruments and effects with values set
// by hand, switched off, moved, removed and given a factory preset or sound, containers with chains muted, soloed, split
// and mixed, sidechains added and removed, a drum sampler's pads on tracks of their own, MIDI clips and notes, audio clips
// with their gain, pitch (mostly none) and fades, warped, reversed and at a tempo of their own, clips muted, cut and split,
// aliases, clips moved, automation points added, moved and removed, modulators and macros with what they modulate and how
// much, sends and their levels, sections, chords and keys, tempo changes and the time signature, freezes (where asked),
// mute, names and colours); and every edit of notes and controllers' values (Notes.h: moved, placed, made longer or shorter,
// deleted, transposed, nudged, muted, slid, their velocities and channels set, duplicated, replaced as a quantize or a chop
// replaces them, a clip's notes set whole, controllers' values added, drawn and removed) and of clips one by one (Clips.h:
// split, duplicated, copied as an alias, deleted, made unique, moved onto another track, trimmed), each on a clip of an alias
// group half of the time where there is one; and, where asked, edits that change one area of the song's values alone
// (editOneArea()). For the randomised tests: the versions' merges (VersionsMergeTests.cpp), the song's text, pieces,
// apply-song and plans (RandomSongTests.cpp), the model's fuzzer (ModelFuzzTests.cpp), and the agreement of the project's
// answers to whether the song is saved (SavedCopyTests.cpp).
//
// Each number is drawn into a variable of its own: the order two calls in one expression are made in isn't the same
// everywhere, and the same seed must make the same song with every compiler.
#pragma once

#include "TestHelpers.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/notes/NoteValues.h"

#include <vector>

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

    // Whether edit() draws the edits that change one area of the song's values alone too (editOneArea()), at the end of its
    // kinds: off by default, so that a seed makes the song it made before they came (the songs of RandomSongTests.cpp,
    // ModelFuzzTests.cpp, SongStateTests.cpp, VersionsMergeTests.cpp, LockFreeTests.cpp and the speed gates); on for the
    // agreement of the project's answers to whether the song is saved (SavedCopyTests.cpp), where an area changed with another
    // hides a mark of its own that is missing.
    bool editsOneAreaAlone = false;

    // An edit that changes one area alone, each through the model's function, of what the song holds (nothing where it holds
    // none): 0 a send's level (the sends'); 1 a container's macro renamed, 2 its value moved, 3 a device in its chains
    // modulated by it, 4 a modulator's parameter (modulation's); 5 a device's value set by hand (the devices'); 6 a chain
    // muted or soloed (the devices' store); 7 a track muted or not (the tracks' switches); 8 an automation point moved (the
    // curves'); 9 a section renamed (the sections'); 10 a tempo change (the tempo map's); 11 a chord (the chord lane's); 12
    // the key set last (the song's standing); 13 the song's MIDILINKS put in or taken away while it holds no link, as a
    // version's links give it (the root's places); 14 a MIDI link (the MIDI links'); 15 the project's notes (the song's own);
    // 16 a note added (a clip's notes); 17 a clip muted or not (the clips'); 18 a track renamed (the tracks').
    static constexpr int numOneAreaKinds = 19;
    void editOneArea (int kind);

    // One of what the edits of one area alone change, so that each finds something to change: on a track that holds clips,
    // an effect, a container with an effect in its chain, an LFO, a send to a new return and a MIDI clip with a note; a
    // section, a chord and an automation point of the track's volume (no MIDI link, so that the place of the links comes
    // alone). Joins the caller's step.
    void addOneOfEach();

private:
    Song& song;
    juce::File sound;

    // A MIDI clip of the song's (none if it has none), one holding notes that play if asked: of an alias group half of the
    // time, where there is one. And one to three of a clip's notes that play, each once, named through that clip.
    ClipID pickMidiClip (bool withNotes);
    std::vector<NoteData> pickNotes (ClipID);
    void editNotesOrClips (int kind);
    void setAValueByHand(); // of one of Phazite's own devices

    int draw (int below) { return below > 0 ? random.nextInt (below) : 0; }
    template <typename T>
    T pick (const std::vector<T>& among)
    {
        return among[(size_t) draw ((int) among.size())];
    }
};
} // namespace phazite::test
