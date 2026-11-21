// SPDX-License-Identifier: AGPL-3.0-or-later
// What must hold for any song the model can make, checked on songs made by random edits from seeds (RandomEdits.h): its
// text reads back as the same song, opens as the same song (makeSong() changes nothing in a song the model made), and
// passes the consistency check (SongCheck.h); its version's pieces join into the same song; and making one song like
// another leaves no difference between them; and a player that compiles a plan after each edit, from what the edit
// changed, plays as a fresh player's whole compile does. Each failure says its seed, which makes the same song again.
#include "GoldenFiles.h"
#include "PlanCheck.h"
#include "RandomEdits.h"
#include "SongCheck.h"
#include "SongIDs.h"
#include "TestHelpers.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/commands/internal/Undo.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/project/internal/Project.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/SongDiff.h"
#include "model/versions/internal/SongPieces.h"

namespace
{
using namespace phazite::test;

// How many songs each property is checked on, and how many edits make each: PHAZITE_RANDOM_SONGS sets the songs (a nightly
// run: 1000).
int getSongCount()
{
    const auto text = juce::SystemStats::getEnvironmentVariable ("PHAZITE_RANDOM_SONGS", {});
    return text.isNotEmpty() ? juce::jmax (1, text.getIntValue()) : 100;
}

constexpr int editsPerSong = 40;

juce::String toText (const juce::ValueTree& song)
{
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, song);
    return text.toString();
}

// The first line two texts differ at, for a failure to show.
std::string findDifference (const juce::String& a, const juce::String& b)
{
    const auto linesA = juce::StringArray::fromLines (a), linesB = juce::StringArray::fromLines (b);

    for (int i = 0; i < juce::jmax (linesA.size(), linesB.size()); ++i)
        if (linesA[i] != linesB[i])
            return "line " + std::to_string (i + 1) + ": \"" + linesA[i].toStdString() + "\" against \"" + linesB[i].toStdString() + "\"";

    return {};
}

// What opening a song leaves out of it as it was edited, which names what is gone (SongIDs.h): the curves and lanes of
// devices taken away, which the song keeps for an undo. Nothing else names what is gone.
void leaveOutWhatNamesNothing (phazite::Song& song)
{
    phazite::forgetCurvesOfGoneDevices (song);
    std::set<std::uint64_t> items;
    findIDProblems (song.getState(), &items);

    const std::function<void (juce::ValueTree)> visit = [&] (juce::ValueTree tree)
    {
        for (auto i = tree.getNumChildren(); --i >= 0;)
        {
            auto child = tree.getChild (i);

            if (child.hasType (phazite::schema::LANE) && items.count (phazite::readID (child[phazite::schema::laneDevice])) == 0)
            {
                tree.removeChild (i, nullptr);
                continue;
            }

            visit (child);
        }
    };

    visit (song.getState());
}

phazite::Song::Ptr makeRandomSong (juce::int64 seed, const juce::File& sounds, int edits = editsPerSong)
{
    auto song = phazite::createSong();
    RandomEdits (*song, seed, sounds).edit (edits);
    return song;
}
} // namespace

TEST_CASE ("Random songs: written as text, a song reads back and opens as the same song, which holds together")
{
    TemporaryFolder sounds;

    for (int i = 0; i < getSongCount(); ++i)
    {
        const auto seed = 20261114 + i;
        INFO ("seed " << seed);
        const auto song = makeRandomSong (seed, sounds.folder);

        // What the song keeps only while an undo could bring it back, which opening it leaves out, left out first.
        leaveOutWhatNamesNothing (*song);
        const auto written = toText (song->getState());
        const auto xml = juce::parseXML (written);
        REQUIRE (xml != nullptr);
        const auto read = juce::ValueTree::fromXml (*xml);
        const auto again = toText (read);
        INFO ("read back: " << findDifference (written, again));
        CHECK (written == again);

        // Opened, as a project opens its song: nothing the model made changes as it opens.
        const auto opened = phazite::makeSong (read);
        REQUIRE (opened != nullptr);
        const auto openedText = toText (opened->getState());

        if (written != openedText)
        {
            const auto folder = getActualFolder().getChildFile ("random-songs");
            folder.createDirectory();
            folder.getChildFile (juce::String (seed) + " written.xml").replaceWithText (written);
            folder.getChildFile (juce::String (seed) + " opened.xml").replaceWithText (openedText);
        }

        INFO ("opened: " << findDifference (written, openedText));
        CHECK (written == openedText);
        checkSong (*opened);
    }
}

TEST_CASE ("Random songs: a song cut into a version's pieces and joined again is the same song")
{
    TemporaryFolder sounds;

    for (int i = 0; i < getSongCount(); ++i)
    {
        const auto seed = 20261214 + i;
        INFO ("seed " << seed);
        const auto song = makeRandomSong (seed, sounds.folder);

        // What only an undo needs, which the loader's checks leave out of a song joined as they do of one opened (SongPieces.h:
        // joinSong()), left out first.
        leaveOutWhatNamesNothing (*song);
        const auto pieces = phazite::splitSong (song->getState());
        const auto joined = phazite::joinSong (pieces.song,
                                               [&pieces] (const juce::String& name) -> std::optional<juce::MemoryBlock>
                                               {
                                                   if (const auto found = pieces.pieces.find (name); found != pieces.pieces.end())
                                                       return found->second;

                                                   return std::nullopt;
                                               });
        REQUIRE (joined.isValid());
        const auto written = toText (song->getState()), again = toText (joined);
        INFO ("joined: " << findDifference (written, again));
        CHECK (written == again);
    }
}

TEST_CASE ("Random songs: a song made like another, which went on from it, has no difference from it and its IDs each once")
{
    TemporaryFolder sounds;

    for (int i = 0; i < getSongCount(); ++i)
    {
        // B goes on from A with edits of its own; A is made like B, and the other way round, from B's own copy of A.
        const auto seed = 20261314 + i;
        INFO ("seed " << seed);
        const auto a = makeRandomSong (seed, sounds.folder);
        const auto b = phazite::makeSong (phazite::test::copyWholeSong (*a));
        RandomEdits (*b, seed + 1000000, sounds.folder).edit (editsPerSong / 2);
        const auto before = phazite::test::copyWholeSong (*a);

        phazite::makeSongLike (*a, b->getState());
        const auto changes = phazite::findChanges (b->getState(), a->getState());
        INFO ("A made like B: " << changes.size() << " differences left");
        CHECK (changes.empty());
        checkIDs (*a);
        checkSong (*phazite::makeSong (phazite::test::copyWholeSong (*a)));

        phazite::makeSongLike (*b, before);
        INFO ("B made like A as it was: " << phazite::findChanges (before, b->getState()).size() << " differences left");
        CHECK (phazite::findChanges (before, b->getState()).empty());
        checkIDs (*b);
    }
}

TEST_CASE ("Random songs: each edit's plan, compiled from what the edit changed, plays as a whole compile of the song does")
{
    // Known bugs 124 and 145's review: a player keeps a track's notes and audio, a channel's devices' values and a curve from
    // plan to plan until the song says they changed (model/playback/internal/SongPlayer.h). A player that lives through
    // random edits, their undos and redos, and the whole song made like an earlier one (as a merge or a branch switch makes
    // it), compiled after each, against a fresh player's whole compile of the song then: the two plans play alike, and the
    // checks of what the first kept (on in the tests) find nothing stale. A tenth of the songs: each edit compiles twice.
    TemporaryFolder sounds, cache;

    for (int i = 0; i < juce::jmax (1, getSongCount() / 10); ++i)
    {
        const auto seed = 20261116 + i;
        INFO ("seed " << seed);
        auto song = phazite::createSong();
        song->setCacheFolder (cache.folder.getChildFile (juce::String (seed))); // for its frozen tracks' files and its renders
        RandomEdits edits (*song, seed, sounds.folder);
        edits.freezes = true;
        phazite::SongPlayer kept (*song);
        auto earlier = phazite::test::copyWholeSong (*song);

        for (int step = 0; step < editsPerSong; ++step)
        {
            INFO ("step " << step);
            const auto kind = edits.random.nextInt (10);

            if (kind == 0 && song->getUndoManager().canUndo())
            {
                phazite::undo (*song);
            }
            else if (kind == 1 && song->getUndoManager().canRedo())
            {
                phazite::redo (*song);
            }
            else if (kind == 2)
            {
                const auto now = phazite::test::copyWholeSong (*song);
                phazite::makeSongLike (*song, earlier);
                earlier = now;
            }
            else
            {
                const phazite::UndoStep change (*song, "Random Edit");
                edits.edit();
            }

            // The renders of the pitched clips made, as an export makes those that are missing, so that their audio plays and
            // is kept.
            phazite::renderMissingClipAudio (*song);

            const auto plan = kept.compile (44100.0, 512, std::nullopt);
            const auto whole = phazite::SongPlayer (*song).compile (44100.0, 512, std::nullopt);
            const auto differences = describePlanDifferences (*plan, *whole);
            CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n").toStdString());
        }
    }
}
