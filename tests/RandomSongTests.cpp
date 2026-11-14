// SPDX-License-Identifier: AGPL-3.0-or-later
// What must hold for any song the model can make, checked on songs made by random edits from seeds (RandomEdits.h): its
// text reads back as the same song, opens as the same song (makeSong() changes nothing in a song the model made), and
// passes the consistency check (SongCheck.h); its version's pieces join into the same song; and making one song like
// another leaves no difference between them. Each failure says its seed, which makes the same song again.
#include "GoldenFiles.h"
#include "RandomEdits.h"
#include "SongCheck.h"
#include "SongIDs.h"
#include "TestHelpers.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
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
        const auto b = phazite::makeSong (a->getState().createCopy());
        RandomEdits (*b, seed + 1000000, sounds.folder).edit (editsPerSong / 2);
        const auto before = a->getState().createCopy();

        phazite::makeSongLike (*a, b->getState());
        const auto changes = phazite::findChanges (b->getState(), a->getState());
        INFO ("A made like B: " << changes.size() << " differences left");
        CHECK (changes.empty());
        checkIDs (*a);
        checkSong (*phazite::makeSong (a->getState().createCopy()));

        phazite::makeSongLike (*b, before);
        INFO ("B made like A as it was: " << phazite::findChanges (before, b->getState()).size() << " differences left");
        CHECK (phazite::findChanges (before, b->getState()).empty());
        checkIDs (*b);
    }
}
