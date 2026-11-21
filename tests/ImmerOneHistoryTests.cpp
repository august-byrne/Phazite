// SPDX-License-Identifier: AGPL-3.0-or-later
// A spike, not a part of Phazite: whether one undo history can span a song held partly as a tree and partly as immutable
// values (the immer library), which moving the song to values one area at a time needs (docs/design/decisions.md,
// "Engineering"). Built only where CMake is told where immer is (-DPHAZITE_IMMER_SPIKE_DIR=<its folder>).
//   phazite_tests.exe --test-suite=immer-spike
//   phazite_tests.exe --test-suite=benchmark --no-skip --test-case="*one history*"
// The way tried: an area of the song held as a value (ValueArea) changes through the song's own history, as one action that
// puts a root in the place of another. So a step, a gesture and a trial (model/commands/internal/Undo.h) hold a tree's
// changes and a value's alike, and Undo and Redo take both back and forth, with nothing of theirs changed. However many
// times an area changes within a step, the step holds one action for it (the root before, the root after), wherever the
// tree's changes fall between them: a drag keeps two roots, not one for each of the mouse's moves.
// Here the song is a real one (its tracks, clips, names and volumes in its tree, changed by the model's functions), and the
// clips' notes are a value beside it, by their clips' IDs: as the song would be once the notes had moved and the clips not.
#include "TestClips.h"
#include "TestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/NoteElements.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"

#include <immer/flex_vector.hpp>
#include <immer/flex_vector_transient.hpp>
#include <immer/map.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>

#if JUCE_WINDOWS
 #include <malloc.h>
#endif

namespace
{
using ID = std::uint64_t;

//==============================================================================
// An area of a song held as an immutable value, in the song's one undo history. Value has isSameAs (another): whether two
// are one value, without a look inside.
template <typename Value>
class ValueArea
{
public:
    using Listener = std::function<void (const Value& before, const Value& after)>;

    ValueArea (juce::UndoManager& history, Value start)
        : undoManager (history),
          shared (std::make_shared<Shared>())
    {
        shared->now = std::move (start);
    }

    const Value& get() const noexcept { return shared->now; }

    // The area's next value, as a part of the history's step in progress: what a tree's change with the undo manager is.
    // The step's first change of the area performs an action that holds the value before and the value after; its later
    // ones only give that action another value after.
    void set (Value next)
    {
        if (next.isSameAs (shared->now))
            return;

        if (auto* open = findOpenAction())
        {
            open->after = next;
            shared->moveTo (std::move (next));
            return;
        }

        auto* action = new Swap (shared, shared->now, std::move (next));
        shared->open = action;
        undoManager.perform (action);
    }

    // Outside the history: a song as it is read from its file.
    void setOutsideHistory (Value next) { shared->moveTo (std::move (next)); }

    // Told of every change, a step's, an undo's and a redo's alike, with the value left and the value come to: what the
    // song's index, what plays it and its views would compare to know what changed.
    void addListener (Listener listener) { shared->listeners.push_back (std::move (listener)); }

    // The actions the history holds for the area: one for each step that changed it and is still kept.
    int countActions() const noexcept { return shared->actions; }

private:
    struct Swap;

    struct Shared
    {
        Value now;
        Swap* open = nullptr; // the action performed last, while its step may still be in progress
        std::vector<Listener> listeners;
        int actions = 0;

        void moveTo (Value next)
        {
            const auto before = std::exchange (now, std::move (next));

            for (const auto& listener : listeners)
                listener (before, now);
        }
    };

    // The actions hold what the area shares with them, so that a history that outlives the area still finds it.
    struct Swap final : juce::UndoableAction
    {
        Swap (std::shared_ptr<Shared> area, Value valueBefore, Value valueAfter)
            : shared (std::move (area)),
              before (std::move (valueBefore)),
              after (std::move (valueAfter))
        {
            ++shared->actions;
        }

        ~Swap() override
        {
            if (shared->open == this)
                shared->open = nullptr;

            --shared->actions;
        }

        bool perform() override
        {
            shared->moveTo (after);
            return true;
        }

        bool undo() override
        {
            shared->moveTo (before);
            return true;
        }

        std::shared_ptr<Shared> shared;
        Value before, after;
    };

    // The area's action in the step in progress, if it has one: the action performed last, while the history still lists
    // it among the actions of its current transaction (none once a step began, or after an undo or a redo). An action the
    // history let go says so itself (~Swap()), so the pointer compared is never one of another action since.
    Swap* findOpenAction() const
    {
        if (shared->open == nullptr)
            return nullptr;

        juce::Array<const juce::UndoableAction*> current;
        undoManager.getActionsInCurrentTransaction (current);

        if (! current.contains (shared->open))
            shared->open = nullptr;

        return shared->open;
    }

    juce::UndoManager& undoManager;
    std::shared_ptr<Shared> shared;
};

//==============================================================================
// The notes of the song's clips as a value, by their clips' IDs.
struct SpikeNote
{
    ID id = 0;
    int pitch = 60;
    double start = 0.0, length = 1.0;

    bool operator== (const SpikeNote&) const = default;
};

struct NotesByClip
{
    immer::map<ID, immer::flex_vector<SpikeNote>> clips;

    bool isSameAs (const NotesByClip& other) const { return clips.identity() == other.clips.identity(); }
};

// The song as it is now: its tree as text, as an undo step keeps it (UndoCheck.h), and its notes' value.
struct Snapshot
{
    juce::String tree;
    NotesByClip notes;
};

// A song of four tracks whose clips' notes are a value beside its tree, and the changes a test makes to it: to the tree by
// the model's functions, to the notes through their area, and to both in one step where one names the other (a clip made
// or deleted, with its notes).
struct MixedSong
{
    MixedSong()
    {
        tracks = phazite::ensureTracks (*song, 4);
        song->getUndoManager().clearUndoHistory();
    }

    Snapshot take() const { return { phazite::test::writeSong (*song, phazite::test::SongPart::undoable), notes.get() }; }

    std::vector<phazite::ClipID> getClips() const
    {
        std::vector<phazite::ClipID> clips;

        for (const auto& clip : phazite::getClipStates (*song))
            clips.push_back (phazite::getClipID (clip));

        return clips;
    }

    // Whether the two halves name the same clips: each clip of the tree has its notes, and no notes are a clip's that is gone.
    bool halvesAgree() const
    {
        std::set<ID> inTree, inValue;

        for (const auto clip : getClips())
            inTree.insert (clip.getRaw());

        for (const auto& [clip, clipNotes] : notes.get().clips)
            inValue.insert (clip);

        return inTree == inValue;
    }

    //==========================================================================
    void addNote (phazite::ClipID clip)
    {
        const auto id = ++made;
        const SpikeNote note { id, 48 + (int) (id % 24), (double) (id % 16) * 0.25, 0.25 };
        const auto* held = notes.get().clips.find (clip.getRaw());
        notes.set ({ notes.get().clips.set (clip.getRaw(), (held != nullptr ? *held : immer::flex_vector<SpikeNote>()).push_back (note)) });
    }

    void changeNotes (phazite::ClipID clip, int how)
    {
        const auto* held = notes.get().clips.find (clip.getRaw());

        if (held == nullptr || held->empty() || how == 0)
            return addNote (clip);

        if (how == 1)
        {
            auto moved = held->transient();

            for (size_t i = 0; i < moved.size(); ++i)
                moved.update (i,
                              [] (SpikeNote note)
                              {
                                  note.start += 0.25;
                                  return note;
                              });

            notes.set ({ notes.get().clips.set (clip.getRaw(), moved.persistent()) });
        }
        else
        {
            notes.set ({ notes.get().clips.set (clip.getRaw(), held->take (held->size() - 1)) });
        }
    }

    void changeTree (phazite::ClipID clip, int how)
    {
        const auto track = tracks[(size_t) (++made % tracks.size())];

        if (how == 0)
        {
            phazite::renameTrack (*song, track, "Name " + juce::String ((juce::int64) made));
        }
        else if (how == 1)
        {
            phazite::setChannelVolume (*song, track, -0.01f * (float) made);
        }
        else
        {
            const auto clipState = phazite::findClipState (*song, clip);
            const auto range = phazite::getClipRange (clipState);
            phazite::setClipPlace (*song, clipState, range.getStart(), range.getLength() + phazite::BeatDuration::fromBeats (4.0),
                                   phazite::getClipOffset (clipState));
        }
    }

    // A clip made, with its notes: the tree's half and the value's in the step in progress.
    phazite::ClipID insertClip()
    {
        const auto place = (double) (++made) * 1024.0; // where no clip was or will be, whatever is undone meanwhile
        const auto clip = phazite::insertMidiClip (*song, tracks[(size_t) (made % tracks.size())],
                                                   { phazite::test::at (place), phazite::test::at (place + 16.0) }, nullptr);
        addNote (clip);
        return clip;
    }

    // A clip deleted, and its notes with it.
    void deleteClip (phazite::ClipID clip)
    {
        phazite::deleteClips (*song, { clip }, false);
        notes.set ({ notes.get().clips.erase (clip.getRaw()) });
    }

    // One change of the song, of a kind drawn by lot: to the notes, to the tree, to both, or a clip made or deleted. Each
    // changes something, so that a step with one is a step the history keeps.
    void changeSomething (juce::Random& random)
    {
        const auto clips = getClips();

        if (clips.empty())
            return (void) insertClip();

        const auto clip = clips[(size_t) random.nextInt ((int) clips.size())];

        switch (random.nextInt (6))
        {
            case 0:
                changeNotes (clip, random.nextInt (3));
                break;
            case 1:
                changeTree (clip, random.nextInt (3));
                break;
            case 2:
                changeNotes (clip, random.nextInt (3));
                changeTree (clip, random.nextInt (3));
                break;
            case 3:
                changeTree (clip, 2);
                addNote (clip);
                break;
            case 4:
                insertClip();
                break;
            default:
                if (clips.size() > 1)
                    deleteClip (clip);
                else
                    insertClip();

                break;
        }
    }

    void changeInStep (const juce::String& name, juce::Random& random)
    {
        const phazite::UndoStep step (*song, name);
        changeSomething (random);
    }

    phazite::Song::Ptr song = phazite::createSong();
    ValueArea<NotesByClip> notes { song->getUndoManager(), NotesByClip() };
    std::vector<phazite::TrackID> tracks;
    ID made = 0; // counts what was made, for IDs, names and places of its own
};

// Where the song differs from a snapshot, or nothing.
juce::String describeDifference (const MixedSong& mixed, const Snapshot& expected)
{
    const auto now = mixed.take();
    auto differences = phazite::test::describeSongDifferences (expected.tree, now.tree);

    if (! now.notes.isSameAs (expected.notes))
        differences.add ("the notes are another value");

    if (! mixed.halvesAgree())
        differences.add ("the tree and the notes name other clips");

    return differences.joinIntoString ("; ");
}

double millisecondsOf (const std::function<void()>& f)
{
    const auto start = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count();
}

std::int64_t getHeapBytesInUse()
{
   #if JUCE_WINDOWS
    _HEAPINFO info;
    info._pentry = nullptr;
    std::int64_t used = 0;

    while (_heapwalk (&info) == _HEAPOK)
        if (info._useflag == _USEDENTRY)
            used += (std::int64_t) info._size;

    return used;
   #else
    return 0;
   #endif
}
} // namespace

//==============================================================================
TEST_CASE ("One history: a step that changes the tree and the notes' value is one step, undone and done again whole"
           * doctest::test_suite ("immer-spike"))
{
    MixedSong mixed;
    const auto empty = mixed.take();
    phazite::ClipID clip;

    {
        const phazite::UndoStep step (*mixed.song, "Add Clip");
        clip = mixed.insertClip();
    }

    const auto withClip = mixed.take();
    CHECK (mixed.halvesAgree());

    {
        const phazite::UndoStep step (*mixed.song, "Grow and Add");
        mixed.changeTree (clip, 2); // the clip longer: the tree's
        mixed.addNote (clip);       // a note in it: the value's
        mixed.changeTree (clip, 0); // a track renamed, a step of its own that joins this one
        mixed.addNote (clip);
    }

    const auto grown = mixed.take();
    CHECK (phazite::getHistorySteps (*mixed.song).done == juce::StringArray { "Add Clip", "Grow and Add" });
    CHECK (mixed.notes.countActions() == 2); // one for each step, however often the step changed the notes

    phazite::undo (*mixed.song);
    CHECK (describeDifference (mixed, withClip) == juce::String());
    phazite::undo (*mixed.song);
    CHECK (describeDifference (mixed, empty) == juce::String());
    CHECK (phazite::getHistorySteps (*mixed.song).ahead == juce::StringArray { "Add Clip", "Grow and Add" });
    phazite::redo (*mixed.song);
    CHECK (describeDifference (mixed, withClip) == juce::String());
    phazite::redo (*mixed.song);
    CHECK (describeDifference (mixed, grown) == juce::String());
    CHECK_FALSE (phazite::canRedo (*mixed.song));

    // A clip deleted takes its notes along, and Undo brings both back.
    {
        const phazite::UndoStep step (*mixed.song, "Delete Clip");
        mixed.deleteClip (clip);
    }

    CHECK (mixed.notes.get().clips.size() == 0);
    CHECK (mixed.halvesAgree());
    phazite::undo (*mixed.song);
    CHECK (describeDifference (mixed, grown) == juce::String());
}

TEST_CASE ("One history: a drag is one step with one action for the notes, however the tree's changes fall between theirs; "
           "given up, it leaves the song and its redo steps as they were"
           * doctest::test_suite ("immer-spike"))
{
    MixedSong mixed;
    phazite::ClipID clip;

    {
        const phazite::UndoStep step (*mixed.song, "Add Clip");
        clip = mixed.insertClip();
    }

    for (int i = 0; i < 3; ++i)
    {
        const phazite::UndoStep step (*mixed.song, "Add Note " + juce::String (i));
        mixed.addNote (clip);
    }

    const auto start = mixed.take();
    const auto actionsAtStart = mixed.notes.countActions();
    int told = 0;
    NotesByClip toldBefore, toldAfter;
    mixed.notes.addListener (
        [&] (const NotesByClip& before, const NotesByClip& after)
        {
            ++told;
            toldBefore = before;
            toldAfter = after;
        });

    {
        phazite::Gesture gesture (*mixed.song, "Move Notes");

        for (int step = 0; step < 20; ++step)
        {
            mixed.changeNotes (clip, 1);

            if (step % 3 == 0)
                mixed.changeTree (clip, 2); // the clip grown between the notes' changes

            if (step % 5 == 0)
            {
                const phazite::UndoStep joins (*mixed.song, "Joins");
                mixed.changeNotes (clip, 1);
            }
        }

        gesture.commit();
    }

    const auto moved = mixed.take();
    CHECK (told == 24);
    CHECK (mixed.notes.countActions() == actionsAtStart + 1);
    CHECK (phazite::getHistorySteps (*mixed.song).done.size() == 5);

    told = 0;
    phazite::undo (*mixed.song);
    CHECK (told == 1); // the whole drag, told as one change
    CHECK (toldBefore.isSameAs (moved.notes));
    CHECK (toldAfter.isSameAs (start.notes));
    CHECK (describeDifference (mixed, start) == juce::String());
    phazite::redo (*mixed.song);
    CHECK (describeDifference (mixed, moved) == juce::String());

    // Two steps undone, then a drag given up: the song as the drag found it, and the two steps still there to redo.
    phazite::undo (*mixed.song);
    phazite::undo (*mixed.song);
    const auto before = mixed.take();
    const auto ahead = phazite::getHistorySteps (*mixed.song).ahead;
    REQUIRE (ahead.size() == 2);

    {
        phazite::Gesture gesture (*mixed.song, "Move Notes");

        for (int step = 0; step < 5; ++step)
        {
            mixed.changeNotes (clip, 1);
            mixed.changeTree (clip, step % 3);
        }

        CHECK_FALSE (mixed.take().notes.isSameAs (before.notes));
        gesture.rollBack();
    }

    CHECK (describeDifference (mixed, before) == juce::String());
    CHECK (phazite::getHistorySteps (*mixed.song).ahead == ahead);
    phazite::redo (*mixed.song);
    phazite::redo (*mixed.song);
    CHECK (describeDifference (mixed, moved) == juce::String());
}

TEST_CASE ("One history: a trial tries changes of both kinds, from its start again, and is kept or put back; and gives way to an undo"
           * doctest::test_suite ("immer-spike"))
{
    MixedSong mixed;
    phazite::ClipID clip;

    {
        const phazite::UndoStep step (*mixed.song, "Add Clip");
        clip = mixed.insertClip();
    }

    const auto start = mixed.take();

    {
        phazite::Trial trial (*mixed.song, "Try", [] {});
        trial.apply (
            [&]
            {
                mixed.changeTree (clip, 0);
                mixed.addNote (clip);
            });
        CHECK (describeDifference (mixed, start) != juce::String());

        // From the start again: the rename and the first note taken back, another note tried.
        trial.apply ([&] { mixed.addNote (clip); }, true);
        const auto tried = mixed.take();
        CHECK (tried.tree == start.tree);
        CHECK (tried.notes.clips.find (clip.getRaw())->size() == start.notes.clips.find (clip.getRaw())->size() + 1);

        trial.putBack();
        CHECK (describeDifference (mixed, start) == juce::String());
        CHECK_FALSE (phazite::canRedo (*mixed.song));
    }

    {
        phazite::Trial trial (*mixed.song, "Try", [] {});
        trial.apply (
            [&]
            {
                mixed.changeTree (clip, 2);
                mixed.addNote (clip);
            });
        trial.keep ("Kept");
    }

    const auto kept = mixed.take();
    CHECK (phazite::getHistorySteps (*mixed.song).done == juce::StringArray { "Add Clip", "Kept" });
    phazite::undo (*mixed.song);
    CHECK (describeDifference (mixed, start) == juce::String());
    phazite::redo (*mixed.song);
    CHECK (describeDifference (mixed, kept) == juce::String());

    // An undo while a trial has changes: the trial's owner keeps them first, as a step, which that undo takes back.
    {
        std::unique_ptr<phazite::Trial> trial;
        trial = std::make_unique<phazite::Trial> (*mixed.song, "Try", [&trial] { trial->keep ("Tried"); });
        trial->apply ([&] { mixed.addNote (clip); });
        phazite::undo (*mixed.song);
        CHECK (describeDifference (mixed, kept) == juce::String());
        CHECK (phazite::getHistorySteps (*mixed.song).ahead == juce::StringArray { "Tried" });
    }
}

TEST_CASE ("One history: the steps the history lets go take their values along" * doctest::test_suite ("immer-spike"))
{
    const auto stepsBefore = phazite::Song::getUndoSteps();
    phazite::Song::setUndoSteps (phazite::Song::minimumUndoSteps);

    {
        MixedSong mixed;
        phazite::ClipID clip;

        {
            const phazite::UndoStep step (*mixed.song, "Add Clip");
            clip = mixed.insertClip();
        }

        for (int i = 0; i < 40; ++i)
        {
            const phazite::UndoStep step (*mixed.song, "Add Note");
            mixed.addNote (clip);
        }

        CHECK (mixed.notes.countActions() == phazite::Song::minimumUndoSteps);

        // And the history cleared, all of them.
        mixed.song->getUndoManager().clearUndoHistory();
        CHECK (mixed.notes.countActions() == 0);
        CHECK (mixed.notes.get().clips.find (clip.getRaw())->size() == 41);
    }

    phazite::Song::setUndoSteps (stepsBefore);
}

TEST_CASE ("One history: 1,500 changes, drags, trials, undos and redos drawn by lot, the song at each the one a history of whole "
           "songs holds"
           * doctest::test_suite ("immer-spike"))
{
    struct Step
    {
        juce::String name;
        Snapshot song; // done: the song before the step; ahead: the song after it
    };

    const auto stepsBefore = phazite::Song::getUndoSteps();
    phazite::Song::setUndoSteps (10000); // every step kept, so that the walk reaches the start

    for (const auto seed : { 20261121, 7, 424242 })
    {
        MixedSong mixed;
        juce::Random random (seed);
        std::vector<Step> done, ahead;
        auto failed = false;
        int steps = 0, drags = 0, givenUp = 0, trials = 0, undos = 0, redos = 0;

        const auto check = [&] (int operation, const char* what, const Snapshot& expected)
        {
            auto differences = describeDifference (mixed, expected);
            const auto history = phazite::getHistorySteps (*mixed.song);
            juce::StringArray doneNames, aheadNames;

            for (const auto& step : done)
                doneNames.add (step.name);

            for (auto step = ahead.rbegin(); step != ahead.rend(); ++step)
                aheadNames.add (step->name);

            if (history.done != doneNames || history.ahead != aheadNames)
                differences << "; the history's steps are others (" << history.done.size() << " done, " << history.ahead.size()
                            << " ahead; " << doneNames.size() << " and " << aheadNames.size() << " expected)";

            if (phazite::canUndo (*mixed.song) == done.empty() || phazite::canRedo (*mixed.song) == ahead.empty())
                differences << "; Undo or Redo is offered wrongly";

            if (differences.isNotEmpty())
            {
                failed = true;
                FAIL_CHECK ("seed " << seed << ", operation " << operation << " (" << what << "): " << differences);
            }
        };

        for (int operation = 0; operation < 1500 && ! failed; ++operation)
        {
            const auto name = "Step " + juce::String (operation);
            const auto before = mixed.take();
            const auto lot = random.nextInt (100);

            if (lot < 45)
            {
                mixed.changeInStep (name, random);
                done.push_back ({ name, before });
                ahead.clear();
                ++steps;
                check (operation, "a step", mixed.take());
            }
            else if (lot < 60)
            {
                const auto giveUp = random.nextInt (10) < 4;
                phazite::Gesture gesture (*mixed.song, name);

                for (int change = 0, changes = 1 + random.nextInt (8); change < changes; ++change)
                {
                    if (random.nextBool())
                        mixed.changeInStep ("Joins", random);
                    else
                        mixed.changeSomething (random);
                }

                if (giveUp)
                {
                    gesture.rollBack();
                    ++givenUp;
                    check (operation, "a drag given up", before);
                }
                else
                {
                    gesture.commit();
                    done.push_back ({ name, before });
                    ahead.clear();
                    ++drags;
                    check (operation, "a drag", mixed.take());
                }
            }
            else if (lot < 68)
            {
                const auto keep = random.nextBool();
                phazite::Trial trial (*mixed.song, "Try", [] {});

                for (int change = 0, changes = 1 + random.nextInt (3); change < changes; ++change)
                    trial.apply ([&] { mixed.changeSomething (random); }, random.nextBool());

                ++trials;

                if (keep)
                {
                    trial.keep (name);
                    done.push_back ({ name, before });
                    ahead.clear();
                    check (operation, "a trial kept", mixed.take());
                }
                else
                {
                    trial.putBack();
                    check (operation, "a trial put back", before);
                }
            }
            else if (lot < 86)
            {
                if (done.empty())
                    continue;

                phazite::undo (*mixed.song);
                const auto expected = done.back().song;
                ahead.push_back ({ done.back().name, before });
                done.pop_back();
                ++undos;
                check (operation, "an undo", expected);
            }
            else
            {
                if (ahead.empty())
                    continue;

                phazite::redo (*mixed.song);
                const auto expected = ahead.back().song;
                done.push_back ({ ahead.back().name, before });
                ahead.pop_back();
                ++redos;
                check (operation, "a redo", expected);
            }
        }

        // Back to the start, and forth to the end: as many steps forth as back, since the end may have steps to redo.
        const auto waiting = ahead.size();
        const auto end = mixed.take();

        while (! failed && ! done.empty())
        {
            const auto now = mixed.take();
            phazite::undo (*mixed.song);
            const auto expected = done.back().song;
            ahead.push_back ({ done.back().name, now });
            done.pop_back();
            check (-1, "the walk back", expected);
        }

        while (! failed && ahead.size() > waiting)
        {
            const auto now = mixed.take();
            phazite::redo (*mixed.song);
            const auto expected = ahead.back().song;
            done.push_back ({ ahead.back().name, now });
            ahead.pop_back();
            check (-1, "the walk forth", expected);
        }

        CHECK (describeDifference (mixed, end) == juce::String());
        MESSAGE ("seed " << seed << ": " << steps << " steps, " << drags << " drags kept, " << givenUp << " given up, " << trials
                         << " trials, " << undos << " undos, " << redos << " redos; " << (int) done.size() << " steps kept at the end, "
                         << mixed.notes.countActions() << " actions for the notes, " << (int) mixed.getClips().size() << " clips");
    }

    phazite::Song::setUndoSteps (stepsBefore);
}

TEST_CASE ("One history: a clip's notes read into a value and written back are the clip's element as it was"
           * doctest::test_suite ("immer-spike"))
{
    // The project file can stay as it is while an area is a value: read into the value as the song opens, written from it
    // into the copy of the song a save writes.
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    const auto clip = phazite::insertMidiClip (*song, track, { phazite::test::at (0.0), phazite::test::at (64.0) }, nullptr);
    std::vector<phazite::NoteData> made;

    for (int i = 0; i < 200; ++i)
        made.push_back ({ .clip = clip,
                          .pitch = 40 + i % 40,
                          .start = phazite::test::at (i * 0.25),
                          .length = phazite::test::lasting (0.2),
                          .velocity = 1 + i % 127,
                          .muted = i % 7 == 0,
                          .slide = i % 11 == 0,
                          .portamento = i % 13 == 0,
                          .channel = 1 + i % 16 });

    phazite::setClipNotes (*song, clip, made);
    const auto clipState = phazite::findClipState (*song, clip);
    auto read = immer::flex_vector<phazite::NoteValues>().transient();
    phazite::forEachNote (clipState, [&read] (const phazite::NoteElement& note) { read.push_back (note.getValues()); });
    const auto value = read.persistent();
    REQUIRE (value.size() == 200);

    auto written = clipState.createCopy();
    phazite::removeAllNotes (written, nullptr);
    REQUIRE_FALSE (written.isEquivalentTo (clipState));

    for (const auto& note : value)
        phazite::appendNote (written, note, nullptr);

    CHECK (written.isEquivalentTo (clipState));
}

//==============================================================================
TEST_CASE ("Benchmark: one history for a tree and a value: what the notes' changes cost through the song's history, and what a "
           "drag keeps"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    // A clip's 5,000 notes as a value, 500 of them moved at each of a drag's 20 steps, through the song's history: the step
    // (the notes changed, and the area given them), the area's part alone, the same with 1,500 changes of the tree's in
    // every step before it (500 notes of another clip, held by the tree, moved by the model: the history's transaction
    // grows to 30,000 actions, which the area looks through for its own), the drag undone, and what a drag kept holds.
    constexpr size_t count = 5000, moved = 500;
    MixedSong mixed;
    phazite::ClipID clip;

    {
        const phazite::UndoStep step (*mixed.song, "Add Clip");
        clip = mixed.insertClip();
    }

    {
        auto notes = immer::flex_vector<SpikeNote>().transient();

        for (size_t i = 0; i < count; ++i)
            notes.push_back ({ i + 1, 48 + (int) (i % 24), (double) i * 0.25, 0.2 });

        mixed.notes.setOutsideHistory ({ mixed.notes.get().clips.set (clip.getRaw(), notes.persistent()) });
    }

    // The tree's clip, with notes the tree holds.
    const auto treeClip =
        phazite::insertMidiClip (*mixed.song, mixed.tracks[2], { phazite::test::at (0.0), phazite::test::at (4096.0) }, nullptr);
    std::vector<phazite::NoteData> made;

    for (size_t i = 0; i < moved; ++i)
        made.push_back ({ .clip = treeClip, .pitch = 60, .start = phazite::test::at ((double) i), .length = phazite::test::lasting (0.2) });

    phazite::setClipNotes (*mixed.song, treeClip, made);
    const auto treeNotes = phazite::getClipNotes (*mixed.song, treeClip);
    mixed.song->getUndoManager().clearUndoHistory();

    const auto moveNotes = [&] (const NotesByClip& from)
    {
        auto notes = from.clips.find (clip.getRaw())->transient();

        for (size_t i = 0; i < moved; ++i)
            notes.update (i * 10,
                          [] (SpikeNote note)
                          {
                              note.start += 0.25;
                              return note;
                          });

        return NotesByClip { from.clips.set (clip.getRaw(), notes.persistent()) };
    };

    const auto median = [] (std::vector<double> times)
    {
        std::sort (times.begin(), times.end());
        return times[times.size() / 2];
    };

    std::vector<double> step, areaAlone, areaAmongTree, undone, undoneAmongTree;

    for (int drag = 0; drag < 6; ++drag)
    {
        const auto amongTree = drag % 2 == 1;
        const auto start = mixed.take().notes;

        {
            phazite::Gesture gesture (*mixed.song, "Move Notes");

            for (int s = 1; s <= 20; ++s)
            {
                if (amongTree)
                {
                    auto placed = treeNotes;

                    for (auto& note : placed)
                    {
                        note.start = note.start + phazite::test::lasting (0.01 * s);
                        note.pitch = 60 + s;
                        note.length = phazite::test::lasting (0.2 + 0.01 * s);
                    }

                    phazite::placeNotes (*mixed.song, placed);
                }

                NotesByClip next;
                const auto changing = millisecondsOf ([&] { next = moveNotes (mixed.notes.get()); });
                const auto setting = millisecondsOf ([&] { mixed.notes.set (next); });
                (amongTree ? areaAmongTree : areaAlone).push_back (setting);

                if (! amongTree)
                    step.push_back (changing + setting);
            }

            gesture.commit();
        }

        (amongTree ? undoneAmongTree : undone).push_back (millisecondsOf ([&] { phazite::undo (*mixed.song); }));
        CHECK (mixed.notes.get().isSameAs (start));
    }

    // What a drag kept holds: 40 of them, there by turns (each a step), with immer's kept blocks taken first.
    constexpr int kept = 40;
    std::vector<immer::flex_vector<SpikeNote>> held;
    auto many = immer::flex_vector<SpikeNote>().transient();

    for (size_t i = 0; i < 20000; ++i)
        many.push_back ({ i + 1 });

    held.push_back (many.persistent());
    // The history cleared first. JUCE keeps the redo steps it put aside last through that (the drag undone last, here):
    // one action, and its two roots, that outlive the clear until redo steps are put aside again.
    mixed.song->getUndoManager().clearUndoHistory();
    const auto actionsBefore = mixed.notes.countActions();
    CHECK (actionsBefore == 1);
    const auto heapBefore = getHeapBytesInUse();

    for (int drag = 0; drag < kept; ++drag)
    {
        phazite::Gesture gesture (*mixed.song, "Move Notes");

        for (int s = 1; s <= 20; ++s)
            mixed.notes.set (moveNotes (mixed.notes.get()));

        gesture.commit();
    }

    const auto heapAfter = getHeapBytesInUse();
    CHECK (mixed.notes.countActions() == actionsBefore + kept);
    CHECK (held.front().size() == 20000);

    MESSAGE ("\n| A clip's 5,000 notes as a value in the song's own history, 500 moved at each of a drag's 20 steps | Median |\n|---|---|\n"
             << "| A step: the notes changed, and the area given them | " << juce::String (median (step), 4) << " ms |\n"
             << "| ... of which the area's part (its action found in the step, or performed) | "
             << juce::String (median (areaAlone) * 1000.0, 2) << " us |\n"
             << "| The area's part where the tree changed 1,500 times in every step too (30,000 actions by the drag's end) | "
             << juce::String (median (areaAmongTree) * 1000.0, 2) << " us |\n"
             << "| The drag undone, the notes alone | " << juce::String (median (undone), 4) << " ms |\n"
             << "| The drag undone, with the tree's 30,000 changes | " << juce::String (median (undoneAmongTree), 2) << " ms |\n"
             << "| What a drag kept holds, each of 40 | " << juce::String ((double) (heapAfter - heapBefore) / kept / 1024.0, 1)
             << " KB |\n");
}
