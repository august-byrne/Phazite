// SPDX-License-Identifier: AGPL-3.0-or-later
// Keeping the versions whole (docs/design/versions.md, docs/versions-format.md): what the history writes is written
// before it goes, a project opened as it was committed has no changes, a branch whose last commit can't be read says so,
// a damaged piece is read from another pack or written again, and a branch isn't renamed while a commit writes its
// record.
#include "BenchmarkSong.h"
#include "GoldenFiles.h"
#include "HeldFile.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/PieceStore.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

namespace
{
using namespace phazite::test;

// Saves a commit with a name, and waits until it is written.
void keepVersion (phazite::ProjectContext& context, const juce::String& name)
{
    REQUIRE (waitForVersionHistory (context));
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    INFO (result->getErrorMessage().toStdString());
    REQUIRE (result->wasOk());
    REQUIRE (waitForVersionHistory (context));
}

juce::StringArray getVersionNames (const phazite::ProjectContext& context)
{
    juce::StringArray names;

    for (const auto& version : phazite::getVersions (context))
        names.add (version.name);

    return names;
}

// The history goes as the project's context lets go of the song, and a new one reads the Versions folder again as the
// song is the context's again: as the project closes and opens.
void closeAndOpenAgain (TestSong& s, const std::function<void()>& meanwhile = {})
{
    auto& song = s.getProject().getSong();
    phazite::setSong (s.context, nullptr);

    if (meanwhile != nullptr)
        meanwhile();

    phazite::setSong (s.context, &song);
    REQUIRE (waitForVersionHistory (s.context));
}

juce::Array<juce::File> findPacks (const TestSong& s)
{
    return s.getProject()
        .getFolder()
        .getChildFile ("Versions")
        .getChildFile ("pieces")
        .findChildFiles (juce::File::findFiles, false, "*.pack");
}

juce::MemoryBlock toBlock (const juce::String& text)
{
    return { text.toRawUTF8(), text.getNumBytesAsUTF8() };
}

// A pack's first piece's bytes changed, its index left as it is: the piece reads as damaged.
void damageFirstPiece (const juce::File& pack)
{
    juce::MemoryBlock bytes;
    REQUIRE (pack.loadFileAsData (bytes));
    REQUIRE (bytes.getSize() > 12);

    for (size_t i = 6; i < 12; ++i)
        bytes[i] = (char) (bytes[i] ^ 0x5a);

    REQUIRE (pack.replaceWithData (bytes.getData(), bytes.getSize()));
}

// One piece of a pack, by its name, damaged in its bytes, its index left as it is (PieceStore.h: each entry 56 bytes, its
// name first and where the piece starts after it; where the index starts 28 bytes from the pack's end).
void damagePiece (const juce::File& pack, const juce::String& name)
{
    juce::MemoryBlock bytes, wanted;
    REQUIRE (pack.loadFileAsData (bytes));
    wanted.loadFromHexString (name);
    const auto* data = static_cast<const char*> (bytes.getData());
    const auto count = (size_t) juce::ByteOrder::littleEndianInt (data + bytes.getSize() - 32);
    const auto indexStart = (size_t) juce::ByteOrder::littleEndianInt64 (data + bytes.getSize() - 28);

    for (size_t i = 0; i < count; ++i)
    {
        const auto* entry = data + indexStart + i * 56;

        if (std::memcmp (entry, wanted.getData(), 32) == 0)
        {
            const auto start = (size_t) juce::ByteOrder::littleEndianInt64 (entry + 32);
            bytes[start + 2] = (char) (bytes[start + 2] ^ 0x5a);
            REQUIRE (pack.replaceWithData (bytes.getData(), bytes.getSize()));
            return;
        }
    }

    FAIL ("the pack doesn't hold that piece");
}

// Every piece of a pack damaged, its index left as it is (PieceStore.h: where it starts is 28 bytes from its end).
void damageEveryPiece (const juce::File& pack)
{
    juce::MemoryBlock bytes;
    REQUIRE (pack.loadFileAsData (bytes));
    REQUIRE (bytes.getSize() > 32);
    const auto indexStart = (size_t) juce::ByteOrder::littleEndianInt64 (static_cast<const char*> (bytes.getData()) + bytes.getSize() - 28);
    REQUIRE (indexStart <= bytes.getSize());

    for (size_t i = 4; i < indexStart; ++i)
        bytes[i] = (char) (bytes[i] ^ 0x5a);

    REQUIRE (pack.replaceWithData (bytes.getData(), bytes.getSize()));
}

// Every history's writes held back while one lives, as a slow disk would hold them (VersionHistory::holdWritesForTests),
// let go as it goes; or, should the test hang meanwhile (a close that waits for them on the message thread), after
// hangTimeout, which only a hang reaches.
class HeldWrites
{
public:
    HeldWrites()
        : releaser (
              [this]
              {
                  if (! released.wait (hangTimeout))
                      phazite::VersionHistory::holdWritesForTests = false;
              })
    {
        phazite::VersionHistory::holdWritesForTests = true;
    }

    ~HeldWrites()
    {
        release();
        releaser.join();
    }

    void release()
    {
        phazite::VersionHistory::holdWritesForTests = false;
        released.signal();
    }

private:
    juce::WaitableEvent released { true };
    std::thread releaser;
};

// Waits until no history that went still writes.
void waitForVersionsFinished()
{
    REQUIRE (dispatchMessagesUntil ([] { return phazite::countVersionsBeingFinished() == 0; }, versionHistoryTimeout));
}
} // namespace

//==============================================================================
TEST_CASE ("A commit, and a snapshot before it, given as the project closes are written after it, the close waiting for neither")
{
    // Known bug 172: the jobs still waiting on the history's thread as it went were dropped, the commit with them; then the
    // close waited for them on the message thread, which froze the window for as long as they took.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (getVersionNames (s.context) == juce::StringArray ("First version"));

    s.setVolume (bass, -6.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (withChangesIn (s.context)); }));
    auto& song = s.getProject().getSong();

    {
        HeldWrites held;
        phazite::getVersionHistory (s.context)->takeSnapshot ("auto");
        phazite::setVersionDraft (s.context, "Written as it closed", {});
        REQUIRE (phazite::saveVersion (s.context));

        // Closed while both wait: at once, with both still to write.
        phazite::setSong (s.context, nullptr);
        CHECK (phazite::countVersionsBeingFinished() == 2);

        // Opened again meanwhile: it reads the folder only after them.
        phazite::setSong (s.context, &song);
        dispatchPendingMessages();
        CHECK (phazite::isReadingVersions (s.context));
        held.release();
    }

    REQUIRE (waitForVersionHistory (s.context));
    CHECK (getVersionNames (s.context) == juce::StringArray ("Written as it closed", "First version"));
    CHECK (phazite::getSnapshots (s.context).size() == 1);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    waitForVersionsFinished();
}

namespace
{
// So many histories of one project's folder, one after another, each gone once its first read has begun and is held
// there (VersionHistory::holdFirstReadsForTests), so that its thread is kept to finish it; then the reads let go. What
// waits for the histories that went must come back, and the project's next history read its folder.
void checkNothingWaitsForHistoriesGoneAsTheyRead (int histories)
{
    TestSong s;
    s.addTrack ("Bass");
    REQUIRE (waitForVersionHistory (s.context));
    auto& song = s.getProject().getSong();
    phazite::setSong (s.context, nullptr);
    waitForVersionsFinished();

    phazite::VersionHistory::holdFirstReadsForTests = true;
    auto held = true;

    for (int opened = 1; opened <= histories; ++opened)
    {
        phazite::setSong (s.context, &song);
        held = held && dispatchMessagesUntil ([opened] { return phazite::VersionHistory::firstReadsHeldForTests.load() == opened; });
        phazite::setSong (s.context, nullptr);
    }

    phazite::VersionHistory::holdFirstReadsForTests = false;
    REQUIRE (held);

    auto returned = std::make_shared<std::atomic<bool>> (false);
    std::thread waiter (
        [returned]
        {
            phazite::waitForVersionsFinished();
            returned->store (true);
        });
    const auto cameBack = dispatchMessagesUntil ([&returned] { return returned->load(); });
    CHECK (cameBack);

    if (cameBack)
        waiter.join();
    else
        waiter.detach(); // it waits still, and nothing can end it

    phazite::setSong (s.context, &song);
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (getVersionNames (s.context) == juce::StringArray ("First version"));
}
} // namespace

TEST_CASE ("A history that goes as its first read begins leaves nothing waiting without end: the read doesn't wait for its own thread")
{
    // Known bug 190: the read, begun before its history went and its thread was kept to finish its jobs, then waited for
    // every thread kept for its folder, its own among them: for itself. It never ended, nor did whatever waited for the
    // histories that went: the tests' folders as they go (AppTests ran into ctest's limit so in some runs on CI's Linux
    // builds), and Phazite as it quits.
    checkNothingWaitsForHistoriesGoneAsTheyRead (1);
}

TEST_CASE ("Two histories of one folder that go as their first reads begin don't wait for each other")
{
    // What the review of known bug 190's fix found: each read, leaving out its own thread, still waited for the other's,
    // kept for the same folder meanwhile. A read waits only for the histories that had gone when its own opened.
    checkNothingWaitsForHistoriesGoneAsTheyRead (2);
}

TEST_CASE ("Closing doesn't wait for what the history reads: a read given as it closes never runs")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    const auto first = phazite::getLastVersion (s.context)->id;
    s.setVolume (bass, -6.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (withChangesIn (s.context)); }));
    const auto before = phazite::VersionStore::countSongsRead();

    {
        HeldWrites held;
        REQUIRE (phazite::saveVersion (s.context));
        phazite::getVersionChanges (s.context, first); // its two songs to read, behind the commit
        REQUIRE (phazite::isReadingVersionChanges (s.context, first));
        phazite::setSong (s.context, nullptr);
        held.release();
    }

    waitForVersionsFinished();
    dispatchPendingMessages();
    CHECK (phazite::VersionStore::countSongsRead().all == before.all); // the commit written, the read let go

    phazite::setSong (s.context, &s.getProject().getSong());
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (getVersionNames (s.context).size() == 3);
}

//==============================================================================
TEST_CASE ("A project opened from a file that is its last commit has no changes, and Local History takes nothing as it opens")
{
    // Known bug 25.
    const TestPlugin plugin;
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    const auto bass = s.addTrack ("Bass");
    const auto synth = s.setInstrument (bass, phazite::devices::Synth::typeID);
    s.setParameter (synth, "cutoff", 1200.0f);
    s.addEffect (bass, phazite::devices::Equalizer::typeID);
    const auto clip = s.addMidiClip (bass, 0.0, 4.0);
    s.addNote (clip, 36, 0.0, 1.0);
    s.addNote (clip, 43, 1.0, 0.5);
    s.addAutomation (s.getVolumeParameter (bass), { { at (0.0), 0.5f }, { at (4.0), 0.8f } });
    s.addReturn (bass, phazite::devices::Equalizer::typeID);
    const auto lfo = s.addModulator (bass, phazite::ModulatorType::lfo);
    s.modulate (lfo, s.getPanParameter (bass), 0.5f);
    const auto vocals = s.addTrack ("Vocals");
    s.addAudioClip (vocals, writeSound (s.getSoundsFolder().getChildFile ("Vocal.wav"), 1.0), 4.0);
    const auto keys = s.addTrack ("Keys");
    TestPlugin::add (song, keys);
    REQUIRE (dispatchMessagesUntil ([&song] { return phazite::getPluginsLoading (song).isEmpty(); }));
    s.setVolume (keys, -3.0f);

    keepVersion (s.context, "All of it");
    REQUIRE_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));

    SUBCASE ("its plug-ins loaded as it is read")
    {
        s.reopen (true);
    }

    SUBCASE ("its plug-ins still loading as it is read")
    {
        s.reopen (false);
    }

    REQUIRE (waitForVersionHistory (s.context));
    auto& reopened = *phazite::findSong (s.context);
    REQUIRE (dispatchMessagesUntil ([&reopened] { return phazite::getPluginsLoading (reopened).isEmpty(); }));
    REQUIRE (waitForVersionHistory (s.context));
    dispatchPendingMessages();

    INFO (phazite::getVersionHistory (s.context)->summariseNextCommit().toStdString());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    CHECK (phazite::getSnapshots (s.context).empty());
}

TEST_CASE ("A song written into the project's file as it was committed opens with no changes, and Local History takes nothing")
{
    // Known bug 25 was seen here: a test's song opened again (TestSong::openSong()), which writes the song's tree into the
    // file as XML of its own, as another tool might.
    TestSong s;
    const auto keys = s.addTrack ("Keys");
    s.setInstrument (keys, phazite::devices::Synth::typeID);
    s.addNote (s.addMidiClip (keys, 0.0, 4.0), 60, 0.0, 1.0);
    s.setVolume (keys, -4.5f);
    keepVersion (s.context, "First");
    REQUIRE (s.getProject().save().wasOk());
    const auto saved = phazite::test::copyWholeSong (*phazite::findSong (s.context));

    s.openSong (saved);
    REQUIRE (waitForVersionHistory (s.context));
    dispatchPendingMessages();

    INFO (phazite::getVersionHistory (s.context)->summariseNextCommit().toStdString());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    CHECK (phazite::getSnapshots (s.context).empty());
}

TEST_CASE ("A device's values written as another tool writes numbers open as the values they are: no change, nothing taken as it opens")
{
    // The review of known bug 25: "1.0" and "5e-1" in a VALUES element stayed as written, which the versions compare as
    // text with the commit's "1" and "0.5".
    TestSong s;
    const auto keys = s.addTrack ("Keys");
    const auto synth = s.setInstrument (keys, phazite::devices::Synth::typeID);
    s.setParameter (synth, "subLevel", 1.0f);
    s.setParameter (synth, "unisonDrift", 0.5f);
    keepVersion (s.context, "First");
    REQUIRE (s.getProject().save().wasOk());
    auto written = phazite::test::copyWholeSong (*phazite::findSong (s.context));

    std::function<juce::ValueTree (const juce::ValueTree&)> findValues = [&] (const juce::ValueTree& element) -> juce::ValueTree
    {
        for (const auto& child : element)
        {
            if (child.hasType (phazite::schema::DEVICE) && child[phazite::schema::deviceType].toString() == phazite::devices::Synth::typeID)
                return child.getChildWithName (phazite::schema::VALUES);

            if (const auto found = findValues (child); found.isValid())
                return found;
        }

        return {};
    };

    auto values = findValues (written);
    REQUIRE (values.isValid());
    REQUIRE (values.hasProperty ("subLevel"));
    REQUIRE (values.hasProperty ("unisonDrift"));
    values.setProperty ("subLevel", "1.0", nullptr);
    values.setProperty ("unisonDrift", "5e-1", nullptr);

    s.openSong (written);
    REQUIRE (waitForVersionHistory (s.context));
    dispatchPendingMessages();

    INFO (phazite::getVersionHistory (s.context)->summariseNextCommit().toStdString());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    CHECK (phazite::getSnapshots (s.context).empty());
}

TEST_CASE ("A project Phazite saved is the same, byte for byte, once opened and saved again")
{
    // makeConsistent() changes nothing in a project Phazite can save: the numbers it holds as Phazite writes them (known
    // bug 25) are already so.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto synth = s.setInstrument (bass, phazite::devices::Synth::typeID);
    s.setParameter (synth, "cutoff", 1234.5f);
    s.setParameter (synth, "unisonDrift", 0.3f);
    s.setVolume (bass, -4.7f);
    s.setPan (bass, 0.15f);
    s.setTempo (127.3);
    const auto clip = s.addMidiClip (bass, 0.0, 4.0);
    s.addNote (clip, 36, 0.0, 0.333);
    s.addNote (clip, 43, 1.1, 0.5, 77);
    s.addAutomation (s.getVolumeParameter (bass), { { at (0.0), 0.37f }, { at (3.3), 0.81f } });
    REQUIRE (s.getProject().save().wasOk());
    s.reopen();
    juce::MemoryBlock first, second;
    REQUIRE (s.getProject().getFile().loadFileAsData (first));
    s.reopen(); // saved, opened, and saved below
    REQUIRE (s.getProject().save().wasOk());
    REQUIRE (s.getProject().getFile().loadFileAsData (second));
    CHECK (first == second);
}

TEST_CASE ("The example song opened from a file that is its last commit has no changes, and Local History takes nothing as it opens")
{
    TestSong s (TestSong::Start::exodus);
    auto& song = *phazite::findSong (s.context);
    REQUIRE (dispatchMessagesUntil ([&song] { return phazite::getPluginsLoading (song).isEmpty(); }));
    REQUIRE (waitForVersionHistory (s.context)); // its first version is the song as it came
    REQUIRE_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));

    s.reopen (false);
    REQUIRE (waitForVersionHistory (s.context));
    dispatchPendingMessages();

    INFO (phazite::getVersionHistory (s.context)->summariseNextCommit().toStdString());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context)));
    CHECK (phazite::getSnapshots (s.context).empty());
}

//==============================================================================
TEST_CASE (
    "A branch whose last commit can't be read says so and offers the newest commit of its log that reads, committing nothing by itself")
{
    // Known bug 59: the history read no commit, made a new "First version" on top of the one it couldn't read, and the
    // older commits left the graph.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "Readable");
    const auto readable = phazite::getLastVersion (s.context)->id;
    const auto before = findPacks (s);

    s.setVolume (bass, -6.0f);
    keepVersion (s.context, "Damaged");
    const auto damaged = phazite::getLastVersion (s.context)->id;
    auto packs = findPacks (s);
    packs.removeValuesIn (before);
    REQUIRE (packs.size() == 1); // the second commit's own pack, which holds its record

    closeAndOpenAgain (s, [&packs] { REQUIRE (packs[0].deleteFile()); });

    // Nothing committed: the branch still at the commit it can't read, which it says, offering the one before.
    CHECK (getVersionNames (s.context) == juce::StringArray ("Readable", "First version"));
    REQUIRE (phazite::getCurrentBranch (s.context).has_value());
    CHECK (phazite::getCurrentBranch (s.context)->head == damaged);
    const auto problem = phazite::getHeadProblem (s.context);
    CHECK (problem.contains ("can't be read"));
    CHECK (problem.contains ("\"Readable\""));
    CHECK (phazite::canResetToReadable (s.context));
    CHECK_FALSE (phazite::canSaveVersion (withChangesIn (s.context))); // Commit greyed, its field saying why (the review of known bug 59)
    CHECK (problem.contains ("Recover Lost Versions"));

    // No commit on top of it meanwhile.
    std::optional<juce::Result> refused;
    CHECK_FALSE (phazite::saveVersion (s.context, [&refused] (juce::Result r) { refused = r; }));
    REQUIRE (refused.has_value());
    CHECK (refused->getErrorMessage().contains ("can't be read"));

    // The branch reset to it: the song as it is, its changes since that commit, and commits on top of it again.
    phazite::CommandRegistry registry;
    phazite::addVersionCommands (registry, s.context, [] (std::function<void (bool)> then) { then (true); }, s.renders);
    REQUIRE (phazite::test::finished (s.context, [&] { return registry.invoke ("version.resetToReadable"); }));
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::getCurrentBranch (s.context)->head == readable);
    CHECK (phazite::getHeadProblem (s.context).isEmpty());
    CHECK (phazite::hasChangesSinceLastVersion (withChangesIn (s.context))); // the volume, which the commit that went had

    keepVersion (s.context, "Again");
    CHECK (getVersionNames (s.context) == juce::StringArray ("Again", "Readable", "First version"));
    CHECK (phazite::getLastVersion (s.context)->parents == juce::StringArray (readable));

    // And so it opens again.
    closeAndOpenAgain (s);
    CHECK (getVersionNames (s.context) == juce::StringArray ("Again", "Readable", "First version"));
    CHECK (phazite::getHeadProblem (s.context).isEmpty());
}

TEST_CASE ("A branch whose last commit can't be read offers the newest that reads from another computer's log, or from the packs")
{
    // The review of known bug 59: only this computer's log was searched, so a project synced or copied from another
    // computer offered nothing, and its first commit started the branch again, the whole history leaving the graph.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "Readable");
    const auto readable = phazite::getLastVersion (s.context)->id;
    const auto before = findPacks (s);
    s.setVolume (bass, -6.0f);
    keepVersion (s.context, "Damaged");
    auto packs = findPacks (s);
    packs.removeValuesIn (before);
    REQUIRE (packs.size() == 1);
    const auto local = s.getProject().getFolder().getChildFile ("Versions").getChildFile ("local");
    const auto mine = local.getChildFile (juce::File::createLegalFileName (phazite::getThisComputer())).getChildFile ("logs");
    REQUIRE (mine.isDirectory());

    SUBCASE ("another computer's log")
    {
        closeAndOpenAgain (s,
                           [&]
                           {
                               REQUIRE (packs[0].deleteFile());
                               REQUIRE (local.getChildFile ("another-computer").createDirectory());
                               REQUIRE (mine.moveFileTo (local.getChildFile ("another-computer").getChildFile ("logs")));
                           });
    }

    SUBCASE ("no log: the packs' commits of the branch")
    {
        closeAndOpenAgain (s,
                           [&]
                           {
                               REQUIRE (packs[0].deleteFile());
                               REQUIRE (mine.deleteRecursively());
                           });
    }

    CHECK (phazite::getHeadProblem (s.context).contains ("\"Readable\""));
    REQUIRE (phazite::canResetToReadable (s.context));
    CHECK_FALSE (phazite::canSaveVersion (withChangesIn (s.context)));
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::resetToReadable (s.context); }));
    CHECK (phazite::getCurrentBranch (s.context)->head == readable);
    CHECK (getVersionNames (s.context) == juce::StringArray ("Readable", "First version"));
}

TEST_CASE ("A branch none of whose commits reads says so, and a commit made by hand starts its line again")
{
    TestSong s;
    s.addTrack ("Bass"); // a version records a change: one of none isn't written
    keepVersion (s.context, "Gone");
    const auto packs = findPacks (s);

    closeAndOpenAgain (s,
                       [&packs]
                       {
                           for (const auto& pack : packs)
                               REQUIRE (pack.deleteFile());
                       });

    CHECK (phazite::getVersions (s.context).empty()); // nothing committed by itself
    CHECK (phazite::getHeadProblem (s.context).contains ("can't be read"));
    CHECK_FALSE (phazite::canResetToReadable (s.context));

    keepVersion (s.context, "Anew");
    CHECK (getVersionNames (s.context) == juce::StringArray ("Anew"));
    CHECK (phazite::getLastVersion (s.context)->parents.isEmpty());
    CHECK (phazite::getHeadProblem (s.context).isEmpty());
}

//==============================================================================
TEST_CASE ("A piece damaged in one pack is read from another that holds it, and written again by the next write that has it")
{
    // Known bug 62: the index kept the first pack that listed a piece, and a write skipped the names it had.
    TemporaryFolder temporary;
    const auto good = toBlock ("<PIECE one=\"1\"/>"), other = toBlock ("<PIECE two=\"2\"/>");
    const auto name = phazite::getPieceName (good), otherName = phazite::getPieceName (other);
    const auto folder = temporary.folder.getChildFile ("pieces");

    SUBCASE ("another pack holds it")
    {
        // The same piece in two packs, as two computers write it; the one the index finds first damaged.
        {
            phazite::PieceStore first (temporary.folder.getChildFile ("first"));
            REQUIRE (first.write ({ { name, good } }));
            phazite::PieceStore second (temporary.folder.getChildFile ("second"));
            REQUIRE (second.write ({ { name, good }, { otherName, other } }));
        }

        REQUIRE (folder.createDirectory());
        auto all = temporary.folder.getChildFile ("first").findChildFiles (juce::File::findFiles, false, "*.pack");
        all.addArray (temporary.folder.getChildFile ("second").findChildFiles (juce::File::findFiles, false, "*.pack"));
        REQUIRE (all.size() == 2);

        for (const auto& pack : all)
            REQUIRE (pack.copyFileTo (folder.getChildFile (pack.getFileName())));

        auto copied = folder.findChildFiles (juce::File::findFiles, false, "*.pack");
        copied.sort();
        damagePiece (copied[0], name); // in the first by name, whose entries the index takes first

        const phazite::PieceStore store (folder);
        const auto read = store.read (name);
        REQUIRE (read.has_value());
        CHECK (*read == good);
        CHECK (store.read (otherName).has_value());
    }

    SUBCASE ("no other pack holds it")
    {
        {
            phazite::PieceStore first (folder);
            REQUIRE (first.write ({ { name, good } }));
        }

        damageFirstPiece (folder.findChildFiles (juce::File::findFiles, false, "*.pack")[0]);
        phazite::PieceStore store (folder);
        CHECK_FALSE (store.read (name).has_value());

        // Written again, as the next commit that holds it writes it: read whole from then on, here and in a store opened anew.
        REQUIRE (store.write ({ { name, good } }));
        CHECK (store.read (name) == std::optional (good));
        const phazite::PieceStore reopened (folder);
        CHECK (reopened.read (name) == std::optional (good));
    }
}

TEST_CASE ("Verify History forgets the damaged pieces it finds, so that the next commit holding one writes it again")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");

    // Every piece damaged: no commit can be read whole.
    for (const auto& pack : findPacks (s))
        damageEveryPiece (pack);

    closeAndOpenAgain (s);
    auto* history = phazite::getVersionHistory (s.context);
    std::optional<bool> checked;
    REQUIRE (history->checkHistory ([&checked] { checked = true; }));
    REQUIRE (dispatchMessagesUntil ([&checked] { return checked.has_value(); }, versionHistoryTimeout));
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (history->getLastCheck().has_value());
    REQUIRE_FALSE (history->getLastCheck()->damagedPieces.isEmpty());

    // The song as it is now, committed: every piece of it whole again, the next check finds nothing damaged.
    s.setVolume (bass, -3.0f);
    keepVersion (s.context, "Whole again");
    checked.reset();
    REQUIRE (history->checkHistory ([&checked] { checked = true; }));
    REQUIRE (dispatchMessagesUntil ([&checked] { return checked.has_value(); }, versionHistoryTimeout));
    REQUIRE (waitForVersionHistory (s.context));
    const auto last = phazite::getLastVersion (s.context)->id;
    CHECK (history->getLastCheck()->damaged.count (last) == 0);
    CHECK (history->readSong (last).isValid());
}

//==============================================================================
TEST_CASE ("A branch isn't renamed while a commit writes its record, which would put the old name back")
{
    // Known bug 62: both wrote the branch's record, the commit's with the name it had as the commit began.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    const auto branch = phazite::getCurrentBranch (s.context)->id;

    s.setVolume (bass, -6.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (withChangesIn (s.context)); }));
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (s.context, [&result] (juce::Result r) { result = r; }));
    const auto renamed = phazite::renameBranch (s.context, branch, "renamed");
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    REQUIRE (waitForVersionHistory (s.context));
    CHECK_FALSE (renamed);
    CHECK (phazite::getCurrentBranch (s.context)->name == "main");

    // Once it is written, it is.
    REQUIRE (phazite::renameBranch (s.context, branch, "renamed"));
    closeAndOpenAgain (s);
    CHECK (phazite::getCurrentBranch (s.context)->name == "renamed");
}

//==============================================================================
TEST_CASE ("A damaged pack another program holds isn't replaced, and the write says why; once it lets go, it is")
{
    // The review of known bug 62: a pack whose pieces were all found damaged is written again under its own name, which
    // a sync program or a virus scanner holding the old file keeps from happening. A pack's name holds its writer and when
    // its pieces were written, so the same name comes again only for the same pieces written by the same computer at the
    // same time on the versions' clock: as here, the clock held, so that it means the same on every system (Windows'
    // clock moves in steps of about 15 ms, Linux's every millisecond).
    const phazite::test::VersionsTime clock (juce::Time::currentTimeMillis());
    TemporaryFolder temporary;
    const auto piece = toBlock ("<PIECE held=\"1\"/>");
    const auto name = phazite::getPieceName (piece);
    const auto folder = temporary.folder.getChildFile ("pieces");

    {
        phazite::PieceStore first (folder);
        REQUIRE (first.write ({ { name, piece } }));
    }

    const auto pack = folder.findChildFiles (juce::File::findFiles, false, "*.pack")[0];
    damageFirstPiece (pack);
    phazite::PieceStore store (folder);
    REQUIRE_FALSE (store.read (name).has_value()); // forgotten
    store.closeFiles();                            // the store's own open file would keep it from being held on Windows

    {
        const HeldFile held (pack);

        if (held.works())
        {
            juce::String problem;
            CHECK_FALSE (store.write ({ { name, piece } }, &problem));
            CHECK (problem.contains (pack.getFileName()));
            CHECK (problem.contains ("Another program"));
            CHECK_FALSE (store.read (name).has_value());
        }
    }

    // Let go of: written (its stamp a millisecond on from the one that failed, so into a pack of its own beside it).
    REQUIRE (store.write ({ { name, piece } }));
    CHECK (store.read (name) == std::optional (piece));
}

TEST_CASE ("A piece whose only copy was found damaged is written again into a new pack once the clock has moved on")
{
    // What happens as a rule: the damaged pack stays (Verify History names it) and the piece reads from the new one.
    phazite::test::VersionsTime clock (juce::Time::currentTimeMillis());
    TemporaryFolder temporary;
    const auto piece = toBlock ("<PIECE again=\"1\"/>");
    const auto name = phazite::getPieceName (piece);
    const auto folder = temporary.folder.getChildFile ("pieces");

    {
        phazite::PieceStore first (folder);
        REQUIRE (first.write ({ { name, piece } }));
    }

    const auto pack = folder.findChildFiles (juce::File::findFiles, false, "*.pack")[0];
    damageFirstPiece (pack);
    phazite::PieceStore store (folder);
    REQUIRE_FALSE (store.read (name).has_value());

    clock.addDays (1.0);
    REQUIRE (store.write ({ { name, piece } }));
    CHECK (store.read (name) == std::optional (piece));
    CHECK (pack.existsAsFile());
    CHECK (folder.findChildFiles (juce::File::findFiles, false, "*.pack").size() == 2);
}

//==============================================================================
TEST_CASE ("A plug-in that loads after the project opens counts as changed once its knobs can be read")
{
    // The review of known bug 126: a plug-in's knobs say whether its state, which differs from the last commit's, counts
    // as a change; they can be read only once it has loaded, and nothing in the song moves then.
    const TestPlugin plugin;
    TestSong s;
    auto& song = *phazite::findSong (s.context);
    phazite::DeviceID device;

    {
        const phazite::UndoStep step (song, "Add Device");
        device = TestPlugin::add (song, s.addTrack ("Keys"));
    }

    REQUIRE (dispatchMessagesUntil ([&song] { return phazite::getPluginsLoading (song).isEmpty(); }));
    keepVersion (s.context, "Gain at one");
    TestPlugin::setGain (song, device, 0.25f); // in its own window
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (withChangesIn (s.context)); }));

    s.reopen (false); // its state saved, the plug-in loading as the history reads
    auto& reopened = *phazite::findSong (s.context);
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (dispatchMessagesUntil ([&reopened] { return phazite::getPluginsLoading (reopened).isEmpty(); }));
    CHECK (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (withChangesIn (s.context)); }, versionHistoryTimeout));
}

//==============================================================================
namespace
{
// A commit another computer wrote (or another tool, or anyone), of a song as given, on a branch of its own after the current
// branch's last commit, put into the project's Versions folder as a synced folder brings it; then the project opened again,
// whose history reads it. Its id.
juce::String writeOthersCommit (TestSong& s, const juce::ValueTree& song, const juce::String& branchName)
{
    REQUIRE (waitForVersionHistory (s.context));
    const auto head = phazite::getLastVersion (s.context)->id;
    const auto folder = s.getProject().getFolder().getChildFile ("Versions");
    juce::String id;

    {
        phazite::VersionStore other (folder, "another-computer");
        phazite::VersionRecord record;
        record.name = "From elsewhere";
        record.parents = { head };
        record.branch = "branch-" + branchName;
        record.saved = juce::Time::currentTimeMillis();
        id = other.writeVersion (record, phazite::splitSong (song));
        REQUIRE (id.isNotEmpty());

        phazite::BranchRecord branch;
        branch.id = record.branch;
        branch.name = branchName;
        branch.colour = juce::Colours::orange;
        branch.head = id;
        branch.computer = "another-computer";
        branch.changed = record.saved;
        REQUIRE (other.writeBranch (branch));
        other.getPieces().closeFiles();
    }

    s.reopen (false);
    REQUIRE (waitForVersionHistory (s.context));
    return id;
}
} // namespace

TEST_CASE ("A song read from the versions' pieces holds together as a project file does as it opens, whoever wrote them")
{
    // Found with known bug 126's work: the loader's checks (Project.h: makeConsistent()) never ran on a song joined from the
    // versions' pieces, so a history another computer or tool wrote put what no Phazite writes into the song: numbers that
    // are none or out of range, an ID twice, a send to a return that isn't there.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto drums = s.addTrack ("Drums");
    s.setVolume (bass, -6.0f);
    keepVersion (s.context, "Mine");

    auto hostile = phazite::test::copyWholeSong (*phazite::findSong (s.context));
    auto tracks = phazite::getTrackElements (hostile);
    REQUIRE (tracks.size() >= 2);
    tracks[0].setProperty (phazite::schema::channelVolume, "nan", nullptr);
    tracks[1].setProperty (phazite::schema::channelPan, "1e300", nullptr);
    hostile.getChildWithName (phazite::schema::TEMPOMAP).getChild (0).setProperty (phazite::schema::bpm, "inf", nullptr);
    tracks[1].appendChild (
        juce::ValueTree (phazite::schema::SEND, { { phazite::schema::sendID, 987654 }, { phazite::schema::sendTo, 876543 } }), nullptr);
    auto twin = tracks[0].createCopy();
    twin.setProperty (phazite::schema::trackName, "Twin", nullptr);
    tracks[0].getParent().appendChild (twin, nullptr); // the same ID twice
    const auto theirs = writeOthersCommit (s, hostile, "elsewhere");
    auto& song = *phazite::findSong (s.context);

    SUBCASE ("A branch from it, switched to")
    {
        REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, theirs, s.save); }));
        checkSong (song);
        CHECK_FALSE (phazite::hasChangesSinceLastVersion (withChangesIn (s.context))); // the version as it is read, the same each time
    }

    SUBCASE ("Restored")
    {
        // Not with an ID twice, as a merge isn't (found in review, known bug 126's round): given anew as it is read, an item
        // would come in as new beside the song's own, unseen. Said, nothing restored.
        CHECK_FALSE (finished (s.context, [&] { return phazite::bringBackVersion (s.context, theirs); }));
        CHECK (phazite::getReadingProblem (s.context).contains ("ID twice"));

        // Without it, restored, holding together.
        auto single = hostile.createCopy();
        auto list = phazite::getTrackElements (single).back().getParent();
        list.removeChild (list.getNumChildren() - 1, nullptr); // the twin, added last
        const auto other = writeOthersCommit (s, single, "elsewhere-2");
        REQUIRE (finished (s.context, [&] { return phazite::bringBackVersion (s.context, other); }));
        checkSong (*phazite::findSong (s.context));
    }

    SUBCASE ("Merged")
    {
        // Not with an ID twice: given anew as it is read, each side on its own, what the two lines share would no longer
        // have the same IDs (found in review, known bug 126's round). Said, nothing merged.
        CHECK_FALSE (finished (s.context, [&] { return phazite::startMerge (s.context, theirs); }));
        CHECK (phazite::getReadingProblem (s.context).contains ("ID twice"));
        CHECK_FALSE (phazite::isMerging (s.context));

        // Without it, merged, holding together.
        auto single = hostile.createCopy();
        auto list = phazite::getTrackElements (single).back().getParent();
        list.removeChild (list.getNumChildren() - 1, nullptr); // the twin, added last
        const auto other = writeOthersCommit (s, single, "elsewhere-2");
        REQUIRE (finished (s.context, [&] { return phazite::startMerge (s.context, other); }));
        phazite::finishMerge (s.context);
        REQUIRE (phazite::isMerging (s.context));
        checkSong (*phazite::findSong (s.context));
    }

    juce::ignoreUnused (drums);
}

TEST_CASE ("A commit whose song holds an ID past 2^61 inside an element Phazite doesn't know is restored, nothing given anew")
{
    // The loader gives nothing in such an element anew, nor counts what is in it past 2^61 (Schema.h: findLargestID()); the
    // versions judge the commit the same way, so it is no song given its IDs anew.
    TestSong s;
    s.addTrack ("Bass");
    keepVersion (s.context, "Mine");

    auto future = phazite::test::copyWholeSong (*phazite::findSong (s.context));
    juce::ValueTree element ("FUTURE");
    element.appendChild (
        juce::ValueTree (phazite::schema::TRACK, { { phazite::schema::trackID, (juce::int64) (phazite::schema::maxID - 1) } }), nullptr);

    // And one in there with the ID a track of the song holds: no ID held twice, as nothing in there is the song's.
    const auto own = phazite::getTrackElements (future);
    REQUIRE_FALSE (own.empty());
    element.appendChild (juce::ValueTree (phazite::schema::TRACK, { { phazite::schema::trackID, own.back()[phazite::schema::trackID] } }),
                         nullptr);
    future.appendChild (element, nullptr);
    const auto theirs = writeOthersCommit (s, future, "elsewhere");

    REQUIRE (finished (s.context, [&] { return phazite::bringBackVersion (s.context, theirs); }));
    CHECK (phazite::getReadingProblem (s.context).isEmpty());
    checkSong (*phazite::findSong (s.context));
}

TEST_CASE (
    "A commit whose song's IDs are given anew as it is read, past the mark, is refused for Restore and Merge as one with an ID twice")
{
    // The loader gives a song whose IDs reach schema::renumberMark its IDs past 2^61 anew (Schema.h: mustGiveIDsAnew()),
    // each reading on its own: what it holds can't be told from what the song has, as with an ID held twice.
    TestSong s;
    s.addTrack ("Bass");
    keepVersion (s.context, "Mine");

    auto hostile = phazite::test::copyWholeSong (*phazite::findSong (s.context));
    auto tracks = phazite::getTrackElements (hostile);
    REQUIRE_FALSE (tracks.empty());
    tracks.back().setProperty (phazite::schema::trackID, (juce::int64) phazite::schema::renumberMark, nullptr);
    const auto theirs = writeOthersCommit (s, hostile, "elsewhere");

    SUBCASE ("Restored")
    {
        CHECK_FALSE (finished (s.context, [&] { return phazite::bringBackVersion (s.context, theirs); }));
        CHECK (phazite::getReadingProblem (s.context).contains ("too large to count on from"));
    }

    SUBCASE ("Merged")
    {
        CHECK_FALSE (finished (s.context, [&] { return phazite::startMerge (s.context, theirs); }));
        CHECK (phazite::getReadingProblem (s.context).contains ("too large to count on from"));
        CHECK_FALSE (phazite::isMerging (s.context));
    }
}

namespace
{
phazite::TrackID findTrackNamed (const phazite::Song& song, const juce::String& name)
{
    for (const auto track : phazite::getTrackIDs (song))
        if (phazite::getTrackName (song, track) == name)
            return track;

    return {};
}

juce::String writeSongText (const phazite::ProjectContext& context)
{
    return phazite::writeElement (phazite::test::copyWholeSong (*phazite::findSong (context))).toString();
}

juce::String readNumbering (const phazite::ProjectContext& context)
{
    return phazite::findSong (context)->getState()[phazite::schema::idNumbering].toString();
}

// Tracks added until the song's next ID is past the mark (Schema.h: renumberMark), so that its next opening gives its IDs
// anew.
void passTheMark (TestSong& s, const juce::String& name)
{
    while (phazite::findNextID (phazite::test::copyWholeSong (*phazite::findSong (s.context))) <= phazite::schema::renumberMark)
        s.addTrack (name);
}

// Known bug 186's song. A file whose largest ID lies just below the mark opens as it is; "Before" is saved, and another
// computer's version from it ("old-line", its branch parted at "Before"); what the session makes passes the mark, and the
// next opening gives every ID past 2^61 anew, the song stamped with its new numbering (Schema.h: idNumbering). The versions
// saved before hold the IDs as they were.
struct GivenAnew
{
    juce::String before, oldLine, stamp;
    phazite::TrackID bass;
};

GivenAnew openGivenAnew (TestSong& s)
{
    GivenAnew made;
    const auto mark = (juce::int64) phazite::schema::renumberMark;

    {
        auto edited = phazite::test::copyWholeSong (*phazite::findSong (s.context));
        auto tracks = phazite::getTrackElements (edited);
        REQUIRE (tracks.size() == 1);
        tracks.front().setProperty (phazite::schema::trackID, mark - 20, nullptr);
        tracks.front().setProperty (phazite::schema::trackName, "Bass", nullptr);
        s.openSong (edited);
    }

    REQUIRE (findTrackNamed (*phazite::findSong (s.context), "Bass").getRaw() == (std::uint64_t) (mark - 20));
    keepVersion (s.context, "Before");
    made.before = phazite::getLastVersion (s.context)->id;
    s.addTrack ("Idea");
    made.oldLine = writeOthersCommit (s, phazite::test::copyWholeSong (*phazite::findSong (s.context)), "old-line");
    passTheMark (s, "More");

    REQUIRE (readNumbering (s.context).isEmpty());
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    checkSong (*phazite::findSong (s.context));
    made.stamp = readNumbering (s.context);
    REQUIRE (made.stamp.length() == phazite::schema::idNumberingDigits);
    made.bass = findTrackNamed (*phazite::findSong (s.context), "Bass");
    REQUIRE (made.bass.isValid());
    REQUIRE (made.bass.getRaw() < phazite::schema::maxCountedID); // given anew
    return made;
}

// What a refusal of a line's Restore and of a Merge for another numbering says, whichever side is the older.
const juce::String otherNumbering = "number their tracks and clips differently";
} // namespace

TEST_CASE ("After a song's IDs were given anew as it opened, a line's Restore and Merge refuse the versions from before, and "
           "take those after")
{
    // Known bug 186 (openGivenAnew()): matched by ID, Bass restored from a version from before came back beside Bass, and a
    // merge brought its tracks in again.
    TestSong s;
    const auto mark = (juce::int64) phazite::schema::renumberMark;
    const auto made = openGivenAnew (s);
    const auto song = [&s]() -> phazite::Song& { return *phazite::findSong (s.context); }; // opened again by each commit of others
    const auto& before = made.before;
    const auto& oldLine = made.oldLine;
    const auto& stamp = made.stamp;
    const auto bass = made.bass;
    phazite::VersionChange bassLine;
    bassLine.name = "Bass";
    bassLine.track = bass;
    auto now = writeSongText (s.context);

    // Bass restored from the version before: refused, with why, the song as it was.
    CHECK_FALSE (finished (s.context, [&] { return phazite::restoreFromVersion (s.context, before, bassLine); }));
    CHECK (phazite::getReadingProblem (s.context).contains ("\"Before\" and the song " + otherNumbering));
    CHECK (phazite::getReadingProblem (s.context).contains ("Open the version to hear it, or restore the whole version"));
    CHECK (writeSongText (s.context) == now);

    // Its changes discarded from it, the last version: refused, with why.
    CHECK_FALSE (phazite::takeBack (s.context, bassLine));
    CHECK (phazite::getRestoreProblem (s.context).contains ("Nothing was discarded: the last version and the song " + otherNumbering));
    CHECK (phazite::getRestoreProblem (s.context).contains ("Discard All Changes"));
    CHECK (writeSongText (s.context) == now);

    // The other computer's version merged, the other side from before: refused, with what does work on another branch.
    CHECK_FALSE (finished (s.context, [&] { return phazite::startMerge (s.context, oldLine); }));
    CHECK (phazite::getReadingProblem (s.context).contains ("Nothing was merged"));
    CHECK (phazite::getReadingProblem (s.context).contains (otherNumbering));
    CHECK (phazite::getReadingProblem (s.context).contains ("Switch to its branch to go on from it"));
    CHECK_FALSE (phazite::isMerging (s.context));
    CHECK (writeSongText (s.context) == now);

    // One numbered as the song is now, whose parting with the song is the version before: refused.
    const auto newLine = writeOthersCommit (s, phazite::test::copyWholeSong (song()), "new-line");
    CHECK (song().getState()[phazite::schema::idNumbering].toString() == stamp); // saved and opened again: nothing given anew
    now = writeSongText (s.context);
    CHECK_FALSE (finished (s.context, [&] { return phazite::startMerge (s.context, newLine); }));
    CHECK (phazite::getReadingProblem (s.context).contains (otherNumbering));
    CHECK_FALSE (phazite::isMerging (s.context));
    CHECK (writeSongText (s.context) == now);

    // The version before opened in the song's place: the song is that version, as it was numbered, and Back to Now brings it
    // back exactly.
    REQUIRE (finished (s.context, [&] { return phazite::openVersion (s.context, before, s.save); }));
    CHECK_FALSE (song().getState().hasProperty (phazite::schema::idNumbering));
    CHECK (findTrackNamed (song(), "Bass").getRaw() == (std::uint64_t) (mark - 20));
    CHECK_FALSE (findTrackNamed (song(), "Idea").isValid());
    REQUIRE (phazite::backToNow (s.context));
    CHECK (writeSongText (s.context) == now);

    // And restored whole: the song is that version as it was numbered, one undo step, which Undo takes back.
    REQUIRE (finished (s.context, [&] { return phazite::bringBackVersion (s.context, before); }));
    CHECK_FALSE (song().getState().hasProperty (phazite::schema::idNumbering));
    CHECK (findTrackNamed (song(), "Bass").getRaw() == (std::uint64_t) (mark - 20));
    checkSong (song());
    phazite::undo (song());
    CHECK (findTrackNamed (song(), "Bass") == bass);
    CHECK (song().getState()[phazite::schema::idNumbering].toString() == stamp);

    // A version saved after: a line restored from it, a line's changes discarded and a merge from it, as ever.
    keepVersion (s.context, "After");
    const auto after = phazite::getLastVersion (s.context)->id;

    // The other way round, the song the older: made the version from before again, a line restored from the version saved
    // after is refused with the same words; Undo brings the song back.
    REQUIRE (finished (s.context, [&] { return phazite::bringBackVersion (s.context, before); }));
    now = writeSongText (s.context);
    CHECK_FALSE (finished (s.context, [&] { return phazite::restoreFromVersion (s.context, after, bassLine); }));
    CHECK (phazite::getReadingProblem (s.context).contains ("\"After\" and the song " + otherNumbering));
    CHECK (writeSongText (s.context) == now);
    phazite::undo (song());
    REQUIRE (readNumbering (s.context) == stamp);

    s.rename (bass, "Bass changed");
    REQUIRE (finished (s.context, [&] { return phazite::restoreFromVersion (s.context, after, bassLine); }));
    CHECK (phazite::getTrackName (song(), bass) == "Bass");

    s.rename (bass, "Bass changed");
    REQUIRE (phazite::takeBack (s.context, bassLine));
    CHECK (phazite::getTrackName (song(), bass) == "Bass");
    CHECK (phazite::getRestoreProblem (s.context).isEmpty());

    auto theirs = phazite::test::copyWholeSong (song());
    for (auto track : phazite::getTrackElements (theirs))
        if (phazite::readID (track[phazite::schema::trackID]) == bass.getRaw())
            track.setProperty (phazite::schema::trackName, "Bass elsewhere", nullptr);

    const auto afterLine = writeOthersCommit (s, theirs, "after-line");
    REQUIRE (finished (s.context, [&] { return phazite::startMerge (s.context, afterLine); }));
    REQUIRE (phazite::finishMerge (s.context));
    CHECK (phazite::getTrackName (song(), bass) == "Bass elsewhere");
    checkSong (song());
}

namespace
{
// Every file in the project's Versions folder, with its size: what a refusal must leave as it is.
juce::String listVersionsFolder (TestSong& s)
{
    juce::StringArray files;

    for (const auto& file : s.getProject().getFolder().getChildFile ("Versions").findChildFiles (juce::File::findFiles, true))
        files.add (file.getFullPathName() + " " + juce::String (file.getSize()));

    files.sort (false);
    return files.joinIntoString ("\n");
}
} // namespace

TEST_CASE ("A merge's list worked out against the song in one numbering takes nothing into it once it is in another")
{
    // Found in review of known bug 186's fix: the list survives Restore This Version and its Undo, which can make the song
    // another numbering's (Schema.h: idNumbering) while it shows, and Merge took old-numbered songs into the song as it is
    // numbered now, or the other way round. It keeps the song's stamp as it was worked out, and refuses another.
    TestSong s;
    const auto made = openGivenAnew (s);
    const auto song = [&s]() -> phazite::Song& { return *phazite::findSong (s.context); };

    SUBCASE ("Listed for the song made a version from before again, then that undone")
    {
        // Restore This Version of the version from before: the song is that version's, without a stamp, as the other
        // computer's version and their parting are; the list shows. Then Undo: the song given its IDs anew again.
        REQUIRE (finished (s.context, [&] { return phazite::bringBackVersion (s.context, made.before); }));
        REQUIRE (readNumbering (s.context).isEmpty());
        REQUIRE (finished (s.context, [&] { return phazite::startMerge (s.context, made.oldLine); }));
        REQUIRE_FALSE (phazite::getMergeLines (s.context).empty());
        phazite::undo (song());
        REQUIRE (readNumbering (s.context) == made.stamp);
    }

    SUBCASE ("Listed for the song as it was given its IDs anew, then Restore This Version of a version from before")
    {
        // A version saved after, and another computer's from it, each with the song's stamp: the list shows. Then the song
        // made the version from before again, without a stamp.
        keepVersion (s.context, "After");
        auto theirs = phazite::test::copyWholeSong (song());

        for (auto track : phazite::getTrackElements (theirs))
            if (phazite::readID (track[phazite::schema::trackID]) == made.bass.getRaw())
                track.setProperty (phazite::schema::trackName, "Bass elsewhere", nullptr);

        const auto newLine = writeOthersCommit (s, theirs, "new-line");
        REQUIRE (readNumbering (s.context) == made.stamp);
        REQUIRE (finished (s.context, [&] { return phazite::startMerge (s.context, newLine); }));
        REQUIRE_FALSE (phazite::getMergeLines (s.context).empty());
        REQUIRE (finished (s.context, [&] { return phazite::bringBackVersion (s.context, made.before); }));
        REQUIRE (readNumbering (s.context).isEmpty());
    }

    // Merge: refused with why, in the words of a merge's other refusal for this, and nothing changed: the song, its undo
    // history (no step begun, Redo still there where it was), the Versions folder; the list stays, to cancel.
    REQUIRE (waitForVersionHistory (s.context));
    const auto now = writeSongText (s.context);
    const auto undoName = song().getUndoManager().getUndoDescription();
    const auto couldRedo = song().getUndoManager().canRedo();
    const auto folder = listVersionsFolder (s);

    CHECK_FALSE (finished (s.context, [&] { return phazite::finishMerge (s.context); }));
    CHECK (phazite::getMergeProblem (s.context).contains ("Nothing was merged"));
    CHECK (phazite::getMergeProblem (s.context).contains (otherNumbering));
    CHECK (phazite::getMergeProblem (s.context).contains ("Switch to its branch to go on from it"));
    CHECK_FALSE (phazite::isMerging (s.context));
    CHECK_FALSE (phazite::getMergeLines (s.context).empty());
    CHECK (writeSongText (s.context) == now);
    CHECK (song().getUndoManager().getUndoDescription() == undoName);
    CHECK (song().getUndoManager().canRedo() == couldRedo);
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (listVersionsFolder (s) == folder);

    // Cancel Merge closes the list, taking nothing.
    phazite::cancelMerge (s.context);
    CHECK (phazite::getMergeLines (s.context).empty());
    CHECK (writeSongText (s.context) == now);
}

TEST_CASE ("A song that crosses the mark again gets another stamp, and the versions saved between are refused for a line's "
           "Restore")
{
    // Schema.h: renumberMark. Restore This Version of a version from before takes its count of IDs near the mark, which Undo
    // doesn't put back: a later session crosses again, the next opening gives the IDs anew once more, with another stamp,
    // and the version saved between the two crossings no longer matches the song by ID.
    TestSong s;
    const auto made = openGivenAnew (s);
    keepVersion (s.context, "Between");
    const auto between = phazite::getLastVersion (s.context)->id;

    REQUIRE (finished (s.context, [&] { return phazite::bringBackVersion (s.context, made.before); }));
    phazite::undo (*phazite::findSong (s.context));
    REQUIRE (readNumbering (s.context) == made.stamp);
    CHECK (phazite::findNextID (phazite::test::copyWholeSong (*phazite::findSong (s.context)))
           > phazite::schema::maxCountedID); // kept by the Undo

    passTheMark (s, "Later");
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    checkSong (*phazite::findSong (s.context));
    const auto second = readNumbering (s.context);
    CHECK (second.length() == phazite::schema::idNumberingDigits);
    CHECK (second != made.stamp);

    // Bass restored from the version saved between: refused, with why, the song as it was.
    const auto bass = findTrackNamed (*phazite::findSong (s.context), "Bass");
    REQUIRE (bass.isValid());
    phazite::VersionChange bassLine;
    bassLine.name = "Bass";
    bassLine.track = bass;
    const auto now = writeSongText (s.context);
    CHECK_FALSE (finished (s.context, [&] { return phazite::restoreFromVersion (s.context, between, bassLine); }));
    CHECK (phazite::getReadingProblem (s.context).contains ("\"Between\" and the song " + otherNumbering));
    CHECK (writeSongText (s.context) == now);
}

TEST_CASE ("The loader's checks change nothing in a song Phazite wrote, read back from its pieces")
{
    // A version read back is what was saved, so that the changes and the badge see none after a switch: the
    // representative project's song, every name Phazite saves in it, split and joined again.
    TestSong s;
    const auto golden = getGoldenFolder().getChildFile ("projects/representative.xml");
    const auto file = s.getProject().getFolder().getChildFile ("Representative.phazite");
    REQUIRE (golden.copyFileTo (file));
    juce::String error;
    const auto project = phazite::Project::open (file, false, error);
    REQUIRE (project != nullptr);
    const auto song = phazite::test::copyWholeSong (project->getSong()); // the song whole, as a version holds it
    const auto pieces = phazite::splitSong (song);
    const auto joined = phazite::joinSong (
        pieces.song,
        [&pieces] (const juce::String& name) -> std::optional<juce::MemoryBlock>
        {
            const auto found = pieces.pieces.find (name);
            return found != pieces.pieces.end() ? std::optional (found->second) : std::nullopt;
        },
        nullptr, nullptr, project->getFolder());

    REQUIRE (joined.isValid());
    CHECK (phazite::writeElement (joined).toString() == phazite::writeElement (song).toString());
}
