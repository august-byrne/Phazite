// SPDX-License-Identifier: AGPL-3.0-or-later
// An area of the song held as an immutable value, in the song's one undo history (model/base/internal/ValueArea.h): a step,
// a gesture and a trial (model/commands/internal/Undo.h) hold a tree's changes and a value's alike, and Undo and Redo take
// both back and forth, with nothing of theirs changed. However many times an area changes within a step, the step holds one
// action for it (the value before, the value after), wherever the tree's changes fall between them.
// Here the song is a real one (its tracks, clips, names and volumes in its tree, changed by the model's functions), and the
// clips' notes are a value beside it, by their clips' IDs: as the song would be once the notes had moved and the clips not.
//   phazite_tests.exe --test-case="Value area*"
//   phazite_tests.exe --test-suite=benchmark --no-skip --test-case="*one history*"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/ChordLaneValue.h"
#include "model/base/internal/ClipNotesValue.h"
#include "model/base/internal/ClipsValue.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/Immer.h"
#include "model/base/internal/MapChanges.h"
#include "model/base/internal/MidiLinksValue.h"
#include "model/base/internal/SectionsValue.h"
#include "model/base/internal/TempoMapValue.h"
#include "model/base/internal/ValueArea.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#if JUCE_WINDOWS
 #include <malloc.h>
#endif

namespace
{
using ID = std::uint64_t;

// A listener that counts what it is told, keeps the values of the last change, and does what a test asks as it is told.
template <typename Value>
struct Told final : phazite::ValueArea<Value>::Listener
{
    int count = 0;
    Value before {}, after {};
    std::function<void()> also;

    void valueAreaChanged (const Value& valueBefore, const Value& valueAfter) override
    {
        ++count;
        before = valueBefore;
        after = valueAfter;

        if (also != nullptr)
            also();
    }
};

//==============================================================================
// The notes of the song's clips as a value, by their clips' IDs.
struct TestNote
{
    ID id = 0;
    int pitch = 60;
    double start = 0.0, length = 1.0;

    bool operator== (const TestNote&) const = default;
};

struct NotesByClip
{
    immer::map<ID, immer::flex_vector<TestNote>> clips;

    bool isSameAs (const NotesByClip& other) const { return clips.identity() == other.clips.identity(); }
};

// A value that says by a free function in its own namespace whether two are the same: here, by its version alone.
struct Versioned
{
    int version = 0;
    juce::String text;
};

bool isSameValue (const Versioned& a, const Versioned& b)
{
    return a.version == b.version;
}

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

    std::vector<phazite::ClipID> getClips() const { return phazite::getAllClips (*song); }

    // Whether the two halves name the same clips: each clip of the tree has its notes, and no notes are a clip's that is gone.
    bool halvesAgree() const
    {
        std::set<ID> inTree, inValue;

        for (const auto clip : getClips())
            inTree.insert (clip.getRaw());

        for (const auto& entry : notes.get().clips)
            inValue.insert (entry.first);

        return inTree == inValue;
    }

    //==========================================================================
    void addNote (phazite::ClipID clip)
    {
        const auto id = ++made;
        const TestNote note { id, 48 + (int) (id % 24), (double) (id % 16) * 0.25, 0.25 };
        const auto* held = notes.get().clips.find (clip.getRaw());
        notes.set ({ notes.get().clips.set (clip.getRaw(), (held != nullptr ? *held : immer::flex_vector<TestNote>()).push_back (note)) });
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
                              [] (TestNote note)
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
            const auto value = phazite::getClipValue (*song, clip);
            REQUIRE (value.has_value());
            const auto range = phazite::getClipRange (*value);
            phazite::setClipPlace (*song, clip, range.getStart(), range.getLength() + phazite::BeatDuration::fromBeats (4.0),
                                   value->offset);
        }
    }

    // A clip made, with its notes: the tree's half and the value's in the step in progress.
    phazite::ClipID insertClip()
    {
        // Where no clip was or will be, whatever is undone meanwhile, and within the furthest beat a song holds a clip at (Schema.h:
        // maxBeats, which a clip read back from a whole copy is kept within: tests/SongCheck.cpp, the state's round trip).
        const auto place = (double) (++made) * 256.0;
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
    phazite::ValueArea<NotesByClip> notes { song->getUndoManager(), NotesByClip() };
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

phazite::NoteData makeNoteData (phazite::ClipID clip, int pitch, double start, double length)
{
    phazite::NoteData note;
    note.clip = clip;
    note.pitch = pitch;
    note.start = phazite::test::at (start);
    note.length = phazite::test::lasting (length);
    return note;
}

//==============================================================================
// A keyed value (ValueArea.h, KeyedAreaValue) of two maps, as an area of entries by their IDs will be: values by ID, and ordered
// lists of IDs by their owners' IDs. Its Undo and Redo move only the entries their step changed.
struct Keyed
{
    static constexpr bool undoesByEntry = true;

    immer::map<ID, int> values {};
    immer::map<ID, immer::flex_vector<ID>> lists {};
};

bool isSameValue (const Keyed& a, const Keyed& b)
{
    return a.values.identity() == b.values.identity() && a.lists.identity() == b.lists.identity();
}

struct KeyedChanges
{
    std::vector<phazite::MapChange<ID, int>> values {};
    std::vector<phazite::MapChange<ID, immer::flex_vector<ID>>> lists {};
};

int keyedChangesFound = 0; // how often findChanges() below was called: what a keyed area's work costs

KeyedChanges findChanges (const Keyed& before, const Keyed& after)
{
    ++keyedChangesFound;
    return { phazite::findMapChanges (before.values, after.values), phazite::findMapChanges (before.lists, after.lists) };
}

Keyed withChanges (const Keyed& now, const KeyedChanges& changes, phazite::ChangeDirection direction)
{
    return { phazite::withMapChanges (now.values, changes.values, direction),
             phazite::withMapChanges (now.lists, changes.lists, direction) };
}

// A value that gives the keyed functions but doesn't say it is keyed, and one that says it isn't: both plain.
struct Unmarked
{
    immer::map<ID, int> values {};

    bool operator== (const Unmarked&) const = default;
};

struct MarkedPlain
{
    static constexpr bool undoesByEntry = false;

    immer::map<ID, int> values {};
};

std::vector<phazite::MapChange<ID, int>> findChanges (const Unmarked& before, const Unmarked& after)
{
    return phazite::findMapChanges (before.values, after.values);
}

Unmarked withChanges (const Unmarked& now, const std::vector<phazite::MapChange<ID, int>>& changes, phazite::ChangeDirection direction)
{
    return { phazite::withMapChanges (now.values, changes, direction) };
}

static_assert (phazite::KeyedAreaValue<Keyed>);
static_assert (phazite::detail::GivesKeyedChanges<Unmarked>);
static_assert (! phazite::KeyedAreaValue<Unmarked>);
static_assert (! phazite::KeyedAreaValue<MarkedPlain>);
static_assert (! phazite::KeyedAreaValue<NotesByClip>);
static_assert (! phazite::KeyedAreaValue<int>);
static_assert (! phazite::KeyedAreaValue<immer::vector<int>>);

// None of the areas the song holds today is keyed: each Undo puts its whole value back. The devices' area will be the first
// keyed one (a plug-in's parameter written outside the history by the plug-in itself).
static_assert (! phazite::KeyedAreaValue<phazite::NotesByClip>);
static_assert (! phazite::KeyedAreaValue<phazite::SongClips>);
static_assert (! phazite::KeyedAreaValue<phazite::TempoMapValue>);
static_assert (! phazite::KeyedAreaValue<phazite::ChordLaneValue>);
static_assert (! phazite::KeyedAreaValue<phazite::MidiLinksValue>);
static_assert (! phazite::KeyedAreaValue<phazite::SongCurves>);
static_assert (! phazite::KeyedAreaValue<phazite::SectionsValue>);

// A key of its own, with a hash of its own, as SongCurves' map has (by its parameters).
struct OwnKey
{
    ID owner = 0;
    int index = 0;

    bool operator== (const OwnKey&) const = default;
};

struct OwnKeyHash
{
    size_t operator() (const OwnKey& key) const noexcept { return std::hash<ID>() (key.owner * 31 + (ID) key.index); }
};

// The keyed value's contents as text, each map by its keys in order: "1=10 2=20 | 7:[1,2]".
juce::String describe (const Keyed& value)
{
    std::map<ID, int> values;
    std::map<ID, immer::flex_vector<ID>> lists;

    for (const auto& [id, held] : value.values)
        values[id] = held;

    for (const auto& [owner, list] : value.lists)
        lists[owner] = list;

    juce::StringArray words;

    for (const auto& [id, held] : values)
        words.add (juce::String ((juce::int64) id) + "=" + juce::String (held));

    words.add ("|");

    for (const auto& [owner, list] : lists)
    {
        juce::StringArray ids;

        for (const auto id : list)
            ids.add (juce::String ((juce::int64) id));

        words.add (juce::String ((juce::int64) owner) + ":[" + ids.joinIntoString (",") + "]");
    }

    return words.joinIntoString (" ");
}

// A song's history with a keyed area in it, and the changes a test makes to the area: in the history (set()) or outside it.
struct KeyedSong
{
    KeyedSong() { song->getUndoManager().clearUndoHistory(); }

    juce::String now() const { return describe (area.get()); }

    static Keyed withValue (const Keyed& from, ID id, std::optional<int> value)
    {
        return { value.has_value() ? from.values.set (id, *value) : from.values.erase (id), from.lists };
    }

    static Keyed withAppended (const Keyed& from, ID owner, ID id)
    {
        const auto* held = from.lists.find (owner);
        return { from.values, from.lists.set (owner, (held != nullptr ? *held : immer::flex_vector<ID>()).push_back (id)) };
    }

    void set (ID id, std::optional<int> value) { area.set (withValue (area.get(), id, value)); }
    void append (ID owner, ID id) { area.set (withAppended (area.get(), owner, id)); }
    void setOutside (ID id, std::optional<int> value) { area.setOutsideHistory (withValue (area.get(), id, value)); }
    void appendOutside (ID owner, ID id) { area.setOutsideHistory (withAppended (area.get(), owner, id)); }

    void inStep (const juce::String& name, const std::function<void()>& change)
    {
        const phazite::UndoStep step (*song, name);
        change();
    }

    phazite::Song::Ptr song = phazite::createSong();
    phazite::ValueArea<Keyed> area { song->getUndoManager(), Keyed() };
};

// The same contents in maps of their own, which share nothing with the value's: alike, not the same.
Keyed rebuilt (const Keyed& value)
{
    auto values = immer::map<ID, int>().transient();
    auto lists = immer::map<ID, immer::flex_vector<ID>>().transient();

    for (const auto& [id, held] : value.values)
        values.set (id, held);

    for (const auto& [owner, list] : value.lists)
        lists.set (owner, list);

    return { std::move (values).persistent(), std::move (lists).persistent() };
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
TEST_CASE ("Value area: a step that changes the tree and the notes' value is one step, undone and done again whole")
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

TEST_CASE ("Value area: a drag is one step with one action for the notes, however the tree's changes fall between theirs; "
           "given up, it leaves the song and its redo steps as they were")
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
    Told<NotesByClip> told;
    mixed.notes.addListener (&told);

    {
        phazite::Gesture gesture (*mixed.song, "Move Notes");

        for (int move = 0; move < 20; ++move)
        {
            mixed.changeNotes (clip, 1);

            if (move % 3 == 0)
                mixed.changeTree (clip, 2); // the clip grown between the notes' changes

            if (move % 5 == 0)
            {
                const phazite::UndoStep joins (*mixed.song, "Joins");
                mixed.changeNotes (clip, 1);
            }
        }

        gesture.commit();
    }

    const auto moved = mixed.take();
    CHECK (told.count == 24);
    CHECK (mixed.notes.countActions() == actionsAtStart + 1);
    CHECK (phazite::getHistorySteps (*mixed.song).done.size() == 5);

    told.count = 0;
    phazite::undo (*mixed.song);
    CHECK (told.count == 1); // the whole drag, told as one change
    CHECK (told.before.isSameAs (moved.notes));
    CHECK (told.after.isSameAs (start.notes));
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

        for (int move = 0; move < 5; ++move)
        {
            mixed.changeNotes (clip, 1);
            mixed.changeTree (clip, move % 3);
        }

        CHECK_FALSE (mixed.take().notes.isSameAs (before.notes));
        gesture.rollBack();
    }

    CHECK (describeDifference (mixed, before) == juce::String());
    CHECK (phazite::getHistorySteps (*mixed.song).ahead == ahead);
    phazite::redo (*mixed.song);
    phazite::redo (*mixed.song);
    CHECK (describeDifference (mixed, moved) == juce::String());
    mixed.notes.removeListener (&told);
}

TEST_CASE ("Value area: a trial tries changes of both kinds, from its start again, and is kept or put back; and gives way to an "
           "undo")
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

TEST_CASE ("Value area: the steps the history lets go take their values along")
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

TEST_CASE ("Value area: 1,500 changes, drags, trials, undos and redos drawn by lot, the song at each the one a history of whole "
           "songs holds")
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

            // And the song's own notes' values (the clips' made and deleted here) kept with its tree (SongCheck.h: values).
            for (const auto& problem : phazite::test::findValueProblems (*mixed.song))
                differences << "; " << juce::String (problem);

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

TEST_CASE ("Value area: a listener may remove itself, or another not told yet, while it is told")
{
    auto song = phazite::createSong();
    phazite::ValueArea<int> area (song->getUndoManager(), 0);
    Told<int> first, second, third;
    area.addListener (&first);
    area.addListener (&second);
    area.addListener (&third);
    first.also = [&] { area.removeListener (&first); };
    second.also = [&]
    {
        if (second.count == 1)
            area.removeListener (&third);
    };

    {
        const phazite::UndoStep step (*song, "One");
        area.set (1);
    }

    CHECK (first.count == 1);
    CHECK (second.count == 1);
    CHECK (third.count == 0); // removed before its turn came

    phazite::undo (*song);
    CHECK (area.get() == 0);
    CHECK (first.count == 1);
    CHECK (second.count == 2);
    CHECK (second.before == 1);
    CHECK (second.after == 0);
    CHECK (third.count == 0);
    area.removeListener (&second);
}

TEST_CASE ("Value area: two areas of one song changed in one step are an action each, and Undo and Redo take both back and "
           "forth")
{
    MixedSong mixed;
    phazite::ValueArea<immer::vector<int>> other (mixed.song->getUndoManager(), immer::vector<int>());
    Told<NotesByClip> notesTold;
    Told<immer::vector<int>> otherTold;
    mixed.notes.addListener (&notesTold);
    other.addListener (&otherTold);
    phazite::ClipID clip;

    {
        const phazite::UndoStep step (*mixed.song, "Add Clip");
        clip = mixed.insertClip();
    }

    const auto start = mixed.take();
    const auto otherAtStart = other.get();

    {
        const phazite::UndoStep step (*mixed.song, "Both");

        for (int i = 0; i < 5; ++i)
        {
            mixed.addNote (clip);
            other.set (other.get().push_back (i));
            mixed.changeTree (clip, i % 3);
        }
    }

    const auto both = mixed.take();
    const auto otherAfter = other.get();
    CHECK (phazite::getHistorySteps (*mixed.song).done == juce::StringArray { "Add Clip", "Both" });
    CHECK (mixed.notes.countActions() == 2);
    CHECK (other.countActions() == 1);
    CHECK (other.get().size() == 5);

    notesTold.count = otherTold.count = 0;
    phazite::undo (*mixed.song);
    CHECK (describeDifference (mixed, start) == juce::String());
    CHECK (other.get().identity() == otherAtStart.identity());
    CHECK (notesTold.count == 1);
    CHECK (otherTold.count == 1);

    phazite::redo (*mixed.song);
    CHECK (describeDifference (mixed, both) == juce::String());
    CHECK (other.get().identity() == otherAfter.identity());
    CHECK (notesTold.count == 2);
    CHECK (otherTold.count == 2);

    mixed.notes.removeListener (&notesTold);
    other.removeListener (&otherTold);
}

TEST_CASE ("Value area: an area gone before the song's history leaves the history's Undo and Redo safe, telling no one")
{
    auto song = phazite::createSong();
    const auto track = phazite::ensureTracks (*song, 1).front();
    song->getUndoManager().clearUndoHistory();
    auto area = std::make_unique<phazite::ValueArea<immer::vector<int>>> (song->getUndoManager(), immer::vector<int>());
    Told<immer::vector<int>> told;
    area->addListener (&told);

    {
        const phazite::UndoStep step (*song, "Both");
        area->set (area->get().push_back (1));
        phazite::renameTrack (*song, track, "Renamed");
    }

    {
        const phazite::UndoStep step (*song, "Again");
        area->set (area->get().push_back (2));
    }

    phazite::undo (*song); // one step undone, waiting to be redone, as the area goes
    CHECK (told.count == 3);
    area.reset();

    phazite::undo (*song);
    CHECK (phazite::getTrackName (*song, track) != "Renamed"); // the step's tree half undone all the same
    phazite::redo (*song);
    phazite::redo (*song);
    CHECK (phazite::getTrackName (*song, track) == "Renamed");
    CHECK (told.count == 3);
    CHECK_FALSE (phazite::canRedo (*song));

    song->getUndoManager().clearUndoHistory();
    CHECK_FALSE (phazite::canUndo (*song));
}

TEST_CASE ("Value area: a value set that is the same as the area's performs nothing, and a step with nothing else is no step")
{
    MixedSong mixed;
    Told<NotesByClip> told;
    mixed.notes.addListener (&told);

    {
        const phazite::UndoStep step (*mixed.song, "Nothing");
        mixed.notes.set (mixed.notes.get());
        mixed.notes.set (NotesByClip()); // an empty map is the one empty root
    }

    CHECK (told.count == 0);
    CHECK (mixed.notes.countActions() == 0);
    CHECK_FALSE (phazite::canUndo (*mixed.song));
    CHECK (phazite::getHistorySteps (*mixed.song).done.isEmpty());

    // And within a step that changes it, the same value again adds nothing to it.
    phazite::ClipID clip;

    {
        const phazite::UndoStep step (*mixed.song, "Add Clip");
        clip = mixed.insertClip();
        const auto now = mixed.notes.get();
        mixed.notes.set (now);
    }

    CHECK (told.count == 1);
    CHECK (mixed.notes.countActions() == 1);
    CHECK (phazite::getHistorySteps (*mixed.song).done == juce::StringArray { "Add Clip" });

    // A plain value, compared by ==.
    phazite::ValueArea<int> plain (mixed.song->getUndoManager(), 3);

    {
        const phazite::UndoStep step (*mixed.song, "Three");
        plain.set (3);
    }

    CHECK (plain.countActions() == 0);
    CHECK (phazite::getHistorySteps (*mixed.song).done == juce::StringArray { "Add Clip" });

    // A value whose type says by its own isSameValue: the same version is the same value, whatever else it holds.
    phazite::ValueArea<Versioned> versioned (mixed.song->getUndoManager(), Versioned { 1, "one" });

    {
        const phazite::UndoStep step (*mixed.song, "Same Version");
        versioned.set (Versioned { 1, "other" });
    }

    CHECK (versioned.countActions() == 0);
    CHECK (versioned.get().text == "one");

    {
        const phazite::UndoStep step (*mixed.song, "Next Version");
        versioned.set (Versioned { 2, "two" });
    }

    CHECK (versioned.countActions() == 1);
    phazite::undo (*mixed.song);
    CHECK (versioned.get().text == "one");
    mixed.notes.removeListener (&told);
}

//==============================================================================
TEST_CASE ("Value area, keyed: the changes between two maps, and a map with them moved back and forth")
{
    const auto before = immer::map<ID, int>().set (1, 10).set (2, 20).set (3, 30);
    const auto after = before.set (2, 21).erase (3).set (4, 40);
    auto changes = phazite::findMapChanges (before, after);
    std::sort (changes.begin(), changes.end(), [] (const auto& a, const auto& b) { return a.key < b.key; });

    REQUIRE (changes.size() == 3);
    CHECK ((changes[0].key == 2 && changes[0].before == 20 && changes[0].after == 21));
    CHECK ((changes[1].key == 3 && changes[1].before == 30 && ! changes[1].after.has_value()));
    CHECK ((changes[2].key == 4 && ! changes[2].before.has_value() && changes[2].after == 40));
    CHECK (phazite::findMapChanges (after, after).empty());

    // Moved back and forth on maps that hold other entries too: those stay, the changes' entries move.
    const auto back = phazite::withMapChanges (after.set (9, 90), changes, phazite::ChangeDirection::back);
    CHECK (back == before.set (9, 90));
    const auto forth = phazite::withMapChanges (before.set (9, 90), changes, phazite::ChangeDirection::forward);
    CHECK (forth == after.set (9, 90));

    // Where each of the changes' entries is so already, the very map comes back.
    CHECK (phazite::withMapChanges (after, changes, phazite::ChangeDirection::forward).identity() == after.identity());
    CHECK (phazite::withMapChanges (before, changes, phazite::ChangeDirection::back).identity() == before.identity());

    // A map with a hash of its own (as SongCurves' map has) is moved the same.
    using OwnMap = immer::map<OwnKey, int, OwnKeyHash>;
    const auto ownBefore = OwnMap().set ({ 1, 0 }, 10).set ({ 1, 1 }, 11).set ({ 2, 0 }, 20);
    const auto ownAfter = ownBefore.set ({ 1, 1 }, 12).erase ({ 2, 0 }).set ({ 3, 0 }, 30);
    const auto ownChanges = phazite::findMapChanges (ownBefore, ownAfter);
    CHECK (ownChanges.size() == 3);
    CHECK (phazite::findMapChanges (ownAfter, ownAfter).empty());
    const OwnMap ownBack = phazite::withMapChanges (ownAfter.set ({ 9, 9 }, 99), ownChanges, phazite::ChangeDirection::back);
    CHECK (ownBack == ownBefore.set ({ 9, 9 }, 99));
    const OwnMap ownForth = phazite::withMapChanges (ownBefore, ownChanges, phazite::ChangeDirection::forward);
    CHECK (ownForth == ownAfter);
    CHECK (phazite::withMapChanges (ownAfter, ownChanges, phazite::ChangeDirection::forward).identity() == ownAfter.identity());
}

TEST_CASE ("Value area, keyed: a value that gives the keyed functions but isn't marked keyed is plain: its Undo puts the whole "
           "value back")
{
    auto song = phazite::createSong();
    song->getUndoManager().clearUndoHistory();
    phazite::ValueArea<Unmarked> area { song->getUndoManager(), Unmarked() };

    {
        const phazite::UndoStep step (*song, "Step");
        area.set ({ area.get().values.set (1, 10) });
    }

    area.setOutsideHistory ({ area.get().values.set (2, 20) });
    const auto written = area.get();
    phazite::undo (*song);
    CHECK (area.get().values.empty()); // the write outside the history lost with the whole value, as for any plain value

    // The functions it gives work; the area doesn't call them.
    const auto changes = findChanges (Unmarked(), written);
    CHECK (changes.size() == 2);
    CHECK (withChanges (Unmarked(), changes, phazite::ChangeDirection::forward).values == written.values);
}

TEST_CASE ("Value area, keyed: with nothing written outside the history, Undo and Redo move to the step's very values, one "
           "action for each step, a drag's too")
{
    KeyedSong keyed;
    const auto start = keyed.area.get();

    keyed.inStep ("One",
                  [&]
                  {
                      keyed.set (1, 10);
                      keyed.append (7, 1);
                  });

    const auto one = keyed.area.get();
    CHECK (keyed.area.countActions() == 1);

    {
        phazite::Gesture gesture (*keyed.song, "Drag");

        for (int move = 0; move < 20; ++move)
        {
            keyed.set (2, move);

            if (move % 5 == 0)
                keyed.inStep ("Joins", [&] { keyed.append (7, (ID) (100 + move)); });
        }

        gesture.commit();
    }

    const auto dragged = keyed.area.get();
    CHECK (keyed.area.countActions() == 2);
    CHECK (phazite::getHistorySteps (*keyed.song).done == juce::StringArray { "One", "Drag" });
    CHECK (describe (dragged) == "1=10 2=19 | 7:[1,100,105,110,115]");

    phazite::undo (*keyed.song);
    CHECK (isSameValue (keyed.area.get(), one));
    phazite::undo (*keyed.song);
    CHECK (isSameValue (keyed.area.get(), start));
    phazite::redo (*keyed.song);
    CHECK (isSameValue (keyed.area.get(), one));
    phazite::redo (*keyed.song);
    CHECK (isSameValue (keyed.area.get(), dragged));
    CHECK (keyed.area.countActions() == 2);
}

TEST_CASE ("Value area, keyed: what is written outside the history between a step and its Undo survives the Undo and the Redo, "
           "and the step's own entries go back and forth")
{
    KeyedSong keyed;
    keyed.area.setOutsideHistory (KeyedSong::withValue (keyed.area.get(), 1, 1));

    keyed.inStep ("Step",
                  [&]
                  {
                      keyed.set (1, 10);
                      keyed.set (3, 30);
                      keyed.append (7, 1);
                  });

    CHECK (keyed.now() == "1=10 3=30 | 7:[1]");

    // Another value, and another owner's list in the same map as the step's list, written outside the history.
    keyed.setOutside (2, 20);
    keyed.appendOutside (8, 2);
    CHECK (keyed.now() == "1=10 2=20 3=30 | 7:[1] 8:[2]");

    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=1 2=20 | 8:[2]");
    phazite::redo (*keyed.song);
    CHECK (keyed.now() == "1=10 2=20 3=30 | 7:[1] 8:[2]");
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=1 2=20 | 8:[2]");
}

TEST_CASE ("Value area, keyed: a write outside the history to an entry a step changed is lost by that step's Undo")
{
    KeyedSong keyed;
    keyed.area.setOutsideHistory (KeyedSong::withValue (keyed.area.get(), 1, 1));
    keyed.inStep ("Step", [&] { keyed.set (1, 10); });
    keyed.setOutside (1, 99);
    keyed.setOutside (2, 20);

    // The step's value before wins, as the tree's Undo of a property puts back the value it had before the step, whatever was
    // written into it since; the other entry written stays.
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=1 2=20 |");
    phazite::redo (*keyed.song);
    CHECK (keyed.now() == "1=10 2=20 |");

    // The same after a redo (the step no longer the history's current one): the write meets the Undo's move.
    keyed.setOutside (1, 55);
    keyed.setOutside (3, 30);
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=1 2=20 3=30 |");
    phazite::redo (*keyed.song);
    CHECK (keyed.now() == "1=10 2=20 3=30 |");
}

TEST_CASE ("Value area, keyed: two steps with a write outside the history between them, undone and done again")
{
    // Written after a step ended, the history's current step is still that one until another begins (JUCE's transaction): the
    // writes are taken into its action's values. Written after an undo, they meet the next undo's and redo's moves.
    KeyedSong keyed;
    keyed.area.setOutsideHistory (KeyedSong::withValue (keyed.area.get(), 1, 1));
    keyed.inStep ("A", [&] { keyed.set (1, 10); });
    keyed.setOutside (2, 20);
    keyed.appendOutside (8, 2);
    keyed.inStep ("B",
                  [&]
                  {
                      keyed.set (3, 30);
                      keyed.append (8, 3);
                  });
    keyed.setOutside (4, 40);

    CHECK (keyed.now() == "1=10 2=20 3=30 4=40 | 8:[2,3]");
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=10 2=20 4=40 | 8:[2]");
    keyed.setOutside (9, 90);
    keyed.appendOutside (6, 9);
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=1 2=20 4=40 9=90 | 6:[9] 8:[2]");
    phazite::redo (*keyed.song);
    CHECK (keyed.now() == "1=10 2=20 4=40 9=90 | 6:[9] 8:[2]");
    phazite::redo (*keyed.song);
    CHECK (keyed.now() == "1=10 2=20 3=30 4=40 9=90 | 6:[9] 8:[2,3]");
    CHECK (keyed.area.countActions() == 2);
}

TEST_CASE ("Value area, keyed: an entry a step added and that was removed outside the history, and one it removed and that was "
           "added again")
{
    KeyedSong keyed;
    keyed.area.setOutsideHistory (KeyedSong::withValue (keyed.area.get(), 1, 10));

    // Added by the step, removed outside: gone after the Undo, back after the Redo.
    keyed.inStep ("Add", [&] { keyed.set (4, 40); });
    keyed.setOutside (4, std::nullopt);
    keyed.setOutside (2, 20);
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=10 2=20 |");
    phazite::redo (*keyed.song);
    CHECK (keyed.now() == "1=10 2=20 4=40 |");

    // Removed by the step, added again outside with another value: the step's value before wins at its Undo, and its Redo
    // removes it again.
    keyed.inStep ("Remove", [&] { keyed.set (1, std::nullopt); });
    keyed.setOutside (1, 77);
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=10 2=20 4=40 |");
    phazite::redo (*keyed.song);
    CHECK (keyed.now() == "2=20 4=40 |");
}

TEST_CASE ("Value area, keyed: a write outside the history in the middle of a drag survives its Escape and its Undo, as one in "
           "the middle of a trial survives the trial put back")
{
    KeyedSong keyed;
    keyed.area.setOutsideHistory (KeyedSong::withValue (keyed.area.get(), 1, 1));
    keyed.inStep ("Before", [&] { keyed.set (5, 5); });
    const auto start = keyed.now();

    // A drag given up: the write outside it stays, and the drag's later changes, made from the value with it, don't take it
    // along; the write to an entry the drag changed already is lost with the drag's change, as the step's value before wins.
    {
        phazite::Gesture gesture (*keyed.song, "Drag");
        keyed.set (1, 11);
        keyed.setOutside (2, 22);
        keyed.setOutside (1, 50);
        keyed.set (3, 33);
        keyed.set (1, 12);
        CHECK (keyed.now() == "1=12 2=22 3=33 5=5 |");
        gesture.rollBack();
    }

    CHECK (keyed.now() == "1=1 2=22 5=5 |");
    CHECK_FALSE (phazite::canRedo (*keyed.song));
    CHECK (keyed.area.countActions() == 1);

    // A drag kept, with a write outside it in the middle: its Undo and Redo leave the write alone.
    {
        phazite::Gesture gesture (*keyed.song, "Drag");
        keyed.set (1, 13);
        keyed.appendOutside (8, 4);
        keyed.set (1, 14);
        gesture.commit();
    }

    CHECK (keyed.now() == "1=14 2=22 5=5 | 8:[4]");
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=1 2=22 5=5 | 8:[4]");
    phazite::redo (*keyed.song);
    CHECK (keyed.now() == "1=14 2=22 5=5 | 8:[4]");
    phazite::undo (*keyed.song);

    // A trial tried from its start again and put back, with a write outside it in the middle.
    {
        phazite::Trial trial (*keyed.song, "Try", [] {});
        trial.apply ([&] { keyed.set (1, 15); });
        keyed.setOutside (3, 30);
        trial.apply ([&] { keyed.set (1, 16); }, true);
        CHECK (keyed.now() == "1=16 2=22 3=30 5=5 | 8:[4]");
        trial.apply ([&] { keyed.set (6, 6); });
        trial.putBack();
    }

    CHECK (keyed.now() == "1=1 2=22 3=30 5=5 | 8:[4]");

    // And one kept, then undone and done again.
    {
        phazite::Trial trial (*keyed.song, "Try", [] {});
        trial.apply ([&] { keyed.set (1, 17); });
        keyed.setOutside (4, 40);
        trial.apply ([&] { keyed.set (6, 6); });
        trial.keep ("Kept");
    }

    CHECK (keyed.now() == "1=17 2=22 3=30 4=40 5=5 6=6 | 8:[4]");
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=1 2=22 3=30 4=40 5=5 | 8:[4]");
    phazite::redo (*keyed.song);
    CHECK (keyed.now() == "1=17 2=22 3=30 4=40 5=5 6=6 | 8:[4]");
    phazite::undo (*keyed.song);
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=1 2=22 3=30 4=40 | 8:[4]");
    CHECK (start == "1=1 5=5 |");
}

TEST_CASE ("Value area, keyed: listeners are told once for each move, with the value left and the value come to")
{
    KeyedSong keyed;
    Told<Keyed> told;
    keyed.area.addListener (&told);
    const auto start = keyed.area.get();

    keyed.inStep ("Step",
                  [&]
                  {
                      keyed.set (1, 10);
                      keyed.set (1, 11);
                  });

    const auto step = keyed.area.get();
    CHECK (told.count == 2);
    CHECK (isSameValue (told.after, step));

    // Undo and Redo with nothing written outside: the very values.
    phazite::undo (*keyed.song);
    CHECK (told.count == 3);
    CHECK (isSameValue (told.before, step));
    CHECK (isSameValue (told.after, start));
    CHECK (isSameValue (keyed.area.get(), start));
    phazite::redo (*keyed.song);
    CHECK (told.count == 4);
    CHECK (isSameValue (told.before, start));
    CHECK (isSameValue (told.after, step));

    // With a write outside between: the value written left, the value moved to come to.
    keyed.setOutside (2, 20);
    CHECK (told.count == 5);
    const auto written = keyed.area.get();
    phazite::undo (*keyed.song);
    CHECK (told.count == 6);
    CHECK (isSameValue (told.before, written));
    CHECK (isSameValue (told.after, keyed.area.get()));
    CHECK (describe (told.after) == "2=20 |");
    const auto undone = keyed.area.get();
    phazite::redo (*keyed.song);
    CHECK (told.count == 7);
    CHECK (isSameValue (told.before, undone));
    CHECK (isSameValue (told.after, keyed.area.get()));
    CHECK (describe (told.after) == "1=11 2=20 |");

    // A move to what the value is already (the step's entry put back outside first) tells no one.
    keyed.setOutside (1, std::nullopt);
    CHECK (told.count == 8);
    phazite::undo (*keyed.song);
    CHECK (told.count == 8);
    CHECK (keyed.now() == "2=20 |");
    keyed.area.removeListener (&told);
}

TEST_CASE ("Value area, keyed: a value alike but not the same, set without a step: a part of the step's action while it is open, "
           "and outside the history after an undo and a redo")
{
    KeyedSong keyed;
    keyed.area.setOutsideHistory (KeyedSong::withValue (keyed.area.get(), 1, 1));
    const auto start = keyed.area.get();
    Keyed alike;

    // In a step: a part of its action, so that its Redo holds the alike value.
    keyed.inStep ("Step",
                  [&]
                  {
                      keyed.set (1, 10);
                      alike = rebuilt (keyed.area.get());
                      keyed.area.setWithoutStep (alike);
                  });

    CHECK (isSameValue (keyed.area.get(), alike));
    CHECK (keyed.area.countActions() == 1);

    // After the step, while its action is still in the history's current step (until another begins, or an undo or a redo):
    // a part of that action too, so that its Undo is the plain move to the very value before, and its Redo to this value.
    const auto later = rebuilt (keyed.area.get());
    keyed.area.setWithoutStep (later);
    CHECK (isSameValue (keyed.area.get(), later));
    CHECK (keyed.area.countActions() == 1);
    phazite::undo (*keyed.song);
    CHECK (isSameValue (keyed.area.get(), start));
    phazite::redo (*keyed.song);
    CHECK (isSameValue (keyed.area.get(), later));

    // Outside the history (no action of the area's is open after the redo): the step's Undo, no longer finding the value it
    // left, writes the step's entries onto the alike value, and comes to a value alike to the one before the step, not that
    // very value; its Redo the same, to a value alike to the step's after.
    const auto other = rebuilt (keyed.area.get());
    keyed.area.setWithoutStep (other);
    CHECK (isSameValue (keyed.area.get(), other));
    CHECK (keyed.area.countActions() == 1);
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=1 |");
    CHECK_FALSE (isSameValue (keyed.area.get(), start));
    phazite::redo (*keyed.song);
    CHECK (keyed.now() == "1=10 |");
    CHECK_FALSE (isSameValue (keyed.area.get(), later));
    CHECK_FALSE (isSameValue (keyed.area.get(), other));
}

TEST_CASE ("Value area, keyed: a later change in a step gives the step's value after only what it changed, so that a write "
           "outside the history to an entry the step changed is lost the same whatever came after it")
{
    // The step sets entry 1, a write outside sets it again, and the step sets entry 3: in either order of the last two, Undo
    // gives entry 1 as before the step and Redo as the step left it, as the tree's Undo and Redo of a property do.
    for (const auto setLast : { true, false })
    {
        CAPTURE (setLast);
        KeyedSong keyed;
        keyed.area.setOutsideHistory (KeyedSong::withValue (keyed.area.get(), 1, 1));

        {
            phazite::Gesture gesture (*keyed.song, "Drag");
            keyed.set (1, 11);

            if (setLast)
            {
                keyed.setOutside (1, 50);
                keyed.set (3, 33);
            }
            else
            {
                keyed.set (3, 33);
                keyed.setOutside (1, 50);
            }

            CHECK (keyed.now() == "1=50 3=33 |");
            gesture.commit();
        }

        CHECK (keyed.now() == "1=50 3=33 |");
        phazite::undo (*keyed.song);
        CHECK (keyed.now() == "1=1 |");
        phazite::redo (*keyed.song);
        CHECK (keyed.now() == "1=11 3=33 |");
        phazite::undo (*keyed.song);
        CHECK (keyed.now() == "1=1 |");
    }
}

TEST_CASE ("Value area, keyed: writes outside the history after a step work its changes out once, and its Undo and Redo use "
           "them")
{
    KeyedSong keyed;
    keyed.area.setOutsideHistory (KeyedSong::withValue (keyed.area.get(), 1, 1));
    keyed.inStep ("Step",
                  [&]
                  {
                      keyed.set (1, 10);
                      keyed.set (3, 30);
                      keyed.append (7, 1);
                  });

    // Five writes outside the history while the step is the history's current one: to entries it didn't change (one added
    // then removed again), to an entry it added, to another owner's list in the map of its list; each works out what it
    // changed, and the step's changes are worked out once, for the first.
    keyedChangesFound = 0;
    keyed.setOutside (2, 20);
    keyed.setOutside (3, 33);
    keyed.appendOutside (8, 2);
    keyed.setOutside (2, std::nullopt);
    keyed.setOutside (4, 40);
    CHECK (keyedChangesFound == 5 + 1);
    CHECK (keyed.now() == "1=10 3=33 4=40 | 7:[1] 8:[2]");

    // Undo and Redo move the step's entries by the changes kept, working out nothing again; the write to the entry the step
    // added is lost with it.
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=1 4=40 | 8:[2]");
    phazite::redo (*keyed.song);
    CHECK (keyed.now() == "1=10 3=30 4=40 | 7:[1] 8:[2]");
    phazite::undo (*keyed.song);
    CHECK (keyed.now() == "1=1 4=40 | 8:[2]");
    CHECK (keyedChangesFound == 5 + 1);
}

namespace
{
// An action of a test's own in a step, which writes a keyed area outside the history as it is undone and as it is redone:
// as the store of a plug-in's states might as its parameters move back.
struct WritesAsItMoves final : juce::UndoableAction
{
    WritesAsItMoves (KeyedSong& s, int undoValue, int redoValue)
        : keyed (s),
          onUndo (undoValue),
          onRedo (redoValue)
    {
    }

    bool perform() override
    {
        if (std::exchange (undone, false))
            keyed.setOutside (2, onRedo);

        return true;
    }

    bool undo() override
    {
        undone = true;
        keyed.setOutside (2, onUndo);
        return true;
    }

    KeyedSong& keyed;
    int onUndo = 0, onRedo = 0;
    bool undone = false;
};
} // namespace

TEST_CASE ("Value area, keyed: a write outside the history while the history undoes or redoes is only a write, met by the "
           "step's moves")
{
    // The other action before the area's in the step, and after it: Undo and Redo run the step's actions in opposite orders.
    for (const auto writerFirst : { true, false })
    {
        CAPTURE (writerFirst);
        KeyedSong keyed;
        keyed.area.setOutsideHistory (KeyedSong::withValue (keyed.area.get(), 1, 1));

        keyed.inStep ("Step",
                      [&]
                      {
                          if (writerFirst)
                              keyed.song->getUndoManager().perform (new WritesAsItMoves (keyed, 20, 21));

                          keyed.set (1, 10);

                          if (! writerFirst)
                              keyed.song->getUndoManager().perform (new WritesAsItMoves (keyed, 20, 21));
                      });

        CHECK (keyed.now() == "1=10 |");
        phazite::undo (*keyed.song); // the step's action still the history's current one as the Undo begins
        CHECK (keyed.now() == "1=1 2=20 |");
        phazite::redo (*keyed.song);
        CHECK (keyed.now() == "1=10 2=21 |");
        phazite::undo (*keyed.song);
        CHECK (keyed.now() == "1=1 2=20 |");
        phazite::redo (*keyed.song);
        CHECK (keyed.now() == "1=10 2=21 |");
        CHECK (keyed.area.countActions() == 1);
    }
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
        auto notes = immer::flex_vector<TestNote>().transient();

        for (size_t i = 0; i < count; ++i)
            notes.push_back ({ i + 1, 48 + (int) (i % 24), (double) i * 0.25, 0.2 });

        mixed.notes.setOutsideHistory ({ mixed.notes.get().clips.set (clip.getRaw(), notes.persistent()) });
    }

    // The tree's clip, with notes the tree holds.
    const auto treeClip =
        phazite::insertMidiClip (*mixed.song, mixed.tracks[2], { phazite::test::at (0.0), phazite::test::at (4096.0) }, nullptr);
    std::vector<phazite::NoteData> made;

    for (size_t i = 0; i < moved; ++i)
        made.push_back (makeNoteData (treeClip, 60, (double) i, 0.2));

    phazite::setClipNotes (*mixed.song, treeClip, made);
    const auto treeNotes = phazite::getClipNotes (*mixed.song, treeClip);
    mixed.song->getUndoManager().clearUndoHistory();

    const auto moveNotes = [&] (const NotesByClip& from)
    {
        auto notes = from.clips.find (clip.getRaw())->transient();

        for (size_t i = 0; i < moved; ++i)
            notes.update (i * 10,
                          [] (TestNote note)
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
    std::vector<immer::flex_vector<TestNote>> held;
    auto many = immer::flex_vector<TestNote>().transient();

    for (size_t i = 0; i < 20000; ++i)
        many.push_back ({ i + 1, 60, 0.0, 1.0 });

    held.push_back (many.persistent());
    // The history cleared first. JUCE keeps the redo steps it put aside last through that (the drag undone last, here):
    // one action, and its two values, that outlive the clear until redo steps are put aside again (JUCE workaround W56
    // (docs/juce-workarounds.md) says where Phazite meets it).
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
