// SPDX-License-Identifier: AGPL-3.0-or-later
// Clean Up Unused Audio... (model/project/UnusedAudio.h and its window, ui/shell/CleanUpPanel.h): the audio files in a
// project's Audio and Generated folders that neither its song, nor a version, nor a branch's song as it was left plays, and
// those ticked deleted.
#include "HeldFile.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/AudioWork.h"
#include "model/project/UnusedAudio.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"
#include "ui/shell/CleanUpPanel.h"

namespace
{
using namespace phazite::test;

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

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}

// A look for the unused audio, in the background as the window's and the Project view's are, waited for.
phazite::UnusedAudio lookForUnused (const phazite::ProjectContext& context)
{
    phazite::UnusedAudioFinder finder (context);
    finder.find();
    REQUIRE (dispatchMessagesUntil ([&finder] { return ! finder.isLooking() && finder.getFound().has_value(); }));
    return *finder.getFound();
}

juce::StringArray getNames (const std::vector<phazite::UnusedAudioFile>& files)
{
    juce::StringArray names;

    for (const auto& file : files)
        names.add (file.name);

    return names;
}

juce::StringArray getNames (const phazite::UnusedAudio& found)
{
    return getNames (found.files);
}

// Those of the files given that a look finds unused, deleted outright, in the background as the window's Delete deletes them,
// waited for: how many went, and why it refused, if it did.
phazite::UnusedAudioFinder::Deleted deleteUnusedSaying (phazite::ProjectContext& context, const juce::Array<juce::File>& files)
{
    phazite::UnusedAudioFinder finder (context);
    finder.find();
    REQUIRE (dispatchMessagesUntil ([&finder] { return ! finder.isLooking() && finder.getFound().has_value(); }));
    std::optional<phazite::UnusedAudioFinder::Deleted> deleted;
    finder.deleteUnused (context, files, phazite::Removal::outright,
                         [&deleted] (phazite::UnusedAudioFinder::Deleted result) { deleted = std::move (result); });
    REQUIRE (dispatchMessagesUntil ([&deleted] { return deleted.has_value(); }));
    CHECK_FALSE (context.isHoldingForWork());
    return *deleted;
}

int deleteUnused (phazite::ProjectContext& context, const juce::Array<juce::File>& files)
{
    return deleteUnusedSaying (context, files).count;
}

// A song that plays one file, a version that plays another, a branch left with a third; and two files nothing plays, in
// Audio and in Generated, beside a file that isn't audio.
struct Files
{
    TestSong s;
    TemporaryFolder sounds;
    phazite::TrackID track = s.addTrack ("Audio");

    Files()
    {
        s.addAudioClip (track, writeSound (sounds.folder.getChildFile ("Played.wav"), 0.5), 0.0);
        const auto kept = s.addAudioClip (track, writeSound (sounds.folder.getChildFile ("Kept.wav"), 0.5), 4.0);
        keepVersion (s.context, "Both");

        REQUIRE (phazite::test::finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
        s.addAudioClip (s.firstTrack, writeSound (sounds.folder.getChildFile ("Tried.wav"), 0.5), 8.0);
        REQUIRE (phazite::test::finished (
            s.context, [&]
            { return phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save); })); // Tried: on the branch, as it was left

        s.select (kept);
        s.run ("edit.delete"); // Kept: in the version alone

        writeSound (s.getProject().getAudioFolder().getChildFile ("Old take.wav"), 0.5);
        writeSound (s.getProject().getGeneratedFolder().getChildFile ("Leftover.wav"), 0.5);
        s.getProject().getAudioFolder().getChildFile ("Notes.txt").replaceWithText ("not audio");
    }
};
} // namespace

TEST_CASE ("Clean Up lists the audio nothing plays: not the song's, nor a version's, nor a branch's as it was left")
{
    Files p;
    REQUIRE (phazite::canCleanUpAudio (p.s.context));
    const auto unused = lookForUnused (p.s.context).files;
    CHECK (getNames (unused) == juce::StringArray ("Audio/Old take.wav", "Generated/Leftover.wav"));
    CHECK (unused[0].size == unused[0].file.getSize());

    // Those given that are still unused go; one the song plays stays, even if asked for.
    const auto played = p.s.getProject().getAudioFolder().getChildFile ("Played.wav");
    REQUIRE (played.existsAsFile());
    CHECK (deleteUnused (p.s.context, { unused[0].file, played }) == 1);
    CHECK_FALSE (unused[0].file.existsAsFile());
    CHECK (unused[1].file.existsAsFile());
    CHECK (played.existsAsFile());
    CHECK (getNames (lookForUnused (p.s.context)) == juce::StringArray ("Generated/Leftover.wav"));
}

TEST_CASE ("A look for unused audio that the song outran is looked again rather than shown")
{
    // A look begun before a file in the Audio folder was put on a track counted it unused once it was done, which the
    // Project view showed ("2 files" where one was: seen under load in ProjectViewTests.cpp).
    TestSong s;
    const auto track = s.addTrack ("Audio");
    s.saveWithPluginStates(); // a folder of its own
    REQUIRE (waitForVersionHistory (s.context));
    const auto take = writeSound (s.getProject().getAudioFolder().getChildFile ("Take.wav"), 0.5);
    REQUIRE (phazite::canCleanUpAudio (s.context));

    phazite::UnusedAudioFinder finder (s.context);
    finder.find(); // what the song plays taken now: not the take
    s.addAudioClip (track, take, 0.0);
    REQUIRE (dispatchMessagesUntil ([&finder] { return ! finder.isLooking() && finder.getFound().has_value(); }));
    CHECK (getNames (*finder.getFound()).isEmpty());
}

TEST_CASE ("Clean Up keeps the audio that the project file as saved, its autosave or a backup plays")
{
    // The audit's model-project-services 13: what the file on the disk, the autosave and the backups play is in use too, so
    // that closing without saving, restoring the autosave or opening a backup never finds its sounds gone.
    TestSong s;
    const auto track = s.addTrack ("Audio");
    auto& project = s.getProject();
    const auto add = [&s, track] (const char* name)
    { return s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile (name), 0.5), 0.0); };

    const auto remove = [&s] (phazite::ClipID clip)
    {
        s.select (clip);
        s.run ("edit.delete");
    };

    // In a backup alone: saved, taken out and saved again.
    const auto backedUp = add ("Backed up.wav");
    REQUIRE (project.save().wasOk());
    remove (backedUp);
    REQUIRE (project.save().wasOk());

    // In the project file as saved alone: saved, then taken out without a save.
    const auto saved = add ("Saved.wav");
    REQUIRE (project.save().wasOk());
    remove (saved);

    // In the autosave alone: written there, then taken out.
    const auto autosaved = add ("Autosaved.wav");
    REQUIRE (project.autosave().wasOk());
    remove (autosaved);

    writeSound (project.getAudioFolder().getChildFile ("Old take.wav"), 0.5);
    CHECK (getNames (lookForUnused (s.context)) == juce::StringArray ("Audio/Old take.wav"));
}

TEST_CASE ("Clean Up keeps the audio that Local History, and the versions Recover Lost Versions brings back, still play")
{
    // Known bug 61: only the commits on branches and the songs left on branches counted, so that restoring a snapshot, or a
    // commit recovered, found its sounds deleted.
    TestSong s;
    const auto track = s.addTrack ("Audio");
    auto& project = s.getProject();
    auto* history = phazite::getVersionHistory (s.context);
    const auto remove = [&s] (phazite::ClipID clip)
    {
        s.select (clip);
        s.run ("edit.delete");
    };

    keepVersion (s.context, "Before");

    // In a snapshot of Local History alone: taken, then the clip taken out, the project saved without it.
    const auto snapshotted = s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Snapshotted.wav"), 0.5), 0.0);
    REQUIRE (waitForVersionHistory (s.context));
    history->takeSnapshot ("auto");
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (phazite::getSnapshots (s.context).size() == 1);
    remove (snapshotted);

    // In a commit no branch holds any more alone: committed, the commit undone, the clip taken out.
    const auto undone = s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Undone.wav"), 0.5), 0.0);
    keepVersion (s.context, "Undone");
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));
    remove (undone);
    REQUIRE (project.save().wasOk());
    project.getFolder().getChildFile ("Backups").deleteRecursively(); // what they play counts too (the test before)

    writeSound (project.getAudioFolder().getChildFile ("Old take.wav"), 0.5);
    CHECK (getNames (lookForUnused (s.context)) == juce::StringArray ("Audio/Old take.wav"));

    // Deleting them is refused alike: they are still in use.
    const auto snapshottedFile = project.getAudioFolder().getChildFile ("Snapshotted.wav");
    const auto undoneFile = project.getAudioFolder().getChildFile ("Undone.wav");
    REQUIRE (snapshottedFile.existsAsFile());
    REQUIRE (undoneFile.existsAsFile());
    CHECK (deleteUnused (s.context, { snapshottedFile, undoneFile }) == 0);
    CHECK (snapshottedFile.existsAsFile());
    CHECK (undoneFile.existsAsFile());

    // And so they play: the snapshot restored, and the commit recovered, hold them.
    const auto snapshot = history->readSnapshot (phazite::getSnapshots (s.context).front().time);
    CHECK (phazite::findAudioFilePaths (snapshot).contains ("Audio/Snapshotted.wav"));
}

TEST_CASE (
    "Clean Up follows the versions' 30 days: what an undone version and a deleted branch's song play is kept, then offered once they went")
{
    // Clean Up keeps the audio of every version the store still holds, and of the songs kept without a version as the rule
    // keeps them (Local History thinned, a deleted branch's song for 30 days): never less than Recover Lost Versions can
    // bring back, so that no recoverable version finds its audio deleted; and once the sweep let a version go, its audio is
    // offered. With the sweep letting go (off in the product: then nothing is offered, as the store keeps every version).
    phazite::test::SweepLetsGo letGo;
    VersionsTime clock (juce::Time::currentTimeMillis());
    TestSong s;
    const auto track = s.addTrack ("Audio");
    auto& project = s.getProject();
    keepVersion (s.context, "Before");

    // Left on a branch that is then deleted.
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
    s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Tried.wav"), 0.5), 0.0);
    REQUIRE (
        phazite::test::finished (s.context, [&] { return phazite::switchBranch (s.context, findBranch (s.context, "main"), s.save); }));
    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (phazite::deleteBranch (s.context, findBranch (s.context, "idea-2")));

    // In a version that was undone, the clip taken out after.
    const auto undone = s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Undone.wav"), 0.5), 0.0);
    keepVersion (s.context, "Undone");
    REQUIRE (phazite::test::finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));
    s.select (undone);
    s.run ("edit.delete");
    REQUIRE (project.save().wasOk());
    project.getFolder().getChildFile ("Backups").deleteRecursively();

    REQUIRE (phazite::getLostVersions (s.context).size() == 2);
    CHECK (getNames (lookForUnused (s.context)).isEmpty());

    // Five weeks on, the project opened once a week and its store swept: nothing can bring them back, and their audio is
    // offered.
    openEachWeek (s, clock, 35.0);
    REQUIRE (phazite::getLostVersions (s.context).empty());
    CHECK (getNames (lookForUnused (s.context)) == juce::StringArray ("Audio/Tried.wav", "Audio/Undone.wav"));
}

TEST_CASE ("Clean Up offers nothing while a version a log names from the last 30 days can't be read")
{
    // Another computer undid a version whose pack hasn't arrived here (or is held as the store opens): what it plays is
    // unknown, and Recover Lost Versions may bring it back once it can be read.
    TestSong s;
    s.addTrack ("Audio");
    keepVersion (s.context, "Before");
    writeSound (s.getProject().getAudioFolder().getChildFile ("Old take.wav"), 0.5);
    CHECK (getNames (lookForUnused (s.context)) == juce::StringArray ("Audio/Old take.wav"));

    {
        phazite::VersionStore elsewhere (phazite::getVersionsFolder (*phazite::findSong (s.context)), "computer-b");
        REQUIRE (elsewhere.logBranchMove ("their-branch", juce::String::repeatedString ("d", 64), {}, "undo", "theirs"));
    }

    const auto found = lookForUnused (s.context);
    CHECK (found.files.empty());
    CHECK_FALSE (found.unreadable.isEmpty());
}

TEST_CASE ("Clean Up offers nothing while a song another computer kept can't be read whole, and offers again once its pieces arrive")
{
    // The review of known bug 61: computer B records Take.wav and leaves it on a branch; the take and B's working.xml reach
    // this computer before B's pack does. The take must never be offered, nor deleted (which a sync program would carry
    // back to B).
    TestSong s;
    const auto track = s.addTrack ("Audio");
    keepVersion (s.context, "Before");
    auto& song = *phazite::findSong (s.context);
    const auto take = s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Take.wav"), 0.5), 0.0);
    const auto theirs = phazite::splitSong (song.getState().createCopy());
    s.select (take);
    s.run ("edit.delete"); // played here no more: only B's song left on its branch plays it
    REQUIRE (s.getProject().save().wasOk());
    s.getProject().getFolder().getChildFile ("Backups").deleteRecursively(); // what they play counts too (a test above)
    writeSound (s.getProject().getAudioFolder().getChildFile ("Old take.wav"), 0.5);
    REQUIRE (waitForVersionHistory (s.context));

    const auto versions = s.getProject().getFolder().getChildFile ("Versions");
    phazite::VersionStore other (versions, "computer-b");
    phazite::BranchRecord left;
    left.id = "b-branch";
    left.name = "b-idea";
    left.head = phazite::getLastVersion (s.context)->id;
    left.working = theirs.song; // its pieces still on their way
    left.changed = 1;
    REQUIRE (other.writeBranch (left));

    const auto takeFile = s.getProject().getAudioFolder().getChildFile ("Take.wav");
    REQUIRE (takeFile.existsAsFile());
    auto found = lookForUnused (s.context);
    CHECK (found.files.empty());
    REQUIRE (found.unreadable.size() == 1);
    CHECK (found.unreadable[0].contains ("computer-b"));
    const auto refused = deleteUnusedSaying (s.context, { takeFile });
    CHECK (refused.count == 0);
    CHECK (refused.refused.isNotEmpty());
    CHECK (takeFile.existsAsFile());

    // B's pack arrives: its song read whole, the take is in use, the old take isn't.
    REQUIRE (other.writeSong (theirs));
    found = lookForUnused (s.context);
    CHECK (found.unreadable.isEmpty());
    CHECK (getNames (found) == juce::StringArray ("Audio/Old take.wav"));
    CHECK (deleteUnused (s.context, { takeFile }) == 0);
    CHECK (takeFile.existsAsFile());
}

TEST_CASE ("Clean Up offers nothing while a commit's pack can't be opened, and doesn't keep that answer for the next look")
{
    // The review of known bug 61: a piece that couldn't be read was kept as "not a commit" for as long as the history
    // lived, and its commit's files were offered at every look after.
    TestSong s;
    const auto track = s.addTrack ("Audio");
    keepVersion (s.context, "Before");
    const auto versions = s.getProject().getFolder().getChildFile ("Versions").getChildFile ("pieces");
    const auto before = versions.findChildFiles (juce::File::findFiles, false, "*.pack");
    const auto clip = s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Committed.wav"), 0.5), 0.0);
    keepVersion (s.context, "With it");
    auto packs = versions.findChildFiles (juce::File::findFiles, false, "*.pack");
    packs.removeValuesIn (before);
    REQUIRE (packs.size() == 1); // the commit's pack, with its record
    REQUIRE (phazite::test::finished (
        s.context, [&] { return phazite::undoLastCommit (s.context); })); // its commit on no branch, which Recover Lost Commits brings back
    s.select (clip);
    s.run ("edit.delete");
    REQUIRE (s.getProject().save().wasOk());
    s.getProject().getFolder().getChildFile ("Backups").deleteRecursively();
    REQUIRE (waitForVersionHistory (s.context));
    const auto committed = s.getProject().getAudioFolder().getChildFile ("Committed.wav");

    {
        const HeldFile held (packs[0]);

        if (! held.works())
            return;

        const auto found = lookForUnused (s.context);
        CHECK (found.files.empty());
        CHECK_FALSE (found.unreadable.isEmpty());
        CHECK (deleteUnused (s.context, { committed }) == 0);
    }

    // The pack can be opened again: the commit's record read, its file in use.
    const auto found = lookForUnused (s.context);
    CHECK (found.unreadable.isEmpty());
    CHECK (getNames (found).isEmpty());
    CHECK (deleteUnused (s.context, { committed }) == 0);
    CHECK (committed.existsAsFile());
}

TEST_CASE ("Clean Up offers nothing while a snapshot another computer took can't be read, its pack not arrived")
{
    // Computer B records Take.wav and a snapshot of its Local History holds it; B deletes the clip. B's history file and the
    // take reach this computer before B's pack does: the take must not be offered.
    TestSong s;
    const auto track = s.addTrack ("Audio");
    keepVersion (s.context, "Before");
    auto& song = *phazite::findSong (s.context);
    const auto take = s.addAudioClip (track, writeSound (s.getSoundsFolder().getChildFile ("Take.wav"), 0.5), 0.0);
    const auto theirs = phazite::splitSong (song.getState().createCopy());
    s.select (take);
    s.run ("edit.delete");
    REQUIRE (s.getProject().save().wasOk());
    s.getProject().getFolder().getChildFile ("Backups").deleteRecursively();
    writeSound (s.getProject().getAudioFolder().getChildFile ("Old take.wav"), 0.5);
    REQUIRE (waitForVersionHistory (s.context));

    phazite::VersionStore other (s.getProject().getFolder().getChildFile ("Versions"), "computer-b");
    REQUIRE (
        other.writeSnapshots ({ { juce::Time::currentTimeMillis(), phazite::getCurrentBranch (s.context)->id, theirs.song, "auto" } }));

    const auto takeFile = s.getProject().getAudioFolder().getChildFile ("Take.wav");
    auto found = lookForUnused (s.context);
    CHECK (found.files.empty());
    CHECK_FALSE (found.unreadable.isEmpty());
    CHECK (deleteUnused (s.context, { takeFile }) == 0);
    CHECK (takeFile.existsAsFile());

    // B's pack arrives: the snapshot read whole, the take is in use, the old take isn't.
    REQUIRE (other.writeSong (theirs));
    found = lookForUnused (s.context);
    CHECK (found.unreadable.isEmpty());
    CHECK (getNames (found) == juce::StringArray ("Audio/Old take.wav"));
}

TEST_CASE ("Clean Up offers nothing while the Versions folder holds a pack the store can't read: held, or damaged")
{
    TestSong s;
    s.addTrack ("Audio");
    keepVersion (s.context, "Before");
    writeSound (s.getProject().getAudioFolder().getChildFile ("Old take.wav"), 0.5);
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (getNames (lookForUnused (s.context)) == juce::StringArray ("Audio/Old take.wav"));
    const auto pieces = s.getProject().getFolder().getChildFile ("Versions").getChildFile ("pieces");

    SUBCASE ("Another computer's pack, held by another program as the store reads it")
    {
        const auto before = pieces.findChildFiles (juce::File::findFiles, false, "*.pack");

        {
            phazite::VersionStore other (s.getProject().getFolder().getChildFile ("Versions"), "computer-b");
            auto song = phazite::findSong (s.context)->getState().createCopy();
            song.setProperty ("madeOnB", true, nullptr);
            REQUIRE (other.writeSong (phazite::splitSong (song)));
            other.getPieces().closeFiles();
        }

        auto packs = pieces.findChildFiles (juce::File::findFiles, false, "*.pack");
        packs.removeValuesIn (before);
        REQUIRE (packs.size() == 1);
        const HeldFile held (packs[0]);

        if (held.works())
        {
            const auto found = lookForUnused (s.context);
            CHECK (found.files.empty());
            CHECK_FALSE (found.unreadable.isEmpty());
        }
    }

    SUBCASE ("A pack whose trailer is damaged")
    {
        REQUIRE (
            pieces.getChildFile (juce::String::repeatedString ("0", 64) + ".pack").replaceWithText ("PZPK and nothing a pack ends with"));
        const auto found = lookForUnused (s.context);
        CHECK (found.files.empty());
        CHECK_FALSE (found.unreadable.isEmpty());
    }
}

TEST_CASE ("Clean Up reads nothing of the versions on the message thread, however many snapshots Local History keeps")
{
    // The review of known bug 129: the window and Delete read every kept song on the message thread.
    TestSong s;
    const auto track = s.addTrack ("Audio");
    keepVersion (s.context, "Before");
    auto* history = phazite::getVersionHistory (s.context);

    for (int i = 0; i < 8; ++i)
    {
        s.setVolume (track, -1.0f - (float) i);
        REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (withChangesIn (s.context)); }));
        history->takeSnapshot ("auto");
        REQUIRE (waitForVersionHistory (s.context));
    }

    REQUIRE (phazite::getSnapshots (s.context).size() == 8);
    writeSound (s.getProject().getAudioFolder().getChildFile ("Old take.wav"), 0.5);
    const auto before = phazite::VersionStore::countSongsRead();

    phazite::ui::CleanUpPanel panel (s.context, phazite::Removal::outright);
    TestWindow window (panel, phazite::ui::CleanUpPanel::width, phazite::ui::CleanUpPanel::height);
    REQUIRE (dispatchMessagesUntil ([&panel] { return ! panel.isLooking(); }));
    REQUIRE (panel.getNumFiles() == 1);
    Mouse buttons (panel);
    buttons.click (panel.getDeleteButton().getBounds().getCentre());
    REQUIRE (dispatchMessagesUntil ([&panel] { return ! panel.isDeleting() && ! panel.isLooking(); }));
    CHECK (panel.getSummary().startsWith ("Deleted 1 file."));

    const auto after = phazite::VersionStore::countSongsRead();
    INFO ("Songs read: " << (after.all - before.all) << ", on the message thread " << (after.onMessageThread - before.onMessageThread));
    CHECK (after.onMessageThread == before.onMessageThread);
    CHECK (after.all > before.all);
}

TEST_CASE ("Clean Up waits while a snapshot is written, saying why")
{
    // The review of known bug 61: a snapshot names files the Versions folder doesn't hold until it is written.
    TestSong s;
    const auto track = s.addTrack ("Audio");
    keepVersion (s.context, "Before");
    REQUIRE (phazite::canCleanUpAudio (s.context));
    s.setVolume (track, -6.0f);
    REQUIRE (dispatchMessagesUntil ([&s] { return phazite::hasChangesSinceLastVersion (withChangesIn (s.context)); }));

    phazite::VersionHistory::holdWritesForTests = true;
    phazite::getVersionHistory (s.context)->takeSnapshot ("auto");
    CHECK_FALSE (phazite::canCleanUpAudio (s.context));
    CHECK (phazite::getWhyNotCleanUpAudio (s.context).contains ("being written"));
    phazite::VersionHistory::holdWritesForTests = false;
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::canCleanUpAudio (s.context));
}

TEST_CASE ("The Clean Up window lists the files ticked, a click keeps one, and Delete deletes the others and says so")
{
    Files p;
    phazite::ui::CleanUpPanel panel (p.s.context, phazite::Removal::outright);
    TestWindow window (panel, phazite::ui::CleanUpPanel::width, phazite::ui::CleanUpPanel::height);
    CHECK (panel.getSummary().startsWith ("Looking for the audio files"));
    REQUIRE (dispatchMessagesUntil ([&panel] { return ! panel.isLooking(); })); // in the background
    REQUIRE (panel.getNumFiles() == 2);
    CHECK (panel.describeFile (0).startsWith ("Audio/Old take.wav, "));
    CHECK (panel.describeFile (0).endsWith (", to delete"));
    CHECK (panel.getSummary().startsWith ("2 of 2 ticked"));

    // A click on the second keeps it.
    auto& list = panel.getList();
    Mouse mouse (list);
    mouse.click (list.getRowPosition (1, true).getCentre());
    dispatchPendingMessages();
    CHECK_FALSE (panel.isTicked (1));
    CHECK (panel.describeFile (1).endsWith (", kept"));
    CHECK (panel.getSummary().startsWith ("1 of 2 ticked"));

    Mouse buttons (panel);
    buttons.click (panel.getDeleteButton().getBounds().getCentre());
    dispatchPendingMessages();
    REQUIRE (dispatchMessagesUntil ([&panel] { return ! panel.isDeleting() && ! panel.isLooking(); }));
    CHECK (panel.getSummary().startsWith ("Deleted 1 file."));
    CHECK (panel.getNumFiles() == 1);
    CHECK (panel.describeFile (0).startsWith ("Generated/Leftover.wav"));
    CHECK (panel.getCancelButton().getButtonText() == "Close");
    CHECK_FALSE (p.s.getProject().getAudioFolder().getChildFile ("Old take.wav").existsAsFile());
}

TEST_CASE ("Clean Up's Delete works on a thread of its own, the window held, and keeps a file the song came to play since the look")
{
    // The audit of 2026-11-20: Delete parsed the project file, its autosave and every backup again, then moved each file to the
    // recycle bin, all on the message thread: seconds, with no progress, for a large song with many backups or many takes.
    TestSong s;
    const auto track = s.addTrack ("Audio");
    auto& project = s.getProject();

    for (int i = 0; i < 8; ++i)
    {
        s.setVolume (track, -1.0f - (float) i);
        REQUIRE (project.save().wasOk()); // a backup each
    }

    juce::Array<juce::File> takes;

    for (int i = 0; i < 6; ++i)
        takes.add (writeSound (project.getAudioFolder().getChildFile ("Take " + juce::String (i + 1) + ".wav"), 0.2));

    REQUIRE (waitForVersionHistory (s.context));
    REQUIRE (project.getFolder().getChildFile ("Backups").getNumberOfChildFiles (juce::File::findFiles) > 1);
    phazite::ui::CleanUpPanel panel (s.context, phazite::Removal::outright);
    TestWindow window (panel, phazite::ui::CleanUpPanel::width, phazite::ui::CleanUpPanel::height);
    REQUIRE (dispatchMessagesUntil ([&panel] { return ! panel.isLooking(); }));
    REQUIRE (panel.getNumFiles() == 6);

    // The song plays one of them after the look.
    s.addAudioClip (track, takes[2], 0.0);

    // Each file's deletion held, as a recycle bin's move that takes long.
    struct Held
    {
        std::atomic<int> reached { 0 };
        std::atomic<bool> going { false }, onMessageThread { false };
    };

    const auto held = std::make_shared<Held>();
    phazite::UnusedAudioFinder::beforeDeletingForTests = [held] (const juce::File&)
    {
        if (juce::MessageManager::existsAndIsCurrentThread())
            held->onMessageThread = true;

        ++held->reached;
        const auto since = juce::Time::getMillisecondCounterHiRes();

        // Until let go; on the message thread the test can't let go, and it goes on at the limit, which fails the test.
        while (! held->going.load() && juce::Time::getMillisecondCounterHiRes() - since < hangTimeout)
            juce::Thread::sleep (1);
    };

    const juce::ScopeGuard letGo { [held]
                                   {
                                       held->going = true;
                                       phazite::UnusedAudioFinder::beforeDeletingForTests = nullptr;
                                   } };

    const auto savedReadBefore = phazite::UnusedAudioFinder::countSavedFilesRead();
    Mouse buttons (panel);
    buttons.click (panel.getDeleteButton().getBounds().getCentre());

    // The message thread is free while the first file's deletion is held: the window held, saying so.
    REQUIRE (dispatchMessagesUntil ([held] { return held->reached.load() == 1; }));
    CHECK_FALSE (held->onMessageThread.load());
    CHECK (panel.isDeleting());
    CHECK (s.context.isHoldingForWork());
    CHECK (panel.getSummary().startsWith ("Deleting 6 files"));
    CHECK_FALSE (panel.getDeleteButton().isEnabled());

    held->going = true;
    REQUIRE (dispatchMessagesUntil ([&panel] { return ! panel.isDeleting() && ! panel.isLooking(); }));
    CHECK_FALSE (held->onMessageThread.load());
    CHECK_FALSE (s.context.isHoldingForWork());
    CHECK (panel.getSummary().startsWith ("Deleted 5 files."));
    CHECK (takes[2].existsAsFile());

    for (const auto& take : takes)
        if (take != takes[2])
            CHECK_FALSE (take.existsAsFile());

    // The saved files read at the look weren't parsed again, and none on the message thread.
    const auto savedReadAfter = phazite::UnusedAudioFinder::countSavedFilesRead();
    CHECK (savedReadAfter.onMessageThread == savedReadBefore.onMessageThread);
    CHECK (savedReadAfter.all == savedReadBefore.all);
}

TEST_CASE ("Clean Up's Delete stops between two files when it is cancelled, and says what went")
{
    TestSong s;
    s.addTrack ("Audio");
    s.saveWithPluginStates(); // a folder of its own
    REQUIRE (waitForVersionHistory (s.context));
    juce::Array<juce::File> takes;

    for (int i = 0; i < 3; ++i)
        takes.add (writeSound (s.getProject().getAudioFolder().getChildFile ("Take " + juce::String (i + 1) + ".wav"), 0.2));

    phazite::UnusedAudioFinder finder (s.context);
    finder.find();
    REQUIRE (dispatchMessagesUntil ([&finder] { return ! finder.isLooking() && finder.getFound().has_value(); }));
    REQUIRE (finder.getFound()->files.size() == 3);

    // The window's hold as the app's session gives it, whose cancel the test calls as the progress window's Cancel would.
    phazite::ProjectContext::Cancel cancel;
    s.context.setWorkHolder (
        [&cancel] (const juce::String&, phazite::ProjectContext::Cancel c, phazite::ProjectContext::Progress)
        {
            cancel = std::move (c);
            return std::make_unique<phazite::ProjectContext::WorkHold>();
        });

    const auto going = std::make_shared<std::atomic<bool>> (false);
    const auto reached = std::make_shared<std::atomic<int>> (0);
    phazite::UnusedAudioFinder::beforeDeletingForTests = [going, reached] (const juce::File&)
    {
        if (++*reached > 1)
            return;

        const auto since = juce::Time::getMillisecondCounterHiRes();

        while (! going->load() && juce::Time::getMillisecondCounterHiRes() - since < hangTimeout)
            juce::Thread::sleep (1);
    };

    const juce::ScopeGuard letGo { [going]
                                   {
                                       *going = true;
                                       phazite::UnusedAudioFinder::beforeDeletingForTests = nullptr;
                                   } };

    std::optional<phazite::UnusedAudioFinder::Deleted> deleted;
    finder.deleteUnused (s.context, { takes[0], takes[1], takes[2] }, phazite::Removal::outright,
                         [&deleted] (phazite::UnusedAudioFinder::Deleted result) { deleted = std::move (result); });
    REQUIRE (dispatchMessagesUntil ([reached] { return reached->load() == 1; }));
    REQUIRE (cancel != nullptr);
    cancel ({});
    *going = true;
    REQUIRE (dispatchMessagesUntil ([&deleted] { return deleted.has_value(); }));
    CHECK (deleted->count == 1); // the file under way as it was cancelled
    CHECK (deleted->refused == "Stopped.");
    CHECK (reached->load() == 1);
    CHECK_FALSE (s.context.isHoldingForWork());
    CHECK_FALSE (takes[0].existsAsFile());
    CHECK (takes[1].existsAsFile());
    CHECK (takes[2].existsAsFile());
    s.context.setWorkHolder (nullptr);
}

namespace
{
// An audio file's work (an import's copy, say) held in its read until let go.
struct HeldAudioJob final : phazite::AudioWork::Job
{
    explicit HeldAudioJob (std::shared_ptr<std::atomic<bool>> going)
        : letGo (std::move (going))
    {
    }

    juce::String getTitle() const override { return "Importing Take.wav"; }

    void read (const phazite::BackgroundJobs::ShouldStop& shouldStop, std::atomic<float>&) override
    {
        while (! letGo->load() && ! shouldStop())
            juce::Thread::sleep (1);
    }

    void finish (phazite::ProjectContext&) override {}

    const std::shared_ptr<std::atomic<bool>> letGo;
};
} // namespace

TEST_CASE ("Clean Up neither looks nor deletes while an audio file's work is under way, and looks again once it can")
{
    // The audit's review, 2026-11-20: an import's copy is in the Audio folder before a clip plays it, so a look offered it,
    // and a delete, now behind the window's hold, could delete it under the clip the import's end then made.
    TestSong s;
    s.addTrack ("Audio");
    s.saveWithPluginStates(); // a folder of its own
    REQUIRE (waitForVersionHistory (s.context));
    const auto take = writeSound (s.getProject().getAudioFolder().getChildFile ("Take.wav"), 0.5);

    phazite::UnusedAudioFinder finder (s.context);
    finder.find();
    REQUIRE (dispatchMessagesUntil ([&finder] { return ! finder.isLooking() && finder.getFound().has_value(); }));
    REQUIRE (finder.getFound()->files.size() == 1);

    const auto going = std::make_shared<std::atomic<bool>> (false);
    const juce::ScopeGuard letGo { [going] { *going = true; } };
    phazite::getAudioWork (s.context).add (std::make_unique<HeldAudioJob> (going));
    CHECK_FALSE (phazite::canCleanUpAudio (s.context));
    CHECK (phazite::getWhyNotCleanUpAudio (s.context).contains ("imported"));

    // Delete refused at once, saying why; the take kept.
    std::optional<phazite::UnusedAudioFinder::Deleted> deleted;
    finder.deleteUnused (s.context, { take }, phazite::Removal::outright,
                         [&deleted] (phazite::UnusedAudioFinder::Deleted result) { deleted = std::move (result); });
    REQUIRE (deleted.has_value());
    CHECK (deleted->count == 0);
    CHECK (deleted->refused.contains ("imported"));
    CHECK (take.existsAsFile());

    // A look refused too, and wanted again once the work is over.
    finder.find();
    CHECK_FALSE (finder.isLooking());
    CHECK_FALSE (finder.shouldLookAgain());
    *going = true;
    waitForAudioWork (s.context);
    CHECK (finder.shouldLookAgain());
    finder.find();
    REQUIRE (dispatchMessagesUntil ([&finder] { return ! finder.isLooking() && finder.getFound().has_value(); }));
    CHECK_FALSE (finder.shouldLookAgain());
    CHECK (getNames (*finder.getFound()) == juce::StringArray ("Audio/Take.wav"));
}
