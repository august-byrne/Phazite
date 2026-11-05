// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Project.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/engine/AppInfo.h"
#include "model/mixer/internal/Freeze.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // The song a project file holds (Schema.h: PHAZITE, the file's one element), or an invalid tree for a file that holds
    // none.
    juce::ValueTree readSong (const juce::File& file)
    {
        if (const auto xml = juce::XmlDocument::parse (file); xml != nullptr && xml->hasTagName (schema::PHAZITE.toString()))
            return juce::ValueTree::fromXml (*xml);

        return {};
    }

    // The same value as the file holds it. Values read from a file are text, and the model may write one back as a number
    // or a bool, which notifies a change though nothing changed. JUCE compares values of different types one way round
    // only (4.0 == "4" holds, "4" == 4.0 doesn't: saving would write "4.0"), so both ways must hold.
    bool isSameValue (const juce::var& a, const juce::var& b)
    {
        return a == b && b == a;
    }

    // What a save writes that the song doesn't hold yet: its devices' states as they are now, outside the undo history
    // (Song::storeDeviceStates()), and its next ID past every ID it holds.
    void storeForSaving (Song& song)
    {
        song.storeDeviceStates();
        countIDs (song);
    }

    // Each element's properties in the order of their names, whatever order they were set in.
    void sortProperties (juce::XmlElement& element)
    {
        std::vector<std::pair<juce::String, juce::String>> properties;

        for (const auto& property : element.getAttributeIterator())
            properties.emplace_back (property.name.toString(), property.value);

        const auto byName = [] (const auto& a, const auto& b) { return a.first.compare (b.first) < 0; };

        if (! std::is_sorted (properties.begin(), properties.end(), byName))
        {
            std::sort (properties.begin(), properties.end(), byName);
            element.removeAllAttributes();

            for (const auto& [name, value] : properties)
                element.setAttribute (name, value);
        }

        for (auto* child : element.getChildIterator())
            sortProperties (*child);
    }

    // Writes a song into a project file, through a temporary file that takes its place once it is whole: each property on a
    // line of its own, in the order of their names, as the golden tests write songs, so that a tool that compares text (git,
    // say) shows a change as the lines it changed.
    bool writeProjectFile (const juce::ValueTree& song, const juce::File& file)
    {
        const auto xml = song.createXml();

        if (xml == nullptr)
            return false;

        sortProperties (*xml);
        juce::XmlElement::TextFormat format;
        format.lineWrapLength = 0; // a property on each line
        juce::TemporaryFile temporary (file);
        return xml->writeTo (temporary.getFile(), format) && temporary.overwriteTargetFileWithTemporary();
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
juce::ValueTree makeNewSong()
{
    // A song of its own, numbering its items itself: its tempo map, its master and one empty track.
    Song song { juce::ValueTree (schema::PHAZITE) };
    ensureTempoMap (song);
    getTrackList (song); // with the master's element first
    ensureTracks (song, 1);
    return song.getState();
}

Song::Ptr createSong (const juce::File& file)
{
    return makeSong (makeNewSong(), file);
}

juce::File getDefaultProjectsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (appName).getChildFile ("Projects");
}

Song::Ptr makeSong (const juce::ValueTree& state, const juce::File& file, const juce::File& cacheFolder)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! state.hasType (schema::PHAZITE))
        return {};

    // What every song has: its tempo map, its track list with the master, and its next ID past every ID it holds; without
    // the frozen files that are gone and the curves of devices gone, which no undo can bring back now; and playing from its
    // start. None of it is the user's change, so it is neither an undo step nor an unsaved change.
    Song::Ptr song (new Song (state));
    song->setFile (file);
    song->setCacheFolder (cacheFolder);
    ensureTempoMap (*song);
    getTrackList (*song);
    countIDs (*song);
    forgetMissingFrozenFiles (*song);
    forgetCurvesOfGoneDevices (*song);
    song->getState().removeProperty (schema::playStart, nullptr);
    song->getUndoManager().clearUndoHistory();
    return song;
}

std::unique_ptr<Project> Project::create (const juce::File& parentFolder, const juce::String& name, juce::String& error,
                                          const std::function<void (Song&)>& addContent)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const auto folder = makeProjectFolder (parentFolder, name, error);

    if (folder == juce::File())
        return {};

    const auto file = folder.getChildFile (folder.getFileName() + fileExtension);
    auto song = makeSong (makeNewSong(), file, getCacheFolder (file));

    // What it starts with goes into its first save: no undo step, and no unsaved change.
    if (addContent != nullptr)
    {
        addContent (*song);
        song->getUndoManager().clearUndoHistory();
    }

    std::unique_ptr<Project> project (new Project (std::move (song), file));

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

std::unique_ptr<Project> Project::open (const juce::File& projectFile, bool restoreAutosave, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (! projectFile.existsAsFile())
    {
        error = "The project file " + projectFile.getFullPathName() + " doesn't exist.";
        return {};
    }

    const auto autosaveFile = getAutosaveFile (projectFile);
    const bool useAutosave = restoreAutosave && autosaveFile.existsAsFile();
    auto song = makeSong (readSong (useAutosave ? autosaveFile : projectFile), projectFile, getCacheFolder (projectFile));

    if (song == nullptr)
    {
        error = "Phazite couldn't read the project file " + projectFile.getFullPathName() + ".";
        return {};
    }

    std::unique_ptr<Project> project (new Project (std::move (song), projectFile));

    if (useAutosave)
        project->changedSinceSave = project->changedSinceAutosave = true; // the restored changes aren't in the project file yet

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
class Project::BackgroundWriter : private juce::Thread
{
public:
    BackgroundWriter()
        : juce::Thread ("Phazite autosave")
    {
    }

    ~BackgroundWriter() override { stopThread (30000); } // a write under way is never cut short: run() doesn't check

    bool isBusy() const noexcept { return busy.load(); }
    bool takeFailure() noexcept { return failed.exchange (false); }
    juce::int64 getWrittenAt() const noexcept { return writtenAt.load(); } // the last whole write, or 0
    void forgetWritten() noexcept { writtenAt = 0; }
    void wait() { waitForThreadToExit (30000); }

    // Writes a state that nothing else changes (a copy) into a file.
    void write (juce::ValueTree copy, const juce::File& target)
    {
        jassert (! isBusy());
        wait(); // the last write's thread, which has finished its work, is gone
        state = std::move (copy);
        file = target;
        busy = true;
        startThread();
    }

private:
    juce::ValueTree state;
    juce::File file;
    std::atomic<bool> busy { false }, failed { false };
    std::atomic<juce::int64> writtenAt { 0 }; // in milliseconds since 1970

    void run() override
    {
        if (writeProjectFile (state, file))
            writtenAt = juce::Time::currentTimeMillis();
        else
            failed = true;

        state = {};
        busy = false;
    }
};

Project::Project (Song::Ptr s, const juce::File& f)
    : song (std::move (s)),
      songState (song->getState()),
      file (f),
      autosaveWriter (std::make_unique<BackgroundWriter>())
{
    // The song's cache folder, where its frozen tracks' files are, is the project's Cache folder. Closing the project empties
    // it of what nothing uses; opening it doesn't, as clips find the renders they use only when they play.
    song->setCacheFolder (getCacheFolder());
    rememberSavedState(); // what opening made of it isn't a change
    changesOutsideHistory = song->getChangesOutsideHistory();
    songState.addListener (this);
}

Project::~Project()
{
    JUCE_ASSERT_MESSAGE_THREAD

    autosaveWriter = nullptr; // before the autosave goes
    songState.removeListener (this);
    getAutosaveFile (file).deleteFile();
    purgeCache (*song);
}

bool Project::hasUnsavedChanges()
{
    JUCE_ASSERT_MESSAGE_THREAD
    compareWithSavedState();
    return changedSinceSave;
}

juce::Result Project::save()
{
    JUCE_ASSERT_MESSAGE_THREAD

    autosaveWriter->wait(); // an autosave being written, which this makes old
    backUpProjectFile();

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true); // what saving stores isn't a change by the user
        storeForSaving (*song);

        if (! writeProjectFile (song->getState(), file))
            return juce::Result::fail ("Phazite couldn't save the project to " + file.getFullPathName() + ".");
    }

    changedSinceSave = false;
    changedSinceAutosave = false;
    changesOutsideHistory = song->getChangesOutsideHistory(); // stored in the song as it saved
    rememberSavedState();                                     // what the file holds now
    getAutosaveFile (file).deleteFile();
    autosavedAt = 0;
    autosaveWriter->forgetWritten();
    return juce::Result::ok();
}

juce::Result Project::autosave()
{
    JUCE_ASSERT_MESSAGE_THREAD
    autosaveWriter->wait();
    compareWithSavedState();

    if (! changedSinceAutosave)
        return juce::Result::ok();

    const auto target = getAutosaveFile (file);
    target.getParentDirectory().createDirectory();

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true);
        storeForSaving (*song);

        if (! writeProjectFile (song->getState(), target))
            return juce::Result::fail ("Phazite couldn't write the autosave " + target.getFullPathName() + ".");
    }

    changedSinceAutosave = false;
    autosavedAt = juce::Time::currentTimeMillis();
    return juce::Result::ok();
}

bool Project::hasChangesToAutosave()
{
    JUCE_ASSERT_MESSAGE_THREAD
    compareWithSavedState();
    return changedSinceAutosave;
}

void Project::autosaveInBackground()
{
    JUCE_ASSERT_MESSAGE_THREAD
    compareWithSavedState();

    if (! changedSinceAutosave || autosaveWriter->isBusy())
        return;

    const auto target = getAutosaveFile (file);
    target.getParentDirectory().createDirectory();

    {
        const juce::ScopedValueSetter<bool> writing (isWriting, true);
        storeForSaving (*song);
        autosaveWriter->write (song->getState().createCopy(), target);
    }

    changedSinceAutosave = false;
}

bool Project::isAutosaving() const
{
    return autosaveWriter->isBusy();
}

std::optional<juce::Time> Project::getLastAutosave() const
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (const auto latest = std::max (autosavedAt, autosaveWriter->getWrittenAt()); latest > 0)
        return juce::Time (latest);

    return std::nullopt;
}

bool Project::takeAutosaveFailure()
{
    if (! autosaveWriter->takeFailure())
        return false;

    changedSinceAutosave = true; // written again at the next
    return true;
}

juce::File Project::saveCopyAs (const juce::File& parentFolder, const juce::String& name, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const auto newFile = makeFolderWithFiles (parentFolder, name, error);

    if (newFile == juce::File())
        return {};

    const juce::ScopedValueSetter<bool> writing (isWriting, true);
    storeForSaving (*song);

    if (! writeProjectFile (song->getState(), newFile))
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
    autosaveWriter->wait(); // its autosave goes with it

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

    // The files the project uses come along, its frozen tracks' too; its earlier versions (Backups) don't. A failure removes
    // the half-made copy (the folder is new), so that the name can be tried again.
    for (const auto* subfolderName : { "Audio", "Generated", "Renders", "Cache" })
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

    // Its versions (model/versions/), if it keeps any: a copy of a project is a copy with its past.
    if (const auto versions = getFolder().getChildFile ("Versions");
        versions.isDirectory() && ! versions.copyDirectoryTo (folder.getChildFile ("Versions")))
    {
        folder.deleteRecursively();
        error = "Phazite couldn't copy the project's versions.";
        return {};
    }

    folder.getChildFile ("Backups").createDirectory();
    return folder.getChildFile (folder.getFileName() + fileExtension);
}

void Project::setFile (const juce::File& newFile)
{
    // The song finds the files it plays by their paths relative to it; what plays it finds them again.
    file = newFile;
    song->setFile (newFile);
    song->setCacheFolder (getCacheFolder()); // the copy of the old one

    if (auto* session = findSession (*song))
        session->update();
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
    savedState = song->getState().createCopy();
    mayHaveChanged = compareAll = false;
    elementsToCompare.clearQuick();
}

void Project::compareWithSavedState()
{
    // A plug-in's own change, which a save stores in the song.
    if (const auto outside = song->getChangesOutsideHistory(); outside != changesOutsideHistory)
    {
        changesOutsideHistory = outside;
        changedSinceSave = changedSinceAutosave = true;
    }

    if (! mayHaveChanged)
        return;

    // The elements that notified, each with its copy at the same place in savedState: the song's own elements keep their
    // places unless one was added or removed, which notifies the song itself.
    const auto state = song->getState();
    const auto isSame = [this, &state] (const juce::ValueTree& element)
    {
        const auto index = state.indexOf (element);
        return index >= 0 && index < savedState.getNumChildren() && holdTheSameProject (element, savedState.getChild (index));
    };

    const auto same =
        compareAll ? holdTheSameProject (state, savedState) : std::all_of (elementsToCompare.begin(), elementsToCompare.end(), isSame);

    mayHaveChanged = compareAll = false;
    elementsToCompare.clearQuick();

    if (! same)
    {
        changedSinceSave = true;
        changedSinceAutosave = true;
    }
}

void Project::markChanged (const juce::ValueTree& tree, const juce::Identifier& property)
{
    if (isWriting)
        return;

    const auto state = song->getState();

    if (! (tree == state || tree.isAChildOf (state)) || ! schema::isCounted (tree, property))
        return;

    if (changedSinceSave)
    {
        changedSinceAutosave = true;
        return;
    }

    // Perhaps nothing changed: compared with savedState when asked, the element of the song it is in.
    mayHaveChanged = true;
    auto element = tree;

    while (element.getParent().isValid() && element.getParent() != state)
        element = element.getParent();

    if (element == state || element.getParent() != state)
        compareAll = true;
    else
        elementsToCompare.addIfNotAlreadyThere (element);
}

void Project::markChildChanged (const juce::ValueTree& parent, const juce::ValueTree& child)
{
    if (! schema::isUncountedElement (parent, child))
        markChanged (parent);
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

        if (schema::isUncountedProperty (a, name))
            continue;

        const auto* other = b.getPropertyPointer (name);

        if (other == nullptr || ! isSameValue (a[name], *other))
            return false;

        ++numCounted;
    }

    for (int i = 0; i < b.getNumProperties(); ++i)
        if (! schema::isUncountedProperty (b, b.getPropertyName (i)))
            --numCounted;

    if (numCounted != 0)
        return false;

    // The counted elements, in the same order, holding the same.
    for (int i = 0, j = 0;; ++i, ++j)
    {
        while (i < a.getNumChildren() && schema::isUncountedElement (a, a.getChild (i)))
            ++i;

        while (j < b.getNumChildren() && schema::isUncountedElement (b, b.getChild (j)))
            ++j;

        if (i == a.getNumChildren() || j == b.getNumChildren())
            return i == a.getNumChildren() && j == b.getNumChildren();

        if (! holdTheSameProject (a.getChild (i), b.getChild (j)))
            return false;
    }
}

//==============================================================================
AutosaveTimer::AutosaveTimer (Project& p, int interval, int firstDelay)
    : project (p),
      intervalSeconds (interval),
      firstDelaySeconds (firstDelay)
{
    startTimer (1000);
}

AutosaveTimer::~AutosaveTimer()
{
    stopTimer();
}

void AutosaveTimer::timerCallback()
{
    const auto recording = isRecording (project.getSong());
    const auto recordingEnded = wasRecording && ! recording;
    wasRecording = recording;

    // A write that finished: one that failed counts its changes as not autosaved, so they are written again, and after a
    // few in a row the app says so. The project file itself is untouched.
    if (const auto autosaving = project.isAutosaving(); wasAutosaving && ! autosaving)
    {
        if (! project.takeAutosaveFailure())
        {
            failures = 0;
        }
        else
        {
            juce::Logger::writeToLog ("Autosave failed: " + Project::getAutosaveFile (project.getFile()).getFullPathName());

            if (++failures == failuresToReport && onRepeatedFailures != nullptr)
                onRepeatedFailures();
        }
    }

    wasAutosaving = project.isAutosaving();

    if (recording || wasAutosaving)
        return;

    if (! project.hasChangesToAutosave())
    {
        changedSince = 0;
        return;
    }

    const auto now = juce::Time::getMillisecondCounter();

    if (changedSince == 0)
        changedSince = now;

    // Due once changes have waited; while the song plays, once it stops, unless it plays on for too long.
    const auto wait = (juce::uint32) (autosavedOnce ? intervalSeconds : firstDelaySeconds) * 1000;
    const auto waited = now - changedSince;
    const auto playing = isSongPlaying (project.getSong());

    if (recordingEnded || (waited >= wait && (! playing || waited >= wait + (juce::uint32) maxWaitWhilePlayingSeconds * 1000)))
    {
        project.autosaveInBackground();
        wasAutosaving = project.isAutosaving();
        autosavedOnce = true;
        changedSince = 0;
    }
}
} // namespace phazite
