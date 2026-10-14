// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestHelpers.h"
#include "model/project/Project.h"
#include "model/transport/Playback.h"

namespace
{
// A folder in the temp directory that is deleted again at the end of the test.
struct TemporaryFolder
{
    TemporaryFolder() { folder.createDirectory(); }

    ~TemporaryFolder() { folder.deleteRecursively(); }

    const juce::File folder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("PhaziteProjectTests", {}, false);
};

std::unique_ptr<phazite::Project> createProject (const juce::File& parentFolder, const juce::String& name)
{
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), parentFolder, name, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project;
}

std::unique_ptr<phazite::Project> openProject (const juce::File& file, bool restoreAutosave = false)
{
    juce::String error;
    auto project = phazite::Project::open (phazite::test::getEngine(), file, restoreAutosave, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project;
}

juce::Array<juce::File> getBackups (const phazite::Project& project)
{
    auto files = project.getBackupsFolder().findChildFiles (juce::File::findFiles, false, "*.phazite");
    files.removeAllInstancesOf (phazite::Project::getAutosaveFile (project.getFile()));
    return files;
}
} // namespace

TEST_CASE ("A new project creates its folder, subfolders and file")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "My Song");

    CHECK (project->getFile() == temp.folder.getChildFile ("My Song").getChildFile ("My Song.phazite"));
    CHECK (project->getFile().existsAsFile());

    for (const auto& folder :
         { project->getAudioFolder(), project->getGeneratedFolder(), project->getRendersFolder(), project->getBackupsFolder() })
        CHECK (folder.isDirectory());

    CHECK_FALSE (project->hasUnsavedChanges());

    // A second project with the same name would overwrite the first, so it is refused.
    juce::String error;
    CHECK (phazite::Project::create (phazite::test::getEngine(), temp.folder, "My Song", error) == nullptr);
    CHECK (error.isNotEmpty());
}

TEST_CASE ("A project saves and reopens with Phazite's own data")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Round Trip");

    project->getEdit().ensureNumberOfAudioTracks (3);
    project->getPhaziteState().setProperty ("outline", "Electronic", nullptr);
    CHECK (project->hasUnsavedChanges());

    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    const auto file = project->getFile();
    const auto id = project->getEdit().getProjectItemRef();
    project = nullptr;

    auto reopened = openProject (file);
    CHECK (te::getAudioTracks (reopened->getEdit()).size() == 3);
    CHECK (reopened->getPhaziteState()["outline"].toString() == juce::String ("Electronic"));
    CHECK (reopened->getEdit().getProjectItemRef() == id); // so its temporary folder stays the same
}

TEST_CASE ("Saving keeps the newest backups")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Backups");
    CHECK (getBackups (*project).isEmpty()); // the first save had nothing to back up

    for (int i = 0; i < phazite::Project::numBackupsToKeep + 3; ++i)
    {
        project->getPhaziteState().setProperty ("version", i, nullptr);
        REQUIRE (project->save().wasOk());
    }

    CHECK (getBackups (*project).size() == phazite::Project::numBackupsToKeep);
}

TEST_CASE ("An autosave survives a crash and can be restored")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Crash");
    const auto file = project->getFile();
    const auto autosaveFile = phazite::Project::getAutosaveFile (file);

    project->getPhaziteState().setProperty ("take", 1, nullptr);
    REQUIRE (project->save().wasOk());

    project->getPhaziteState().setProperty ("take", 2, nullptr);
    REQUIRE (project->autosave().wasOk());
    CHECK (phazite::Project::hasNewerAutosave (file));

    // Closing deletes the autosave; a crash wouldn't. Put back a copy, as if Phazite had crashed.
    juce::MemoryBlock leftOverAutosave;
    REQUIRE (autosaveFile.loadFileAsData (leftOverAutosave));
    project = nullptr;
    CHECK_FALSE (phazite::Project::hasNewerAutosave (file));
    REQUIRE (autosaveFile.replaceWithData (leftOverAutosave.getData(), leftOverAutosave.getSize()));
    CHECK (phazite::Project::hasNewerAutosave (file));

    // Opening without restoring shows the saved version.
    CHECK ((int) openProject (file)->getPhaziteState()["take"] == 1);
    REQUIRE (autosaveFile.replaceWithData (leftOverAutosave.getData(), leftOverAutosave.getSize()));

    // Restoring brings the autosaved change back, as an unsaved change to the same project file.
    auto restored = openProject (file, true);
    CHECK ((int) restored->getPhaziteState()["take"] == 2);
    CHECK (restored->hasUnsavedChanges());
    CHECK (restored->getFile() == file);

    REQUIRE (restored->save().wasOk());
    CHECK_FALSE (phazite::Project::hasNewerAutosave (file));
    restored = nullptr;

    CHECK ((int) openProject (file)->getPhaziteState()["take"] == 2);
}

TEST_CASE ("Autosave writes nothing when nothing changed")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Unchanged");

    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (project->getFile()).exists());
}

TEST_CASE ("Save As makes a copy with the unsaved changes, and leaves the original file alone")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Original");
    REQUIRE (project->getAudioFolder().getChildFile ("kick.wav").replaceWithText ("audio, as far as this test cares"));
    project->getPhaziteState().setProperty ("outline", "Pop", nullptr); // not saved yet

    juce::String error;
    const auto copyFile = project->saveCopyAs (temp.folder, "Copy", error);
    INFO (error.toStdString());
    REQUIRE (copyFile.existsAsFile());
    CHECK (copyFile == temp.folder.getChildFile ("Copy").getChildFile ("Copy.phazite"));
    CHECK (copyFile.getSiblingFile ("Audio").getChildFile ("kick.wav").existsAsFile());
    CHECK (copyFile.getSiblingFile ("Backups").isDirectory());

    // The copy has the change. The original's file doesn't, and the project still has it unsaved.
    CHECK (project->hasUnsavedChanges());
    const auto copy = openProject (copyFile);
    CHECK (copy->getPhaziteState()["outline"].toString() == juce::String ("Pop"));
    CHECK (openProject (project->getFile())->getPhaziteState()["outline"].toString().isEmpty());

    // The copy is a project of its own, with an ID of its own.
    CHECK (copy->getEdit().getProjectItemRef().isProjectItemID());
    CHECK (copy->getEdit().getProjectItemRef() != project->getEdit().getProjectItemRef());

    // A name that is taken is refused.
    error.clear();
    CHECK (project->saveCopyAs (temp.folder, "Copy", error) == juce::File());
    CHECK (error.isNotEmpty());
}

TEST_CASE ("Recent projects list the newest first and skip missing ones")
{
    TemporaryFolder temp;
    const auto a = createProject (temp.folder, "A")->getFile();
    const auto b = createProject (temp.folder, "B")->getFile();
    const auto c = createProject (temp.folder, "C")->getFile();

    phazite::RecentProjects recent (phazite::test::getEngine());

    for (const auto& file : { a, b, c, a })
        recent.add (file);

    CHECK (recent.getProjects() == juce::Array<juce::File> { a, c, b });

    recent.remove (c);
    CHECK (recent.getProjects() == juce::Array<juce::File> { a, b });

    b.getParentDirectory().deleteRecursively();
    CHECK (recent.getProjects() == juce::Array<juce::File> { a });

    recent.remove (a);
}

TEST_CASE ("A project opens with the playhead at the song's start, where playback starts")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Start");
    phazite::movePlayhead (project->getEdit(), te::TimePosition::fromSeconds (6.0));
    CHECK_FALSE (project->hasUnsavedChanges()); // where playback starts changes nothing in the song

    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    auto reopened = openProject (file);
    CHECK (reopened->getEdit().getTransport().getPosition().inSeconds() == doctest::Approx (0.0));
    CHECK (phazite::getPlayStart (reopened->getEdit()).inSeconds() == doctest::Approx (0.0));
}
