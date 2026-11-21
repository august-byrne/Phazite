// SPDX-License-Identifier: AGPL-3.0-or-later
// The model's fuzzer: songs of a project made by long runs of random edits (RandomEdits.h: every kind the model has, the
// edits of notes, controllers' values and clips of alias groups among them), each one undo step, a share of them in a
// gesture committed or rolled back, with undos and redos among them; checked as they go. After every operation the song
// holds together (SongCheck.h, with the alias rule) and the operation made one undo step at most; and every so many
// operations: what the views were told covers what changed (ProjectContext::Listener::songChanged(): a clip whose notes
// changed was in a group of changed clips), a plan compiled from what changed plays as a whole compile does while the
// player's checks are on (PlanCheck.h), every step since the project opened undoes and redoes exactly (UndoCheck.h's walk),
// and the project saved and opened again is the same song, whole and as an undo step keeps it, and goes on from there.
// Each failure says the song's seed and the operation's number, which make it again.
#include "PlanCheck.h"
#include "RandomEdits.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/SongIndex.h"
#include "model/commands/internal/Undo.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/project/internal/ProjectContext.h"

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>

namespace
{
using namespace phazite::test;

constexpr int operationsPerSong = 200;
constexpr int operationsBetweenChecks = 25; // between the views' check, the plan's, the walk, and a save and reopening

// What the views were told since it was last taken: every group of changes, joined.
struct Told final : phazite::ProjectContext::Listener
{
    explicit Told (phazite::ProjectContext& c)
        : context (c)
    {
        context.addListener (this);
    }

    ~Told() override { context.removeListener (this); }

    void projectChanged() override {}

    void songChanged (const phazite::SongChanges& changes) override
    {
        clips = clips || changes.clips;
        anyClip = anyClip || changes.changedClips.any;

        for (const auto clip : changes.changedClips.named)
            named.insert (clip);
    }

    bool mayInclude (phazite::ClipID clip) const { return clips && (anyClip || named.count (clip) > 0); }

    void forget()
    {
        clips = anyClip = false;
        named.clear();
    }

    phazite::ProjectContext& context;
    bool clips = false, anyClip = false;
    std::set<phazite::ClipID> named;
};

// Every MIDI clip's notes, described in full (TestClips.h), by the clip.
std::map<phazite::ClipID, juce::String> describeAllNotes (const phazite::Song& song)
{
    std::map<phazite::ClipID, juce::String> notes;

    for (const auto& clip : phazite::getClipStates (song))
        if (phazite::isMidiState (clip))
            notes[phazite::getClipID (clip)] = describeClipNotes (song, phazite::getClipID (clip));

    return notes;
}

// The song as an edit left it holds together (SongCheck.h), but for what it keeps for an undo (copyAsOpened()); and what
// finds its items by their IDs holds what it does.
void checkEditedSong (const phazite::Song& song)
{
    checkSong (*copyAsOpened (song));
    CHECK (song.getIndex().isExact());
}

// One song's run: its operations, each from the seed, checked as they go.
struct Run
{
    explicit Run (juce::int64 s)
        : seed (s),
          random (s)
    {
        startSegment();
    }

    phazite::Song& getSong() const { return *phazite::findSong (song.context); }

    // From the project as it opened (or began): the edits drawn from a seed of the segment's own, the undo check following
    // every step from here, the player that compiles from what changed, and what the views were told from now.
    void startSegment()
    {
        auto& now = getSong();
        edits = std::make_unique<RandomEdits> (now, seed * 1000 + segment, song.getSoundsFolder());
        undoCheck = std::make_unique<UndoCheck> (now);
        player = std::make_unique<phazite::SongPlayer> (now);
        player->compile (44100.0, 512, std::nullopt);
        phazite::test::dispatchPendingMessages();
        told.forget();
        notes = describeAllNotes (now);
        ++segment;
    }

    int getDepth() const { return getSong().getUndoManager().getUndoDescriptions().size(); }

    // One operation: an edit as a step of its own; a gesture of one to three edits, committed or rolled back; an undo; or a
    // redo. Then the song holds together, and the views are told as a turn of the message loop tells them.
    void operate (int number)
    {
        auto& now = getSong();
        const auto which = random.nextInt (100);
        const auto depthBefore = getDepth();

        if (which < 8)
        {
            phazite::undo (now);
        }
        else if (which < 14)
        {
            phazite::redo (now);
        }
        else if (which < 26)
        {
            const auto count = 1 + random.nextInt (3);
            const auto rolledBack = random.nextBool();
            const auto before = rolledBack ? writeSong (now, SongPart::undoableWithoutSwitches) : juce::String();
            phazite::Gesture gesture (now, "Random Drag");

            for (int i = 0; i < count; ++i)
            {
                edits->edit();
                phazite::test::dispatchPendingMessages(); // the drag's steps come over several events
            }

            if (rolledBack)
            {
                gesture.rollBack();
                const auto after = writeSong (now, SongPart::undoableWithoutSwitches);
                CHECK_MESSAGE (after == before, "a gesture rolled back left the song changed: "
                                                    << describeSongDifferences (before, after).joinIntoString ("; ").toStdString());
                CHECK (getDepth() == depthBefore);
            }
            else
            {
                gesture.commit();
                CHECK (getDepth() - depthBefore <= 1);
            }
        }
        else
        {
            {
                const phazite::UndoStep step (now, "Random Edit");
                edits->edit();
            }

            CHECK (getDepth() - depthBefore <= 1);
        }

        // The renders of the pitched clips made, as an export makes those that are missing, so that what plays of them is
        // the same for every plan compiled (RandomSongTests.cpp does so too).
        phazite::renderMissingClipAudio (now);
        phazite::test::dispatchPendingMessages();
        checkEditedSong (now);

        if ((number + 1) % operationsBetweenChecks == 0)
            checkAtInterval();
    }

    // What the views were told, a plan, the undo walk, and the project saved and opened again.
    void checkAtInterval()
    {
        auto& now = getSong();

        // A clip whose notes changed since the last time, or that came or went, was in a group of changed clips the views
        // were told of.
        const auto notesNow = describeAllNotes (now);
        std::set<phazite::ClipID> clips;

        for (const auto& entry : notes)
            clips.insert (entry.first);

        for (const auto& entry : notesNow)
            clips.insert (entry.first);

        for (const auto clip : clips)
        {
            const auto before = std::as_const (notes).find (clip);
            const auto after = notesNow.find (clip);

            if (before != notes.end() && after != notesNow.end() && before->second == after->second)
                continue;

            CHECK_MESSAGE (told.mayInclude (clip), "clip " << clip.getRaw() << " changed, and the views weren't told it did");
        }

        // A plan from what changed since the last, as a whole compile, with the player's checks on.
        REQUIRE (phazite::SongPlayer::isChecking());
        const auto plan = player->compile (44100.0, 512, std::nullopt);
        const auto wholePlan = phazite::SongPlayer (now).compile (44100.0, 512, std::nullopt);
        const auto differences = describePlanDifferences (*plan, *wholePlan);
        CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n").toStdString());

        // Every step since the project opened, back and forth.
        undoCheck->walkBackAndForth();

        // Saved and opened again: the same song.
        const auto asOpened = copyAsOpened (now);
        const auto whole = writeSong (*asOpened, SongPart::whole);
        const auto undoable = writeSong (*asOpened, SongPart::undoable);
        undoCheck.reset();
        player.reset();
        edits.reset();
        song.reopen();
        auto& opened = getSong();
        const auto wholeOpened = writeSong (opened, SongPart::whole);
        CHECK_MESSAGE (wholeOpened == whole, "saved and opened again, the song differs: "
                                                 << describeSongDifferences (whole, wholeOpened).joinIntoString ("; ").toStdString());
        CHECK (writeSong (opened, SongPart::undoable) == undoable);
        checkSong (opened);
        startSegment();
    }

    const juce::int64 seed;
    juce::Random random;
    TestSong song;
    Told told { song.context };
    std::unique_ptr<RandomEdits> edits;
    std::unique_ptr<UndoCheck> undoCheck;
    std::unique_ptr<phazite::SongPlayer> player;
    std::map<phazite::ClipID, juce::String> notes;
    int segment = 0;
};
} // namespace

TEST_CASE ("The model's fuzzer: random edits, gestures, undos and redos keep a project's song whole, its plans and undo steps exact, "
           "the views told, and its file the same song")
{
    const auto started = juce::Time::getMillisecondCounterHiRes();

    for (const juce::int64 seed : { 20261121, 20261122, 20261123 })
    {
        Run run (seed);

        for (int number = 0; number < operationsPerSong; ++number)
        {
            INFO ("seed " << seed << ", operation " << number);
            run.operate (number);
        }

        INFO ("seed " << seed << ", at the end");
        run.undoCheck->walkBackAndForth();
    }

    MESSAGE ("The model's fuzzer took " << juce::roundToInt ((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0) << " s");
}
