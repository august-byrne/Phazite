// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Project.h"
#include "model/base/Migrations.h"
#include "model/base/Schema.h"
#include "model/engine/EngineSetup.h"
#include "model/engine/LoudnessMeterPlugin.h"
#include "model/transport/Playback.h"

namespace phazite
{
namespace
{
    // The ID saved in a project file. The Edit names its temporary folder (freeze files, comp renders)
    // after it, so a project keeps its ID from one session to the next. te::loadEditFromFile replaces
    // the saved ID with the one it is given, so this reads it first.
    te::ProjectItemID readSavedID (const juce::File& file)
    {
        if (const auto root = juce::XmlDocument (file).getDocumentElement (true)) // the outer element only
            return te::ProjectItemID (root->getStringAttribute (te::IDs::projectID));

        return {};
    }

    // The same value as the file holds it. Values read from a file are text, and Tracktion may write one back as a number
    // or a bool, which notifies a change though nothing changed. JUCE compares values of different types one way round
    // only (4.0 == "4" holds, "4" == 4.0 doesn't: saving would write "4.0"), so both ways must hold.
    bool isSameValue (const juce::var& a, const juce::var& b)
    {
        return a == b && b == a;
    }

    // What every project has, added before the project watches for changes: the format version it is in, and the
    // master's loudness meter. It isn't the user's change, so it is neither an undo step nor an unsaved change.
    void addPhaziteParts (te::Edit& edit)
    {
        const auto wasChanged = edit.hasChangedSinceSaved();
        writeCurrentSchemaVersion (edit.state);
        ensureLoudnessMeter (edit);
        edit.getUndoManager().clearUndoHistory();

        if (! wasChanged)
            edit.resetChangedStatus();
    }

    // Makes the folder <parentFolder>/<name>, named as files can be, for a project of that name, or sets error and
    // returns nothing if the name is empty, the folder exists or it can't be made.
    juce::File makeProjectFolder (const juce::File& parentFolder, const juce::String& name, juce::String& error)
    {
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

        return folder;
    }
} // namespace

//==============================================================================
std::unique_ptr<Project> Project::create (te::Engine& engine, const juce::File& parentFolder, const juce::String& name, juce::String& error,
                                          const std::function<void (te::Edit&)>& addContent)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const auto folder = makeProjectFolder (parentFolder, name, error);

    if (folder == juce::File())
        return {};

    const auto file = folder.getChildFile (folder.getFileName() + fileExtension);
    auto edit = te::createEmptyEdit (engine, file);

    if (edit == nullptr)
    {
        error = "Phazite couldn't create the project.";
        return {};
    }

    addPhaziteParts (*edit);

    if (addContent != nullptr)
        addContent (*edit);

    std::unique_ptr<Project> project (new Project (std::move (edit), file));

    for (const auto& subfolder :
         { project->getAudioFolder(), project->getGeneratedFolder(), project->getRendersFolder(), project->getBackupsFolder() })
        subfolder.createDirectory();

    if (const auto result = project->save(); result.failed())
    {
        error = result.getErrorMessage();
        return {};
    }

    if (addContent == nullptr)
        return project;

    // Opened again from that save, which Tracktion puts in order while it loads.
    project = nullptr;
    return open (engine, file, false, error);
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
    auto state = te::loadEditFromFile (engine, sourceFile, readSavedID (sourceFile));

    // Projects saved by an earlier Phazite are upgraded before the Edit is made; a newer Phazite's can't be read.
    if (! migrateToCurrentSchema (state))
    {
        error = "The project file " + projectFile.getFullPathName() + " was saved by a newer version of Phazite (project format "
                + juce::String (getSchemaVersion (state)) + "). Update Phazite to open it.";
        return {};
    }

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

bool Project::isFromNewerPhazite (te::Engine& engine, const juce::File& projectFile)
{
    return getSchemaVersion (te::loadEditFromFile (engine, projectFile, te::ProjectItemID {})) > schema::currentVersion;
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
      songFollower (std::make_unique<SongFollower> (*edit))
{
    // A project opens with the playhead at the song's start, where playback starts.
    movePlayhead (*edit, te::TimePosition());
    rememberSavedState(); // what opening made of it, Phazite's parts included, isn't a change
    edit->state.addListener (this);
}

Project::~Project()
{
    JUCE_ASSERT_MESSAGE_THREAD

    songFollower = nullptr;
    edit->state.removeListener (this);
    getAutosaveFile (file).deleteFile();
}

juce::ValueTree Project::getPhaziteState()
{
    return phazite::getPhaziteState (*edit);
}

bool Project::hasUnsavedChanges()
{
    JUCE_ASSERT_MESSAGE_THREAD
    compareWithSavedState();
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
    rememberSavedState(); // what the file holds now
    getAutosaveFile (file).deleteFile();
    return juce::Result::ok();
}

juce::Result Project::autosave()
{
    JUCE_ASSERT_MESSAGE_THREAD
    compareWithSavedState();

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

    const auto newFile = makeFolderWithFiles (parentFolder, name, error);

    if (newFile == juce::File())
        return {};

    const juce::ScopedValueSetter<bool> writing (isWriting, true);
    edit->flushState(); // the plug-ins' current states, as a save writes them

    // The copy is a project of its own, so it gets an ID of its own.
    auto state = edit->state.createCopy();
    state.setProperty (te::IDs::projectID, te::ProjectItemID::createNewID (te::ProjectID {}).toString(), nullptr);

    if (const auto xml = state.createXml(); xml == nullptr || ! xml->writeTo (newFile))
    {
        newFile.getParentDirectory().deleteRecursively(); // so that the name can be tried again
        error = "Phazite couldn't write the project file " + newFile.getFullPathName() + ".";
        return {};
    }

    return newFile;
}

juce::Result Project::moveTo (const juce::File& parentFolder, const juce::String& name)
{
    JUCE_ASSERT_MESSAGE_THREAD

    juce::String error;
    const auto newFile = makeFolderWithFiles (parentFolder, name, error);

    if (newFile == juce::File())
        return juce::Result::fail (error);

    const auto oldFile = file;
    setFile (newFile);

    if (const auto result = save(); result.failed())
    {
        setFile (oldFile);
        newFile.getParentDirectory().deleteRecursively(); // so that the name can be tried again
        return result;
    }

    getAutosaveFile (oldFile).deleteFile(); // what it held is saved now
    return juce::Result::ok();
}

juce::File Project::makeFolderWithFiles (const juce::File& parentFolder, const juce::String& name, juce::String& error) const
{
    const auto folder = makeProjectFolder (parentFolder, name, error);

    if (folder == juce::File())
        return {};

    // The files the project uses come along; its earlier versions (Backups) don't. A failure removes the half-made copy
    // (the folder is new), so that the name can be tried again.
    for (const auto* subfolderName : { "Audio", "Generated", "Renders" })
    {
        const auto source = getFolder().getChildFile (subfolderName);
        const auto target = folder.getChildFile (subfolderName);

        if (! (source.isDirectory() ? source.copyDirectoryTo (target) : target.createDirectory().wasOk()))
        {
            folder.deleteRecursively();
            error = "Phazite couldn't copy the project's " + juce::String (subfolderName) + " folder.";
            return {};
        }
    }

    folder.getChildFile ("Backups").createDirectory();
    return folder.getChildFile (folder.getFileName() + fileExtension);
}

void Project::setFile (const juce::File& newFile)
{
    // The Edit finds its file through editFileRetriever, and through it the files it uses, by their paths relative to it;
    // its clips keep the files they found until told to look again.
    file = newFile;
    edit->editFileRetriever = [newFile] { return newFile; };
    edit->sendSourceFileUpdate();
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

void Project::rememberSavedState()
{
    savedState = edit->state.createCopy();
    mayHaveChanged = false;
}

void Project::compareWithSavedState()
{
    if (! mayHaveChanged)
        return;

    mayHaveChanged = false;

    if (! holdTheSameProject (edit->state, savedState))
    {
        changedSinceSave = true;
        changedSinceAutosave = true;
    }
}

void Project::markChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (isWriting || ! isCounted (tree, property))
        return;

    if (changedSinceSave)
        changedSinceAutosave = true;
    else
        mayHaveChanged = true; // perhaps nothing changed: compared with savedState when asked
}

void Project::markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (! isUncountedElement (parent, child))
        markChanged (parent);
}

bool Project::isUncountedProperty (const juce::ValueTree& tree, const juce::Identifier& property)
{
    return (tree.hasType (te::IDs::TRANSPORT)
            && (property == te::IDs::position || property == te::IDs::start || property == te::IDs::loopPoint1
                || property == te::IDs::loopPoint2))
           || (te::TrackList::isTrack (tree) && schema::isTrackViewProperty (property))
           || (tree.hasType (schema::PHAZITE) && schema::isGridProperty (property));
}

bool Project::isUncountedElement (const juce::ValueTree& parent, const juce::ValueTree& element)
{
    return element.hasType (te::IDs::INPUTDEVICES) || (parent.hasType (schema::PHAZITE) && element.hasType (schema::PIANOROLL));
}

bool Project::isCounted (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (isUncountedProperty (tree, property))
        return false;

    for (auto element = tree; element.isValid(); element = element.getParent())
        if (isUncountedElement (element.getParent(), element))
            return false;

    return true;
}

bool Project::holdTheSameProject (const juce::ValueTree& a, const juce::ValueTree& b)
{
    if (a.getType() != b.getType())
        return false;

    // Each of a's counted properties is in b with the same value, and b has no others.
    int numCounted = 0;

    for (int i = 0; i < a.getNumProperties(); ++i)
    {
        const auto name = a.getPropertyName (i);

        if (isUncountedProperty (a, name))
            continue;

        const auto* other = b.getPropertyPointer (name);

        if (other == nullptr || ! isSameValue (a[name], *other))
            return false;

        ++numCounted;
    }

    for (int i = 0; i < b.getNumProperties(); ++i)
        if (! isUncountedProperty (b, b.getPropertyName (i)))
            --numCounted;

    if (numCounted != 0)
        return false;

    // The counted elements, in the same order, holding the same.
    for (int i = 0, j = 0;; ++i, ++j)
    {
        while (i < a.getNumChildren() && isUncountedElement (a, a.getChild (i)))
            ++i;

        while (j < b.getNumChildren() && isUncountedElement (b, b.getChild (j)))
            ++j;

        if (i == a.getNumChildren() || j == b.getNumChildren())
            return i == a.getNumChildren() && j == b.getNumChildren();

        if (! holdTheSameProject (a.getChild (i), b.getChild (j)))
            return false;
    }
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
} // namespace phazite
