// SPDX-License-Identifier: AGPL-3.0-or-later
// The song tied to its version (docs/design/versions.md: The song tied to its version; roadmap S4.5): the project file says
// which branch the song is on and which version it grew from (Schema.h: versionsBranch, versionsBase), every command that
// makes the song grow from another version moves them, and the changes are worked out against that version, so that work
// saved elsewhere since (another computer's, or newer than a project file put back) is offered to be taken in, never taken
// out by the next version.
#include "AppScreen.h"
#include "SongCheck.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/SongPieces.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"
#include "ui/versions/VersionsView.h"

#include <atomic>

namespace
{
using namespace phazite::test;
namespace schema = phazite::schema;

juce::String keepVersion (phazite::ProjectContext& context, const juce::String& name = {})
{
    REQUIRE (waitForVersionHistory (context));
    phazite::setVersionDraft (context, name, {});
    std::optional<juce::Result> result;
    REQUIRE (phazite::saveVersion (context, [&result] (juce::Result r) { result = r; }));
    REQUIRE (dispatchMessagesUntil ([&result] { return result.has_value(); }, versionHistoryTimeout));
    REQUIRE (result->wasOk());
    REQUIRE (waitForVersionHistory (context));
    return phazite::getLastVersion (context)->id;
}

phazite::VersionHistory& history (phazite::ProjectContext& context)
{
    auto* found = phazite::getVersionHistory (context);
    REQUIRE (found != nullptr);
    return *found;
}

// What the song says of where it stands, and what its project file says.
phazite::VersionHistory::Names namesOf (phazite::ProjectContext& context)
{
    return phazite::VersionHistory::readNames (phazite::findSong (context)->getState());
}

phazite::VersionHistory::Names namesInFile (const juce::File& file)
{
    const auto xml = juce::parseXML (file);
    REQUIRE (xml != nullptr);
    phazite::VersionHistory::Names names;
    names.branch = xml->getStringAttribute (schema::versionsBranch.toString());
    names.base = xml->getStringAttribute (schema::versionsBase.toString());
    return names;
}

bool operator== (const phazite::VersionHistory::Names& a, const phazite::VersionHistory::Names& b)
{
    return a.branch == b.branch && a.base == b.base;
}

// Where the song stands, as the history has it: the current branch and the version the song grew from.
phazite::VersionHistory::Names expected (phazite::ProjectContext& context)
{
    return { phazite::getCurrentBranch (context)->id, history (context).getBaseVersionID() };
}

juce::String findBranch (const phazite::ProjectContext& context, const juce::String& name)
{
    for (const auto& branch : phazite::getBranches (context))
        if (branch.name == name)
            return branch.id;

    return {};
}

float volume (phazite::ProjectContext& context, phazite::TrackID track)
{
    return phazite::getChannelVolume (*phazite::findSong (context), track);
}

// A version another computer saved on a branch, on top of one, with one track's volume set in it, and that computer's record
// of the branch at it: as a synced Versions folder brings them. Its id.
juce::String saveElsewhere (phazite::ProjectContext& context, const juce::String& branchID, const juce::String& parent,
                            phazite::TrackID track, float decibels)
{
    auto song = history (context).readSong (parent);
    REQUIRE (song.isValid());

    for (const auto& element : phazite::getTrackElements (song))
        if (phazite::readID (element[schema::trackID]) == track.getRaw())
            juce::ValueTree (element).setProperty (schema::channelVolume, decibels, nullptr);

    phazite::VersionStore store (phazite::getVersionsFolder (*phazite::findSong (context)), "elsewhere");
    phazite::VersionRecord record;
    record.name = "From elsewhere";
    record.parents = { parent };
    record.branch = branchID;
    record.saved = juce::Time::currentTimeMillis();
    const auto id = store.writeVersion (record, phazite::splitSong (song));
    REQUIRE (id.isNotEmpty());

    phazite::BranchRecord branch;

    for (const auto& b : history (context).getBranches())
        if (b.id == branchID)
            branch = b;

    branch.head = id;
    branch.computer = "elsewhere";
    branch.working = {};
    REQUIRE (store.writeBranch (branch));
    store.getPieces().closeFiles();
    return id;
}

// The changes once the history has worked them out.
std::vector<phazite::VersionChange> changesOf (phazite::ProjectContext& context)
{
    REQUIRE (waitForVersionHistory (context));
    return phazite::getChangesSinceLastVersion (withChangesIn (context));
}
} // namespace

TEST_CASE ("The project file says which branch the song is on and which version it grew from, and an untouched project stays saved")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    s.saveWithPluginStates();
    REQUIRE (waitForVersionHistory (s.context));

    // In the song, and in its file, without an unsaved change.
    CHECK (namesOf (s.context) == expected (s.context));
    CHECK (namesOf (s.context).base == first);
    CHECK (namesInFile (s.getProject().getFile()) == expected (s.context));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());
    CHECK (changesOf (s.context).empty());

    // A version saved moves them, into the file too, as the project holds no other change (it was saved first, as Save
    // Version saves it).
    s.setVolume (bass, -6.0f);
    s.saveWithPluginStates();
    const auto quieter = keepVersion (s.context, "Quieter");
    CHECK (namesOf (s.context).base == quieter);
    REQUIRE (dispatchMessagesUntil ([&s] { return s.getProject().hasVersionsNamesWritten(); }));
    REQUIRE (namesInFile (s.getProject().getFile()).base == quieter);
    CHECK_FALSE (s.getProject().hasUnsavedChanges());

    // Opened again: where it was, with nothing changed and nothing unsaved; and so again after a save.
    for (auto round = 0; round < 2; ++round)
    {
        s.reopen();
        REQUIRE (waitForVersionHistory (s.context));
        CHECK (namesOf (s.context) == expected (s.context));
        CHECK (history (s.context).getBaseVersionID() == quieter);
        CHECK (changesOf (s.context).empty());
        CHECK_FALSE (s.getProject().hasUnsavedChanges());
        CHECK (phazite::getBaseProblem (s.context).isEmpty());
        CHECK (phazite::countNewerVersions (s.context) == 0);
        checkSong (*phazite::findSong (s.context));
    }

    // The autosave holds them too, as the song has them: the song's unsaved change and where it stands go together.
    s.setVolume (bass, -9.0f);
    REQUIRE (s.getProject().autosave().wasOk());
    CHECK (namesInFile (phazite::Project::getAutosaveFile (s.getProject().getFile())) == expected (s.context));
    CHECK (phazite::findVersionsNamedByProject (s.getProject().getFile()).contains (quieter));
}

TEST_CASE ("Every command that makes the song grow from another version moves where it stands with it, in the same step")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    s.setVolume (bass, -6.0f);
    const auto quieter = keepVersion (s.context, "Quieter");
    const auto main = phazite::getCurrentBranch (s.context)->id;
    REQUIRE (namesOf (s.context) == phazite::VersionHistory::Names { main, quieter });

    const auto check = [&s] (const juce::String& branch, const juce::String& base)
    {
        REQUIRE (waitForVersionHistory (s.context));
        INFO ("expected " << branch << " " << base << ", the song says " << namesOf (s.context).branch << " " << namesOf (s.context).base
                          << ", the history " << expected (s.context).branch << " " << expected (s.context).base);
        CHECK (namesOf (s.context) == phazite::VersionHistory::Names { branch, base });
        CHECK (expected (s.context) == phazite::VersionHistory::Names { branch, base });
    };

    SUBCASE ("Undo Last Version: the version before")
    {
        REQUIRE (finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));
        check (main, first);
        CHECK (changesOf (s.context).size() == 1); // what Quieter recorded
    }

    SUBCASE ("Create Branch from now: the new branch, the same version")
    {
        REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
        const auto idea = phazite::getCurrentBranch (s.context)->id;
        CHECK (phazite::getCurrentBranch (s.context)->name == "idea-2");
        CHECK (idea != main);
        check (idea, quieter);
        s.reopen();
        check (idea, quieter);
    }

    SUBCASE ("Create Branch from a version, and a switch back and forth: each branch's own")
    {
        REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, first, s.save); }));
        const auto idea = findBranch (s.context, "idea-2");
        check (idea, first);
        CHECK (volume (s.context, bass) == doctest::Approx (0.0f));

        REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, main, s.save); }));
        check (main, quieter);
        REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, idea, s.save); }));
        check (idea, first);
        CHECK_FALSE (s.getProject().hasUnsavedChanges());
        CHECK (namesInFile (s.getProject().getFile()) == expected (s.context));
    }

    SUBCASE ("Restore This Version: the song becomes that version's, and still grows from where it was")
    {
        REQUIRE (finished (s.context, [&] { return phazite::bringBackVersion (s.context, first); }));
        check (main, quieter);
        CHECK (volume (s.context, bass) == doctest::Approx (0.0f));
        CHECK (changesOf (s.context).size() == 1); // the volume taken back, against Quieter

        // Its Undo leaves where it stands as it was too.
        phazite::undo (*phazite::findSong (s.context));
        check (main, quieter);
    }

    SUBCASE ("A merge taken leaves it; its version moves it; Cancel Merge leaves it")
    {
        REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
        const auto idea = findBranch (s.context, "idea-2");
        s.addTrack ("Pad");
        const auto pad = keepVersion (s.context, "Pad");
        REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, main, s.save); }));
        check (main, quieter);

        REQUIRE (finished (s.context, [&] { return phazite::startMerge (s.context, pad); }));
        REQUIRE (finished (s.context, [&] { return phazite::finishMerge (s.context); }));
        REQUIRE (phazite::isMerging (s.context));
        check (main, quieter);

        phazite::cancelMerge (s.context);
        REQUIRE_FALSE (phazite::isMerging (s.context));
        check (main, quieter);

        phazite::undo (*phazite::findSong (s.context)); // the merge waits again
        REQUIRE (phazite::isMerging (s.context));
        const auto merged = keepVersion (s.context);
        check (main, merged);
        // The one parent the graph needs: Pad follows Quieter, and the merge gave nothing anew.
        CHECK (phazite::getLastVersion (s.context)->parents == juce::StringArray (pad));
        CHECK_FALSE (phazite::isMerging (s.context));
        juce::ignoreUnused (idea);
    }

    SUBCASE ("Recover Lost Versions leaves it")
    {
        REQUIRE (finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));
        const auto lost = phazite::getLostVersions (s.context); // the version undone, which its log names
        REQUIRE (lost.size() == 1);
        REQUIRE (phazite::recoverLostVersions (s.context, lost.front().key).isNotEmpty());
        check (main, first);
    }

    SUBCASE ("Save a Copy As: the copy's history, which it grows from as this one does")
    {
        juce::String error;
        const auto copied = s.getProject().saveCopyAs (s.getProject().getFolder().getParentDirectory(), "Copy", error);
        REQUIRE_MESSAGE (copied != juce::File(), error.toStdString());
        CHECK (namesInFile (copied) == phazite::VersionHistory::Names { main, quieter });
        CHECK (phazite::findVersionsNamedByProject (copied).contains (quieter));

        juce::String openError;
        const auto copy = phazite::Project::open (copied, false, openError);
        REQUIRE (copy != nullptr);
        phazite::ProjectContext other (getEngineContext());
        phazite::setSong (other, &copy->getSong());
        REQUIRE (waitForVersionHistory (other));
        CHECK (namesOf (other) == phazite::VersionHistory::Names { main, quieter });
        CHECK (phazite::getBaseProblem (other).isEmpty());
        CHECK (changesOf (other).empty());
        phazite::setSong (other, nullptr);
        phazite::waitForVersionsFinished();
    }

    SUBCASE ("Opened without its Versions folder: the names point at nothing, and a first version starts the history")
    {
        s.saveWithPluginStates();
        phazite::setSong (s.context, nullptr);
        phazite::waitForVersionsFinished();
        REQUIRE (s.getProject().getFolder().getChildFile ("Versions").deleteRecursively());
        phazite::setSong (s.context, &s.getProject().getSong());
        REQUIRE (waitForVersionHistory (s.context));
        REQUIRE (phazite::getVersions (s.context).size() == 1);
        CHECK (phazite::getLastVersion (s.context)->name == "First version");
        CHECK (expected (s.context).base == phazite::getLastVersion (s.context)->id);
        CHECK (namesOf (s.context) == expected (s.context));
        CHECK (expected (s.context).branch != main);
        CHECK (phazite::getBaseProblem (s.context).isEmpty());
    }
}

TEST_CASE ("An autosave restored after a crash, and a backup opened for a damaged file, grow from the versions they name")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    s.saveWithPluginStates(); // the file holds First
    s.setVolume (bass, -6.0f);
    s.saveWithPluginStates(); // which goes into the backups
    const auto quieter = keepVersion (s.context, "Quieter");
    REQUIRE (dispatchMessagesUntil ([&s] { return s.getProject().hasVersionsNamesWritten(); }));
    REQUIRE (namesInFile (s.getProject().getFile()).base == quieter);

    SUBCASE ("The autosave restored")
    {
        s.setVolume (bass, -9.0f); // not saved, autosaved as the crash comes
        REQUIRE (s.reopenAs (TestSong::Reopening::afterCrashRestoring));
        REQUIRE (waitForVersionHistory (s.context));
        CHECK (history (s.context).getBaseVersionID() == quieter);
        CHECK (volume (s.context, bass) == doctest::Approx (-9.0f));
        CHECK (changesOf (s.context).size() == 1);
        CHECK (phazite::countNewerVersions (s.context) == 0);
    }

    SUBCASE ("A backup of the project file, older than the versions saved since, opened for a damaged file")
    {
        // The song before Quieter, as the backup made by the save before it holds it.
        const auto backup = phazite::Project::readNewestBackup (s.getProject().getFile());
        REQUIRE (backup.second.isValid());
        phazite::setSong (s.context, nullptr);
        juce::String error;
        auto opened = phazite::Project::openWithSong (s.getProject().getFile(), backup.second, error);
        REQUIRE (opened != nullptr);
        phazite::setSong (s.context, &opened->getSong());
        REQUIRE (waitForVersionHistory (s.context));
        const auto said = phazite::VersionHistory::readNames (backup.second);
        CHECK (history (s.context).getBaseVersionID() == said.base);
        CHECK (said.base == first);
        CHECK (phazite::countNewerVersions (s.context) == 1); // Quieter, which it didn't grow from
        phazite::setSong (s.context, nullptr);
        opened = nullptr;
        phazite::setSong (s.context, &s.getProject().getSong());
        REQUIRE (waitForVersionHistory (s.context));
    }
}

TEST_CASE ("A version another computer saved on the branch under a closed project is offered to be taken, as one undo step")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    s.saveWithPluginStates();
    const auto main = phazite::getCurrentBranch (s.context)->id;
    const auto theirs = saveElsewhere (s.context, main, first, bass, -6.0f);

    // Opened again: the song grew from First still; the branch's newer version isn't a change of the song's, nor taken out.
    REQUIRE (s.reopenAs (TestSong::Reopening::withoutSaving));
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::getLastVersion (s.context)->id == theirs);
    CHECK (phazite::getBaseVersion (s.context)->id == first);
    CHECK (changesOf (s.context).empty());
    CHECK (phazite::countNewerVersions (s.context) == 1);
    CHECK (phazite::describeNewerVersions (s.context).contains ("1 newer version"));
    CHECK (phazite::describeTakeNewer (s.context) == "Take the Newer Versions");
    REQUIRE (phazite::canTakeNewerVersions (s.context));
    CHECK_FALSE (s.getProject().hasUnsavedChanges());

    // Taken: the song is that version's, which it grows from; one undo step, whose Undo takes both back, and Redo both again.
    UndoCheck undo (s.context);
    undo.check ("Take the Newer Versions", [&] { REQUIRE (finished (s.context, [&] { return phazite::takeNewerVersions (s.context); })); });
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (volume (s.context, bass) == doctest::Approx (-6.0f));
    CHECK (namesOf (s.context) == phazite::VersionHistory::Names { main, theirs });
    CHECK (history (s.context).getBaseVersionID() == theirs);
    CHECK (changesOf (s.context).empty());
    CHECK (phazite::countNewerVersions (s.context) == 0);

    phazite::undo (*phazite::findSong (s.context));
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (volume (s.context, bass) == doctest::Approx (0.0f));
    CHECK (namesOf (s.context).base == first);
    CHECK (history (s.context).getBaseVersionID() == first);
    CHECK (changesOf (s.context).empty());
    CHECK (phazite::countNewerVersions (s.context) == 1);

    phazite::redo (*phazite::findSong (s.context));
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (history (s.context).getBaseVersionID() == theirs);
    CHECK (changesOf (s.context).empty());

    // The next version follows it.
    s.setVolume (bass, -3.0f);
    keepVersion (s.context, "Mine");
    CHECK (phazite::getLastVersion (s.context)->parents == juce::StringArray (theirs));
}

TEST_CASE ("The newer versions merged with the song's own changes: its version follows both lines, and nothing is lost")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto drums = s.addTrack ("Drums");
    const auto first = keepVersion (s.context, "First");
    s.saveWithPluginStates();
    const auto main = phazite::getCurrentBranch (s.context)->id;
    const auto theirs = saveElsewhere (s.context, main, first, bass, -6.0f);

    REQUIRE (s.reopenAs (TestSong::Reopening::withoutSaving));
    REQUIRE (waitForVersionHistory (s.context));
    s.setVolume (drums, -3.0f); // the song's own
    REQUIRE (changesOf (s.context).size() == 1);
    CHECK (phazite::describeTakeNewer (s.context) == "Merge the Newer Versions");

    // Merged in, as one undo step, heard at once; its Undo takes it back.
    REQUIRE (finished (s.context, [&] { return phazite::takeNewerVersions (s.context); }));
    REQUIRE (phazite::getMergeLines (s.context).size() == 1);
    UndoCheck undo (s.context);
    undo.check ("Merge", [&] { REQUIRE (finished (s.context, [&] { return phazite::finishMerge (s.context); })); });
    REQUIRE (phazite::isMerging (s.context));
    CHECK (volume (s.context, bass) == doctest::Approx (-6.0f));
    CHECK (volume (s.context, drums) == doctest::Approx (-3.0f));
    CHECK (phazite::canSaveVersion (withChangesIn (s.context)));

    const auto merged = keepVersion (s.context);
    // Following the newer version alone, which follows First: the graph needs no other parent.
    CHECK (phazite::getLastVersion (s.context)->parents == juce::StringArray (theirs));
    CHECK_FALSE (phazite::isMerging (s.context));
    CHECK (phazite::getMergeProblem (s.context).isEmpty());
    CHECK (phazite::countNewerVersions (s.context) == 0);
    CHECK (phazite::getDivergentVersions (s.context).isEmpty());
    CHECK (namesOf (s.context).base == merged);
}

TEST_CASE ("A project file put back from a backup: no version is saved over the versions saved since, until they are taken in")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    s.saveWithPluginStates();
    const auto backup = s.getProject().getFile().loadFileAsString();

    s.setVolume (bass, -6.0f);
    s.saveWithPluginStates();
    const auto quieter = keepVersion (s.context, "Quieter");
    REQUIRE (dispatchMessagesUntil ([&s] { return s.getProject().hasVersionsNamesWritten(); }));
    REQUIRE (namesInFile (s.getProject().getFile()).base == quieter);

    // The file put back as it was before Quieter, and a change made: Save Version is greyed, saying why; the changes are
    // the song's own, not Quieter's taken out.
    REQUIRE (s.getProject().getFile().replaceWithText (backup));
    REQUIRE (s.reopenAs (TestSong::Reopening::withoutSaving));
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (volume (s.context, bass) == doctest::Approx (0.0f));
    CHECK (changesOf (s.context).empty());
    CHECK (phazite::countNewerVersions (s.context) == 1);
    s.addTrack ("Keys");
    REQUIRE (changesOf (s.context).size() == 1);
    CHECK_FALSE (phazite::canSaveVersion (withChangesIn (s.context)));
    CHECK_FALSE (phazite::saveVersion (s.context));
    CHECK (history (s.context).whyNotSaveOnBranch().contains ("leave on no branch"));

    // Merged in, both kept; then a version.
    REQUIRE (finished (s.context, [&] { return phazite::takeNewerVersions (s.context); }));
    REQUIRE (finished (s.context, [&] { return phazite::finishMerge (s.context); }));
    CHECK (volume (s.context, bass) == doctest::Approx (-6.0f));
    REQUIRE (phazite::canSaveVersion (withChangesIn (s.context)));
    keepVersion (s.context);
    CHECK (phazite::getLastVersion (s.context)->parents.contains (quieter));
}

TEST_CASE ("Save Version over another computer's newer version goes its own way beside it, which the view offers to merge")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto drums = s.addTrack ("Drums");
    const auto first = keepVersion (s.context, "First");
    s.saveWithPluginStates();
    const auto main = phazite::getCurrentBranch (s.context)->id;
    const auto theirs = saveElsewhere (s.context, main, first, bass, -6.0f);
    REQUIRE (s.reopenAs (TestSong::Reopening::withoutSaving));
    REQUIRE (waitForVersionHistory (s.context));

    // Saved without taking it: a line of its own, the other computer's version left as it is, both to merge.
    s.setVolume (drums, -3.0f);
    REQUIRE (changesOf (s.context).size() == 1);
    REQUIRE (phazite::canSaveVersion (withChangesIn (s.context)));
    const auto mine = keepVersion (s.context, "Mine");
    CHECK (phazite::getLastVersion (s.context)->parents == juce::StringArray (first));
    CHECK (phazite::getDivergentVersions (s.context) == juce::StringArray (theirs));
    CHECK (phazite::describeDivergence (s.context).isNotEmpty());

    // Both are there after it opens again.
    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::getLastVersion (s.context)->id == mine);
    CHECK (phazite::getDivergentVersions (s.context) == juce::StringArray (theirs));
    CHECK (volume (s.context, drums) == doctest::Approx (-3.0f));
}

TEST_CASE ("A version another computer saves while the project is open is lost to neither: the next version goes its own way")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto drums = s.addTrack ("Drums");
    const auto first = keepVersion (s.context, "First");
    const auto main = phazite::getCurrentBranch (s.context)->id;
    const auto theirs = saveElsewhere (s.context, main, first, bass, -6.0f); // arriving now, the project open

    s.setVolume (drums, -3.0f);
    const auto mine = keepVersion (s.context, "Mine");
    CHECK (phazite::getLastVersion (s.context)->parents == juce::StringArray (first));

    s.reopen();
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (phazite::getLastVersion (s.context)->id == mine);
    CHECK (phazite::getDivergentVersions (s.context) == juce::StringArray (theirs));
    CHECK (changesOf (s.context).empty());
}

TEST_CASE ("A version the song's names point at that can't be read: the changes are against the branch's last version, said once")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    s.setVolume (bass, -3.0f);
    keepVersion (s.context, "Quieter");
    s.setVolume (bass, -6.0f);
    s.saveWithPluginStates();

    // A version that isn't in the folder, as one let go of by the store's housekeeping would be.
    auto text = s.getProject().getFile().loadFileAsString();
    const auto named = namesInFile (s.getProject().getFile()).base;
    REQUIRE (named.isNotEmpty());
    REQUIRE (s.getProject().getFile().replaceWithText (text.replace (named, juce::String::repeatedString ("a", 64))));
    REQUIRE (s.reopenAs (TestSong::Reopening::withoutSaving));
    REQUIRE (waitForVersionHistory (s.context));

    CHECK (phazite::getBaseProblem (s.context).contains ("can't be read"));
    CHECK (history (s.context).getBaseVersionID() == phazite::getLastVersion (s.context)->id);
    CHECK (namesOf (s.context) == expected (s.context));
    CHECK (volume (s.context, bass) == doctest::Approx (-6.0f));
    CHECK (changesOf (s.context).size() == 1);
    CHECK (phazite::canSaveVersion (withChangesIn (s.context)));

    keepVersion (s.context);
    CHECK (phazite::getBaseProblem (s.context).isEmpty());
}

TEST_CASE ("A switch whose save fails keeps both branches' songs as they were; the save after lets go of the one entered")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    const auto main = phazite::getCurrentBranch (s.context)->id;
    REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
    const auto idea = phazite::getCurrentBranch (s.context)->id;
    s.setVolume (bass, -9.0f); // left on idea-2
    REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, main, s.save); }));

    const auto leftOn = [&s] (const juce::String& branch)
    {
        phazite::VersionStore store (phazite::getVersionsFolder (*phazite::findSong (s.context)), phazite::getThisComputer());

        for (const auto& record : store.readBranches())
            if (record.id == branch && record.computer == phazite::getThisComputer())
                return record.working;

        return juce::String();
    };

    REQUIRE (leftOn (idea).isNotEmpty());

    // The save after the switch fails: idea-2's record names the song left there still.
    const auto failing = [] (std::function<void (bool)> then) { then (false); };
    REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, idea, failing); }));
    CHECK (leftOn (idea).isNotEmpty());
    CHECK (volume (s.context, bass) == doctest::Approx (-9.0f));

    // Saved: let go of.
    REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, main, s.save); }));
    REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, idea, s.save); }));
    CHECK (leftOn (idea).isEmpty());
    CHECK (volume (s.context, bass) == doctest::Approx (-9.0f));
}

TEST_CASE ("The versions a project's files name are its file's, its autosave's and its backups'")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    s.saveWithPluginStates();
    s.setVolume (bass, -6.0f);
    s.saveWithPluginStates(); // a backup of the file that names First
    const auto quieter = keepVersion (s.context, "Quieter");
    REQUIRE (dispatchMessagesUntil ([&s] { return s.getProject().hasVersionsNamesWritten(); }));
    REQUIRE (namesInFile (s.getProject().getFile()).base == quieter);

    const auto named = phazite::findVersionsNamedByProject (s.getProject().getFile());
    CHECK (named.contains (first));
    CHECK (named.contains (quieter));

    for (const auto& version : named)
        CHECK (history (s.context).findVersion (version) != nullptr); // each a version, once (the first version's too)
    CHECK (phazite::findVersionsNamedByProject (s.getProject().getFolder().getChildFile ("None.phazite")).isEmpty());
}

TEST_CASE ("The Versions view says when the branch has newer versions, and its button takes them in")
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    dispatchPendingMessages();
    auto& context = screen.session.getContext();
    REQUIRE (dispatchMessagesUntil ([&context] { return ! context.isHoldingForWork(); }));
    REQUIRE (waitForVersionHistory (context));
    REQUIRE (screen.registry.invoke ("track.add"));
    REQUIRE (screen.registry.invoke ("view.versions"));

    // Saved, with a version; then another computer saves one on the branch, and the project opens again.
    screen.session.saveProject();
    REQUIRE (dispatchMessagesUntil ([&context] { return ! context.isHoldingForWork(); }));
    REQUIRE (waitForVersionHistory (context));
    const auto track = phazite::getTrackIDs (*phazite::findSong (context)).front();
    const auto base = keepVersion (context, "Mine");
    screen.session.saveProject();
    REQUIRE (waitForVersionHistory (context));
    const auto theirs = saveElsewhere (context, phazite::getCurrentBranch (context)->id, base, track, -6.0f);
    const auto file = screen.session.getProject()->getFile();
    screen.session.openProject (file);
    REQUIRE (dispatchMessagesUntil ([&context] { return ! context.isHoldingForWork(); }));
    REQUIRE (waitForVersionHistory (context));

    const auto views = findAll<phazite::ui::VersionsView> (screen.shell);
    REQUIRE (views.size() == 1);
    auto& view = *views[0];
    REQUIRE (dispatchMessagesUntil ([&view] { return view.getNewerText().isNotEmpty(); }));
    CHECK (view.getNewerText().contains ("1 newer version"));
    REQUIRE (view.getNewerButton().isVisible());
    CHECK (view.getNewerButton().getButtonText() == "Take the Newer Versions");

    // Another computer's: a version may be saved meanwhile, going its own way, so the field doesn't say otherwise.
    CHECK_FALSE (view.getMessageField().getTextToShowWhenEmpty().contains ("to save a version"));

    Mouse mouse (view.getNewerButton());
    mouse.click (view.getNewerButton().getLocalBounds().getCentre());
    REQUIRE (dispatchMessagesUntil ([&context, &theirs] { return phazite::getBaseVersion (context)->id == theirs; }));
    REQUIRE (waitForVersionHistory (context));
    REQUIRE (dispatchMessagesUntil ([&view] { return view.getNewerText().isEmpty(); }));
    CHECK_FALSE (view.getNewerButton().isVisible());
    CHECK (phazite::getChannelVolume (*phazite::findSong (context), track) == doctest::Approx (-6.0f));
}

TEST_CASE ("A song whose branch isn't in the folder yet keeps growing from its own version, and no version is saved over another")
{
    // The review of S4.5: the project file synced before another computer's record of its branch (or a backup named a branch
    // deleted since). The song fell back to main's last version, main's work since listed as removals, and Save Version saved
    // the other branch's song as main's.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto drums = s.addTrack ("Drums");
    const auto first = keepVersion (s.context, "First");
    const auto main = phazite::getCurrentBranch (s.context)->id;
    REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
    const auto idea = phazite::getCurrentBranch (s.context)->id;
    s.setVolume (bass, -6.0f);
    const auto onIdea = keepVersion (s.context, "Idea");
    const auto other = history (s.context).makeBranch (first, false); // a branch to switch to, the folder has
    REQUIRE (other.isNotEmpty());
    s.saveWithPluginStates();
    REQUIRE (dispatchMessagesUntil ([&s] { return s.getProject().hasVersionsNamesWritten(); }));
    const auto versionsFolder = s.getProject().getFolder().getChildFile ("Versions");
    const auto findPacks = [&versionsFolder]
    { return versionsFolder.getChildFile ("pieces").findChildFiles (juce::File::findFiles, false, "*.pack"); };
    const auto packsBefore = findPacks();
    const auto onMain = saveElsewhere (s.context, main, first, drums, -3.0f); // main moved on meanwhile, on another computer
    auto packsOfMain = findPacks();
    packsOfMain.removeValuesIn (packsBefore);
    REQUIRE (packsOfMain.size() == 1);

    const auto file = s.getProject().getFile();
    const auto withoutIdea = [&] (bool mainUnreadable)
    {
        phazite::setSong (s.context, nullptr);
        phazite::waitForVersionsFinished();
        REQUIRE (versionsFolder.getChildFile ("branches").getChildFile (idea).deleteRecursively());

        // Main's last version can't be read on this computer's record either: synced, its pack not yet.
        if (mainUnreadable)
        {
            phazite::VersionStore here (versionsFolder, phazite::getThisComputer());
            auto records = here.readBranches();
            const auto mine = std::find_if (records.begin(), records.end(), [&main] (const phazite::BranchRecord& record)
                                            { return record.id == main && record.computer != "elsewhere"; });
            REQUIRE (mine != records.end());
            mine->head = onMain;
            REQUIRE (here.writeBranch (*mine));
            here.getPieces().closeFiles();
            REQUIRE (packsOfMain[0].deleteFile());
        }

        phazite::setSong (s.context, &s.getProject().getSong());
        REQUIRE (waitForVersionHistory (s.context));
    };

    SUBCASE ("Its version reads: the changes are its own, and Create Branch goes on from it")
    {
        withoutIdea (false);
        CHECK (phazite::getCurrentBranch (s.context)->id == main);
        CHECK (history (s.context).getBaseVersionID() == onIdea);
        CHECK (changesOf (s.context).empty()); // main's work isn't listed as taken out
        CHECK (phazite::getBaseProblem (s.context).contains ("doesn't have"));
        CHECK (phazite::countNewerVersions (s.context) == 0);
        CHECK (namesOf (s.context).branch == idea); // kept, for when it syncs

        s.setVolume (bass, -9.0f);
        REQUIRE (changesOf (s.context).size() == 1);
        CHECK_FALSE (phazite::canSaveVersion (s.context));
        CHECK_FALSE (phazite::saveVersion (s.context));

        // Nor a switch, which would leave the song on main as main's, nor Create Branch from a version (a switch to it): said.
        CHECK_FALSE (phazite::canSwitchBranch (s.context));
        CHECK (phazite::whyNotSwitchBranch (s.context).startsWith ("Create Branch first"));
        CHECK_FALSE (phazite::switchBranch (s.context, other, s.save));
        phazite::selectVersion (s.context, first);
        CHECK_FALSE (phazite::newBranch (s.context, first, s.save));
        CHECK (phazite::getCurrentBranch (s.context)->id == main);
        CHECK (phazite::canMakeBranch (s.context)); // Create Branch: from the song as it is, whatever is selected

        // Nor a merge into main, whose version couldn't be saved there, named so.
        phazite::selectVersion (s.context, onMain);
        CHECK_FALSE (phazite::canMerge (s.context));
        CHECK_FALSE (phazite::startMerge (s.context, onMain));
        CHECK (phazite::describeMergeInto (s.context).contains ("Create Branch First"));
        CHECK_FALSE (phazite::canMergeAnyBranch (s.context));
        phazite::selectVersion (s.context, {});

        REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
        const auto kept = phazite::getCurrentBranch (s.context)->id;
        CHECK (kept != main);
        CHECK (phazite::getBaseProblem (s.context).isEmpty());
        REQUIRE (phazite::canSaveVersion (withChangesIn (s.context)));
        keepVersion (s.context, "Kept");
        CHECK (phazite::getLastVersion (s.context)->parents == juce::StringArray (onIdea));

        // Everything works from there: to the other branch and back, the song as it was, growing from its own version.
        CHECK (phazite::canSwitchBranch (s.context));
        REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, other, s.save); }));
        REQUIRE (finished (s.context, [&] { return phazite::switchBranch (s.context, kept, s.save); }));
        CHECK (history (s.context).getBaseVersionID() == phazite::getLastVersion (s.context)->id);
        CHECK (volume (s.context, bass) == doctest::Approx (-9.0f));
        CHECK (changesOf (s.context).empty());
    }

    SUBCASE ("Its version doesn't read either: no version is saved until it does, and Create Branch goes on from the song")
    {
        withoutIdea (false); // the file's names kept as they are, the branch missing
        REQUIRE (namesInFile (file).base == onIdea);
        auto text = file.loadFileAsString();
        REQUIRE (file.replaceWithText (text.replace (onIdea, juce::String::repeatedString ("b", 64))));
        REQUIRE (s.reopenAs (TestSong::Reopening::withoutSaving));
        REQUIRE (waitForVersionHistory (s.context));
        CHECK (phazite::getBaseProblem (s.context).contains ("can't be read either"));

        // Grown from main's last version now, which Undo Last Version would move back, main's work taken off it: not either.
        REQUIRE (history (s.context).getBaseVersionID() == onMain);
        CHECK_FALSE (phazite::canUndoLastCommit (s.context));
        CHECK_FALSE (phazite::undoLastCommit (s.context));
        CHECK (phazite::getCurrentBranch (s.context)->head == onMain);
        s.setVolume (bass, -9.0f);
        REQUIRE (changesOf (s.context).size() >= 1);
        CHECK_FALSE (phazite::canSaveVersion (s.context));
        CHECK_FALSE (phazite::saveVersion (s.context));

        REQUIRE (finished (s.context, [&] { return phazite::newBranch (s.context, {}, s.save); }));
        REQUIRE (phazite::canSaveVersion (withChangesIn (s.context)));
        keepVersion (s.context, "Kept");
        CHECK (volume (s.context, bass) == doctest::Approx (-9.0f));
    }

    SUBCASE ("Main's last version doesn't read either: Reset Branch doesn't move main, which the song isn't on")
    {
        withoutIdea (true);
        CHECK (history (s.context).getBaseVersionID() == onIdea);
        CHECK (phazite::getCurrentBranch (s.context)->id == main);
        CHECK (history (s.context).getReadableCommit() == first); // what would be offered, the song on main
        CHECK_FALSE (phazite::canResetToReadable (s.context));
        CHECK_FALSE (phazite::resetToReadable (s.context));
        CHECK (phazite::getCurrentBranch (s.context)->head == onMain);
        CHECK (history (s.context).getBaseVersionID() == onIdea);
    }
}

TEST_CASE ("Where the names can't be written into the project file, it is said once and the next save writes them; none makes a backup")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    s.saveWithPluginStates();
    REQUIRE (dispatchMessagesUntil ([&s] { return s.getProject().hasVersionsNamesWritten(); }));
    const auto file = s.getProject().getFile();
    const auto backups = [&s]
    {
        return s.getProject()
            .getBackupsFolder()
            .findChildFiles (juce::File::findFiles, false, "*" + juce::String (phazite::Project::fileExtension))
            .size();
    };

    // Written in the background, off the message thread, as the file was saved but for them: no backup of it.
    s.setVolume (bass, -3.0f);
    s.saveWithPluginStates();
    const auto backupsBefore = backups();
    std::atomic<int> onMessageThread { 0 }, elsewhere { 0 };
    juce::String quieter;

    {
        const phazite::WholeFileHook watching (
            [file, &onMessageThread, &elsewhere] (const juce::File& written)
            {
                if (written == file)
                    ++(juce::MessageManager::existsAndIsCurrentThread() ? onMessageThread : elsewhere);

                return true;
            });
        quieter = keepVersion (s.context, "Quieter");
        REQUIRE (dispatchMessagesUntil ([&s] { return s.getProject().hasVersionsNamesWritten(); }));
    }

    CHECK (onMessageThread.load() == 0);
    CHECK (elsewhere.load() == 1);
    CHECK (namesInFile (file).base == quieter);
    CHECK (backups() == backupsBefore);
    CHECK_FALSE (s.getProject().hasUnsavedChanges());

    // A write that fails: said once, pending, and written by the next save.
    juce::StringArray told;
    s.getProject().onVersionsNamesFailed = [&told] (const juce::String& message) { told.add (message); };
    s.setVolume (bass, -6.0f);
    s.saveWithPluginStates();
    juce::String quietest;

    {
        const phazite::WholeFileHook failing ([file] (const juce::File& written) { return written != file; });
        quietest = keepVersion (s.context, "Quietest");
        REQUIRE (dispatchMessagesUntil ([&told] { return ! told.isEmpty(); }));
    }

    CHECK (told.size() == 1);
    CHECK (namesInFile (file).base == quieter);
    CHECK_FALSE (s.getProject().hasVersionsNamesWritten());

    REQUIRE (s.getProject().save().wasOk());
    CHECK (namesInFile (file).base == quietest);
    CHECK (s.getProject().hasVersionsNamesWritten());
    juce::ignoreUnused (first);
}

TEST_CASE ("Undo past a Take of the newer versions and a version saved after it, then Redo, grows from that version again")
{
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto drums = s.addTrack ("Drums");
    const auto first = keepVersion (s.context, "First");
    s.saveWithPluginStates();
    const auto main = phazite::getCurrentBranch (s.context)->id;
    saveElsewhere (s.context, main, first, bass, -6.0f);
    REQUIRE (s.reopenAs (TestSong::Reopening::withoutSaving));
    REQUIRE (waitForVersionHistory (s.context));

    REQUIRE (phazite::describeTakeNewer (withChangesIn (s.context)) == "Take the Newer Versions"); // the song's changes known as none
    REQUIRE (finished (s.context, [&] { return phazite::takeNewerVersions (s.context); }));
    auto& song = *phazite::findSong (s.context);
    CHECK (phazite::describeUndo (&song) == "Undo Take the Newer Versions");
    s.setVolume (drums, -3.0f);
    CHECK (phazite::describeUndo (&song) == "Undo Change Volume");
    const auto mine = keepVersion (s.context, "Mine");

    CHECK (phazite::describeUndo (&song) == "Undo Change Volume");
    phazite::undo (song); // the drums
    CHECK (phazite::describeUndo (&song) == "Undo Take the Newer Versions");
    phazite::undo (song); // the Take
    CHECK (namesOf (s.context).base == first);
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (volume (s.context, bass) == doctest::Approx (0.0f));
    CHECK (history (s.context).getBaseVersionID() == first);
    phazite::redo (song);
    phazite::redo (song);
    REQUIRE (dispatchMessagesUntil ([&s, &mine] { return history (s.context).getBaseVersionID() == mine; }, versionHistoryTimeout));
    CHECK (changesOf (s.context).empty());
    CHECK (phazite::countNewerVersions (s.context) == 0);
    CHECK (namesOf (s.context).base == mine);
}

TEST_CASE (
    "A project file not saved after Undo Last Version grows from the branch's last version as it opens, the undone one's work its change")
{
    // The review of S4.5: Undo Last Version with unsaved edits, then Don't Save: the file still names the version undone, which
    // follows the branch's last; growing from it, the next version put the undone one back under it, its work not a change.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    const auto first = keepVersion (s.context, "First");
    s.saveWithPluginStates();
    s.setVolume (bass, -6.0f);
    s.saveWithPluginStates();
    const auto quieter = keepVersion (s.context, "Quieter");
    REQUIRE (dispatchMessagesUntil ([&s] { return s.getProject().hasVersionsNamesWritten(); }));

    s.addTrack ("Keys"); // unsaved
    REQUIRE (finished (s.context, [&] { return phazite::undoLastCommit (s.context); }));
    REQUIRE (history (s.context).getBaseVersionID() == first);
    REQUIRE (s.getProject().hasUnsavedChanges());
    REQUIRE (namesInFile (s.getProject().getFile()).base == quieter);

    REQUIRE (s.reopenAs (TestSong::Reopening::withoutSaving));
    REQUIRE (waitForVersionHistory (s.context));
    CHECK (history (s.context).getBaseVersionID() == first);
    CHECK (namesOf (s.context).base == first);
    CHECK (phazite::getBaseProblem (s.context).isEmpty());
    CHECK (changesOf (s.context).size() == 1); // Quieter's volume
    CHECK (volume (s.context, bass) == doctest::Approx (-6.0f));

    keepVersion (s.context, "Quieter again");
    CHECK (phazite::getLastVersion (s.context)->parents == juce::StringArray (first));
}

TEST_CASE ("A names write under way finishes before a save or a copy of the file: its failure the save's to take, none told after")
{
    // The review of S4.5: a save waited for the names write but left its failure for the timer, which told it after the save had
    // written them; a copy (Save a Copy As, a first save's move) didn't wait for it at all.
    TestSong s;
    const auto bass = s.addTrack ("Bass");
    keepVersion (s.context, "First");
    s.saveWithPluginStates();
    REQUIRE (dispatchMessagesUntil ([&s] { return s.getProject().hasVersionsNamesWritten(); }));
    const auto file = s.getProject().getFile();
    juce::StringArray told;
    s.getProject().onVersionsNamesFailed = [&told] (const juce::String& message) { told.add (message); };

    // The first names write held on its thread until the test lets go: then failing, or slow (a copy), and a save or a copy then.
    auto fails = true, copies = false;
    std::atomic<bool> held { false }, release { false }, failing { true };
    std::atomic<int> order { 0 }, namesAt { -1 }, copyAt { -1 };

    SUBCASE ("a save")
    {
        fails = true;
    }

    SUBCASE ("a copy")
    {
        fails = false;
        copies = true;
    }

    SUBCASE ("a copy, the write failing")
    {
        copies = true;
    }

    failing = fails;
    const phazite::WholeFileHook hook (
        [file, &held, &release, &failing, &order, &namesAt, &copyAt] (const juce::File& written)
        {
            if (juce::MessageManager::existsAndIsCurrentThread())
            {
                if (written != file && written.hasFileExtension (phazite::Project::fileExtension))
                    copyAt = order++;

                return true;
            }

            if (written != file || held.exchange (true))
                return true;

            while (! release.load())
                juce::Thread::sleep (1);

            if (! failing.load())
                juce::Thread::sleep (100); // a slow drive

            namesAt = order++;
            return ! failing.load();
        });

    s.setVolume (bass, -3.0f);
    s.saveWithPluginStates();
    const auto quieter = keepVersion (s.context, "Quieter");
    REQUIRE (dispatchMessagesUntil ([&held] { return held.load(); }));
    release = true;

    if (! copies)
    {
        // Saved over it: the names in the file, nothing pending, and nothing told, then or later.
        REQUIRE (s.getProject().save().wasOk());
        CHECK (s.getProject().hasVersionsNamesWritten());
        CHECK (namesInFile (file).base == quieter);
        s.setVolume (bass, -6.0f);
        s.saveWithPluginStates();
        keepVersion (s.context, "Quietest");
        REQUIRE (dispatchMessagesUntil ([&s] { return s.getProject().hasVersionsNamesWritten(); }));
        CHECK (told.isEmpty());
    }
    else
    {
        // The copy made once the names are in the file.
        juce::String error;
        const auto copied = s.getProject().saveCopyAs (s.getProject().getFolder().getParentDirectory(), "Copy", error);
        REQUIRE_MESSAGE (copied != juce::File(), error.toStdString());
        CHECK (namesAt.load() >= 0);
        CHECK (namesAt.load() < copyAt.load());
        CHECK (namesInFile (copied).base == quieter);

        if (fails)
        {
            // Taken by the copy, which tells it, once; the next save writes the names.
            CHECK (told.size() == 1);
            REQUIRE (s.getProject().save().wasOk());
            CHECK (namesInFile (file).base == quieter);
            CHECK (s.getProject().hasVersionsNamesWritten());
            CHECK (told.size() == 1);
        }
        else
        {
            CHECK (namesInFile (file).base == quieter);
            CHECK (told.isEmpty());
        }
    }
}
