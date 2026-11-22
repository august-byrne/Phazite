// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's index (model/base/internal/SongIndex.h), which the model's functions that take IDs find tracks and clips by
// (a note is found in its clip's value: model/base/internal/SongValues.h): it finds what a walk through the song finds
// after every kind of change, their undos and redos, the whole song made like another (a merge, Bring Back, a branch
// switch), a track list put in the place of the song's, IDs changed in place or shared, and a paste of thousands of notes;
// and what listens to the song finds a change in it as it is told of it. Every lookup the tests make is compared with a walk
// too (TestMain.cpp turns that on), and UndoCheck.h compares the whole index at each step.
#include "TestClips.h"
#include "TestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/Clipboard.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/SongRange.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongIndex.h"
#include "model/commands/internal/Undo.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/versions/internal/ApplySong.h"

#include <atomic>
#include <thread>

using phazite::test::at;
using phazite::test::lasting;

namespace
{
// Whether a note is found in its clip by its ID: in the clip's value (SongValues.h), or among the clip's elements in a song that
// keeps everything in its tree (one made from a tree here, as background work makes one).
bool findsNote (const phazite::Song& song, const phazite::NoteRef& ref)
{
    return phazite::hasNote (song, ref);
}

bool isExact (const phazite::Song& song)
{
    return song.getIndex().isExact();
}

// A clip of notes on a track, every note of it asked for (so that the index holds them).
phazite::ClipID addClipOfNotes (phazite::Song& song, phazite::TrackID track, double start, int count)
{
    const auto clip = phazite::test::addMidiClip (song, track, { at (start), at (start + count * 0.25) });
    REQUIRE (clip.isValid());

    for (int i = 0; i < count; ++i)
        phazite::test::addMidiNote (song, clip, 48 + i % 24, at (i * 0.25), lasting (0.25));

    for (const auto& note : phazite::getClipNotes (song, clip))
        REQUIRE (findsNote (song, note.getRef()));

    return clip;
}

// Every track, clip and note of the song found by its ID.
void findEverything (const phazite::Song& song)
{
    for (const auto& track : phazite::getTrackStates (song))
    {
        CHECK (phazite::findTrackState (song, phazite::getTrackID (track)) == track);

        for (const auto& clipState : phazite::getClipStates (track))
        {
            CHECK (phazite::findClipState (song, phazite::getClipID (clipState)) == clipState);

            for (const auto& note : phazite::getClipNotes (song, phazite::getClipID (clipState)))
                CHECK (findsNote (song, note.getRef()));
        }
    }
}
} // namespace

TEST_CASE ("The song's index finds what a walk through the song finds, after every kind of edit, its undo and its redo")
{
    auto song = phazite::createSong();
    phazite::test::UndoCheck undo (*song); // compares the whole index with a walk at each of its checks
    std::vector<phazite::TrackID> tracks;
    phazite::ClipID a, b;

    undo.checkSeveral ("tracks, clips and notes",
                       [&]
                       {
                           tracks = phazite::ensureTracks (*song, 4);
                           a = addClipOfNotes (*song, tracks[0], 0.0, 32);
                           b = addClipOfNotes (*song, tracks[1], 0.0, 16);
                       });

    std::vector<phazite::ClipID> aliases;
    undo.checkSeveral ("an alias on another track, and a note added to both",
                       [&]
                       {
                           aliases = phazite::copyClips (*song, { a }, true);
                           REQUIRE (aliases.size() == 1);
                           phazite::moveClip (*song, aliases[0], at (16.0), tracks[2]);
                           REQUIRE (phazite::addNote (*song, aliases[0], 72, at (17.0), lasting (1.0), 100).isValid());
                       });

    CHECK (phazite::getAliasStates (*song, phazite::findClipState (*song, a)).size() == 2);
    CHECK (phazite::getClipNotes (*song, a).size() == 33);

    undo.checkSeveral ("the alias made unique", [&] { phazite::makeClipsUnique (*song, { a }); });
    CHECK (phazite::getAliasStates (*song, phazite::findClipState (*song, a)).size() == 1);

    undo.checkSeveral ("two tracks put in a folder",
                       [&]
                       {
                           const auto folder = phazite::addTrackAtEnd (*song, true, nullptr);
                           phazite::moveTracksTo (*song, { tracks[1], tracks[2] }, folder, phazite::TrackDrop::into);
                       });

    undo.checkSeveral ("a clip moved onto another track", [&] { phazite::moveClip (*song, b, at (32.0), tracks[3]); });
    undo.checkSeveral ("a clip split", [&] { CHECK (phazite::splitClips (*song, { a }, at (4.0)).size() == 1); });
    undo.checkSeveral ("notes duplicated, some deleted",
                       [&]
                       {
                           const auto notes = getRefs (phazite::getClipNotes (*song, b));
                           const auto copies = phazite::duplicateNotes (*song, notes);
                           CHECK (copies.size() == notes.size());
                           phazite::deleteNotes (*song, { notes.begin(), notes.begin() + 4 });
                       });

    undo.checkSeveral ("a clip deleted", [&] { phazite::deleteClips (*song, { b }, false); });
    undo.checkSeveral ("a track deleted", [&] { phazite::deleteTracks (*song, { tracks[3] }, nullptr); });
    undo.checkSeveral ("tracks duplicated", [&] { CHECK (phazite::duplicateTracks (*song, { tracks[0] }, nullptr).size() == 1); });

    findEverything (*song);
    undo.walkBackAndForth();
    CHECK (isExact (*song));
    findEverything (*song);
}

TEST_CASE ("The song's index follows the whole song made like another, as a merge, Bring Back and a branch switch make it")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 3);
    const auto first = addClipOfNotes (*song, tracks[0], 0.0, 64);
    addClipOfNotes (*song, tracks[1], 0.0, 64);
    const auto then = phazite::test::copyWholeSong (*song);

    // Changed since: a track gone, a clip moved, notes added and gone, a track added with a clip.
    phazite::deleteTracks (*song, { tracks[2] }, nullptr);
    phazite::moveClip (*song, first, at (64.0), tracks[1]);
    phazite::deleteNotes (*song, getRefs (phazite::getClipNotes (*song, first)));
    const auto added = phazite::addTrackAtEnd (*song, false, nullptr);
    addClipOfNotes (*song, added, 0.0, 16);
    REQUIRE (isExact (*song));

    {
        const phazite::UndoStep step (*song, "Bring Back");
        phazite::makeSongLike (*song, then);
    }

    CHECK (isExact (*song));
    CHECK (phazite::getClipTrack (*song, first) == tracks[0]);
    CHECK (phazite::getClipNotes (*song, first).size() == 64);
    CHECK (phazite::findTrackState (*song, tracks[2]).isValid());
    CHECK_FALSE (phazite::findTrackState (*song, added).isValid());
    findEverything (*song);

    phazite::undo (*song);
    CHECK (isExact (*song));
    CHECK (phazite::findTrackState (*song, added).isValid());
    findEverything (*song);

    phazite::redo (*song);
    CHECK (isExact (*song));
    findEverything (*song);

    // Another branch's song, outside the history.
    auto other = phazite::createSong();
    const auto otherTracks = phazite::ensureTracks (*other, 2);
    addClipOfNotes (*other, otherTracks[1], 8.0, 8);
    phazite::makeSongTheSame (*song, other->getState());
    CHECK (isExact (*song));
    CHECK (phazite::getTrackStates (*song).size() == 2);
    findEverything (*song);
}

TEST_CASE ("The song's index follows a track list put in the place of the song's, IDs changed in place, and IDs two elements share")
{
    auto song = phazite::createSong();
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto clip = addClipOfNotes (*song, tracks[0], 0.0, 8);
    auto state = song->getState();

    // Another track list, a copy, in the place of the song's: its elements are the ones found.
    {
        const phazite::UndoStep step (*song, "Replace");
        auto list = state.getChildWithName (phazite::schema::TRACKS);
        const auto copy = list.createCopy();
        state.removeChild (list, &song->getUndoManager());
        state.appendChild (copy, &song->getUndoManager());
        CHECK (phazite::findClipState (*song, clip).isAChildOf (copy));
    }

    CHECK (isExact (*song));
    phazite::undo (*song);
    CHECK (isExact (*song));
    CHECK (phazite::findClipState (*song, clip).isAChildOf (state.getChildWithName (phazite::schema::TRACKS)));

    // A clip's ID changed in place, as no edit changes one: found by its new one only.
    auto clipState = phazite::findClipState (*song, clip);
    const auto renumbered = phazite::ClipID::fromRaw (song->makeID());
    clipState.setProperty (phazite::schema::clipID, (juce::int64) renumbered.getRaw(), nullptr);
    CHECK_FALSE (phazite::findClipState (*song, clip).isValid());
    CHECK (phazite::findClipState (*song, renumbered) == clipState);
    CHECK (isExact (*song));

    // Two clips with one ID, as no edit makes them: the first a walk meets, as before.
    auto twin = clipState.createCopy();
    phazite::findTrackState (*song, tracks[1]).appendChild (twin, nullptr);
    CHECK (phazite::findClipState (*song, renumbered) == clipState);
    CHECK (isExact (*song));
    clipState.getParent().removeChild (clipState, nullptr);
    CHECK (phazite::findClipState (*song, renumbered) == twin);
    CHECK (isExact (*song));

    // A track that becomes a folder holds the tracks in it.
    auto folder = phazite::findTrackState (*song, tracks[0]);
    auto inside = phazite::findTrackState (*song, tracks[1]);
    inside.getParent().removeChild (inside, nullptr);
    folder.appendChild (inside, nullptr);
    CHECK_FALSE (phazite::findTrackState (*song, tracks[1]).isValid()); // only a folder holds tracks
    folder.setProperty (phazite::schema::trackKind, phazite::schema::trackIsFolder, nullptr);
    CHECK (phazite::findTrackState (*song, tracks[1]) == inside);
    CHECK (phazite::getClipTrack (*song, renumbered) == tracks[1]);
    CHECK (isExact (*song));
}

namespace
{
// Asks the song's index about each change of a track or a clip it is told of, as it is told: what was added is found, what
// was removed isn't.
struct Asker final : juce::ValueTree::Listener
{
    explicit Asker (const phazite::Song* s = nullptr)
        : song (s)
    {
    }

    juce::ValueTree find (const juce::ValueTree& element) const
    {
        if (element.hasType (phazite::schema::TRACK))
            return phazite::findTrackState (*song, phazite::getTrackID (element));

        return phazite::findClipState (*song, phazite::getClipID (element));
    }

    static bool isAsked (const juce::ValueTree& element)
    {
        return element.hasType (phazite::schema::TRACK) || element.hasType (phazite::schema::CLIP);
    }

    void ask (bool answer) { (answer ? right : wrong)++; }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child) override
    {
        if (isAsked (child))
            ask (find (child) == child);
    }

    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int) override
    {
        if (isAsked (child))
            ask (! find (child).isValid());
    }

    const phazite::Song* song; // set once the song is made
    int right = 0, wrong = 0;
};
} // namespace

TEST_CASE ("What listens to the song, on any of its elements, finds a change in the index as it is told of it")
{
    // A song made from a tree, not by makeSong(), so that a listener can be on its track list before the index is.
    auto id = [next = juce::int64 { 100 }]() mutable { return ++next; };
    juce::ValueTree state (phazite::schema::PHAZITE), list (phazite::schema::TRACKS), track (phazite::schema::TRACK),
        clip (phazite::schema::CLIP);
    track.setProperty (phazite::schema::trackID, id(), nullptr);
    clip.setProperty (phazite::schema::clipID, id(), nullptr);
    clip.setProperty (phazite::schema::clipStart, 0.0, nullptr);
    clip.setProperty (phazite::schema::clipLength, 16.0, nullptr);

    const auto makeNote = [&id] (int pitch)
    {
        juce::ValueTree note (phazite::schema::NOTE);
        note.setProperty (phazite::schema::noteID, id(), nullptr);
        note.setProperty (phazite::schema::notePitch, pitch, nullptr);
        note.setProperty (phazite::schema::noteStart, 0.0, nullptr);
        note.setProperty (phazite::schema::noteLength, 1.0, nullptr);
        return note;
    };

    for (int pitch = 60; pitch < 64; ++pitch)
        clip.appendChild (makeNote (pitch), nullptr);

    track.appendChild (clip, nullptr);
    list.appendChild (track, nullptr);
    state.appendChild (list, nullptr);

    // On the track list before the song, and its index, are made; then on the song's root and a track.
    Asker onList, onRoot, onTrack;
    juce::ValueTree listHandle = list, rootHandle = state, trackHandle = track;
    listHandle.addListener (&onList);
    const phazite::Song::Ptr song = new phazite::Song (state);

    for (auto* asker : { &onList, &onRoot, &onTrack })
        asker->song = song.get();

    rootHandle.addListener (&onRoot);
    trackHandle.addListener (&onTrack);

    auto* undo = &song->getUndoManager();
    clip.appendChild (makeNote (65), undo); // a note added, and one taken out: no track or clip of the index's changes
    clip.removeChild (clip.getChild (0), undo);
    auto otherClip = clip.createCopy();
    otherClip.setProperty (phazite::schema::clipID, id(), nullptr);
    track.appendChild (otherClip, undo); // a clip added and taken out
    track.removeChild (otherClip, undo);
    auto otherTrack = juce::ValueTree (phazite::schema::TRACK);
    otherTrack.setProperty (phazite::schema::trackID, id(), nullptr);
    list.appendChild (otherTrack, undo); // a track added and taken out
    list.removeChild (otherTrack, undo);
    song->getUndoManager().undo(); // and all of it back, then again, as Undo and Redo make it
    song->getUndoManager().redo();

    for (auto* asker : { &onList, &onRoot, &onTrack })
    {
        CHECK (asker->right > 0);
        CHECK (asker->wrong == 0);
    }

    listHandle.removeListener (&onList);
    rootHandle.removeListener (&onRoot);
    trackHandle.removeListener (&onTrack);
    CHECK (isExact (*song));
}

TEST_CASE ("The song's index holds what the song does after each of a few hundred random edits, undos and redos")
{
    // Seeded, so that a failure happens again: tracks added, removed, moved in the list and into and out of folders;
    // clips added, moved between tracks, linked and unlinked, split; notes added, moved and deleted; a folder removed with
    // its clips' notes indexed; Undo and Redo; and the whole song made like one it was before, as a merge makes it.
    auto song = phazite::createSong();
    phazite::test::UndoCheck undo (*song);
    juce::Random random (122);
    auto* history = &song->getUndoManager();
    const auto snapshot = [&song] { return phazite::test::copyWholeSong (*song); };
    auto earlier = snapshot();
    phazite::ensureTracks (*song, 3);

    const auto pick = [&random] (const auto& items) { return items[(size_t) random.nextInt ((int) items.size())]; };
    const auto tracksOf = [&song] (bool folders)
    {
        std::vector<phazite::TrackID> found;

        for (const auto& state : phazite::getTrackStates (*song))
            if (phazite::isFolderState (state) == folders && ! phazite::isReturnState (state))
                found.push_back (phazite::getTrackID (state));

        return found;
    };
    const auto clipsOf = [&song] { return phazite::getAllClips (*song); };
    const auto at4 = [&random] { return at (random.nextInt (32) * 1.0); };

    for (int step = 0; step < 400; ++step)
    {
        INFO ("step " << step);
        const auto kind = random.nextInt (16);

        if (kind == 0 && history->canUndo())
        {
            phazite::undo (*song);
        }
        else if (kind == 1 && history->canRedo())
        {
            phazite::redo (*song);
        }
        else
        {
            const phazite::UndoStep change (*song, "Random Edit");
            const auto tracks = tracksOf (false), folders = tracksOf (true);
            const auto clips = clipsOf();

            switch (kind)
            {
                case 2:
                    phazite::addTrackAtEnd (*song, random.nextInt (4) == 0, nullptr);
                    break;
                case 3:
                    if (tracks.size() > 2)
                        phazite::deleteTracks (*song, { pick (tracks) }, nullptr);
                    break;
                case 4: // moved within the list, as a drag of its header moves it
                {
                    auto list = song->getState().getChildWithName (phazite::schema::TRACKS);
                    if (list.getNumChildren() > 1)
                        list.moveChild (random.nextInt (list.getNumChildren()), random.nextInt (list.getNumChildren()), history);
                    break;
                }
                case 5: // into a folder, or out of one
                    if (! folders.empty() && ! tracks.empty())
                        phazite::moveTracksTo (*song, { pick (tracks) }, pick (folders),
                                               random.nextBool() ? phazite::TrackDrop::into : phazite::TrackDrop::after);
                    break;
                case 6:
                    if (! tracks.empty())
                        if (const auto clip =
                                phazite::insertMidiClip (*song, pick (tracks), { at4(), at4() + phazite::test::lasting (4.0) }, nullptr);
                            clip.isValid())
                            for (int n = 0; n < 4; ++n)
                                phazite::addNote (*song, clip, 48 + random.nextInt (24),
                                                  phazite::getContentStart (*song, clip) + phazite::test::lasting (n),
                                                  phazite::test::lasting (0.5), 100);
                    break;
                case 7:
                    if (! clips.empty() && ! tracks.empty())
                        phazite::moveClip (*song, pick (clips), at4(), pick (tracks));
                    break;
                case 8: // linked: a copy as an alias, or a clip of the song given another's group
                    if (! clips.empty())
                    {
                        const auto aliases = phazite::copyClips (*song, { pick (clips) }, true);

                        if (random.nextBool() && clips.size() > 1)
                        {
                            const auto clip = pick (clips);
                            const auto linked = aliases.empty() ? pick (clips) : aliases.front();
                            const auto link = phazite::getClipValue (*song, linked).value().link;
                            if (link != 0 && clip != linked && phazite::isMidiClip (*song, clip))
                                phazite::setClipLink (*song, clip, link);
                        }
                    }
                    break;
                case 9:
                    if (! clips.empty())
                        phazite::makeClipsUnique (*song, { pick (clips) });
                    break;
                case 10:
                    if (! clips.empty())
                    {
                        const auto clip = pick (clips);
                        phazite::splitClips (*song, { clip },
                                             phazite::getClipRange (*song, clip).getStart() + phazite::test::lasting (1.0));
                    }
                    break;
                case 11:
                    if (! clips.empty())
                        if (const auto clip = pick (clips); phazite::isMidiClip (*song, clip))
                            phazite::addNote (*song, clip, 60, phazite::getClipRange (*song, clip).getStart(),
                                              phazite::test::lasting (0.25), 90);
                    break;
                case 12:
                case 13:
                    if (! clips.empty())
                    {
                        if (const auto notes = phazite::getClipNotes (*song, pick (clips)); ! notes.empty())
                        {
                            const auto note = pick (notes);

                            if (kind == 12)
                                phazite::moveNote (*song, note.getRef(), note.start + phazite::test::lasting (0.5), note.pitch + 1);
                            else
                                phazite::deleteNotes (*song, { note.getRef() });
                        }
                    }
                    break;
                case 14: // a folder removed, every note of its tracks' clips asked for first
                    if (! folders.empty())
                    {
                        const auto folder = pick (folders);

                        for (const auto& clip : clipsOf())
                            for (const auto& note : phazite::getClipNotes (*song, clip))
                                CHECK (findsNote (*song, note.getRef()));

                        phazite::deleteTracks (*song, { folder }, nullptr);
                    }
                    break;
                case 15: // the whole song made like it was a while ago, as a merge or Bring Back makes it
                    phazite::makeSongLike (*song, earlier);
                    earlier = snapshot();
                    break;
                default:
                    break;
            }
        }

        REQUIRE (isExact (*song));
    }

    findEverything (*song);
    undo.walkBackAndForth();
    CHECK (isExact (*song));
}

TEST_CASE ("The song's index follows a paste of thousands of notes, and its undo")
{
    auto song = phazite::createSong();
    phazite::ProjectContext context (phazite::test::getEngineContext());
    const auto tracks = phazite::ensureTracks (*song, 2);
    const auto clip = phazite::test::addMidiClip (*song, tracks[0], { at (0.0), at (2000.0) });
    std::vector<phazite::NoteData> notes;

    for (int i = 0; i < 3000; ++i)
        notes.push_back ({ .clip = clip, .pitch = 36 + i % 48, .start = at (i * 0.25), .length = lasting (0.25) });

    phazite::setClipNotes (*song, clip, notes);
    song->getUndoManager().clearUndoHistory();
    phazite::setSong (context, song.get());

    auto& selection = phazite::getSelection (context);
    selection.setNoteClips ({ clip });
    phazite::selectNotes (selection, getRefs (phazite::getClipNotes (*song, clip)));
    REQUIRE (phazite::getSelectedNotes (selection).size() == 3000);

    auto& clipboard = phazite::getClipboard (context);
    phazite::copySelection (clipboard, selection, phazite::getRangeSelection (context));
    phazite::movePlayhead (*song, at (1000.0));
    REQUIRE (phazite::canPaste (clipboard, context));

    {
        const phazite::UndoStep step (*song, "Paste");
        phazite::paste (clipboard, context);
    }

    CHECK (phazite::getClipNotes (*song, clip).size() > 3000);
    CHECK (isExact (*song));
    findEverything (*song);
    phazite::undo (*song);
    CHECK (phazite::getClipNotes (*song, clip).size() == 3000);
    CHECK (isExact (*song));
    findEverything (*song);
    phazite::setSong (context, nullptr);
}

namespace
{
// A song of one track with a clip of notes, made from a tree on whatever thread calls this, as background work makes one
// of its own from a copy (the versions' store makes a song it reads consistent so): without makeSong(), which is the
// message thread's.
phazite::Song::Ptr makeSongOfOwn (int numNotes, juce::int64 firstID)
{
    juce::ValueTree state (phazite::schema::PHAZITE), list (phazite::schema::TRACKS), track (phazite::schema::TRACK),
        clip (phazite::schema::CLIP);
    auto id = firstID;
    track.setProperty (phazite::schema::trackID, ++id, nullptr);
    clip.setProperty (phazite::schema::clipID, ++id, nullptr);
    clip.setProperty (phazite::schema::clipStart, 0.0, nullptr);
    clip.setProperty (phazite::schema::clipLength, numNotes * 0.25, nullptr);

    for (int i = 0; i < numNotes; ++i)
    {
        juce::ValueTree note (phazite::schema::NOTE);
        note.setProperty (phazite::schema::noteID, ++id, nullptr);
        note.setProperty (phazite::schema::notePitch, 48 + i % 24, nullptr);
        note.setProperty (phazite::schema::noteStart, i * 0.25, nullptr);
        note.setProperty (phazite::schema::noteLength, 0.25, nullptr);
        clip.appendChild (note, nullptr);
    }

    track.appendChild (clip, nullptr);
    list.appendChild (track, nullptr);
    state.appendChild (list, nullptr);
    return new phazite::Song (state);
}

// Looks into a song and changes it, a fixed number of times: each of its notes found, one added and found, then taken out
// and not found. Whether every answer was right.
bool lookAndChange (phazite::Song& song, int rounds)
{
    const auto track = phazite::getTrackStates (song).front();
    auto clipState = phazite::getClipStates (track).front();
    const auto clip = phazite::getClipID (clipState);
    auto right = true;

    for (int round = 0; round < rounds; ++round)
    {
        right = right && phazite::findTrackState (song, phazite::getTrackID (track)) == track
                && phazite::findClipState (song, clip) == clipState;

        for (const auto& note : clipState)
            right =
                right && findsNote (song, { clip, phazite::NoteID::fromRaw ((std::uint64_t) (juce::int64) note[phazite::schema::noteID]) });

        juce::ValueTree added (phazite::schema::NOTE);
        const auto addedID = phazite::NoteID::fromRaw ((std::uint64_t) 1000000 + (std::uint64_t) round);
        added.setProperty (phazite::schema::noteID, (juce::int64) addedID.getRaw(), nullptr);
        clipState.appendChild (added, &song.getUndoManager());
        right = right && findsNote (song, { clip, addedID });
        clipState.removeChild (added, &song.getUndoManager());
        right = right && ! findsNote (song, { clip, addedID });
    }

    return right && song.getIndex().isExact();
}
} // namespace

TEST_CASE ("A song made on another thread is that thread's: its index answers there while the message thread's answers here")
{
    // Each song's index is used by the thread that made its song; the two share nothing but atomics, so neither asserts.
    constexpr int rounds = 200;
    auto mine = makeSongOfOwn (64, 0);
    std::atomic<bool> theirsRight { false };

    std::thread other (
        [&theirsRight]
        {
            auto theirs = makeSongOfOwn (64, 5000);
            theirsRight = lookAndChange (*theirs, rounds);
        }); // the song made, used and destroyed there

    const auto mineRight = lookAndChange (*mine, rounds);
    other.join();
    CHECK (mineRight);
    CHECK (theirsRight);
}

TEST_CASE ("A lookup into a song from a thread other than the one that made it is a JUCE assertion")
{
    auto song = phazite::createSong(); // the message thread's
    const auto clip = addClipOfNotes (*song, phazite::ensureTracks (*song, 1).front(), 0.0, 4);

    const phazite::test::ExpectedAssertion fromAnotherThread ("SongIndex.cpp");
    std::thread other ([&song, clip] { juce::ignoreUnused (phazite::findClipState (*song, clip)); }); // while this thread waits
    other.join();

#if JUCE_DEBUG
    CHECK (fromAnotherThread.getCount() > 0);
#endif
}
