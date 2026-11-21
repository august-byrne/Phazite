// SPDX-License-Identifier: AGPL-3.0-or-later
// Where a song's tracks and clips are, by their IDs (Schema.h: trackID, clipID), and which clips are linked (clipLink): what
// the model's functions that take IDs find elements by, so that none looks through the song for each ID (known bug 122: a
// drag of 500 notes in a clip of 5,000 looked through every track for each note several times). Each song keeps one
// (Song::getIndex()). A note is found in its clip's value by its ID (model/base/internal/SongValues.h: ClipNotes::find()),
// which needs no index.
//
// The song's tree stays the only truth of its tracks and clips. The index is made from it as it is first asked (one walk
// through the tracks and their clips), kept exact by the tree's own notifications of what was added, removed or moved (an
// edit, an undo, a redo, a merge or a Bring Back that applies a whole song), and made again from the tree whenever a change
// is one it doesn't follow by itself (an ID that changes, a track that becomes a folder, two elements with one ID, the track
// list replaced). An ID that two elements share is looked for through the song, as before, which finds the first.
//
// Why what it answers is the tree's, even while the tree is telling its listeners of a change. JUCE tells a change to the
// listeners of the element that changed first, then to those of its parent and so on up, and on each element in the order
// its handles were first given a listener. The index listens to the track list's element (TRACKS) from the moment its song
// is made, and every other listener of a song listens to its root (the rule in model/base/Song.h, which
// tools/check-architecture.sh counts: song-listeners), so the index hears of every change to the tracks and clips before
// anything else, and what another listener asks while it is told of the change finds the change already there. And it
// doesn't take that on trust: each answer it gives is checked against the tree first, cheaply (an element still has the ID
// asked for and is still where the index holds it: on a track in the track list through folders), and a miss is checked too
// (the track list and the tracks hold as many children as the index last saw); either failing, the index is made again from
// the tree there and then, and answers from it.
//
// While checking is on (as the tests run it: tests/TestMain.cpp), answers are compared with what a walk through the song
// finds, and a difference is a JUCE assertion, which fails the test it fires in: every one in a small song, and a share in a
// big one, so that the checks cost about as much as the work checked. tests/UndoCheck.h and tests/SongCheck.h compare the
// whole index with a walk too (isExact()).
//
// What it costs: about 100 bytes for each track and each clip, made as the song is first asked for one (a walk of the tracks
// and their elements, as a single lookup made before). Used by one thread, the one that made its song (for the live song,
// the message thread; a song that background work makes for itself from a copy, that work's), as the song's tree is: a lookup
// may make the index, or a part of it, and one from another thread is a JUCE assertion in Debug builds. Nothing of it is
// shared between songs but the atomic switch of checking and the atomic count the speed gates read, so songs on two threads
// never meet.
#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace phazite
{
class SongIndex final : private juce::ValueTree::Listener
{
public:
    explicit SongIndex (juce::ValueTree songState); // a PHAZITE element
    ~SongIndex() override;

    // A track's element (a track, a folder or a return, in the track list or in a folder, as Tracks.h finds them: not the
    // master's) and a clip's (on such a track), by an ID; invalid for none.
    juce::ValueTree findTrack (std::uint64_t id) const;
    juce::ValueTree findClip (std::uint64_t id) const;

    // The tracks' elements in the song's order, each folder's before those in it (Tracks.h: getTrackStates()).
    std::vector<juce::ValueTree> getTracks() const;

    // Where a track is among those: its index there, and its number among the tracks that aren't folders, from 1, as a
    // track without a name of its own is called ("Track 3"); none (-1 and 0) for a track that isn't one of them.
    struct TrackPlace
    {
        int index = -1, number = 0;
        bool operator== (const TrackPlace&) const = default;
    };

    TrackPlace getTrackPlace (const juce::ValueTree& trackState) const;

    // The clips of a clip's alias group (clipLink): the clip first, then the others in the song's order; only the clip if it
    // is in none. For a clip outside the song, those of the track list it is in, looked for.
    std::vector<juce::ValueTree> findLinked (const juce::ValueTree& clipState) const;

    // Whether it holds exactly what a walk through the song finds now, every track, clip and alias group:
    // for the tests.
    bool isExact() const;

    // Whether answers are compared with a walk through the song (above). Off unless set; any thread.
    static void setChecking (bool) noexcept;
    static bool isChecking() noexcept;

    // How many elements were looked at to find what was asked for, since the program started: one for each element a walk
    // passed (the index's own, and the model's that read a track's, a clip's or the song's elements: countVisited()), and one
    // for each answer the index gave. For the speed gates (tests/PerformanceGateTests.cpp), which count what an edit looks
    // at; the checks' walks aren't counted.
    static std::uint64_t countElementsVisited() noexcept;
    static void countVisited (std::uint64_t) noexcept;

private:
    struct Entry
    {
        juce::ValueTree element;
        int count = 1;    // the elements with this ID: more than one, and it is looked for through the song
        int children = 0; // a track's, as many as the index last saw
    };

    using Entries = std::unordered_map<std::uint64_t, Entry>;

    struct Clip
    {
        juce::ValueTree element;
        int count = 1;
        juce::String link; // its alias group's, as the clip holds it (schema::clipLink), or none
    };

    struct Group
    {
        std::vector<juce::ValueTree> clips; // in no order
        int sortedAt = -1;                  // the layout they were put in the song's order at
    };

    // What tells it the song's track list was put in place of another, or taken away: a listener of the root's that hears
    // only that, as the index's own listener, on the track list, hears all that happens in it.
    struct ListWatcher final : juce::ValueTree::Listener
    {
        explicit ListWatcher (const SongIndex& i)
            : index (i)
        {
        }

        void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override;
        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override;
        const SongIndex& index;
    };

    juce::ValueTree root;
    const juce::Thread::ThreadID owner; // the thread that made its song, which alone may look into it
    ListWatcher listWatcher { *this };
    mutable juce::ValueTree trackList; // the TRACKS it listens to
    mutable int listChildren = 0;      // as many as the index last saw
    mutable bool built = false;
    mutable Entries tracks;
    mutable std::unordered_map<std::uint64_t, Clip> clips;
    mutable std::unordered_map<juce::String, Group> groups;
    mutable std::vector<juce::ValueTree> order;                   // the tracks', while orderKnown
    mutable std::unordered_map<std::uint64_t, TrackPlace> places; // theirs, by their IDs, the first of an ID's
    mutable bool orderKnown = false;
    mutable bool hasDuplicates = false; // a track's or a clip's ID that another has: made again at the next change
    mutable int layout = 0;             // one more each time a track or a clip is added, removed or moved

    void listen() const;    // to the song's track list as it is now, letting go of one it held
    void prepare() const;   // the index as the song is now: listening to its track list, built
    void knowOrder() const; // the tracks' order and places, made again if they changed
    void orderChanged() const;
    void build() const;
    void addTrack (const juce::ValueTree&) const;
    void removeTrack (const juce::ValueTree&) const;
    void addClip (const juce::ValueTree&) const;
    void removeClip (const juce::ValueTree&) const;
    bool isIndexedTrack (const juce::ValueTree&) const;
    Clip* findIndexedClip (const juce::ValueTree&) const;
    void forget() const; // to be built again, from the song as it is then
    void seeChildren (const juce::ValueTree& parent) const;

    // The checks of an answer against the tree (above): an element still in the song where the index holds it, and whether
    // the track list or a track holds another count of children than the index last saw.
    bool isInSong (const juce::ValueTree& element, std::uint64_t id, bool clip) const;
    bool isBehind() const;

    // What a walk through the song finds: what the index is checked against, and what it gives for an ID two elements share.
    juce::ValueTree walkToTrack (std::uint64_t id) const;
    juce::ValueTree walkToClip (std::uint64_t id) const;
    std::vector<juce::ValueTree> walkTracks() const;
    size_t getSize() const; // about how many elements a walk to a clip passes

    juce::ValueTree findTrackIndexed (std::uint64_t id) const;
    juce::ValueTree findClipIndexed (std::uint64_t id) const;
    std::vector<juce::ValueTree> findLinkedIndexed (const juce::ValueTree& clipState) const;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override;

    JUCE_DECLARE_NON_COPYABLE (SongIndex)
};
} // namespace phazite
