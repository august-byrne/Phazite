// SPDX-License-Identifier: AGPL-3.0-or-later
// The versions' store swept (model/versions/internal/StoreSweep.h; docs/design/versions.md: The store; roadmap S4.4): what
// each kind of naming keeps, what goes once this computer wrote it more than 30 days before the keeping time and nothing
// names it, that nothing younger goes, that each computer sweeps only its own packs, that a clock set ahead and a long
// break let go of no more than a week's worth, and that a write that fails, a crash, a pack another program holds and a
// file that doesn't read whole leave every piece readable. And Verify History. The days pass on the versions' clock, which
// the tests set (VersionsTime), the store swept once a week of them as a project opened that often would be. The letting go
// is off in the product (StoreSweep.h: sweepLetsGo): every test but one turns it on (SweepLetsGo); that one checks that the
// product's sweep lets go of nothing.
#include "HeldFile.h"
#include "TestDeviceValues.h"
#include "TestSong.h"
#include "TestSongOwn.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/SongText.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/StoreSweep.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"

#include <algorithm>
#include <thread>

namespace
{
using namespace phazite::test;
namespace schema = phazite::schema;

// A plug-in Phazite knows by its name alone, which no helper loads: a device whose state is bytes the store keeps.
juce::PluginDescription testSynth()
{
    juce::PluginDescription description;
    description.name = "Test Synth";
    return description;
}

// A store of a project's versions on a computer, its clock at a time the test moves, its first sweep noted as it begins
// (a first sweep leaves nothing out), and songs that differ by a volume, each written whole as a version or as a song kept
// without one.
struct Store
{
    TemporaryFolder temporary;
    VersionsTime clock { juce::Time::currentTimeMillis() };
    const juce::File folder = temporary.folder.getChildFile ("Versions");
    std::unique_ptr<phazite::VersionStore> store = std::make_unique<phazite::VersionStore> (folder, phazite::getThisComputer());
    phazite::Song::Ptr song = phazite::createSong (temporary.folder.getChildFile ("Song.phazite"));
    phazite::TrackID track = phazite::ensureTracks (*song, 1)[0];

    Store() { sweep(); }

    phazite::SongPieces makeSong (float volume)
    {
        phazite::setChannelVolume (*song, track, volume);
        return phazite::splitSong (phazite::test::copyWholeSong (*song));
    }

    juce::String saveVersion (float volume, juce::StringArray parents = {}, const juce::String& branch = "main",
                              phazite::VersionStore* into = nullptr)
    {
        phazite::VersionRecord record;
        record.parents = std::move (parents);
        record.branch = branch;
        record.saved = clock.get();
        const auto id = (into != nullptr ? *into : *store).writeVersion (record, makeSong (volume));
        REQUIRE (id.isNotEmpty());
        return id;
    }

    juce::String keepSong (float volume)
    {
        const auto pieces = makeSong (volume);
        REQUIRE (store->writeSong (pieces));
        return pieces.song;
    }

    void putBranch (const juce::String& id, const juce::String& head, const juce::String& working = {}, bool deleted = false,
                    phazite::VersionStore* into = nullptr)
    {
        auto& writer = into != nullptr ? *into : *store;
        phazite::BranchRecord branch;
        branch.id = id;
        branch.name = id;
        branch.head = head;
        branch.computer = writer.getComputer();
        branch.changed = clock.get();
        branch.working = working;
        branch.deleted = deleted;
        REQUIRE (writer.writeBranch (branch));
    }

    phazite::Sweep sweep (const juce::ValueTree& merging = {}, phazite::VersionStore* which = nullptr)
    {
        return phazite::sweepStore (which != nullptr ? *which : *store, merging, 50, [] { return false; }, true);
    }

    // Days passing, the store swept once a week of them and at their end: what the last sweep did.
    phazite::Sweep advance (double days, const juce::ValueTree& merging = {})
    {
        phazite::Sweep last;

        while (days > 0.0)
        {
            const auto step = juce::jmin (7.0, days);
            clock.addDays (step);
            days -= step;
            last = sweep (merging);
        }

        return last;
    }

    // The store as the project next opens it.
    void reopen()
    {
        store = nullptr;
        store = std::make_unique<phazite::VersionStore> (folder, phazite::getThisComputer());
    }

    bool readsVersion (const juce::String& id) const { return readsVersionIn (*store, id); }

    static bool readsVersionIn (const phazite::VersionStore& from, const juce::String& id)
    {
        const auto version = from.readVersion (id);
        return version.has_value() && from.readSong (version->song).isValid();
    }

    bool readsSong (const juce::String& piece) const { return store->readSong (piece).isValid(); }
    bool holds (const juce::String& piece) const { return store->getPieces().has (piece); }
};

constexpr double month = 31.0; // days: past the 30 a piece nothing names is kept
} // namespace

namespace
{
void verify (phazite::ProjectContext& context)
{
    REQUIRE (phazite::verifyHistory (context));
    REQUIRE (dispatchMessagesUntil ([&context] { return ! phazite::isVerifyingHistory (context); }, versionHistoryTimeout));
}

void keepVersion (phazite::ProjectContext& context, const juce::String& name)
{
    REQUIRE (waitForVersionHistory (context));
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (waitForVersionHistory (context));
}
} // namespace

TEST_CASE ("The sweep keeps whatever is named: a branch's line, another computer's, a line gone its own way, a song left on a branch")
{
    SweepLetsGo letGo;
    Store s;
    const auto first = s.saveVersion (-1.0f), second = s.saveVersion (-2.0f, { first });
    const auto theirs = s.saveVersion (-3.0f, { first });  // another computer's line of main, in this computer's pack
    const auto otherBranch = s.saveVersion (-4.0f);        // a branch only another computer records
    const auto left = s.keepSong (-5.0f);                  // left on main
    const auto nothing = s.saveVersion (-6.0f, { first }); // named by nothing
    s.putBranch ("main", second, left);

    {
        phazite::VersionStore elsewhere (s.folder, "elsewhere");
        s.putBranch ("main", theirs, {}, false, &elsewhere);
        s.putBranch ("theirs", otherBranch, {}, false, &elsewhere);
    }

    const auto swept = s.advance (month);
    CHECK (swept.joined);
    CHECK (swept.sure);
    CHECK (swept.versionsGone == juce::StringArray (nothing));

    for (const auto& id : { first, second, theirs, otherBranch })
        CHECK (s.readsVersion (id));

    CHECK (s.readsSong (left));
    CHECK_FALSE (s.holds (nothing));

    s.reopen();
    CHECK (s.readsVersion (second));
    CHECK (s.readsSong (left));
}

TEST_CASE ("The sweep keeps Local History's snapshots of the last 30 days, and lets go of those thinned away")
{
    SweepLetsGo letGo;
    Store s;
    const auto then = s.clock.get();
    const auto kept = s.keepSong (-1.0f), thinned = s.keepSong (-2.0f);
    s.advance (28.0);

    // Kept: a snapshot of a minute ago, whose song's pieces are four weeks old. Thinned: one as old as its pieces.
    REQUIRE (s.store->writeSnapshots ({ { then, "main", thinned, "auto" }, { s.clock.get() - 60000, "main", kept, "auto" } }));
    s.advance (3.0);
    CHECK (s.readsSong (kept));
    CHECK_FALSE (s.holds (thinned));
}

TEST_CASE ("The sweep keeps the song before a merge that waits, in the open song or in a song left on a branch")
{
    SweepLetsGo letGo;
    Store s;
    const auto head = s.saveVersion (-1.0f);
    const auto beforeOpen = s.keepSong (-2.0f), beforeLeft = s.keepSong (-3.0f);

    juce::ValueTree merging (schema::MERGING);
    merging.setProperty (schema::mergingVersion, head, nullptr);
    merging.setProperty (schema::mergingHead, head, nullptr);
    merging.setProperty (schema::mergingBefore, beforeLeft, nullptr);
    phazite::setWaitingMerge (*s.song, merging);
    const auto leftWithMerge = s.keepSong (-4.0f);
    phazite::setWaitingMerge (*s.song, {});
    s.putBranch ("main", head, leftWithMerge);

    merging.setProperty (schema::mergingBefore, beforeOpen, nullptr);
    s.advance (month, merging);
    CHECK (s.readsSong (beforeOpen));
    CHECK (s.readsSong (beforeLeft));
    CHECK (s.readsSong (leftWithMerge));
}

TEST_CASE ("What left every branch is kept 30 days by the logs from when it left, then it goes, its bounce and message with it")
{
    SweepLetsGo letGo;
    // Saved on day 0, undone (and a branch deleted) on day 20: kept on day 40, though its pieces are 40 days old, by the
    // logs alone; gone after day 50.
    Store s;
    const auto first = s.saveVersion (-1.0f), undone = s.saveVersion (-2.0f, { first });
    const auto deletedHead = s.saveVersion (-3.0f, { first }, "idea");
    const auto deletedSong = s.keepSong (-4.0f);
    s.putBranch ("main", undone);
    s.putBranch ("idea", deletedHead, deletedSong);
    REQUIRE (s.store->writeMessage (undone, { "Renamed", {}, s.clock.get() }));
    const auto bounce = s.store->getBounceFile (undone);
    REQUIRE (bounce.getParentDirectory().createDirectory());
    REQUIRE (bounce.replaceWithText ("a bounce"));

    s.advance (20.0);
    s.putBranch ("main", first);
    s.putBranch ("idea", deletedHead, {}, true);
    REQUIRE (s.store->logBranchMove ("main", undone, first, "undo", "main"));
    REQUIRE (s.store->logBranchMove ("idea", deletedHead, {}, "delete", "idea", deletedSong));

    s.advance (20.0);
    CHECK (s.readsVersion (undone));
    CHECK (s.readsVersion (deletedHead));
    CHECK (s.readsSong (deletedSong));
    CHECK (bounce.existsAsFile());

    const auto swept = s.advance (11.0);
    CHECK_FALSE (s.holds (undone));
    CHECK_FALSE (s.holds (deletedHead));
    CHECK_FALSE (s.holds (deletedSong));
    CHECK_FALSE (bounce.existsAsFile());
    CHECK (s.store->readMessages().count (undone) == 0);
    CHECK (s.readsVersion (first));
}

TEST_CASE ("A plug-in's state that no version or kept song holds goes; one a version holds stays; states of any bytes")
{
    SweepLetsGo letGo;
    // States of random bytes, as real plug-ins write them, which are no text: the sweep only asks whether the store holds
    // them, and raises no assertion (tests/TestMain.cpp fails any).
    Store s;
    const auto plugin = phazite::test::addPluginDevice (*s.song, s.track, testSynth());
    REQUIRE (plugin.isValid());
    juce::Random random (42);

    const auto setState = [&s, plugin, &random]
    {
        juce::MemoryBlock state (4000);

        for (size_t i = 0; i < state.getSize(); ++i)
            state[i] = (char) random.nextInt (256);

        phazite::test::storePluginState (*s.song, plugin, state);
    };

    setState();
    const auto version = s.saveVersion (-1.0f);
    s.putBranch ("main", version);
    setState();
    const auto replaced = s.keepSong (-1.0f); // a song left at a save the next one replaced

    const auto stateOf = [&s] (const juce::String& songPiece)
    {
        juce::StringArray states, toVisit (songPiece);

        while (! toVisit.isEmpty())
        {
            const auto bytes = s.store->getPieces().read (toVisit[0]);
            toVisit.remove (0);

            if (bytes.has_value())
                for (const auto& reference : phazite::findReferences (*bytes))
                    (reference.what.startsWith ("_bytes") ? states : toVisit).add (reference.piece);
        }

        return states;
    };

    const auto replacedState = stateOf (replaced), versionState = stateOf (s.store->readVersion (version)->song);
    REQUIRE (replacedState.size() == 1);
    REQUIRE (versionState.size() == 1);
    REQUIRE (replacedState[0] != versionState[0]);

    s.advance (month);
    CHECK_FALSE (s.holds (replacedState[0]));
    CHECK (s.holds (versionState[0]));
    CHECK (s.readsVersion (version));
}

TEST_CASE ("Nothing younger than 30 days goes, and what a young piece names stays with it")
{
    SweepLetsGo letGo;
    Store s;
    const auto old = s.saveVersion (-1.0f);
    s.advance (10.0);
    const auto young = s.saveVersion (-2.0f), youngSong = s.keepSong (-3.0f); // named by nothing yet, sharing old pieces
    const auto swept = s.advance (month - 10.0);
    CHECK_FALSE (s.holds (old));
    CHECK (s.readsVersion (young));
    CHECK (s.readsSong (youngSong));
    CHECK_FALSE (swept.versionsGone.contains (young));
}

TEST_CASE ("A laptop away for weeks keeps what it saved: each computer holds its own copy, and sweeps its own packs alone")
{
    SweepLetsGo letGo;
    // (1) The desktop keeps a song in Local History. (2) The laptop, synced, goes offline and saves a version of that song on
    // day 10. (3) On day 31 the desktop sweeps: nothing it can see names the song any more (the snapshot thinned away), so
    // its own copy goes. (4) The sync carries that to the laptop, and the laptop's work to the desktop: the laptop's version
    // still reads whole, on both.
    Store s;
    const auto snapshot = s.keepSong (-1.0f);
    REQUIRE (s.store->writeSnapshots ({ { s.clock.get(), "main", snapshot, "auto" } }));
    const auto pieces = s.folder.getChildFile ("pieces");
    const auto laptopFolder = s.temporary.folder.getChildFile ("Laptop Versions");
    REQUIRE (s.folder.copyDirectoryTo (laptopFolder));
    const auto synced = pieces.findChildFiles (juce::File::findFiles, false, "*.pack");

    s.clock.addDays (10.0);
    juce::String version;

    {
        phazite::VersionStore laptop (laptopFolder, "laptop");
        version = s.saveVersion (-1.0f, {}, "main", &laptop); // the snapshot's song, as the laptop saves it
        s.putBranch ("main", version, {}, false, &laptop);
        REQUIRE (laptop.logBranchMove ("main", {}, version, "commit", "main"));
    }

    s.advance (month - 10.0);
    CHECK_FALSE (s.holds (snapshot)); // the desktop's own copy went

    // The sync: what the desktop deleted goes on the laptop, and each one's new packs and records reach the other.
    for (const auto& pack : synced)
        if (! pack.existsAsFile())
            laptopFolder.getChildFile ("pieces").getChildFile (pack.getFileName()).deleteFile();

    for (const auto& pack : pieces.findChildFiles (juce::File::findFiles, false, "*.pack"))
        REQUIRE (pack.copyFileTo (laptopFolder.getChildFile ("pieces").getChildFile (pack.getFileName())));

    REQUIRE (laptopFolder.getChildFile ("pieces").copyDirectoryTo (pieces));
    REQUIRE (laptopFolder.getChildFile ("branches").copyDirectoryTo (s.folder.getChildFile ("branches")));

    const phazite::VersionStore onLaptop (laptopFolder, "laptop");
    CHECK (Store::readsVersionIn (onLaptop, version));
    s.reopen();
    CHECK (s.readsVersion (version));
}

TEST_CASE ("Two computers that both sweep one folder each keep what is named, and delete only their own packs")
{
    SweepLetsGo letGo;
    Store s;
    phazite::VersionStore laptop (s.folder, "laptop");
    s.sweep ({}, &laptop); // its first sweep, noted
    const auto mine = s.saveVersion (-1.0f), mineGone = s.saveVersion (-2.0f);
    const auto theirs = s.saveVersion (-3.0f, {}, "idea", &laptop), theirsGone = s.saveVersion (-4.0f, {}, "idea", &laptop);
    s.putBranch ("main", mine);
    s.putBranch ("idea", theirs, {}, false, &laptop);

    for (int week = 0; week < 5; ++week)
    {
        s.clock.addDays (7.0);
        s.sweep();
        laptop.getPieces().readNewPacks();
        s.sweep ({}, &laptop);
        s.store->getPieces().readNewPacks();
    }

    s.reopen();
    const phazite::VersionStore laptopNow (s.folder, "laptop");

    for (const auto* reader : std::vector<const phazite::VersionStore*> { s.store.get(), &laptopNow })
    {
        CHECK (Store::readsVersionIn (*reader, mine));
        CHECK (Store::readsVersionIn (*reader, theirs));
        CHECK_FALSE (reader->getPieces().has (mineGone));
        CHECK_FALSE (reader->getPieces().has (theirsGone));
    }
}

TEST_CASE ("Another computer's work is never swept here, whether its record or its log or neither has arrived")
{
    SweepLetsGo letGo;
    Store s;
    phazite::VersionStore laptop (s.folder, "laptop");
    const auto kept = s.saveVersion (-1.0f, {}, "idea", &laptop), undone = s.saveVersion (-2.0f, { kept }, "idea", &laptop);
    const auto alone = s.saveVersion (-3.0f, {}, "other", &laptop);

    SUBCASE ("Its record arrived before its log: the version it undid is named by nothing here")
    {
        s.putBranch ("idea", kept, {}, false, &laptop);
    }

    SUBCASE ("Its pack arrived before its record")
    {
    }

    s.advance (60.0);
    CHECK (s.readsVersion (kept));
    CHECK (s.readsVersion (undone));
    CHECK (s.readsVersion (alone));
}

TEST_CASE ("A clock set ahead a year and then put right loses nothing that can be brought back")
{
    SweepLetsGo letGo;
    VersionsTime clock (juce::Time::currentTimeMillis());
    const auto start = clock.get();
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    auto& song = *phazite::findSong (s.context);
    REQUIRE (waitForVersionHistory (s.context));
    keepVersion (s.context, "First");
    phazite::setChannelVolume (song, bass, -6.0f);
    keepVersion (s.context, "Undone");
    const auto undone = phazite::getLastVersion (s.context)->id;
    REQUIRE (finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));
    phazite::getVersionHistory (s.context)->takeSnapshot ("auto");
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (phazite::getLostVersions (s.context).size() == 1);
    REQUIRE (phazite::getSnapshots (s.context).size() == 1);

    // A year ahead, opened twice (and a snapshot taken meanwhile): kept by a week past the last sweep at most.
    clock.addDays (365.0);
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));

    // Put right: everything still there to bring back.
    clock.set (start + 24LL * 60 * 60 * 1000);
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (phazite::getLostVersions (s.context).size() == 1);
    CHECK (! phazite::getSnapshots (s.context).empty());
    CHECK (phazite::getVersionHistory (s.context)->getStore()->readVersion (undone).has_value());
    CHECK (phazite::recoverLostVersions (s.context, phazite::getLostVersions (s.context)[0].key).isNotEmpty());
}

TEST_CASE ("A project opened after 60 days away lets nothing go at that open, and what is past its 30 days a week later")
{
    SweepLetsGo letGo;
    Store s;
    const auto first = s.saveVersion (-1.0f), undone = s.saveVersion (-2.0f, { first });
    s.putBranch ("main", first);
    REQUIRE (s.store->logBranchMove ("main", undone, first, "undo", "main"));

    s.clock.addDays (60.0);
    s.reopen();
    phazite::sweepStore (*s.store, {}, 50, [] { return false; });
    CHECK (s.holds (undone));

    s.clock.addDays (7.0);
    s.reopen();
    phazite::sweepStore (*s.store, {}, 50, [] { return false; });
    CHECK_FALSE (s.holds (undone));
    CHECK (s.readsVersion (first));
}

TEST_CASE ("A sweep whose new pack can't be written changes nothing, and every piece still reads")
{
    SweepLetsGo letGo;
    Store s;
    const auto kept = s.saveVersion (-1.0f), unnamed = s.saveVersion (-2.0f);
    s.putBranch ("main", kept);
    s.advance (28.0);
    const auto packs = s.store->getPieces().getNumPacks();
    s.clock.addDays (3.0);

    {
        // As a full disk fails a write: the joined pack never takes its name.
        const phazite::WholeFileHook full ([] (const juce::File& file) { return ! file.getFileName().startsWith ("joining-"); });
        CHECK_FALSE (s.sweep().joined);
    }

    CHECK (s.store->getPieces().getNumPacks() == packs);
    CHECK (s.readsVersion (kept));
    CHECK (s.readsVersion (unnamed));
    CHECK (s.folder.getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "joining-*").isEmpty());

    s.reopen();
    CHECK (s.readsVersion (unnamed));
}

TEST_CASE ("A joined pack damaged before it is read back is never trusted: nothing goes, and every piece still reads")
{
    SweepLetsGo letGo;
    Store s;
    const auto kept = s.saveVersion (-1.0f), unnamed = s.saveVersion (-2.0f), alsoKept = s.saveVersion (-3.0f);
    s.putBranch ("main", kept);
    s.putBranch ("idea", alsoKept); // so that the joined pack is none of those there
    s.advance (28.0);
    const auto before = s.folder.getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack");
    s.clock.addDays (3.0);

    // A byte of the new pack's pieces changed as it reached the disk: its index still hashes to its name.
    phazite::PieceStore::joinWrittenForTests = [] (const juce::File& pack)
    {
        juce::MemoryBlock bytes;
        REQUIRE (pack.loadFileAsData (bytes));
        bytes[5] = (char) (bytes[5] ^ 0x5a);
        REQUIRE (pack.replaceWithData (bytes.getData(), bytes.getSize()));
    };

    const auto swept = s.sweep();
    phazite::PieceStore::joinWrittenForTests = nullptr;
    CHECK_FALSE (swept.joined);

    for (const auto& pack : before)
        CHECK (pack.existsAsFile());

    CHECK (s.folder.getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack").size() == before.size());
    CHECK (s.readsVersion (kept));
    CHECK (s.readsVersion (unnamed));
    s.reopen();
    CHECK (s.readsVersion (kept));
}

TEST_CASE ("A sweep cut off after its pack is written, before the old ones go, leaves every piece readable")
{
    SweepLetsGo letGo;
    Store s;
    const auto kept = s.saveVersion (-1.0f), unnamed = s.saveVersion (-2.0f);
    s.putBranch ("main", kept);
    s.advance (28.0);
    const auto pieces = s.folder.getChildFile ("pieces");
    const auto copy = s.temporary.folder.getChildFile ("Old packs");
    REQUIRE (pieces.copyDirectoryTo (copy));

    s.advance (3.0);

    // The old packs back, as a crash before they were deleted would have left them, beside the joined one.
    for (const auto& pack : copy.findChildFiles (juce::File::findFiles, false, "*.pack"))
        REQUIRE (pack.copyFileTo (pieces.getChildFile (pack.getFileName())));

    s.reopen();
    CHECK (s.readsVersion (kept));
    CHECK (s.readsVersion (unnamed));

    // The next sweep finishes it.
    s.advance (7.0);
    CHECK (s.readsVersion (kept));
    CHECK_FALSE (s.holds (unnamed));
}

TEST_CASE ("What a sweep cut off had noted goes at the next sure walk, but not a version named again meanwhile")
{
    SweepLetsGo letGo;
    Store s;
    const auto live = s.saveVersion (-1.0f);
    s.putBranch ("main", live);
    s.sweep(); // the first sweep, noted

    // Noted before a join a crash cut off: a version no longer in the store, and one named again since (brought back).
    const auto gone = juce::String::repeatedString ("b", 64);

    for (const auto& version : { gone, live })
    {
        REQUIRE (s.store->writeMessage (version, { "Renamed", {}, s.clock.get() }));
        const auto bounce = s.store->getBounceFile (version);
        REQUIRE (bounce.getParentDirectory().createDirectory());
        REQUIRE (bounce.replaceWithText ("a bounce"));
    }

    REQUIRE (s.store->writeSwept ({ s.clock.get(), { gone, live } }));
    s.sweep();
    CHECK_FALSE (s.store->getBounceFile (gone).existsAsFile());
    CHECK (s.store->readMessages().count (gone) == 0);
    CHECK (s.store->getBounceFile (live).existsAsFile());
    CHECK (s.store->readMessages().count (live) == 1);
    CHECK (s.store->readSwept().bounces.isEmpty());
}
TEST_CASE ("A pack another program holds open stays for the next sweep, every piece readable meanwhile")
{
    SweepLetsGo letGo;
    Store s;
    phazite::PieceStore::smallPackForTests = 16; // no pack small enough to be gathered: kept stays out of the held pack
    const auto kept = s.saveVersion (-1.0f), unnamed = s.saveVersion (-2.0f);
    s.putBranch ("main", kept);
    s.advance (28.0);
    s.clock.addDays (3.0);

    // The pack the unnamed version's record is in, by its index.
    juce::File itsPack;
    juce::MemoryBlock name;
    name.loadFromHexString (unnamed);

    for (const auto& pack : s.folder.getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack"))
        if (juce::MemoryBlock bytes;
            pack.loadFileAsData (bytes)
            && std::search (static_cast<const char*> (bytes.getData()), static_cast<const char*> (bytes.getData()) + bytes.getSize(),
                            static_cast<const char*> (name.getData()), static_cast<const char*> (name.getData()) + name.getSize())
                   != static_cast<const char*> (bytes.getData()) + bytes.getSize())
            itsPack = pack;

    REQUIRE (itsPack.existsAsFile());
    s.store->getPieces().closeFiles(); // the store's own open file would keep it from being held on Windows

    {
        // Another program (a sync program, a virus scanner) holds it as the sweep runs, so that it can't even be read: it
        // stays, as it was.
        const HeldFile held (itsPack);

        if (held.works())
        {
            s.sweep();
            CHECK (itsPack.existsAsFile());
            CHECK (s.readsVersion (kept));
        }
    }

    s.reopen();
    CHECK (s.readsVersion (kept));
    CHECK (s.readsVersion (unnamed));

    s.advance (7.0);
    phazite::PieceStore::smallPackForTests = 0;
    CHECK (s.readsVersion (kept));
    CHECK_FALSE (s.holds (unnamed));
}

TEST_CASE ("A piece something names that can't be read keeps everything: the sweep leaves nothing out")
{
    SweepLetsGo letGo;
    Store s;
    const auto kept = s.saveVersion (-1.0f), unnamed = s.saveVersion (-2.0f);
    s.putBranch ("main", kept);
    s.putBranch ("idea", juce::String::repeatedString ("a", 64)); // a last version not in the store (not arrived yet)
    const auto swept = s.advance (month);
    CHECK_FALSE (swept.sure);
    CHECK (swept.leftOut.isEmpty());
    CHECK (s.readsVersion (unnamed));
}

TEST_CASE ("A file that names something and doesn't read whole, or a sync program's copy of one, keeps everything")
{
    SweepLetsGo letGo;
    // Each kind of file, damaged a month on: the walk is unsure, and nothing is left out, not even what nothing names.
    const std::vector<std::pair<const char*, std::function<void (Store&)>>> damages {
        { "the songs left on branches", [] (Store& s)
          { REQUIRE (s.store->getLocalFolder().getChildFile ("working.xml").replaceWithText ("<WORKING><LEFT branch=\"main\" song=")); } },
        { "a record of a branch",
          [] (Store& s)
          {
              for (const auto& record :
                   s.folder.getChildFile ("branches").getChildFile ("main").findChildFiles (juce::File::findFiles, false))
                  REQUIRE (record.replaceWithText ("<BRANCH id=\"main\" head=\""));
          } },
        { "Local History",
          [] (Store& s)
          {
              const auto history = s.store->getLocalFolder().getChildFile ("history");
              REQUIRE (history.replaceWithText (juce::String (s.clock.get()) + " garbled\n" + history.loadFileAsString()));
          } },
        { "a log",
          [] (Store& s)
          {
              REQUIRE (s.store->getLocalFolder().getChildFile ("logs").getChildFile ("main").appendText (juce::String (s.clock.get())
                                                                                                         + " not-a-move\n"));
          } },
        { "a sync program's copy of a file two computers wrote, as one computer id on two machines makes",
          [] (Store& s)
          {
              const auto local = s.store->getLocalFolder();
              REQUIRE (local.getChildFile ("working.xml").copyFileTo (local.getChildFile ("working (conflicted copy).xml")));
          } },
    };

    for (const auto& [what, damage] : damages)
    {
        const auto damaged = juce::String (what);
        CAPTURE (damaged);
        Store s;
        const auto kept = s.saveVersion (-1.0f), unnamed = s.saveVersion (-2.0f);
        const auto left = s.keepSong (-3.0f), snapshotSong = s.keepSong (-4.0f);
        s.putBranch ("main", kept, left);
        REQUIRE (s.store->logBranchMove ("main", {}, kept, "commit", "main"));
        s.advance (28.0);
        REQUIRE (s.store->writeSnapshots ({ { s.clock.get(), "main", snapshotSong, "auto" } }));
        REQUIRE (s.store->areRecordsWhole());

        damage (s);
        CHECK_FALSE (s.store->areRecordsWhole());
        const auto swept = s.advance (3.0);
        CHECK_FALSE (swept.sure);
        CHECK (swept.leftOut.isEmpty());
        CHECK (s.readsVersion (unnamed));
        CHECK (s.readsSong (left));
        CHECK (s.readsSong (snapshotSong));
    }
}
TEST_CASE ("Where this computer's record of a branch is gone, its log says where the branch last was, though another computer records it")
{
    SweepLetsGo letGo;
    Store s;
    const auto older = s.saveVersion (-1.0f), newer = s.saveVersion (-2.0f, { older });
    phazite::VersionStore laptop (s.folder, "laptop");
    s.putBranch ("main", older, {}, false, &laptop);                           // the laptop's record, behind
    REQUIRE (s.store->logBranchMove ("main", older, newer, "commit", "main")); // this computer's log; its record lost
    s.advance (month + 30.0);
    CHECK (s.readsVersion (newer));
}

namespace
{
// While one lives, joins wait as they begin to copy (PieceStore::holdJoinsForTests); let go as it goes, or after
// hangTimeout, should the test hang meanwhile.
class HeldJoins
{
public:
    HeldJoins()
        : releaser (
              [this]
              {
                  if (! released.wait (hangTimeout))
                      phazite::PieceStore::holdJoinsForTests = false;
              })
    {
        phazite::PieceStore::holdJoinsForTests = true;
    }

    ~HeldJoins()
    {
        phazite::PieceStore::holdJoinsForTests = false;
        released.signal();
        releaser.join();
    }

private:
    juce::WaitableEvent released;
    std::thread releaser;
};
} // namespace

TEST_CASE ("While a sweep joins packs, the window's room text, the audio's key and Save As wait for none of it")
{
    SweepLetsGo letGo;
    VersionsTime clock (juce::Time::currentTimeMillis());
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    REQUIRE (waitForVersionHistory (s.context));
    keepVersion (s.context, "First");
    phazite::setChannelVolume (*phazite::findSong (s.context), bass, -6.0f);
    keepVersion (s.context, "Undone");
    const auto undone = phazite::getLastVersion (s.context)->id;
    REQUIRE (finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));
    openEachWeek (s, clock, 28.0);

    {
        // The next open's sweep, a week after the last, leaves the undone version out: its join held as it copies.
        const HeldJoins held;
        clock.addDays (7.0);
        s.reopen();
        REQUIRE (dispatchMessagesUntil ([] { return phazite::PieceStore::joinsHeldForTests.load() == 1; }));

        const auto started = juce::Time::getMillisecondCounterHiRes();
        CHECK (phazite::getVersionsRoom (s.context).songs > 0);
        CHECK (phazite::getVersionHistory (s.context)->findVersionsAudioKey().isNotEmpty());
        phazite::waitForVersionsWritten (s.context); // as Save As does: the sweep stops rather than be waited for
        CHECK (juce::Time::getMillisecondCounterHiRes() - started < hangTimeout / 2.0);
        CHECK (phazite::PieceStore::holdJoinsForTests.load()); // the join still held: the stop ended the wait, not the releaser
        CHECK (phazite::getVersionHistory (s.context)->getStore()->getPieces().has (undone)); // stopped: nothing changed
    }

    REQUIRE (waitForVersionHistory (s.context));
    openEachWeek (s, clock, 7.0);
    CHECK_FALSE (phazite::getVersionHistory (s.context)->getStore()->getPieces().has (undone));
}

TEST_CASE ("Verify History checks every version's audio: a file missing, of another size, or changed in place, each said once")
{
    SweepLetsGo letGo;
    TestSong s;
    const auto track = s.addTrack ("Drums");

    for (const auto* name : { "Hat.wav", "Kick.wav", "Snare.wav" })
        s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile (name), 0.25), 0.0);

    keepVersion (s.context, "Drums");
    phazite::setChannelVolume (*phazite::findSong (s.context), track, -3.0f);
    keepVersion (s.context, "Quieter"); // the same files, named by two versions

    verify (s.context);
    CHECK (phazite::describeHistoryCheck (s.context).contains ("3 audio files: all whole"));

    // Kick gone; Snare another size; Hat changed in place, its size the same.
    const auto audio = s.getProject().getAudioFolder();
    REQUIRE (audio.getChildFile ("Kick.wav").deleteFile());
    writeSound (audio.getChildFile ("Snare.wav"), 0.5);
    juce::MemoryBlock hat;
    REQUIRE (audio.getChildFile ("Hat.wav").loadFileAsData (hat));
    static_cast<char*> (hat.getData())[hat.getSize() - 1] ^= 0x01;
    REQUIRE (audio.getChildFile ("Hat.wav").replaceWithData (hat.getData(), hat.getSize()));

    verify (s.context);
    const auto said = phazite::describeHistoryCheck (s.context);
    CHECK (said.contains ("2 versions can't play their audio as they were saved"));
    CHECK (said.contains ("\"Drums\" (Audio/Hat.wav changed since; Audio/Kick.wav missing; Audio/Snare.wav changed: another size)"));
    CHECK (said.contains ("\"Quieter\" (Audio/Hat.wav changed since; Audio/Kick.wav missing; Audio/Snare.wav changed: another size)"));
    CHECK_FALSE (said.contains ("all whole"));
}

TEST_CASE ("Verify History walks Local History's songs and the songs left on branches, and names one with a damaged piece")
{
    SweepLetsGo letGo;
    TestSong s;
    s.addTrack ("Bass");
    keepVersion (s.context, "First");
    auto& song = *phazite::findSong (s.context);
    auto* history = phazite::getVersionHistory (s.context);

    // A song another track makes, kept with one of its pieces lost: as a snapshot of Local History, and as the song left on
    // a branch.
    const auto lead = s.addTrack ("Lead");
    phazite::setChannelVolume (song, lead, -5.0f);
    auto pieces = phazite::splitSong (phazite::test::copyWholeSong (song));
    juce::String lost;

    for (const auto& [name, bytes] : pieces.pieces)
        if (phazite::readElement (bytes).hasType (schema::TRACK) && phazite::readElement (bytes)[schema::trackName].toString() == "Lead")
            lost = name;

    REQUIRE (lost.isNotEmpty());
    pieces.pieces.erase (lost);

    // Another song for the snapshot, the same track lost from it.
    phazite::test::changeSongInStep (song);
    auto snapshotPieces = phazite::splitSong (phazite::test::copyWholeSong (song));
    snapshotPieces.pieces.erase (lost);

    {
        phazite::VersionStore store (phazite::getVersionsFolder (song), phazite::getThisComputer());
        REQUIRE (store.getPieces().write (pieces.pieces));
        REQUIRE (store.getPieces().write (snapshotPieces.pieces));
        REQUIRE (
            store.writeSnapshots ({ { juce::Time::currentTimeMillis(), history->findCurrentBranch()->id, snapshotPieces.song, "auto" } }));
        auto idea = *history->findCurrentBranch();
        idea.id = "idea";
        idea.name = "idea";
        idea.working = pieces.song;
        REQUIRE (store.writeBranch (idea));
    }

    verify (s.context);
    const auto said = phazite::describeHistoryCheck (s.context);
    CHECK (said.contains ("The automatic save of "));
    CHECK (said.contains ("The song left on "));
    CHECK (said.contains ("can't be read whole"));
}

TEST_CASE ("Verify History stops when asked, and says it didn't finish")
{
    SweepLetsGo letGo;
    TestSong s;
    s.addTrack ("Bass");
    keepVersion (s.context, "First");

    phazite::VersionHistory::holdReadsForTests = true;
    REQUIRE (phazite::verifyHistory (s.context));
    CHECK (phazite::describeHistoryCheck (s.context).startsWith ("Verifying the history... "));
    CHECK (phazite::describeVerify (s.context) == "Stop Verifying History");
    CHECK (phazite::canVerifyHistory (s.context));
    phazite::verifyOrStopVerifying (s.context);
    phazite::VersionHistory::holdReadsForTests = false;
    REQUIRE (dispatchMessagesUntil ([&s] { return ! phazite::isVerifyingHistory (s.context); }, versionHistoryTimeout));
    CHECK (phazite::describeHistoryCheck (s.context) == "Verify History was stopped before it was done.");
    CHECK (phazite::describeVerify (s.context).isEmpty());
}

TEST_CASE ("A history swept as it opens is whole, its room shown smaller, and what went is no longer offered")
{
    SweepLetsGo letGo;
    VersionsTime clock (juce::Time::currentTimeMillis());
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    auto& song = *phazite::findSong (s.context);
    REQUIRE (waitForVersionHistory (s.context));

    for (int i = 0; i < 4; ++i)
    {
        phazite::setChannelVolume (song, bass, -1.0f - (float) i);
        keepVersion (s.context, "Take " + juce::String (i));
    }

    for (int i = 0; i < 3; ++i)
        REQUIRE (finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));

    REQUIRE (phazite::getLostVersions (s.context).size() == 1);
    const auto before = phazite::getVersionsRoom (s.context).songs;

    openEachWeek (s, clock, 35.0); // swept on day 35: what went on day 0 is 35 days gone
    CHECK (phazite::getLostVersions (s.context).empty());
    CHECK (phazite::getVersionsRoom (s.context).songs < before);

    verify (s.context);
    CHECK (phazite::describeHistoryCheck (s.context).contains ("all whole"));
    CHECK_FALSE (phazite::describeHistoryCheck (s.context).contains ("no branch holds"));
}

TEST_CASE ("A clock set ahead once, then put right, lets weekly sweeps go on")
{
    SweepLetsGo letGo;
    Store s;
    const auto start = s.clock.get();
    const auto unnamed = s.saveVersion (-1.0f);
    s.sweep(); // the first sweep, noted
    const auto sweepNow = [&s] { phazite::sweepStore (*s.store, {}, 50, [] { return false; }); };

    s.clock.set (start + 400LL * 24 * 60 * 60 * 1000); // a year ahead: its sweep notes that
    sweepNow();
    CHECK (s.holds (unnamed));

    s.clock.set (start + 40LL * 24 * 60 * 60 * 1000); // put right: the noted time after now is written again as now
    sweepNow();
    CHECK (s.store->readSwept().time == s.clock.get());
    CHECK (s.holds (unnamed));

    s.clock.addDays (7.0); // a week on: due again, and what nothing names, 47 days old, goes
    sweepNow();
    CHECK_FALSE (s.holds (unnamed));
}

TEST_CASE (
    "A log a crash cut off or a power loss zeroed is written again whole at the next move, and only recent damage makes the walk unsure")
{
    SweepLetsGo letGo;
    Store s;
    const auto log = s.store->getLocalFolder().getChildFile ("logs").getChildFile ("main");
    REQUIRE (s.store->logBranchMove ("main", {}, juce::String::repeatedString ("a", 64), "branch", "main"));
    const auto now = juce::String (s.clock.get());

    SUBCASE ("A line cut off: passed over (its time is old), and gone once a move is logged after it")
    {
        REQUIRE (log.appendText ("123 c d com", false, false));
        CHECK (s.store->areRecordsWhole());
        REQUIRE (s.store->logBranchMove ("main", {}, juce::String::repeatedString ("b", 64), "branch", "main"));
        CHECK_FALSE (log.loadFileAsString().contains ("com"));
        CHECK (s.store->readBranchLog ("main").size() == 2);
        CHECK (s.store->areRecordsWhole());
    }

    SUBCASE ("Zero bytes at its end: unsure while the log is recent; every move logged after them read")
    {
        {
            juce::FileOutputStream out (log);
            REQUIRE (out.openedOk());
            out.writeRepeatedByte (0, 64);
        }

        CHECK_FALSE (s.store->areRecordsWhole());
        REQUIRE (s.store->logBranchMove ("main", {}, juce::String::repeatedString ("b", 64), "branch", "main"));
        CHECK (s.store->readBranchLog ("main").size() == 2);
        CHECK (s.store->areRecordsWhole());
    }

    SUBCASE ("A line of another shape: unsure if its time is recent, passed over if old or without one")
    {
        REQUIRE (log.appendText ("100 old shape\nno time at all\n", false, false));
        CHECK (s.store->areRecordsWhole());
        REQUIRE (log.appendText (now + " new shape\n", false, false));
        CHECK_FALSE (s.store->areRecordsWhole());
    }

    SUBCASE ("Files the system puts in any folder are passed over")
    {
        for (const auto& file :
             { s.store->getLocalFolder().getChildFile ("desktop.ini"), s.store->getLocalFolder().getChildFile (".DS_Store"),
               log.getSiblingFile ("Thumbs.db"), s.folder.getChildFile ("branches").getChildFile ("main").getChildFile (".directory") })
        {
            REQUIRE (file.getParentDirectory().createDirectory());
            REQUIRE (file.replaceWithText ("system"));
        }

        CHECK (s.store->areRecordsWhole());
    }
}

TEST_CASE ("A year of snapshots keeps the count of this computer's packs small: packs of a size are joined together")
{
    SweepLetsGo letGo;
    Store s;
    phazite::PieceStore::smallPackForTests = 1024; // so that the test's packs are of every size class, not all small
    auto head = s.saveVersion (-1.0f);
    s.putBranch ("main", head);
    s.sweep();
    auto most = 0;

    for (int day = 1; day <= 365; ++day)
    {
        s.clock.addDays (1.0);
        head = s.saveVersion (-1.0f - (float) (day % 50) * 0.1f, { head });
        s.putBranch ("main", head);
        phazite::sweepStore (*s.store, {}, 50, [] { return false; });
        most = std::max (most, s.store->getPieces().getNumOwnPacks());
    }

    phazite::PieceStore::smallPackForTests = 0;
    CHECK (most <= 51);
    CHECK (s.readsVersion (head));
}

TEST_CASE ("What a sweep would free counts the bounces of the versions that go, under the real threshold")
{
    SweepLetsGo letGo;
    Store s;
    phazite::setSweepFloorForTests (-1); // 4 MiB, and a tenth of this computer's packs
    const auto first = s.saveVersion (-1.0f), undone = s.saveVersion (-2.0f, { first });
    s.putBranch ("main", first);
    REQUIRE (s.store->logBranchMove ("main", undone, first, "undo", "main"));
    s.sweep(); // the first sweep, noted

    // A few kilobytes of pieces, and 25 MB of bounce.
    const auto bounce = s.store->getBounceFile (undone);
    REQUIRE (bounce.getParentDirectory().createDirectory());
    juce::MemoryBlock bytes (25 * 1024 * 1024, true);
    REQUIRE (bounce.replaceWithData (bytes.getData(), bytes.getSize()));

    for (int week = 0; week < 5; ++week)
    {
        s.clock.addDays (7.0);
        phazite::sweepStore (*s.store, {}, 50, [] { return false; });
    }

    CHECK_FALSE (s.holds (undone));
    CHECK_FALSE (bounce.existsAsFile());
    CHECK (s.readsVersion (first));
}

TEST_CASE ("Under the real threshold, a few kilobytes nothing names wait, and megabytes go")
{
    SweepLetsGo letGo;
    Store s;
    phazite::setSweepFloorForTests (-1);
    const auto kept = s.saveVersion (-1.0f), small = s.saveVersion (-2.0f);
    s.putBranch ("main", kept);
    s.sweep();
    const auto sweepWeeks = [&s] (int weeks)
    {
        for (int week = 0; week < weeks; ++week)
        {
            s.clock.addDays (7.0);
            phazite::sweepStore (*s.store, {}, 50, [] { return false; });
        }
    };

    sweepWeeks (5);
    CHECK (s.holds (small)); // too little to rewrite packs for

    // A version with a plug-in state of 5 MiB that doesn't compress, named by nothing: it and the small one go together.
    const auto plugin = phazite::test::addPluginDevice (*s.song, s.track, testSynth());
    juce::MemoryBlock state (5 * 1024 * 1024);
    juce::Random random (7);

    for (size_t i = 0; i < state.getSize(); ++i)
        state[i] = (char) random.nextInt (256);

    phazite::test::storePluginState (*s.song, plugin, state);
    const auto big = s.saveVersion (-3.0f);
    sweepWeeks (5);
    CHECK_FALSE (s.holds (big));
    CHECK_FALSE (s.holds (small));
    CHECK (s.readsVersion (kept));
}

TEST_CASE ("What a join a crash cut off left beside the packs counts in the room until it goes, a day later")
{
    SweepLetsGo letGo;
    Store s;
    s.saveVersion (-1.0f);
    const auto size = s.store->getPieces().getSize();
    const auto leftover = s.folder.getChildFile ("pieces").getChildFile ("joining-left.tmp");
    REQUIRE (leftover.replaceWithText (juce::String::repeatedString ("x", 1000)));
    s.store->getPieces().removeLeftovers();
    CHECK (s.store->getPieces().getSize() == size + 1000);

    REQUIRE (leftover.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::days (2.0)));
    s.sweep();
    CHECK_FALSE (leftover.existsAsFile());
    CHECK (s.store->getPieces().getSize() == size);
}

TEST_CASE ("Packs a crash left twice are joined into one at the next sweep, whatever their size")
{
    SweepLetsGo letGo;
    Store s;
    phazite::PieceStore::smallPackForTests = 16; // every pack above small
    const auto kept = s.saveVersion (-1.0f);
    s.putBranch ("main", kept);
    s.sweep();

    // Every pack copied beside itself under another name, as a join cut off after its rename leaves its pieces twice.
    const auto pieces = s.folder.getChildFile ("pieces");
    const auto before = pieces.findChildFiles (juce::File::findFiles, false, "*.pack");
    phazite::PieceStore twice (s.temporary.folder.getChildFile ("Twice"), phazite::getThisComputer());
    std::map<juce::String, juce::MemoryBlock> all;

    for (const auto& name : s.store->getPieces().getNames())
        all.emplace (name, *s.store->getPieces().read (name));

    s.clock.addDays (1.0); // another time: another pack
    REQUIRE (twice.write (all));
    twice.closeFiles();

    for (const auto& pack : twice.getFolder().findChildFiles (juce::File::findFiles, false, "*.pack"))
        REQUIRE (pack.copyFileTo (pieces.getChildFile (pack.getFileName())));

    s.reopen();
    REQUIRE (s.store->getPieces().getNumOwnPacks() == (int) before.size() + 1);
    s.advance (7.0);
    phazite::PieceStore::smallPackForTests = 0;
    CHECK (s.store->getPieces().getNumOwnPacks() == 1);
    CHECK (s.readsVersion (kept));
}

TEST_CASE (
    "Another computer's versions are made this computer's own whole: a piece shared with one that can't be yet is copied with the one that can")
{
    SweepLetsGo letGo;
    Store s;
    phazite::VersionStore b (s.folder, "computer-b");

    // B's two versions, both holding the first track's piece: one whole, one whose second track's piece hasn't arrived.
    const auto whole = s.saveVersion (-1.0f, {}, "theirs", &b);
    const auto before = s.makeSong (-1.0f);
    phazite::ensureTracks (*s.song, 2);
    auto partial = s.makeSong (-1.0f);

    for (auto piece = partial.pieces.begin(); piece != partial.pieces.end();)
        piece = before.pieces.count (piece->first) == 0 && piece->first != partial.song ? partial.pieces.erase (piece) : std::next (piece);

    phazite::VersionRecord record;
    record.parents.add (whole);
    record.branch = "theirs";
    record.saved = s.clock.get();
    const auto waiting = b.writeVersion (record, partial);
    REQUIRE (waiting.isNotEmpty());
    b.getPieces().closeFiles();
    const auto theirPacks = s.folder.getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack");

    // Read as this computer opens the folder; the one that can't be copied yet first, so that it meets the shared piece
    // first.
    s.reopen();
    phazite::Adoption work;
    work.versions = { waiting, whole };
    const auto adopted = phazite::adoptVersions (*s.store, work, nullptr);
    CHECK (adopted.written);
    CHECK (adopted.unreadable == juce::StringArray { waiting });
    CHECK (work.isEmpty());
    s.store->getPieces().closeFiles();

    // B's sweep takes its packs away: the whole one still reads here, from this computer's own copy.
    for (const auto& pack : theirPacks)
        REQUIRE (pack.deleteFile());

    s.reopen();
    CHECK (s.readsVersion (whole));
    CHECK_FALSE (s.holds (waiting));
}

TEST_CASE (
    "A pack this computer writes never takes the name of another computer's, though it holds the same pieces written the same millisecond")
{
    SweepLetsGo letGo;
    Store s;
    phazite::VersionStore b (s.folder, "computer-b");
    const auto theirs = s.saveVersion (-1.0f, {}, "theirs", &b);
    b.getPieces().closeFiles();
    const auto theirPacks = s.folder.getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack");
    REQUIRE (theirPacks.size() == 1);
    juce::MemoryBlock before;
    REQUIRE (theirPacks[0].loadFileAsData (before));

    // The same version written here at the same time on the versions' clock: a pack of this computer's own beside B's.
    s.reopen();
    s.saveVersion (-1.0f, {}, "theirs");
    s.store->getPieces().closeFiles();
    juce::MemoryBlock after;
    REQUIRE (theirPacks[0].loadFileAsData (after));
    CHECK (after == before);
    CHECK (s.store->getPieces().getNumOwnPacks() == 1);
    CHECK (s.folder.getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack").size() == 2);

    REQUIRE (theirPacks[0].deleteFile());
    s.reopen();
    CHECK (s.readsVersion (theirs));
}

TEST_CASE ("With the product's setting a due sweep lets go of nothing: no piece, bounce or edited message goes, packs are only joined")
{
    REQUIRE_FALSE (phazite::doesSweepLetGo());
    VersionsTime clock (juce::Time::currentTimeMillis());
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");

    const auto findBranch = [&s] (const juce::String& name)
    {
        for (const auto& branch : phazite::getBranches (s.context))
            if (branch.name == name)
                return branch.id;

        return juce::String();
    };

    // A branch with a version of its own and a change left on it, deleted; a version undone, with a bounce and an edited
    // message; and snapshots of Local History, hours apart.
    REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
    s.setVolume (bass, -9.0f);
    keepVersion (s.context, "Idea");
    s.setVolume (bass, -12.0f);
    REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch ("main"), s.save); }));
    REQUIRE (phazite::deleteBranch (s.context, findBranch ("idea-2")));

    s.setVolume (bass, -3.0f);
    keepVersion (s.context, "Loud");
    const auto loud = phazite::getLastVersion (s.context);
    REQUIRE (loud.has_value());
    REQUIRE (phazite::editVersionMessage (s.context, loud->id, "Louder", {}));
    REQUIRE (waitForVersionHistory (s.context));
    const auto bounce = phazite::getVersionHistory (s.context)->getStore()->getBounceFile (loud->id);
    REQUIRE (bounce.getParentDirectory().createDirectory());
    REQUIRE (bounce.replaceWithText ("a bounce"));
    REQUIRE (finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));

    for (int snapshot = 0; snapshot < 6; ++snapshot)
    {
        s.setVolume (bass, -20.0f - (float) snapshot);
        phazite::getVersionHistory (s.context)->takeSnapshot ("auto");
        REQUIRE (waitForVersionHistory (s.context));
        clock.set (clock.get() + 5LL * 60 * 60 * 1000);
    }

    auto store = phazite::getVersionHistory (s.context)->getStore();
    const auto before = store->getPieces().getNames();
    const auto packsBefore = store->getPieces().getNumPacks();
    REQUIRE (packsBefore > 1);

    // Two months on, the project opened every week, and a sweep due at its last open: all of it is past its 30 days.
    openEachWeek (s, clock, 60.0);
    clock.addDays (7.0);
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));

    store = phazite::getVersionHistory (s.context)->getStore();
    const auto after = store->getPieces().getNames();
    auto missing = 0;

    for (const auto& name : before)
        missing += after.contains (name) ? 0 : 1;

    CHECK (missing == 0);
    CHECK (bounce.existsAsFile());
    CHECK (store->readMessages().count (loud->id) == 1);
    CHECK (store->readSwept().time == clock.get());
    CHECK (store->getPieces().getNumPacks() < packsBefore);

    // What is past its 30 days is no longer offered, but still in the store.
    CHECK (phazite::getLostVersions (s.context).empty());
    const auto snapshots = phazite::getSnapshots (s.context);
    CHECK (
        std::none_of (snapshots.begin(), snapshots.end(), [] (const phazite::SnapshotInfo& each) { return each.reason == "Auto-saved"; }));

    verify (s.context);
    CHECK (phazite::describeHistoryCheck (s.context).contains ("all whole"));
}

TEST_CASE ("A join with nothing to write writes no pack: the packs whose every piece goes just go")
{
    SweepLetsGo letGo;
    Store s;
    phazite::PieceStore::smallPackForTests = 16; // no pack small enough to be gathered
    const auto kept = s.saveVersion (-1.0f);
    s.putBranch ("main", kept);
    const auto unnamed = s.saveVersion (-2.0f); // in a pack of its own, every piece of it named by nothing
    REQUIRE (s.store->getPieces().getNumOwnPacks() == 2);

    s.advance (month);
    phazite::PieceStore::smallPackForTests = 0;
    CHECK_FALSE (s.holds (unnamed));
    CHECK (s.readsVersion (kept));
    CHECK (s.store->getPieces().getNumPacks() == 1);
    CHECK (s.folder.getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*").size() == 1);
}
