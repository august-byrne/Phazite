// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Project.h"
#include "LoudnessMeterPlugin.h"
#include "Playback.h"

namespace phazite
{
namespace
{
    const juce::Identifier phaziteStateType { "PHAZITE" };
    constexpr const char* recentProjectsKey = "phaziteRecentProjects";

    // The ID saved in a project file. The Edit names its temporary folder (freeze files, comp renders)
    // after it, so a project keeps its ID from one session to the next. te::loadEditFromFile replaces
    // the saved ID with the one it is given, so this reads it first.
    te::ProjectItemID readSavedID (const juce::File& file)
    {
        if (const auto root = juce::XmlDocument (file).getDocumentElement (true)) // the outer element only
            return te::ProjectItemID (root->getStringAttribute (te::IDs::projectID));

        return {};
    }

    // What every project has, added before the project watches for changes: the master's loudness meter. It
    // isn't the user's change, so it is neither an undo step nor an unsaved change.
    void addPhaziteParts (te::Edit& edit)
    {
        const auto wasChanged = edit.hasChangedSinceSaved();
        ensureLoudnessMeter (edit);
        edit.getUndoManager().clearUndoHistory();

        if (! wasChanged)
            edit.resetChangedStatus();
    }

    // Creates an Edit from a saved state, with projectFile as the file it saves to.
    std::unique_ptr<te::Edit> createEditFromState (te::Engine& engine, const juce::ValueTree& state, const juce::File& projectFile)
    {
        if (! state.isValid())
            return {};

        auto id = te::ProjectItemID::fromProperty (state, te::IDs::projectID);

        if (! id.isValid())
            id = te::ProjectItemID::createNewID (te::ProjectID {});

        return te::Edit::createEdit ({ engine,
                                       state,
                                       id,
                                       te::Edit::forEditing,
                                       nullptr,
                                       te::Edit::getDefaultNumUndoLevels(),
                                       [projectFile] { return projectFile; },
                                       {} });
    }
} // namespace

//==============================================================================
std::unique_ptr<Project> Project::create (te::Engine& engine, const juce::File& parentFolder, const juce::String& name, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const auto safeName = juce::File::createLegalFileName (name.trim());

    if (safeName.isEmpty())
    {
        error = "The project needs a name.";
        return {};
    }

    const auto folder = parentFolder.getChildFile (safeName);

    if (folder.exists())
    {
        error = "There is already a folder called \"" + safeName + "\" in " + parentFolder.getFullPathName() + ".";
        return {};
    }

    if (const auto result = folder.createDirectory(); result.failed())
    {
        error = "Phazite couldn't create the project folder: " + result.getErrorMessage();
        return {};
    }

    const auto file = folder.getChildFile (safeName + fileExtension);
    auto edit = te::createEmptyEdit (engine, file);

    if (edit == nullptr)
    {
        error = "Phazite couldn't create the project.";
        return {};
    }

    addPhaziteParts (*edit);
    std::unique_ptr<Project> project (new Project (std::move (edit), file));

    for (const auto& subfolder :
         { project->getAudioFolder(), project->getGeneratedFolder(), project->getRendersFolder(), project->getBackupsFolder() })
        subfolder.createDirectory();

    if (const auto result = project->save(); result.failed())
    {
        error = result.getErrorMessage();
        return {};
    }

    return project;
}

std::unique_ptr<Project> Project::open (te::Engine& engine, const juce::File& projectFile, bool restoreAutosave, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! projectFile.existsAsFile())
    {
        error = "The project file " + projectFile.getFullPathName() + " doesn't exist.";
        return {};
    }

    const auto autosaveFile = getAutosaveFile (projectFile);
    const bool useAutosave = restoreAutosave && autosaveFile.existsAsFile();

    // Loads on this, the message thread: Tracktion's EditLoader would use a background thread.
    const auto sourceFile = useAutosave ? autosaveFile : projectFile;
    const auto state = te::loadEditFromFile (engine, sourceFile, readSavedID (sourceFile));
    auto edit = createEditFromState (engine, state, projectFile);

    if (edit == nullptr)
    {
        error = "Phazite couldn't read the project file " + projectFile.getFullPathName() + ".";
        return {};
    }

    addPhaziteParts (*edit);
    std::unique_ptr<Project> project (new Project (std::move (edit), projectFile));

    if (useAutosave)
    {
        project->changedSinceSave = true; // the restored changes aren't in the project file yet
        project->edit->markAsChanged();
    }

    return project;
}

bool Project::hasNewerAutosave (const juce::File& projectFile)
{
    const auto autosaveFile = getAutosaveFile (projectFile);
    return autosaveFile.existsAsFile() && autosaveFile.getLastModificationTime() >= projectFile.getLastModificationTime();
}

juce::File Project::getAutosaveFile (const juce::File& projectFile)
{
    return projectFile.getSiblingFile ("Backups").getChildFile (projectFile.getFileNameWithoutExtension() + " (autosave)" + fileExtension);
}

//==============================================================================
Project::Project (std::unique_ptr<te::Edit> e, const juce::File& f)
    : edit (std::move (e)),
      file (f),
      songLoop (std::make_unique<SongLoopFollower> (*edit))
{
    edit->state.addListener (this);
}

Project::~Project()
{
    JUCE_ASSERT_MESSAGE_THREAD

    songLoop = nullptr;
    edit->state.removeListener (this);
    getAutosaveFile (file).deleteFile();
}

juce::ValueTree Project::getPhaziteState()
{
    return phazite::getPhaziteState (*edit);
}

juce::ValueTree getPhaziteState (te::Edit& edit)
{
    return edit.state.getOrCreateChildWithName (phaziteStateType, nullptr);
}

juce::ValueTree findPhaziteState (const te::Edit& edit)
{
    return edit.state.getChildWithName (phaziteStateType);
}

bool Project::hasUnsavedChanges() const
{
    return changedSinceSave || edit->hasChangedSinceSaved();
}

juce::Result Project::save()
{
    JUCE_ASSERT_MESSAGE_THREAD

    backUpProjectFile();

    bool saved = false;

    {
        // Saving writes plug-in states into the Edit's state; that isn't a change by the user.
        const juce::ScopedValueSetter<bool> writing (isWriting, true);

        // No dialogs, save even if Tracktion sees no change, don't offer to discard. With these
        // arguments Tracktion saves synchronously: write a temporary file, then rename it.
        te::EditFileOperations (*edit).save (false, true, false, [&saved] (bool ok) { saved = ok; });
    }

    if (! saved)
        return juce::Result::fail ("Phazite couldn't save the project to " + file.getFullPathName() + ".");

    changedSinceSave = false;
    changedSinceAutosave = false;
    getAutosaveFile (file).deleteFile();
    return juce::Result::ok();
}

juce::Result Project::autosave()
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! changedSinceAutosave)
        return juce::Result::ok();

    const auto target = getAutosaveFile (file);
    target.getParentDirectory().createDirectory();

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true);
        edit->flushState(); // the plug-ins' current states, as a save writes them

        const auto xml = edit->state.createXml();
        juce::TemporaryFile temporary (target);

        if (xml == nullptr || ! xml->writeTo (temporary.getFile()) || ! temporary.overwriteTargetFileWithTemporary())
            return juce::Result::fail ("Phazite couldn't write the autosave " + target.getFullPathName() + ".");
    }

    changedSinceAutosave = false;
    return juce::Result::ok();
}

juce::File Project::saveCopyAs (const juce::File& parentFolder, const juce::String& name, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const auto safeName = juce::File::createLegalFileName (name.trim());

    if (safeName.isEmpty())
    {
        error = "The project needs a name.";
        return {};
    }

    const auto folder = parentFolder.getChildFile (safeName);

    if (folder.exists())
    {
        error = "There is already a folder called \"" + safeName + "\" in " + parentFolder.getFullPathName() + ".";
        return {};
    }

    if (const auto result = folder.createDirectory(); result.failed())
    {
        error = "Phazite couldn't create the project folder: " + result.getErrorMessage();
        return {};
    }

    // From here on, a failure removes the half-made copy (the folder is new), so the name can be
    // tried again.
    const auto fail = [&folder, &error] (const juce::String& message)
    {
        folder.deleteRecursively();
        error = message;
        return juce::File();
    };

    // The files the project uses come along; its earlier versions (Backups) don't.
    for (const auto* subfolderName : { "Audio", "Generated", "Renders" })
    {
        const auto source = getFolder().getChildFile (subfolderName);
        const auto target = folder.getChildFile (subfolderName);
        const bool copied = source.isDirectory() ? source.copyDirectoryTo (target) : target.createDirectory().wasOk();

        if (! copied)
            return fail ("Phazite couldn't copy the project's " + juce::String (subfolderName) + " folder.");
    }

    folder.getChildFile ("Backups").createDirectory();
    const auto newFile = folder.getChildFile (safeName + fileExtension);

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true);
        edit->flushState(); // the plug-ins' current states, as a save writes them

        // The copy is a project of its own, so it gets an ID of its own.
        auto state = edit->state.createCopy();
        state.setProperty (te::IDs::projectID, te::ProjectItemID::createNewID (te::ProjectID {}).toString(), nullptr);

        if (const auto xml = state.createXml(); xml == nullptr || ! xml->writeTo (newFile))
            return fail ("Phazite couldn't write the project file " + newFile.getFullPathName() + ".");
    }

    return newFile;
}

void Project::backUpProjectFile()
{
    if (! file.existsAsFile())
        return;

    const auto backups = getBackupsFolder();
    backups.createDirectory();

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    file.copyFileTo (backups.getNonexistentChildFile (file.getFileNameWithoutExtension() + " " + stamp, fileExtension, false));

    // Keep the newest numBackupsToKeep. A copy keeps the original's modification time, which is
    // when that version was saved.
    auto versions = backups.findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
    versions.removeIf ([autosaveFile = getAutosaveFile (file)] (const juce::File& f) { return f == autosaveFile; });

    std::sort (versions.begin(), versions.end(),
               [] (const juce::File& a, const juce::File& b)
               {
                   const auto ta = a.getLastModificationTime(), tb = b.getLastModificationTime();
                   return ta != tb ? ta > tb : a.getFileName() > b.getFileName();
               });

    for (int i = numBackupsToKeep; i < versions.size(); ++i)
        versions.getReference (i).deleteFile();
}

void Project::markChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (! isWriting && ! isPlaybackState (tree, property))
    {
        changedSinceSave = true;
        changedSinceAutosave = true;
    }
}

bool Project::isPlaybackState (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType (te::IDs::TRANSPORT)
        && (property == te::IDs::position || property == te::IDs::loopPoint1 || property == te::IDs::loopPoint2))
        return true;

    for (auto t = tree; t.isValid(); t = t.getParent())
        if (t.hasType (te::IDs::INPUTDEVICES))
            return true;

    return false;
}

//==============================================================================
AutosaveTimer::AutosaveTimer (Project& p, int intervalSeconds)
    : project (p)
{
    startTimer (intervalSeconds * 1000);
}

AutosaveTimer::~AutosaveTimer()
{
    stopTimer();
}

void AutosaveTimer::timerCallback()
{
    // A failed autosave is retried at the next interval; the project file itself is untouched.
    [[maybe_unused]] const auto result = project.autosave();
}

//==============================================================================
RecentProjects::RecentProjects (te::Engine& e)
    : engine (e)
{
}

void RecentProjects::add (const juce::File& projectFile)
{
    auto list = load();
    list.addFile (projectFile);
    store (list);
}

void RecentProjects::remove (const juce::File& projectFile)
{
    auto list = load();
    list.removeFile (projectFile);
    store (list);
}

juce::Array<juce::File> RecentProjects::getProjects() const
{
    auto list = load();
    list.removeNonExistentFiles();

    juce::Array<juce::File> projects;

    for (int i = 0; i < list.getNumFiles(); ++i)
        projects.add (list.getFile (i));

    return projects;
}

juce::RecentlyOpenedFilesList RecentProjects::load() const
{
    juce::RecentlyOpenedFilesList list;
    list.setMaxNumberOfItems (maxNumProjects);
    list.restoreFromString (engine.getPropertyStorage().getPropertiesFile().getValue (recentProjectsKey));
    return list;
}

void RecentProjects::store (const juce::RecentlyOpenedFilesList& list)
{
    auto& settings = engine.getPropertyStorage().getPropertiesFile();
    settings.setValue (recentProjectsKey, list.toString());
    settings.saveIfNeeded();
}
} // namespace phazite
