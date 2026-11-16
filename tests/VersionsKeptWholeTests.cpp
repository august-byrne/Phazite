// SPDX-License-Identifier: AGPL-3.0-or-later
// Keeping the versions whole (docs/design/versions.md, docs/versions-format.md): what the history writes is written
// before it goes, a project opened as it was committed has no changes, a branch whose last commit can't be read says so,
// a damaged piece is read from another pack or written again, and a branch isn't renamed while a commit writes its
// record.
#include "BenchmarkSong.h"
#include "HeldFile.h"
#include "TestClips.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
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

// Every piece of a pack damaged, its index left as it is (PieceStore.h: where it starts is 12 bytes from its end).
void damageEveryPiece (const juce::File& pack)
{
    juce::MemoryBlock bytes;
    REQUIRE (pack.loadFileAsData (bytes));
    REQUIRE (bytes.getSize() > 16);
    const auto indexStart = (size_t) juce::ByteOrder::littleEndianInt64 (static_cast<const char*> (bytes.getData()) + bytes.getSize() - 12);
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
    REQUIRE (getVersionNames (s.context) == juce::StringArray ("Initial commit"));

    s.setVolume (bass, -6.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (s.context); }));
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
    CHECK (getVersionNames (s.context) == juce::StringArray ("Written as it closed", "Initial commit"));
    CHECK (phazite::getSnapshots (s.context).size() == 1);
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    waitForVersionsFinished();
}

TEST_CASE ("Closing doesn't wait for what the history reads: a read given as it closes never runs")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    const auto first = phazite::getLastVersion (s.context)->id;
    s.setVolume (bass, -6.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (s.context); }));
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
    REQUIRE (dispatchMessagesUntil ([&song] { return phazite::getPluginsLoading (song).isEmpty(); }, 60000));
    s.setVolume (keys, -3.0f);

    keepVersion (s.context, "All of it");
    REQUIRE_FALSE (phazite::hasChangesSinceLastVersion (s.context));

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
    REQUIRE (dispatchMessagesUntil ([&reopened] { return phazite::getPluginsLoading (reopened).isEmpty(); }, 60000));
    REQUIRE (waitForVersionHistory (s.context));
    dispatchPendingMessages();

    INFO (phazite::getVersionHistory (s.context)->summariseNextCommit().toStdString());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
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
    const auto saved = phazite::findSong (s.context)->getState().createCopy();

    s.openSong (saved);
    REQUIRE (waitForVersionHistory (s.context));
    dispatchPendingMessages();

    INFO (phazite::getVersionHistory (s.context)->summariseNextCommit().toStdString());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
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
    s.setParameter (synth, "drift", 0.5f);
    keepVersion (s.context, "First");
    REQUIRE (s.getProject().save().wasOk());
    auto written = phazite::findSong (s.context)->getState().createCopy();

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
    REQUIRE (values.hasProperty ("drift"));
    values.setProperty ("subLevel", "1.0", nullptr);
    values.setProperty ("drift", "5e-1", nullptr);

    s.openSong (written);
    REQUIRE (waitForVersionHistory (s.context));
    dispatchPendingMessages();

    INFO (phazite::getVersionHistory (s.context)->summariseNextCommit().toStdString());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
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
    s.setParameter (synth, "drift", 0.3f);
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
    REQUIRE (dispatchMessagesUntil ([&song] { return phazite::getPluginsLoading (song).isEmpty(); }, 60000));
    keepVersion (s.context, "As it came");
    REQUIRE_FALSE (phazite::hasChangesSinceLastVersion (s.context));

    s.reopen (false);
    REQUIRE (waitForVersionHistory (s.context));
    dispatchPendingMessages();

    INFO (phazite::getVersionHistory (s.context)->summariseNextCommit().toStdString());
    CHECK_FALSE (phazite::hasChangesSinceLastVersion (s.context));
    CHECK (phazite::getSnapshots (s.context).empty());
}

//==============================================================================
TEST_CASE (
    "A branch whose last commit can't be read says so and offers the newest commit of its log that reads, committing nothing by itself")
{
    // Known bug 59: the history read no commit, made a new "Initial commit" on top of the one it couldn't read, and the
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
    CHECK (getVersionNames (s.context) == juce::StringArray ("Readable", "Initial commit"));
    REQUIRE (phazite::getCurrentBranch (s.context).has_value());
    CHECK (phazite::getCurrentBranch (s.context)->head == damaged);
    const auto problem = phazite::getHeadProblem (s.context);
    CHECK (problem.contains ("can't be read"));
    CHECK (problem.contains ("\"Readable\""));
    CHECK (phazite::canResetToReadable (s.context));
    CHECK_FALSE (phazite::canSaveVersion (s.context)); // Commit greyed, its field saying why (the review of known bug 59)
    CHECK (problem.contains ("Recover Lost Commits"));

    // No commit on top of it meanwhile.
    std::optional<juce::Result> refused;
    CHECK_FALSE (phazite::saveVersion (s.context, [&refused] (juce::Result r) { refused = r; }));
    REQUIRE (refused.has_value());
    CHECK (refused->getErrorMessage().contains ("can't be read"));

    // The branch reset to it: the song as it is, its changes since that commit, and commits on top of it again.
    phazite::CommandRegistry registry;
    phazite::addVersionCommands (registry, s.context, [] (std::function<void (bool)> then) { then (true); }, s.renders);
    REQUIRE (registry.invoke ("version.resetToReadable"));
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::getCurrentBranch (s.context)->head == readable);
    CHECK (phazite::getHeadProblem (s.context).isEmpty());
    CHECK (phazite::hasChangesSinceLastVersion (s.context)); // the volume, which the commit that went had

    keepVersion (s.context, "Again");
    CHECK (getVersionNames (s.context) == juce::StringArray ("Again", "Readable", "Initial commit"));
    CHECK (phazite::getLastVersion (s.context)->parents == juce::StringArray (readable));

    // And so it opens again.
    closeAndOpenAgain (s);
    CHECK (getVersionNames (s.context) == juce::StringArray ("Again", "Readable", "Initial commit"));
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
    CHECK_FALSE (phazite::canSaveVersion (s.context));
    REQUIRE (phazite::resetToReadable (s.context));
    CHECK (phazite::getCurrentBranch (s.context)->head == readable);
    CHECK (getVersionNames (s.context) == juce::StringArray ("Readable", "Initial commit"));
}

TEST_CASE ("A branch none of whose commits reads says so, and a commit made by hand starts its line again")
{
    TestSong s;
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
        damageFirstPiece (copied[0]); // the first by name, whose entries the index takes first

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
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (s.context); }));
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
    // a sync program or a virus scanner holding the old file keeps from happening.
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

    REQUIRE (store.write ({ { name, piece } }));
    CHECK (store.read (name) == std::optional (piece));
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

    REQUIRE (dispatchMessagesUntil ([&song] { return phazite::getPluginsLoading (song).isEmpty(); }, 60000));
    keepVersion (s.context, "Gain at one");
    TestPlugin::setGain (song, device, 0.25f); // in its own window
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (s.context); }));

    s.reopen (false); // its state saved, the plug-in loading as the history reads
    auto& reopened = *phazite::findSong (s.context);
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (dispatchMessagesUntil ([&reopened] { return phazite::getPluginsLoading (reopened).isEmpty(); }, 60000));
    CHECK (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (s.context); }, versionHistoryTimeout));
}
