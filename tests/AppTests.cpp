// SPDX-License-Identifier: AGPL-3.0-or-later
// The app's own code (src/app, the phazite_app library) without a window: the session that opens, makes and saves
// projects. Nothing here may reach a dialog (an AlertWindow, a FileChooser or a menu): Linux CI has no display.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "app/AppServices.h"
#include "app/ProjectSession.h"
#include "model/base/Schema.h"
#include "services/RecentProjects.h"
#include "services/Settings.h"

namespace
{
// A project made and closed again, as an earlier session would have left it.
juce::File createClosedProject (const juce::File& parentFolder, const juce::String& name)
{
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), parentFolder, name, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project->getFile();
}
} // namespace

TEST_CASE ("The session opens and saves a project")
{
    auto& engine = phazite::test::getEngine();
    auto& uiBehaviour = phazite::getUIBehaviour (engine);
    phazite::test::TemporaryFolder temp;
    const auto file = createClosedProject (temp.folder, "Song");

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);

    {
        phazite::ProjectSession session (engine, uiBehaviour, recent, temp.folder);
        int changes = 0;
        session.onProjectChanged = [&changes] { ++changes; };

        // Without an autosave to restore, it opens at once, as the project the commands and live MIDI act on.
        session.openProject (file);
        REQUIRE (session.getProject() != nullptr);
        CHECK (session.getProject()->getFile() == file);
        CHECK (session.getContext().getEdit() == &session.getProject()->getEdit());
        CHECK (session.getSelection().getEdit() == &session.getProject()->getEdit());
        CHECK (uiBehaviour.getLastFocusedEdit() == &session.getProject()->getEdit());
        CHECK (recent.getProjects() == juce::Array<juce::File> { file });
        CHECK (changes == 1);
        phazite::test::settle();
        CHECK_FALSE (session.getProject()->hasUnsavedChanges());

        // Its commands save it.
        phazite::CommandRegistry registry;
        session.addCommands (registry);
        const auto* save = registry.find ("project.save");
        REQUIRE (save != nullptr);
        CHECK (save->isEnabled());
        session.getProject()->getPhaziteState().setProperty (phazite::schema::outline, "pop", nullptr);
        REQUIRE (session.getProject()->hasUnsavedChanges());
        REQUIRE (registry.invoke ("project.save"));
        CHECK_FALSE (session.getProject()->hasUnsavedChanges());

        // With nothing unsaved, closing asks nothing.
        auto closed = false;
        session.closeThen ([&closed] (bool wasClosed) { closed = wasClosed; });
        CHECK (closed);
    }

    // The session's project goes with it.
    CHECK (uiBehaviour.getLastFocusedEdit() == nullptr);
}

TEST_CASE ("The first start makes My First Song")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    phazite::Settings settings;
    phazite::RecentProjects recent (settings); // no recent projects: a first start

    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, temp.folder);
    session.openAtStartup();

    REQUIRE (session.getProject() != nullptr);
    const auto file = session.getProject()->getFile();
    CHECK (file
           == temp.folder.getChildFile ("My First Song").getChildFile (juce::String ("My First Song") + phazite::Project::fileExtension));
    CHECK (file.existsAsFile());
    CHECK (te::getAudioTracks (*session.getContext().getEdit()).size() == 1); // the starter song
    CHECK (recent.getProjects() == juce::Array<juce::File> { file });

    // It is what the project starts with: no unsaved changes, nothing to undo.
    phazite::test::settle();
    CHECK_FALSE (session.getProject()->hasUnsavedChanges());
    CHECK_FALSE (session.getContext().getEdit()->getUndoManager().canUndo());
}

TEST_CASE ("Startup opens the most recent project")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    const auto older = createClosedProject (temp.folder, "Older");
    const auto newer = createClosedProject (temp.folder, "Newer");

    phazite::Settings settings;
    phazite::RecentProjects recent (settings);
    recent.add (older);
    recent.add (newer);

    phazite::ProjectSession session (engine, phazite::getUIBehaviour (engine), recent, temp.folder);
    session.openAtStartup();

    REQUIRE (session.getProject() != nullptr);
    CHECK (session.getProject()->getFile() == newer);
    CHECK (recent.getProjects() == juce::Array<juce::File> { newer, older });
}

TEST_CASE ("The app's services are made together, with the settings in memory")
{
    auto& engine = phazite::test::getEngine();
    phazite::test::TemporaryFolder temp;
    phazite::AppServices services (engine, { .settingsInMemory = true, .packsFolder = temp.folder });

    // The library shows Phazite Sounds' folder, installed or not.
    CHECK (services.library.getFactorySounds() == services.soundPack.getFolder());
    CHECK (services.soundPack.getFolder().isAChildOf (temp.folder));
    CHECK_FALSE (services.soundPack.isInstalled());

    // The recent projects are kept in the services' settings.
    const auto project = temp.folder.getChildFile ("Song/Song.phazite");
    REQUIRE (project.create().wasOk());
    services.recentProjects.add (project);
    CHECK (services.settings.get (phazite::Settings::recentProjects) == juce::StringArray { project.getFullPathName() });

    // The library indexes on its own thread; it is done before the services go.
    CHECK (phazite::test::runTimersUntil ([&services] { return ! services.library.isIndexing(); }, 10000));
}
