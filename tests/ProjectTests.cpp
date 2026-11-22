// SPDX-License-Identifier: AGPL-3.0-or-later
#include "GoldenFiles.h"
#include "HeldFile.h"
#include "PlanCheck.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "devices/drums/DrumSampler.h"
#include "devices/eq/Equalizer.h"
#include "devices/fm/FmSynth.h"
#include "devices/sampler/Sampler.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Grid.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/KeptByAge.h"
#include "model/base/WholeFile.h"
#include "model/base/internal/Schema.h"
#include "model/base/internal/WholeState.h"
#include "model/chords/internal/Chords.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/SongPlayer.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
#include "model/versions/internal/ApplySong.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace
{
phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

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
    auto project = phazite::Project::create (parentFolder, name, error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project;
}

std::unique_ptr<phazite::Project> openProject (const juce::File& file, bool restoreAutosave = false)
{
    juce::String error;
    auto project = phazite::Project::open (file, restoreAutosave, error);
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

// The song's first track, which every new song has.
phazite::TrackID getFirstTrack (const phazite::Song& song)
{
    const auto tracks = phazite::getTrackIDs (song);
    REQUIRE_FALSE (tracks.empty());
    return tracks.front();
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

    // With the song, in memory and as the file, which holds it alone.
    CHECK (project->getSong().getState().hasType (phazite::schema::PHAZITE));
    CHECK (phazite::test::readXml (project->getFile())->hasTagName ("PHAZITE"));

    // A second project with the same name would overwrite the first, so it is refused.
    juce::String error;
    CHECK (phazite::Project::create (temp.folder, "My Song", error) == nullptr);
    CHECK (error.isNotEmpty());
}

TEST_CASE ("A project named as Windows reserves a name, NUL, makes a folder and a file Windows can make")
{
    // Windows makes no file or folder named NUL (or NUL.phazite): it is a device's name there.
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "NUL");

    CHECK (project->getFile() == temp.folder.getChildFile ("NUL_").getChildFile ("NUL_.phazite"));
    CHECK (project->getFile().existsAsFile());
    CHECK (project->getAudioFolder().isDirectory());
}

TEST_CASE ("A new project whose first save fails leaves no folder behind, and its name can be tried again")
{
    TemporaryFolder temp;
    const auto folder = temp.folder.getChildFile ("Full Disk");
    juce::String error;

    {
        // The project file's write fails, as on a full disk, once its folder and subfolders are made.
        const phazite::WholeFileHook failure ([] (const juce::File& file) { return ! file.hasFileExtension (".phazite"); });
        CHECK (phazite::Project::create (temp.folder, "Full Disk", error) == nullptr);
    }

    CHECK (error.contains ("not enough space"));
    CHECK_FALSE (folder.exists());

    // Tried again, it is made; and a failure never takes a folder that was there before.
    error.clear();
    auto project = phazite::Project::create (temp.folder, "Full Disk", error);
    REQUIRE (project != nullptr);
    CHECK (error.isEmpty());
    CHECK (project->getFile().existsAsFile());
    project.reset();

    // Another project failing beside it: its own folder goes (the deletion is reached: the folder was made, the save failed),
    // the one made before it stays whole.
    error.clear();

    {
        const phazite::WholeFileHook failure ([] (const juce::File&) { return false; });
        CHECK (phazite::Project::create (temp.folder, "Second", error) == nullptr);
    }

    CHECK (error.contains ("not enough space"));
    CHECK_FALSE (temp.folder.getChildFile ("Second").exists());
    CHECK (folder.getChildFile ("Full Disk.phazite").existsAsFile());
    CHECK (folder.getChildFile ("Audio").isDirectory());
}

TEST_CASE ("A project saves and reopens with Phazite's own data")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Round Trip");

    phazite::ensureTracks (project->getSong(), 3);
    project->getSong().getState().setProperty ("outline", "Electronic", nullptr);
    CHECK (project->hasUnsavedChanges());

    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    const auto file = project->getFile();
    project = nullptr;

    auto reopened = openProject (file);
    CHECK (phazite::getTrackIDs (reopened->getSong()).size() == 3);
    CHECK (reopened->getSong().getState()["outline"].toString() == juce::String ("Electronic"));
    CHECK (reopened->getSong().getCacheFolder() == reopened->getCacheFolder()); // where its frozen files are
}

TEST_CASE ("The project file holds each property on a line of its own, in the order of their names")
{
    namespace schema = phazite::schema;
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Lines");
    auto state = project->getSong().getState();
    phazite::ensureTracks (project->getSong(), 2);

    // Set against the order of their names, which the file follows instead.
    state.setProperty (schema::patternsKeepTheirSpeed, true, nullptr);
    state.setProperty (schema::outline, "Electronic", nullptr);
    auto track = phazite::getTrackList (project->getSong()).getChild (0);
    track.setProperty (schema::trackName, "Bass", nullptr);
    track.setProperty (schema::channelVolume, -6.0, nullptr);
    track.setProperty (schema::trackColour, "ff336699", nullptr);
    REQUIRE (project->save().wasOk());

    for (const auto& line : juce::StringArray::fromLines (project->getFile().loadFileAsString()))
    {
        INFO (line.toStdString());
        CHECK ((line.startsWith ("<?xml") || line.indexOf ("=\"") == line.lastIndexOf ("=\""))); // past the XML declaration
    }

    const std::function<void (const juce::XmlElement&)> checkOrder = [&checkOrder] (const juce::XmlElement& element)
    {
        for (int i = 1; i < element.getNumAttributes(); ++i)
        {
            INFO ((element.getTagName() + ": " + element.getAttributeName (i - 1) + ", " + element.getAttributeName (i)).toStdString());
            CHECK (element.getAttributeName (i - 1).compare (element.getAttributeName (i)) < 0);
        }

        for (const auto* child : element.getChildIterator())
            checkOrder (*child);
    };

    const auto xml = phazite::test::readXml (project->getFile());
    checkOrder (*xml);

    // And it reads back as it was.
    const auto file = project->getFile();
    project = nullptr;
    auto reopened = openProject (file);
    const auto reopenedTrack = phazite::getTrackList (reopened->getSong()).getChild (0);
    CHECK (reopened->getSong().getState()[schema::outline].toString() == "Electronic");
    CHECK (reopenedTrack[schema::trackName].toString() == "Bass");
    CHECK (juce::approximatelyEqual ((double) reopenedTrack[schema::channelVolume], -6.0));
    CHECK_FALSE (reopened->hasUnsavedChanges());
}

TEST_CASE ("The song numbers its items itself, each new one past every ID it holds, never one twice, and saves where it got to")
{
    namespace schema = phazite::schema;
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "IDs");
    auto& song = project->getSong();
    auto state = song.getState();
    const auto next = [&state] { return (std::uint64_t) (juce::int64) state[schema::nextID]; };
    const auto idOf = [] (const juce::ValueTree& element, const juce::Identifier& property)
    { return (std::uint64_t) (juce::int64) element[property]; };
    const auto addTrack = [&song]
    {
        const phazite::UndoStep step (song, "Add Track");
        const auto track = phazite::insertTrack (song, {}, nullptr);
        REQUIRE (track.isValid());
        return phazite::findTrackState (song, track);
    };

    // Past the first track's, its fader's and the master's.
    const auto first = phazite::getTrackStates (song).front();
    CHECK (next() > idOf (first, schema::trackID));
    CHECK (next() > idOf (first, schema::channelFader));
    CHECK (next() > idOf (phazite::getMasterState (song), schema::channelFader));

    // A new track takes the next IDs, which an undo doesn't give back: the track after it gets new ones.
    auto before = next();
    const auto added = addTrack();
    CHECK (idOf (added, schema::trackID) >= before);
    CHECK (next() > idOf (added, schema::channelFader));
    const auto undone = idOf (added, schema::trackID);
    phazite::undo (song);
    before = next();
    const auto another = addTrack();
    CHECK (idOf (another, schema::trackID) != undone);
    CHECK (idOf (another, schema::trackID) >= before);

    // A song whose count is further on (a project whose last items went before it was saved) goes on from there.
    state.setProperty (schema::nextID, (juce::int64) 50000, nullptr);
    CHECK (song.makeID() >= 50000u);

    // Saved, and opened again where it got to.
    REQUIRE (project->save().wasOk());
    const auto saved = next();
    const auto file = project->getFile();
    project = nullptr;
    auto reopened = openProject (file);
    CHECK ((std::uint64_t) (juce::int64) reopened->getSong().getState()[schema::nextID] == saved);
    CHECK (reopened->getSong().makeID() >= saved);
}

TEST_CASE ("Saves that change nothing make no backup: the newest backup holds the file already")
{
    // The audit's model-project-services 6: every save copied the project file into Backups/, changed or not, so that ten
    // presses of Ctrl+S left ten copies of one file.
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Same");
    CHECK (getBackups (*project).isEmpty()); // the first save had nothing to back up

    for (int i = 0; i < 10; ++i)
        REQUIRE (project->save().wasOk());

    CHECK (getBackups (*project).size() == 1); // the file as it was made

    // A change saved, and saved again ten times: the file as it was before it, and as it is, one backup each.
    project->getSong().getState().setProperty (phazite::schema::notes, "Changed", &project->getSong().getUndoManager());

    for (int i = 0; i < 10; ++i)
        REQUIRE (project->save().wasOk());

    const auto backups = getBackups (*project);
    REQUIRE (backups.size() == 2);
    CHECK ((backups[0].hasIdenticalContentTo (project->getFile()) || backups[1].hasIdenticalContentTo (project->getFile())));
}

TEST_CASE ("Of times that are the same, keepByAge() keeps the one later in the list: in a list oldest first, the newer")
{
    using namespace phazite::keptByAge;
    constexpr juce::int64 now = 1000 * day;

    // Two of one time three hours before (the newest of an hour kept), and two of one time ten days before (of a day).
    const std::vector<juce::int64> oldestFirst { now - 10 * day, now - 10 * day, now - 3 * hour, now - 3 * hour };
    CHECK (phazite::keepByAge (oldestFirst, now) == std::vector<bool> { false, true, false, true });
}

TEST_CASE ("Backups of one second are kept by their numbers, and what else is in Backups/ is neither thinned nor offered")
{
    // The review of S4.6: a newest-first list handed keepByAge() the older of two backups of one second (Linux dates files
    // in whole seconds), its names ordered as text ("_10" before "_2"); and every .phazite file in Backups/ was thinned by
    // age as a backup, and offered for a damaged project file.
    constexpr juce::int64 minute = 60 * 1000, hour = 60 * minute, day = 24 * hour;
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Kept");
    auto& song = project->getSong();
    const auto backups = project->getBackupsFolder();
    const auto now = juce::Time (2026, 0, 20, 12, 0, 0, 0, false).toMilliseconds();

    struct BackupTime
    {
        ~BackupTime() { phazite::Project::setBackupTime (0); }
    } const backupTime;

    const auto put = [&backups] (const juce::String& name, juce::int64 time)
    {
        const auto file = backups.getChildFile (name);
        REQUIRE (file.replaceWithText (name));
        REQUIRE (file.setLastModificationTime (juce::Time (time)));
        return file;
    };

    // Ten backups of one second, five hours before: of an hour, the newest is kept, the tenth.
    put ("Kept 2026-01-20 07-00-00.phazite", now - 5 * hour);

    for (int n = 2; n <= 10; ++n)
        put ("Kept 2026-01-20 07-00-00_" + juce::String (n) + ".phazite", now - 5 * hour);

    // And what isn't one of the project's backups, two months old: another project's file, another name's autosave, a
    // temporary file a crash left, a copy the user made.
    const juce::StringArray others { "Other.phazite", "Old name (autosave).phazite", "Kept (autosave)_temp1a2b3c.phazite",
                                     "Kept 2026-01-20 07-00-00 copy.phazite" };

    for (const auto& name : others)
        put (name, now - 60 * day);

    phazite::Project::setBackupTime (now);
    REQUIRE (project->getFile().setLastModificationTime (juce::Time (now - minute)));
    song.getState().setProperty (phazite::schema::notes, "Changed", &song.getUndoManager());
    REQUIRE (project->save().wasOk());

    CHECK (backups.getChildFile ("Kept 2026-01-20 07-00-00_10.phazite").existsAsFile());
    CHECK_FALSE (backups.getChildFile ("Kept 2026-01-20 07-00-00_9.phazite").existsAsFile());
    CHECK_FALSE (backups.getChildFile ("Kept 2026-01-20 07-00-00_2.phazite").existsAsFile());
    CHECK_FALSE (backups.getChildFile ("Kept 2026-01-20 07-00-00.phazite").existsAsFile());

    for (const auto& name : others)
        CHECK (backups.getChildFile (name).existsAsFile());

    // The file it backed up, a minute before, is the newest of the project's: offered for a damaged file, though a song
    // that isn't one of its backups is newer there.
    const auto other = backups.getChildFile ("Other.phazite");
    REQUIRE (project->getFile().copyFileTo (other));
    REQUIRE (other.setLastModificationTime (juce::Time (now + hour)));
    const auto offered = phazite::Project::readNewestBackup (project->getFile());
    CHECK (offered.second.isValid());
    CHECK (offered.first.getParentDirectory() == backups);
    CHECK (offered.first.getLastModificationTime().toMilliseconds() == now - minute);
}

TEST_CASE ("Backups are kept by age: a day's work leaves backups from an hour, a day and a week before, none older than kept")
{
    // The audit's model-project-services 6: ten backups were kept, which Auto Save, backing up every 10 minutes, turned over
    // in under two hours. They are kept as Local History is, each by when its version was saved: every one of the last two
    // hours, the newest of each hour for a day, the newest of each day for 30 days, and the newest of those older than that.
    constexpr juce::int64 minute = 60 * 1000, hour = 60 * minute, day = 24 * hour;
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Weeks");
    auto& song = project->getSong();

    struct BackupTime
    {
        ~BackupTime() { phazite::Project::setBackupTime (0); }
    } const backupTime;

    // A save at a time: the clock the backups go by there, and the file's time the time it was saved.
    const auto saveAt = [&project, &song] (juce::int64 time, const juce::String& change)
    {
        phazite::Project::setBackupTime (time);
        song.getState().setProperty (phazite::schema::notes, change, &song.getUndoManager());
        REQUIRE (project->save().wasOk());
        REQUIRE (project->getFile().setLastModificationTime (juce::Time (time)));
    };

    const auto backupAges = [&project] (juce::int64 now)
    {
        std::vector<juce::int64> ages;

        for (const auto& backup : getBackups (*project))
            ages.push_back (now - backup.getLastModificationTime().toMilliseconds());

        return ages;
    };

    const auto count = [] (const std::vector<juce::int64>& ages, juce::int64 from, juce::int64 to)
    { return std::count_if (ages.begin(), ages.end(), [from, to] (juce::int64 age) { return age > from && age <= to; }); };

    // Six weeks of work, each day from 9:00 to 17:00 (UTC): the last two days a change saved every half hour, the days
    // before at the start and the end of the day.
    const auto start = juce::Time (2026, 0, 5, 0, 0, 0, 0, false).toMilliseconds();
    REQUIRE (project->getFile().setLastModificationTime (juce::Time (start - day)));
    juce::int64 now = 0;

    for (juce::int64 d = 0; d < 42; ++d)
    {
        for (auto t = 9 * hour; t <= 17 * hour; t += d < 40 ? 8 * hour : 30 * minute)
        {
            now = start + d * day + t;
            saveAt (now, juce::String (now));
        }
    }

    const auto ages = backupAges (now);
    CHECK (count (ages, 45 * minute, 75 * minute) >= 1);                 // an hour before
    CHECK (count (ages, 23 * hour, 25 * hour) >= 1);                     // a day before
    CHECK (count (ages, 7 * day - 12 * hour, 7 * day + 12 * hour) == 1); // a week before: that day's last
    CHECK (count (ages, -1, 2 * hour) == 4);                             // every one of the last two hours
    CHECK (count (ages, 2 * hour, day) == 7);                            // the newest of each hour for a day
    CHECK (count (ages, day, 30 * day) == 30);                           // the newest of each day for 30 days
    CHECK (count (ages, 30 * day, 100 * day) == 1);                      // and the newest of those older
    CHECK (count (ages, 31 * day - 1, 31 * day) == 1);                   // that day's last

    // Opened again after two months away and worked on for three hours: the file it opened with is still a backup.
    const auto lastSaved = now;
    now += 60 * day;
    saveAt (now, "Back");

    for (int i = 1; i <= 6; ++i)
        saveAt (now += 30 * minute, "Back " + juce::String (i));

    const auto later = backupAges (now);
    CHECK (count (later, 30 * day, 1000 * day) == 1);
    CHECK (std::count (later.begin(), later.end(), now - lastSaved) == 1);
}

TEST_CASE ("An autosave survives a crash and can be restored")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Crash");
    const auto file = project->getFile();
    const auto autosaveFile = phazite::Project::getAutosaveFile (file);

    project->getSong().getState().setProperty ("take", 1, nullptr);
    REQUIRE (project->save().wasOk());

    project->getSong().getState().setProperty ("take", 2, nullptr);
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
    CHECK ((int) openProject (file)->getSong().getState()["take"] == 1);
    REQUIRE (autosaveFile.replaceWithData (leftOverAutosave.getData(), leftOverAutosave.getSize()));

    // Restoring brings the autosaved change back, as an unsaved change to the same project file.
    auto restored = openProject (file, true);
    CHECK ((int) restored->getSong().getState()["take"] == 2);
    CHECK (restored->hasUnsavedChanges());
    CHECK (restored->getFile() == file);

    REQUIRE (restored->save().wasOk());
    CHECK_FALSE (phazite::Project::hasNewerAutosave (file));
    restored = nullptr;

    CHECK ((int) openProject (file)->getSong().getState()["take"] == 2);
}

TEST_CASE ("A restored autosave, or a backup opened in the project's place, is unsaved as closing checks it, until it is saved")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Restored");
    const auto file = project->getFile();
    const auto autosaveFile = phazite::Project::getAutosaveFile (file);

    // An autosave as a crash leaves it.
    project->getSong().getState().setProperty ("take", 2, nullptr);
    REQUIRE (project->autosave().wasOk());
    juce::MemoryBlock leftOverAutosave;
    REQUIRE (autosaveFile.loadFileAsData (leftOverAutosave));
    project = nullptr;
    REQUIRE (autosaveFile.replaceWithData (leftOverAutosave.getData(), leftOverAutosave.getSize()));

    // Restored and nothing changed since: closing compares the whole song with what is saved (checkUnsavedChanges()), and
    // the project file doesn't hold what was restored.
    auto restored = openProject (file, true);
    CHECK (restored->checkUnsavedChanges());
    CHECK (restored->hasUnsavedChanges());
    CHECK (restored->checkUnsavedChanges()); // asked again, the same
    CHECK (autosaveFile.existsAsFile());

    REQUIRE (restored->save().wasOk());
    CHECK_FALSE (restored->checkUnsavedChanges());
    const auto saved = phazite::test::copyWholeSong (restored->getSong());
    restored = nullptr;

    // A song from elsewhere opened as the project's, whose file doesn't read: the same.
    REQUIRE (file.replaceWithText ("<PHAZITE damaged"));
    juce::String error;
    auto opened = phazite::Project::openWithSong (file, saved, error);
    REQUIRE (opened != nullptr);
    CHECK (opened->checkUnsavedChanges());
    CHECK (opened->hasUnsavedChanges());
    REQUIRE (opened->save().wasOk());
    CHECK_FALSE (opened->checkUnsavedChanges());
    opened = nullptr;
    CHECK ((int) openProject (file)->getSong().getState()["take"] == 2);
}

TEST_CASE ("A save that can't replace the project file fails, and leaves the changes unsaved and the autosave in place")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Unwritable");
    const auto file = project->getFile();
    const auto autosaveFile = phazite::Project::getAutosaveFile (file);

    project->getSong().getState().setProperty ("take", 2, nullptr);
    REQUIRE (project->autosave().wasOk());

    // A folder where the project file goes, which a file can't take the place of.
    REQUIRE (file.deleteFile());
    REQUIRE (file.createDirectory().wasOk());
    REQUIRE (file.getChildFile ("In the way").replaceWithText ("in the way"));

    const auto result = project->save();
    CHECK (result.failed());
    CHECK (result.getErrorMessage().contains (file.getFullPathName()));
    CHECK (project->hasUnsavedChanges());
    CHECK (project->checkUnsavedChanges());
    CHECK (autosaveFile.existsAsFile());
    CHECK (file.isDirectory());
    CHECK (file.getParentDirectory().findChildFiles (juce::File::findFiles, false, "*_temp*").isEmpty()); // nothing left beside it

    // Once it can be written, the save works.
    REQUIRE (file.deleteRecursively());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());
    CHECK_FALSE (autosaveFile.exists());
    project = nullptr;
    CHECK ((int) openProject (file)->getSong().getState()["take"] == 2);
}

TEST_CASE ("A change is autosaved in the background soon after it is made, and the project says when, until it is saved")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Timed");
    const auto autosaveFile = phazite::Project::getAutosaveFile (project->getFile());
    phazite::AutosaveTimer timer (*project, 1, 1); // as the app's, but a second after the first change and every second
    CHECK_FALSE (project->getLastAutosave().has_value());

    const auto changed = juce::Time::getCurrentTime();
    project->getSong().getState().setProperty ("take", 1, nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return project->getLastAutosave().has_value(); }));
    CHECK (*project->getLastAutosave() >= changed);
    CHECK (autosaveFile.existsAsFile());
    CHECK (phazite::Project::hasNewerAutosave (project->getFile()));
    CHECK (project->hasUnsavedChanges()); // the project's file is as it was

    // Saving deletes the autosave, and the window says nothing more of it.
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->getLastAutosave().has_value());
    CHECK_FALSE (autosaveFile.exists());
}

TEST_CASE ("Autosave writes nothing when nothing changed")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Unchanged");

    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (project->getFile()).exists());
}

TEST_CASE ("Autosave comes soon after the first change, then waits its interval")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Timed");
    const auto autosaveFile = phazite::Project::getAutosaveFile (project->getFile());

    // As the app has it, but the first autosave a second after the first change rather than 15 s; by a clock that moves
    // only as the test says, and ticked by the test as it moves it.
    phazite::test::HeldClock clock;
    phazite::AutosaveTimer timer (*project, 120, 1);
    clock.advance (1500.0);
    timer.tickNow();
    CHECK_FALSE (project->isAutosaving()); // nothing changed

    // A second after the first change, and not before.
    project->getSong().getState().setProperty (phazite::schema::outline, "pop", nullptr);
    timer.tickNow(); // which sees the change
    clock.advance (999.0);
    timer.tickNow();
    CHECK_FALSE (project->isAutosaving());
    clock.advance (2.0);
    timer.tickNow();
    CHECK (phazite::test::runTimersUntil ([&] { return autosaveFile.existsAsFile() && ! project->isAutosaving(); }));

    // The next change waits for the interval, and not longer.
    const auto first = autosaveFile.loadFileAsString();
    timer.tickNow(); // which sees the autosave done
    project->getSong().getState().setProperty (phazite::schema::outline, "rock", nullptr);
    timer.tickNow();
    clock.advance (119999.0);
    timer.tickNow();
    CHECK_FALSE (project->isAutosaving());
    CHECK (autosaveFile.loadFileAsString() == first);
    clock.advance (2.0);
    timer.tickNow();
    CHECK (phazite::test::runTimersUntil ([&] { return autosaveFile.loadFileAsString() != first && ! project->isAutosaving(); }));
}

TEST_CASE ("Auto Save saves the project file a moment after the last change, in the background, and changes since wait for the next")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Auto");
    const auto file = project->getFile();
    phazite::AutosaveTimer timer (*project, 120, 1);
    timer.setAutoSave (true);

    // A change: saved into the project file itself, not into an autosave, a moment after it was made.
    project->getSong().getState().setProperty (phazite::schema::outline, "pop", nullptr);
    CHECK (project->hasUnsavedChanges());
    REQUIRE (phazite::test::runTimersUntil ([&project] { return ! project->hasUnsavedChanges(); }));
    CHECK (openProject (file)->getSong().getState()[phazite::schema::outline].toString() == "pop");
    CHECK_FALSE (phazite::Project::getAutosaveFile (file).exists());

    // Changes one after another: saved once they stop, the last of them too.
    for (const auto* outline : { "rock", "jazz", "house" })
        project->getSong().getState().setProperty (phazite::schema::outline, outline, nullptr);

    REQUIRE (phazite::test::runTimersUntil ([&project] { return ! project->hasUnsavedChanges(); }));
    CHECK (openProject (file)->getSong().getState()[phazite::schema::outline].toString() == "house");

    // A change made while a save is being written stays unsaved until the next.
    project->getSong().getState().setProperty (phazite::schema::outline, "funk", nullptr);
    project->saveInBackground();
    project->getSong().getState().setProperty (phazite::schema::outline, "soul", nullptr);
    REQUIRE (phazite::test::runTimersUntil ([&project] { return ! project->isAutosaving(); }));
    CHECK (project->finishBackgroundSave());
    CHECK (project->hasUnsavedChanges());
    REQUIRE (phazite::test::runTimersUntil ([&project] { return ! project->hasUnsavedChanges(); }));
    CHECK (openProject (file)->getSong().getState()[phazite::schema::outline].toString() == "soul");

    // Off, it autosaves for a crash again, the project file as it was: its first a second after the change, not after its
    // interval (by a clock that moves only as the test says).
    phazite::test::HeldClock clock;
    timer.setAutoSave (false);
    project->getSong().getState().setProperty (phazite::schema::outline, "disco", nullptr);
    timer.tickNow(); // which sees the change
    clock.advance (1001.0);
    timer.tickNow();
    CHECK (phazite::test::runTimersUntil ([&file] { return phazite::Project::getAutosaveFile (file).existsAsFile(); }));
    CHECK (project->hasUnsavedChanges());
}

TEST_CASE ("Save As makes a copy with the unsaved changes, and leaves the original file alone")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Original");
    REQUIRE (project->getAudioFolder().getChildFile ("kick.wav").replaceWithText ("audio, as far as this test cares"));
    project->getSong().getState().setProperty ("outline", "Pop", nullptr); // not saved yet

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
    CHECK (copy->getSong().getState()["outline"].toString() == juce::String ("Pop"));
    CHECK (openProject (project->getFile())->getSong().getState()["outline"].toString().isEmpty());

    // A name that is taken is refused.
    error.clear();
    CHECK (project->saveCopyAs (temp.folder, "Copy", error) == juce::File());
    CHECK (error.isNotEmpty());
}

TEST_CASE ("A project's first save moves it to a folder of its own, where it goes on with its audio and its undo history")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder.getChildFile ("Unsaved"), "Untitled");
    const auto oldFile = project->getFile();
    auto& song = project->getSong();
    const auto track = phazite::ensureTracks (song, 1).front();

    // Audio imported into it (the project's own copy, by its path relative to the project), unsaved and autosaved.
    const auto kick = phazite::test::writeSound (temp.folder.getChildFile ("Samples").getChildFile ("Kick.wav"), 0.5);
    const auto clip = phazite::importFile (song, track, kick, beats (0.0));
    REQUIRE (phazite::isAudioClip (song, clip));
    REQUIRE (phazite::getClipFile (song, clip).isAChildOf (project->getAudioFolder()));
    REQUIRE (song.getUndoManager().canUndo());
    REQUIRE (project->autosave().wasOk());
    REQUIRE (phazite::Project::getAutosaveFile (oldFile).existsAsFile());

    // A folder of that name is there already: nothing changes.
    const auto projects = temp.folder.getChildFile ("Projects");
    REQUIRE (projects.getChildFile ("Taken").createDirectory().wasOk());
    std::optional<juce::Result> moved;
    const auto keep = [&moved] (const juce::Result& result) { moved = result; };
    CHECK (project->moveTo (projects, "Taken", keep) == nullptr); // it can't begin: told at once
    REQUIRE (moved.has_value());
    CHECK (moved->failed());
    CHECK (project->getFile() == oldFile);
    CHECK (project->hasUnsavedChanges());

    // Copied in the background, however small, the project staying where it was until the copy is whole.
    moved.reset();
    const auto copying = project->moveTo (projects, "Beat", keep);
    REQUIRE (copying != nullptr);
    CHECK (project->getFile() == oldFile);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&moved] { return moved.has_value(); }));
    REQUIRE (moved->wasOk());
    CHECK (projects.findChildFiles (juce::File::findDirectories, false).size() == 2); // Taken and Beat, nothing half made
    const auto newFile = projects.getChildFile ("Beat").getChildFile ("Beat.phazite");
    CHECK (project->getFile() == newFile);
    CHECK (newFile.existsAsFile());
    CHECK_FALSE (project->hasUnsavedChanges());
    CHECK (song.getUndoManager().canUndo());

    // The clip plays the new folder's copy of its audio. The old folder stays, for its owner to delete once the project
    // has closed, without the autosave, whose changes are saved now.
    const auto clipFile = phazite::getClipFile (song, clip);
    CHECK (clipFile == newFile.getSiblingFile ("Audio").getChildFile ("Kick.wav"));
    CHECK (clipFile.existsAsFile());
    CHECK (oldFile.existsAsFile());
    CHECK_FALSE (phazite::Project::getAutosaveFile (oldFile).exists());

    // It saves there from now on, and opens from there with its audio.
    song.getState().setProperty (phazite::schema::outline, "pop", nullptr);
    REQUIRE (project->save().wasOk());
    project = nullptr;
    auto reopened = openProject (newFile);
    const auto& reopenedSong = reopened->getSong();
    CHECK (reopenedSong.getState()[phazite::schema::outline].toString() == juce::String ("pop"));
    const auto clips = phazite::getClips (reopenedSong, getFirstTrack (reopenedSong));
    REQUIRE (clips.size() == 1);
    REQUIRE (phazite::isAudioClip (reopenedSong, clips.front()));
    CHECK (phazite::getClipFile (reopenedSong, clips.front()) == newFile.getSiblingFile ("Audio").getChildFile ("Kick.wav"));
}

TEST_CASE ("A plan after a first save plays the audio of the project's new folder, its file asked about again")
{
    // Known bug 124's review: a player keeps each track's audio from plan to plan; its files are found by paths within the
    // project's folder, so a first save or Save As, which moves the project, makes every track's audio placed again.
    TemporaryFolder temp;
    auto project = createProject (temp.folder.getChildFile ("Unsaved"), "Untitled");
    auto& song = project->getSong();
    const auto track = phazite::ensureTracks (song, 1).front();
    const auto kick = phazite::test::writeSound (temp.folder.getChildFile ("Samples").getChildFile ("Kick.wav"), 0.5);

    for (const auto beat : { 0.0, 4.0, 8.0 })
        REQUIRE (phazite::importFile (song, track, kick, beats (beat)).isValid());

    phazite::SongPlayer kept (song);
    const auto audioOf = [track] (const phazite::engine::Plan& plan)
    {
        for (const auto& strip : plan.strips)
            if (strip.id == (std::int64_t) track.getRaw())
                return strip.audio;

        return std::vector<phazite::engine::AudioRegion>();
    };

    const auto before = kept.compile (44100.0, 512, std::nullopt);
    REQUIRE (audioOf (*before).size() == 3);

    std::optional<juce::Result> moved;
    const auto copying = project->moveTo (temp.folder.getChildFile ("Projects"), "Beat", [&moved] (const juce::Result& r) { moved = r; });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&moved] { return moved.has_value(); }));
    REQUIRE (moved->wasOk());

    const auto asked = phazite::SongPlayer::countFilesAsked();
    const auto after = kept.compile (44100.0, 512, std::nullopt);
    CHECK (phazite::SongPlayer::countFilesAsked() - asked == 1); // the new folder's copy, once for its three clips

    // What plays is the copy, opened anew, as a whole compile plays it.
    const auto audio = audioOf (*after);
    REQUIRE (audio.size() == 3);
    CHECK (audio[0].file != audioOf (*before)[0].file);
    const auto differences =
        phazite::test::describePlanDifferences (*after, *phazite::SongPlayer (song).compile (44100.0, 512, std::nullopt));
    CHECK_MESSAGE (differences.isEmpty(), differences.joinIntoString ("\n").toStdString());
}

TEST_CASE ("A project opens with the playhead at the song's start, where playback starts")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Start");
    phazite::movePlayhead (project->getSong(), beats (12.0));
    CHECK_FALSE (project->hasUnsavedChanges()); // where playback starts changes nothing in the song

    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    auto reopened = openProject (file);
    CHECK (phazite::getPlayheadBeat (reopened->getSong()).inBeats() == doctest::Approx (0.0));
    CHECK (phazite::getPlayStartBeat (reopened->getSong()).inBeats() == doctest::Approx (0.0));
}

TEST_CASE ("A new project opens from its first save with what it starts with, without undo steps or unsaved changes")
{
    TemporaryFolder temp;
    juce::String error;
    std::unique_ptr<phazite::Project> project;

    SUBCASE ("The Exodus example song, as Help > Example Songs makes it")
    {
        project = phazite::Project::create (
            temp.folder, "Exodus", error,
            phazite::getExampleSongContent (phazite::test::getEngineContext(), "exodus", phazite::test::getFactoryPresetsFolder()));
        INFO (error.toStdString());
        REQUIRE (project != nullptr);
        const auto& song = project->getSong();
        std::map<juce::String, phazite::TrackID> tracks;

        for (const auto id : phazite::getTrackIDs (song))
            tracks[phazite::getTrackName (song, id)] = id;

        // Its tracks with clips, each played by one of Phazite's own instruments: each drum on the drum sampler, the bass's
        // two parts and the sparkle over them, and the arpeggio's three layers.
        for (const auto& [name, instrumentType] :
             { std::pair ("Kick", phazite::devices::DrumSampler::typeID), std::pair ("Snare", phazite::devices::DrumSampler::typeID),
               std::pair ("Clap", phazite::devices::DrumSampler::typeID), std::pair ("Open Hat", phazite::devices::DrumSampler::typeID),
               std::pair ("Perc", phazite::devices::DrumSampler::typeID), std::pair ("Rolling Bass", phazite::devices::FmSynth::typeID),
               std::pair ("Sub", phazite::devices::Synth::typeID), std::pair ("Sparkle", phazite::devices::FmSynth::typeID),
               std::pair ("Square", phazite::devices::Synth::typeID), std::pair ("Wide Saw", phazite::devices::Sampler::typeID),
               std::pair ("Supersaw", phazite::devices::Synth::typeID) })
        {
            CAPTURE (name);
            REQUIRE (tracks.contains (name));
            CHECK_FALSE (phazite::getClips (song, tracks[name]).empty());
            const auto instrument = phazite::findDeviceState (song, phazite::getInstrument (song, tracks[name]));
            CHECK (instrument[phazite::schema::deviceType].toString() == juce::String (instrumentType));
        }
    }

    SUBCASE ("An outline's sections, as New Project lays them out")
    {
        project = phazite::Project::create (temp.folder, "Outlined", error,
                                            [] (phazite::Song& song) { phazite::applyOutline (song, *phazite::findOutline ("pop")); });
        INFO (error.toStdString());
        REQUIRE (project != nullptr);
        CHECK (phazite::getAppliedOutline (project->getSong()) == juce::String ("pop"));
        CHECK_FALSE (phazite::getSections (project->getSong()).empty());
    }

    // It is opened from its save: nothing of what was added is an undo step or, once the message loop has run, an unsaved
    // change.
    CHECK (project->getFile().existsAsFile());
    CHECK_FALSE (project->getSong().getUndoManager().canUndo());
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (project->hasUnsavedChanges());
    CHECK_FALSE (project->getSong().getUndoManager().canUndo());
}

TEST_CASE ("A project with MIDI clips and an alias opens without unsaved changes")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Notes");
    auto& song = project->getSong();
    const auto trackID = getFirstTrack (song);
    const auto clip = phazite::insertMidiClip (song, trackID, { beats (0.0), beats (4.0) }, nullptr);
    REQUIRE (clip.isValid());
    REQUIRE (phazite::addNote (song, clip, 60, beats (0.0), phazite::BeatDuration::fromBeats (1.0), 100).isValid());
    REQUIRE (phazite::addNote (song, clip, 64, beats (2.0), phazite::BeatDuration::fromBeats (1.0), 100).isValid());
    const auto aliases = phazite::copyClips (song, { clip }, true);
    REQUIRE (aliases.size() == 1);
    phazite::moveClip (song, aliases[0], beats (4.0), trackID);

    // As the app's message loop runs between edits.
    phazite::test::dispatchPendingMessages();
    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    // Once the message loop has run after it opened, nothing has changed.
    project = openProject (file);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (file).exists());
}

TEST_CASE ("A project forgets, as it opens, the automation of devices it hasn't any more, and keeps the rest")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Automated");
    auto& song = project->getSong();
    const auto track = getFirstTrack (song);
    const auto eq = phazite::insertDeviceState (song, track, phazite::makeOwnDeviceState (song, phazite::devices::Equalizer::typeID));
    REQUIRE (eq.isValid());
    const phazite::ParameterRef gainRef { eq, "output" };
    const phazite::ParameterRef volumeRef { phazite::getFader (song, track), phazite::getVolumeParameterID (song, track) };
    phazite::addPoint (song, gainRef, { beats (2.0), phazite::getValueSetByHand (song, gainRef), 0.0f });
    phazite::addPoint (song, volumeRef, { beats (2.0), 0.5f, 0.0f });

    // A device removed leaves its curve in the song, for an undo to bring back with it; the file keeps it too.
    phazite::removeDevice (song, eq);
    CHECK (phazite::getCurveValue (song, gainRef).has_value());
    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    // Opened, nothing can bring the device back: its curve goes, without an unsaved change, and the volume's stays.
    project = openProject (file);
    const auto& reopened = project->getSong();
    CHECK_FALSE (phazite::getCurveValue (reopened, gainRef).has_value());
    REQUIRE (phazite::getCurveValue (reopened, volumeRef).has_value());
    CHECK (phazite::getPoints (reopened, volumeRef).size() == 1);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (project->hasUnsavedChanges());
}

TEST_CASE ("Only a change to what the project holds is an unsaved change, made with an undo step or not")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Changes");
    phazite::TrackID trackID;
    phazite::ClipID clipID; // the original, at beat 0

    {
        auto& song = project->getSong();
        trackID = getFirstTrack (song);
        clipID = phazite::insertMidiClip (song, trackID, { beats (0.0), beats (4.0) }, nullptr);
        REQUIRE (clipID.isValid());
        REQUIRE (phazite::addNote (song, clipID, 60, beats (0.0), phazite::BeatDuration::fromBeats (1.0), 100).isValid());
        REQUIRE (phazite::addNote (song, clipID, 64, beats (2.0), phazite::BeatDuration::fromBeats (1.0), 100).isValid());
        const auto aliases = phazite::copyClips (song, { clipID }, true);
        REQUIRE (aliases.size() == 1);
        phazite::moveClip (song, aliases[0], beats (4.0), trackID);
        phazite::setTrackKey (song, trackID, phazite::Key { 2, phazite::Scale::minor });
        phazite::test::dispatchPendingMessages();
        REQUIRE (project->save().wasOk());
    }

    const auto file = project->getFile();
    project = nullptr;
    project = openProject (file);
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (project->hasUnsavedChanges());

    auto& song = project->getSong();
    auto trackState = phazite::findTrackState (song, trackID);
    const auto clipValue = phazite::getClipValue (song, clipID);
    REQUIRE (trackState.isValid());
    REQUIRE (clipValue.has_value());

    // Notifications that change nothing; and a clip set to the value it holds, which tells nobody (its area's value is the
    // same).
    struct Told final : phazite::SongValues::Listener
    {
        int calls = 0;
        void clipsChanged (const std::vector<phazite::SongValues::ClipChange>&, const std::vector<phazite::TrackID>&) override { ++calls; }
    } told;

    song.getValues()->addListener (&told);
    phazite::setClip (song, *clipValue);
    song.getValues()->removeListener (&told);
    CHECK (told.calls == 0);
    trackState.sendPropertyChangeMessage (phazite::schema::trackName);
    CHECK_FALSE (project->hasUnsavedChanges());

    // The same value written as another type (the file holds text).
    REQUIRE (trackState[phazite::schema::keyRoot].isString());
    trackState.setProperty (phazite::schema::keyRoot, 2, nullptr);
    CHECK_FALSE (project->hasUnsavedChanges());

    // A note taken out and put back as it was (by Undo).
    const auto firstNote = phazite::getClipNotes (song, clipID).front().getRef();

    {
        const phazite::UndoStep step (song, "Delete Note");
        phazite::deleteNotes (song, { firstNote });
    }

    phazite::undo (song);
    CHECK_FALSE (project->hasUnsavedChanges());

    // Where the playhead is.
    phazite::movePlayhead (song, beats (4.0));
    trackState.sendPropertyChangeMessage (phazite::schema::trackName);
    CHECK_FALSE (project->hasUnsavedChanges());

    // A change taken back before anything asked.
    trackState.setProperty (phazite::schema::keyRoot, 5, nullptr);
    trackState.setProperty (phazite::schema::keyRoot, 2, nullptr);
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (file).exists());

    // Each property compared at its place in the saved copy, among the elements that count: a lane shown first in the
    // track (view state) moves nothing there.
    phazite::showLane (song, trackID, { phazite::getFader (song, trackID), phazite::getVolumeParameterID (song, trackID) });
    auto lanes = trackState.getChildWithName (phazite::schema::LANES);
    REQUIRE (lanes.isValid());
    trackState.moveChild (trackState.indexOf (lanes), 0, nullptr);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (project->hasUnsavedChanges());

    {
        // The notes changed as the model changes them, outside the undo history, as a value taken back is.
        const phazite::Song::OutsideHistory outside (song);
        const auto velocity = phazite::getNoteData (song, firstNote)->velocity;
        phazite::setNoteVelocities (song, { firstNote }, 7);
        phazite::setNoteVelocities (song, { firstNote }, velocity);
        CHECK_FALSE (project->hasUnsavedChanges());
        phazite::setNoteVelocities (song, { firstNote }, 9);
        CHECK (project->hasUnsavedChanges());
        REQUIRE (project->save().wasOk());

        // Many notifications at once, compared by their track whole: taken back, nothing; one left, a change.
        for (int i = 0; i < 100; ++i)
            REQUIRE (phazite::test::addMidiNote (song, clipID, 40 + i % 40, beats (i * 0.03), phazite::BeatDuration::fromBeats (0.02), 100)
                         .isValid());

        phazite::test::dispatchPendingMessages();
        REQUIRE (project->save().wasOk());

        // Each note given its velocity again, from one on.
        const auto putBack = [&song] (const std::vector<phazite::NoteData>& notes, size_t from)
        {
            for (auto i = from; i < notes.size(); ++i)
                phazite::setNoteVelocities (song, { notes[i].getRef() }, notes[i].velocity);
        };

        const auto notes = phazite::getClipNotes (song, clipID);
        REQUIRE (notes.size() > phazite::Project::mostToCompare);
        phazite::setNoteVelocities (song, getRefs (notes), 1);
        putBack (notes, 0);
        CHECK_FALSE (project->hasUnsavedChanges());

        // All but one put back: that one is the only difference.
        phazite::setNoteVelocities (song, getRefs (notes), 1);
        putBack (notes, 1);
        CHECK (project->hasUnsavedChanges());
        putBack ({ notes[0] }, 0);
        REQUIRE (project->save().wasOk());

        // One that notified, then taken out of the song before many more did, the others put back: the note gone is the only
        // difference, which the clip it was in, compared whole, finds.
        const auto setAllAndBack = [&song, &putBack, clipID]
        {
            const auto before = phazite::getClipNotes (song, clipID);
            phazite::setNoteVelocities (song, getRefs (before), 4);
            putBack (before, 0);
        };

        auto gone = phazite::getClipNotes (song, clipID).front().getRef();
        phazite::setNoteVelocities (song, { gone }, 2);
        phazite::deleteNotes (song, { gone });
        setAllAndBack();
        CHECK_FALSE (project->isWholeAsSaved());
        CHECK (project->hasUnsavedChanges());

        // The reverse: the note put back where it was, as it was (each clip of its group made like itself as it was saved),
        // among as many notifications: nothing to save.
        REQUIRE (project->save().wasOk());
        std::vector<std::pair<phazite::ClipID, juce::ValueTree>> saved;

        for (const auto clip : phazite::getClips (song, trackID))
            saved.emplace_back (clip, phazite::copyWholeClip (song, clip));

        gone = phazite::getClipNotes (song, clipID).front().getRef();
        phazite::setNoteVelocities (song, { gone }, 2);
        phazite::deleteNotes (song, { gone });
        setAllAndBack();

        for (const auto& [clip, asSaved] : saved)
            phazite::makeClipNotesLike (song, clip, asSaved);

        CHECK_FALSE (project->hasUnsavedChanges());
        CHECK (project->isWholeAsSaved());
    }

    // Real changes count, made without an undo step or with one; saving clears them.
    song.getState().setProperty (phazite::schema::outline, "pop", nullptr);
    CHECK (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK (phazite::Project::getAutosaveFile (file).existsAsFile());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    REQUIRE (phazite::test::addMidiNote (song, clipID, 67, beats (3.0), phazite::BeatDuration::fromBeats (0.5), 100).isValid());
    CHECK (project->hasUnsavedChanges());
    phazite::test::dispatchPendingMessages();
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    // With an undo step, as the editors make changes.
    REQUIRE (phazite::addNote (song, clipID, 69, beats (3.5), phazite::BeatDuration::fromBeats (0.25), 100).isValid());
    CHECK (project->hasUnsavedChanges());
    phazite::test::dispatchPendingMessages();
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    // A change made outside the song's history, such as a plug-in's own settings changed in its window, counts, and
    // autosave writes it.
    song.markChangedOutsideHistory();
    CHECK (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK (phazite::Project::getAutosaveFile (file).existsAsFile());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    trackState.setProperty (phazite::schema::keyRoot, 3, nullptr);
    CHECK (project->hasUnsavedChanges());
}

TEST_CASE ("A clip changed, added, moved to another track or deleted through the model is an unsaved change, which its Undo "
           "takes back")
{
    // The clips are heard from the song's values (SongValues.h: Listener::clipsChanged()), compared with the saved copy clip by
    // clip and by each track's clips, as the whole song's comparison says. A project found unsaved stays so until it is
    // saved: so each change is undone before anything asks (as saved), then redone (unsaved), undone and saved again.
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Clips");
    auto& song = project->getSong();
    const auto tracks = phazite::ensureTracks (song, 2);
    const auto clip = phazite::insertMidiClip (song, tracks[0], { beats (0.0), beats (4.0) }, nullptr);
    REQUIRE (clip.isValid());
    phazite::test::dispatchPendingMessages();
    REQUIRE (project->save().wasOk());
    REQUIRE_FALSE (project->hasUnsavedChanges());

    const auto check = [&] (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());

        {
            const phazite::UndoStep step (song, name);
            change();
        }

        CHECK_FALSE (project->isWholeAsSaved());
        phazite::undo (song);
        CHECK (project->isWholeAsSaved());
        CHECK_FALSE (project->hasUnsavedChanges());
        phazite::redo (song);
        CHECK (project->hasUnsavedChanges());
        phazite::undo (song);
        REQUIRE (project->save().wasOk());
        REQUIRE_FALSE (project->hasUnsavedChanges());
    };

    check ("Mute Clip", [&] { phazite::toggleClipsMuted (song, { clip }); });
    check ("Move Clip", [&] { phazite::moveClip (song, clip, beats (8.0), tracks[0]); });
    check ("Move Clip", [&] { phazite::moveClip (song, clip, beats (0.0), tracks[1]); });
    check ("Add Clip", [&] { REQUIRE (phazite::insertMidiClip (song, tracks[1], { beats (0.0), beats (4.0) }, nullptr).isValid()); });
    check ("Delete Clip", [&] { phazite::deleteClips (song, { clip }, false); });

    // Changed and changed back before anything asked: as saved.
    phazite::toggleClipsMuted (song, { clip });
    phazite::toggleClipsMuted (song, { clip });
    CHECK_FALSE (project->hasUnsavedChanges());

    // More clips changed than are compared one by one (their tracks compared whole then): changed back, as saved; all but
    // one changed back, unsaved.
    std::vector<phazite::ClipID> many;

    for (size_t i = 0; i <= phazite::Project::mostToCompare; ++i)
        many.push_back (phazite::insertMidiClip (song, tracks[1], { beats (8.0 + 4.0 * (double) i), beats (4.0) }, nullptr));

    phazite::test::dispatchPendingMessages();
    REQUIRE (project->save().wasOk());
    phazite::toggleClipsMuted (song, many);
    phazite::toggleClipsMuted (song, many);
    CHECK (project->isWholeAsSaved());
    CHECK_FALSE (project->hasUnsavedChanges());

    phazite::toggleClipsMuted (song, many);
    phazite::toggleClipsMuted (song, std::vector<phazite::ClipID> (many.begin() + 1, many.end()));
    CHECK_FALSE (project->isWholeAsSaved());
    CHECK (project->hasUnsavedChanges());
}

TEST_CASE ("A tempo map edited through the model is an unsaved change, which its Undo takes back, and one changed back is saved "
           "again")
{
    // The tempo map is heard from the song's values (SongValues.h: Listener::tempoMapChanged()), compared with the saved
    // copy's as one value (WholeState.h: holdSameTempoMap()), as the whole song's comparison says. A project found unsaved
    // stays so until it is saved: so each change is undone before anything asks (as saved), then redone (unsaved), undone and
    // saved again.
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Tempo");
    auto& song = project->getSong();
    phazite::addTempoChange (song, beats (8.0), 140.0);
    phazite::test::dispatchPendingMessages();
    REQUIRE (project->save().wasOk());
    REQUIRE_FALSE (project->hasUnsavedChanges());

    const auto check = [&] (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());

        {
            const phazite::UndoStep step (song, name);
            change();
        }

        CHECK_FALSE (project->isWholeAsSaved());
        phazite::undo (song);
        CHECK (project->isWholeAsSaved());
        CHECK_FALSE (project->hasUnsavedChanges());
        phazite::redo (song);
        CHECK (project->hasUnsavedChanges());
        phazite::undo (song);
        REQUIRE (project->save().wasOk());
        REQUIRE_FALSE (project->hasUnsavedChanges());
    };

    check ("Change Tempo", [&] { phazite::setTempo (song, 100.0); });
    check ("Change Time Signature", [&] { phazite::setTimeSignature (song, { 6, 8 }); });
    check ("Add Tempo Change", [&] { phazite::addTempoChange (song, beats (16.0), 90.0); });
    check ("Move Tempo Change", [&] { phazite::moveTempoChange (song, 1, beats (12.0), 150.0); });
    check ("Change Tempo Ramp", [&] { phazite::setTempoCurve (song, 0, phazite::tempoRamp); });
    check ("Delete Tempo Change", [&] { phazite::deleteTempoChange (song, 1); });

    // Changed and changed back before anything asked: as saved. A tempo's, a time signature's, a ramp's; and a tempo change
    // deleted and added again where it was, at its tempo, which the map holds as it did.
    const auto changedBack = [&] (const juce::String& name, const std::function<void()>& change, const std::function<void()>& back)
    {
        INFO (name.toStdString());
        change();
        back();
        CHECK (project->isWholeAsSaved());
        CHECK_FALSE (project->hasUnsavedChanges());
    };

    changedBack ("the tempo", [&] { phazite::setTempo (song, 100.0); }, [&] { phazite::setTempo (song, 120.0); });
    changedBack (
        "the time signature", [&] { phazite::setTimeSignature (song, { 6, 8 }); }, [&] { phazite::setTimeSignature (song, { 4, 4 }); });
    changedBack (
        "the ramp", [&] { phazite::setTempoCurve (song, 0, phazite::tempoRamp); },
        [&] { phazite::setTempoCurve (song, 0, phazite::tempoJump); });
    changedBack (
        "a tempo change deleted and added again", [&] { phazite::deleteTempoChange (song, 1); },
        [&] { phazite::addTempoChange (song, beats (8.0), 140.0); });

    // The same with a tempo change after it, which its element then comes after: the map holds what it did. With two
    // tempo changes and with three, the one at beat 8 the second of the song's tempos each time.
    for (const auto after : { 1, 2 })
    {
        INFO (after << " after it");

        for (int i = 1; i <= after; ++i)
            phazite::addTempoChange (song, beats (8.0 + 8.0 * i), 90.0 + i);

        phazite::test::dispatchPendingMessages();
        REQUIRE (project->save().wasOk());
        REQUIRE_FALSE (project->hasUnsavedChanges());

        changedBack (
            "a tempo change deleted and added again before others", [&] { phazite::deleteTempoChange (song, 1); },
            [&] { phazite::addTempoChange (song, beats (8.0), 140.0); });
    }

    // Changed and not back: unsaved, as the whole song's comparison says.
    phazite::setTempo (song, 133.0);
    CHECK_FALSE (project->isWholeAsSaved());
    CHECK (project->hasUnsavedChanges());
}

TEST_CASE ("A chord lane edited through the model is an unsaved change, which its Undo takes back, and one changed back is saved "
           "again")
{
    // The chord lane is heard from the song's values (SongValues.h: Listener::chordLaneChanged()), compared with the saved
    // copy's as one value with whether each has a lane (WholeState.h: holdSameChordLane()), as the whole song's comparison
    // says: first with no lane (the first chord makes it, and its Undo takes it away), then with one.
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Chords");
    auto& song = project->getSong();
    phazite::test::dispatchPendingMessages();
    REQUIRE (project->save().wasOk());
    REQUIRE_FALSE (project->hasUnsavedChanges());
    REQUIRE_FALSE (phazite::hasChordLane (song));

    const auto edit = [&song] (const juce::String& name, const std::function<void()>& change)
    {
        const phazite::UndoStep step (song, name);
        change();
    };

    const auto check = [&] (const juce::String& name, const std::function<void()>& change)
    {
        INFO (name.toStdString());
        edit (name, change);
        CHECK_FALSE (project->isWholeAsSaved());
        phazite::undo (song);
        CHECK (project->isWholeAsSaved());
        CHECK_FALSE (project->hasUnsavedChanges());
        phazite::redo (song);
        CHECK (project->hasUnsavedChanges());
        phazite::undo (song);
        REQUIRE (project->save().wasOk());
        REQUIRE_FALSE (project->hasUnsavedChanges());
    };

    const phazite::Numeral one { 1, 0, phazite::ChordType::minor, 0 }, four { 4, 0, phazite::ChordType::major, 0 };
    check ("Add Chord, which makes the lane", [&] { phazite::putLaneChord (song, { beats (0.0), beats (4.0) }, one); });
    check ("Change Key, which makes the lane", [&] { phazite::setLaneKey (song, beats (0.0), { 2, phazite::Scale::minor }); });
    CHECK_FALSE (phazite::hasChordLane (song));

    edit ("Add Chord", [&] { phazite::putLaneChord (song, { beats (0.0), beats (4.0) }, one); });
    phazite::test::dispatchPendingMessages();
    REQUIRE (project->save().wasOk());
    REQUIRE_FALSE (project->hasUnsavedChanges());

    check ("Change Chord", [&] { phazite::setLaneChordNumeral (song, beats (0.0), four); });
    check ("Change Chord Length", [&] { phazite::setLaneChordEnd (song, beats (0.0), beats (8.0)); });
    check ("Add Key Change", [&] { phazite::setLaneKey (song, beats (4.0), { 7, phazite::Scale::major }); });
    check ("Delete Chord", [&] { phazite::removeLaneChord (song, beats (0.0)); });
    check ("Insert Time", [&] { phazite::insertLaneTime (song, beats (2.0), phazite::BeatDuration::fromBeats (2.0)); });

    // Changed and changed back before anything asked: as saved. A chord's numeral, and a chord deleted and added again where
    // it was, which the lane holds as it did.
    const auto changedBack = [&] (const juce::String& name, const std::function<void()>& change, const std::function<void()>& back)
    {
        INFO (name.toStdString());
        edit (name, change);
        edit (name, back);
        CHECK (project->isWholeAsSaved());
        CHECK_FALSE (project->hasUnsavedChanges());
    };

    changedBack (
        "the numeral", [&] { phazite::setLaneChordNumeral (song, beats (0.0), four); },
        [&] { phazite::setLaneChordNumeral (song, beats (0.0), one); });
    changedBack (
        "a chord deleted and added again", [&] { phazite::removeLaneChord (song, beats (0.0)); },
        [&] { phazite::putLaneChord (song, { beats (0.0), beats (4.0) }, one); });

    // Changed and not back: unsaved, as the whole song's comparison says.
    edit ("Change Chord", [&] { phazite::setLaneChordNumeral (song, beats (0.0), four); });
    CHECK_FALSE (project->isWholeAsSaved());
    CHECK (project->hasUnsavedChanges());
}

TEST_CASE ("The check for unsaved changes says what the whole song's comparison says: notes swapped, a delete undone, a clip "
           "and a track moved and back, with few notifications or many")
{
    // hasUnsavedChanges() compares only what notified: a value at its place in the saved copy, and an element whose elements
    // were added, removed or moved whole. Each sequence here is compared with the comparison of everything.
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Partial");
    auto& song = project->getSong();
    const auto tracks = phazite::ensureTracks (song, 4);
    const auto clip = phazite::insertMidiClip (song, tracks[0], { beats (0.0), beats (4.0) }, nullptr);
    const auto other = phazite::insertMidiClip (song, tracks[2], { beats (0.0), beats (4.0) }, nullptr);
    REQUIRE (clip.isValid());
    REQUIRE (other.isValid());

    // A track whose clip comes before two sends, which the mixer puts after it: -6 dB, then 0 dB.
    const auto sent = phazite::insertMidiClip (song, tracks[3], { beats (0.0), beats (4.0) }, nullptr);
    const auto firstSend = phazite::addSend (song, tracks[3], phazite::addReturnTrack (song, "A", nullptr), false);
    const auto secondSend = phazite::addSend (song, tracks[3], phazite::addReturnTrack (song, "B", nullptr), false);
    REQUIRE (sent.isValid());
    REQUIRE (firstSend.isValid());
    REQUIRE (secondSend.isValid());
    phazite::setSendLevel (song, firstSend, -6.0f);
    phazite::setSendLevel (song, secondSend, 0.0f);
    REQUIRE (phazite::addNote (song, clip, 60, beats (0.0), phazite::BeatDuration::fromBeats (1.0), 10).isValid());
    REQUIRE (phazite::addNote (song, clip, 64, beats (1.0), phazite::BeatDuration::fromBeats (1.0), 20).isValid());

    for (int i = 0; i < 80; ++i)
        REQUIRE (phazite::addNote (song, other, 40 + i % 40, beats (i * 0.04), phazite::BeatDuration::fromBeats (0.02), 100).isValid());

    const auto othersNotes = getRefs (phazite::getClipNotes (song, other));
    REQUIRE (othersNotes.size() > phazite::Project::mostToCompare);

    // The notes changed as the model changes them, outside the undo history (Song::OutsideHistory), so that the undos below
    // take back what they did before: as a value taken back is.
    const auto clipNotes = [&song, clip] { return getRefs (phazite::getClipNotes (song, clip)); };

    // From the song as saved, its clip's two notes 10 and 20 loud.
    const auto startSaved = [&]
    {
        {
            const phazite::Song::OutsideHistory outside (song);
            const auto notes = clipNotes();
            REQUIRE (notes.size() == 2);
            phazite::setNoteVelocities (song, { notes[0] }, 10);
            phazite::setNoteVelocities (song, { notes[1] }, 20);
        }

        phazite::test::dispatchPendingMessages();
        REQUIRE (project->save().wasOk());
        REQUIRE_FALSE (project->hasUnsavedChanges());
    };

    // Notifications elsewhere that change nothing: none, or more than are compared one by one.
    const auto notifyElsewhere = [&song, &othersNotes] (bool many)
    {
        if (! many)
            return;

        const phazite::Song::OutsideHistory outside (song);
        phazite::setNoteVelocities (song, othersNotes, 1);
        phazite::setNoteVelocities (song, othersNotes, 100);
    };

    const auto answer = [&project]
    {
        const auto whole = project->isWholeAsSaved();
        const auto partial = project->hasUnsavedChanges();
        CHECK (partial == ! whole);
        return partial;
    };

    for (const auto many : { false, true })
    {
        INFO (juce::String (many ? "past " : "under ") << (int) phazite::Project::mostToCompare << " notifications");

        // The two notes swapped, each then given the other's loudness: at each place the value is the saved one, so only
        // the clip, compared whole for what was taken out and put in, tells the change.
        startSaved();

        {
            const phazite::Song::OutsideHistory outside (song);
            const auto notes = phazite::getClipNotes (song, clip);
            phazite::deleteNotes (song, { notes[0].getRef() });
            notifyElsewhere (many);
            REQUIRE (phazite::replaceNotes (song, {}, { notes[0] }) == std::vector { notes[0].getRef() }); // after the other
            phazite::setNoteVelocities (song, { notes[1].getRef() }, 10);
            phazite::setNoteVelocities (song, { notes[0].getRef() }, 20);
        }

        CHECK (answer());

        // A note deleted, the other edited outside the history to look like it, the delete undone: the clip holds its notes
        // as before the step, the outside edit gone with the delete, and the song is as saved. A clip's notes are one value,
        // which an Undo puts back whole, and nothing in Phazite writes notes outside the history where a step of theirs can
        // still be undone.
        startSaved();
        auto notes = phazite::getClipNotes (song, clip);
        REQUIRE (notes.size() == 2);

        {
            const phazite::UndoStep step (song, "Delete Note");
            phazite::deleteNotes (song, { notes[0].getRef() });
        }

        notifyElsewhere (many);

        {
            const phazite::Song::OutsideHistory outside (song);
            phazite::placeNote (song, notes[1].getRef(), notes[0].start, notes[0].pitch, notes[0].length);
            phazite::setNoteVelocities (song, { notes[1].getRef() }, notes[0].velocity);
        }

        phazite::undo (song);

        {
            const auto undone = phazite::getClipNotes (song, clip);
            REQUIRE (undone.size() == notes.size());

            for (std::size_t i = 0; i < notes.size(); ++i)
            {
                CHECK (undone[i].id == notes[i].id);
                CHECK (undone[i].pitch == notes[i].pitch);
                CHECK (undone[i].start == notes[i].start);
                CHECK (undone[i].length == notes[i].length);
                CHECK (undone[i].velocity == notes[i].velocity);
            }
        }

        CHECK_FALSE (answer());

        // A note deleted, undone, redone and undone again: as saved.
        startSaved();
        notes = phazite::getClipNotes (song, clip);

        {
            const phazite::UndoStep step (song, "Delete Note");
            phazite::deleteNotes (song, { notes[1].getRef() });
        }

        phazite::undo (song);
        notifyElsewhere (many);
        phazite::redo (song);
        phazite::undo (song);
        CHECK_FALSE (answer());

        // The clip moved to another track and back.
        startSaved();
        phazite::moveClip (song, clip, beats (0.0), tracks[1]);
        notifyElsewhere (many);
        phazite::moveClip (song, clip, beats (0.0), tracks[0]);
        answer();

        // A clip moved to another track and back, which puts it after the sends of its track, the second send then set to
        // what the first is: at the second's place in the saved copy is the first, the same, but the track holds its clip
        // and its sends in another order. Undone, so that the next round starts with the clip before them.
        startSaved();

        {
            const phazite::UndoStep step (song, "Move and Change");
            phazite::moveClip (song, sent, beats (0.0), tracks[1]);
            notifyElsewhere (many);
            phazite::moveClip (song, sent, beats (0.0), tracks[3]);
            phazite::setSendLevel (song, secondSend, -6.0f);
        }

        CHECK (answer());
        phazite::undo (song);

        // A track moved and moved back, a value in it changed meanwhile.
        startSaved();
        phazite::moveTracksAfter (song, { tracks[0] }, tracks[1]);
        notifyElsewhere (many);

        {
            const phazite::Song::OutsideHistory outside (song);
            phazite::setNoteVelocities (song, { clipNotes().front() }, 77);
        }

        phazite::moveTracksAfter (song, { tracks[1] }, tracks[0]);
        CHECK (answer());

        // Tempo changes at beats 8 and 16 (and 24) saved, the one at 8 deleted and added again where it was, at its tempo,
        // which puts its element after the others: the tempo map holds what it did, which both comparisons compare as one
        // value. Taken away after, so that the next round starts with the song's one tempo.
        for (const auto count : { 2, 3 })
        {
            INFO (count << " tempo changes");

            for (int i = 1; i <= count; ++i)
                phazite::addTempoChange (song, beats (8.0 * i), 130.0 + i);

            REQUIRE (phazite::getTempoChanges (song).size() == (size_t) count + 1);
            startSaved();
            phazite::deleteTempoChange (song, 1);
            notifyElsewhere (many);
            phazite::addTempoChange (song, beats (8.0), 131.0);
            CHECK_FALSE (answer());

            phazite::removeTempoChanges (song);
        }
    }
}

TEST_CASE ("A plug-in's state that Save a Copy As or an autosave stores, which the project file doesn't hold, is an unsaved change")
{
    // A plug-in whose state changed without saying so (no Song::markChangedOutsideHistory()): a store writes its state into
    // the song without a notification. Where the project file doesn't get it (a copy saved elsewhere, an autosave), the
    // project has unsaved changes, so that closing it asks; nothing else that notifies need come first.
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Stored");
    auto& song = project->getSong();
    const auto track = getFirstTrack (song);
    const auto synth = phazite::setInstrument (song, track, phazite::makeOwnDeviceState (song, phazite::devices::Synth::typeID));
    REQUIRE (synth.isValid());
    juce::String pluginState ("A");
    song.setDeviceStateStore ([&song, synth, &pluginState] (std::uint64_t)
                              { phazite::findDeviceState (song, synth).setProperty (phazite::schema::pluginState, pluginState, nullptr); });
    phazite::test::dispatchPendingMessages();
    REQUIRE (project->save().wasOk());
    REQUIRE_FALSE (project->hasUnsavedChanges());

    // Save a Copy As: the copy holds the new state, this project's file the old.
    pluginState = "B";
    juce::String error;
    REQUIRE (project->saveCopyAs (temp.folder, "Copy", error) != juce::File());
    CHECK (project->hasUnsavedChanges());
    CHECK (project->checkUnsavedChanges());

    // The same state stored again: nothing to save.
    REQUIRE (project->save().wasOk());
    REQUIRE (project->saveCopyAs (temp.folder, "Copy 2", error) != juce::File());
    CHECK_FALSE (project->hasUnsavedChanges());

    // An autosave after a save, the one before it having failed (it writes the changes again): the same.
    phazite::setTrackName (song, track, "Changed");

    {
        const phazite::WholeFileHook failing ([] (const juce::File&) { return false; });
        project->autosaveInBackground();
        REQUIRE (phazite::test::runTimersUntil ([&project] { return ! project->isAutosaving(); }));
    }

    REQUIRE (project->save().wasOk());
    REQUIRE (project->takeAutosaveFailure());
    REQUIRE_FALSE (project->hasUnsavedChanges());
    pluginState = "C";
    REQUIRE (project->autosave().wasOk());
    CHECK (project->hasUnsavedChanges());
    CHECK (project->checkUnsavedChanges());

    // A save whose write fails (a full disk, a file another program holds): the song holds the new state, the file not.
    REQUIRE (project->save().wasOk());
    REQUIRE_FALSE (project->hasUnsavedChanges());
    pluginState = "D";

    {
        const phazite::WholeFileHook failing ([] (const juce::File&) { return false; });
        CHECK (project->save().failed());
    }

    CHECK (project->hasUnsavedChanges());
    CHECK (project->checkUnsavedChanges());

    // A save in the background (Auto Save), a copy saved elsewhere while it writes: the background save's copy, the saved
    // one once written, hasn't the state stored after it.
    REQUIRE (project->save().wasOk());
    phazite::setTrackName (song, track, "Changed again");
    REQUIRE (project->hasUnsavedChanges());
    juce::WaitableEvent letGo;
    const auto file = project->getFile();

    {
        // Let go however this scope ends: a failed REQUIRE in it would leave the writer waiting, and the project for it.
        struct LetGoAtEnd
        {
            juce::WaitableEvent& event;
            ~LetGoAtEnd() { event.signal(); }
        } const letGoAtEnd { letGo };

        const phazite::WholeFileHook slow (
            [&letGo, file] (const juce::File& written)
            {
                if (written == file)
                    letGo.wait (-1);

                return true;
            });

        project->saveInBackground();
        REQUIRE (project->hasBackgroundSave());
        pluginState = "E";
        REQUIRE (project->saveCopyAs (temp.folder, "Copy 3", error) != juce::File());
        letGo.signal();
        REQUIRE (phazite::test::runTimersUntil ([&project] { return ! project->isAutosaving(); }));
    }

    REQUIRE (project->finishBackgroundSave());
    CHECK (project->hasUnsavedChanges());
    CHECK (project->checkUnsavedChanges());
}

TEST_CASE ("A write mode, the master's too, is an unsaved change without an undo step, and the master's lanes are view state")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Modes");
    auto& song = project->getSong();
    const auto master = phazite::getMasterID (song);
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (project->hasUnsavedChanges());

    phazite::showLane (song, master, { phazite::getFader (song, master), phazite::getVolumeParameterID (song, master) });
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (project->hasUnsavedChanges());

    phazite::setAutomationMode (song, master, phazite::AutomationMode::latch);
    phazite::test::dispatchPendingMessages();
    CHECK (project->hasUnsavedChanges());
    CHECK_FALSE (song.getUndoManager().canUndo());
    CHECK (phazite::isWritingAutomation (song));
}

TEST_CASE ("View settings are saved, but changing them isn't an unsaved change")
{
    namespace schema = phazite::schema;
    phazite::test::TestPlugin testPlugin;
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Views");
    auto& song = project->getSong();

    const auto track = phazite::insertTrack (song, {}, nullptr);
    REQUIRE (track.isValid());
    const auto other = phazite::insertTrack (song, track, nullptr);
    REQUIRE (other.isValid());
    const auto folder = phazite::insertFolder (song, other, nullptr);
    REQUIRE (folder.isValid());
    const auto plugin = phazite::test::TestPlugin::add (song, track);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return phazite::getPluginsLoading (song).isEmpty(); }));

    // Without undo steps, so that the view settings below can be seen to make none. The save stores the plug-in's
    // settings, but is no undo step.
    song.getUndoManager().clearUndoHistory();
    phazite::test::dispatchPendingMessages();
    REQUIRE (project->save().wasOk());
    REQUIRE_FALSE (project->hasUnsavedChanges());
    REQUIRE_FALSE (song.getUndoManager().canUndo());

    // Every view setting: tracks' heights (one shown so short that it collapses), a collapsed folder, a pinned track, drum
    // mode and rows, both editors' grids, which gives the piano roll's its element, and where a device's window was (as the
    // window does when it moves).
    phazite::setSavedTrackHeight (song, track, 200);
    phazite::setCollapsed (song, other, true); // as dragging its edge well below the minimum does
    phazite::setCollapsed (song, folder, true);
    phazite::setPinned (song, track, true);
    phazite::setDrumTrack (song, track, true);
    phazite::setDrumRowOrder (song, track, { 38, 36 });
    phazite::hideDrumRow (song, track, 42);
    phazite::hideDrumRow (song, other, 42);
    phazite::showAllDrumRows (song, other);
    phazite::saveGridSettings (song, phazite::GridOwner::playlist, { .snap = false, .adaptiveOffset = 2 });
    phazite::saveGridSettings (song, phazite::GridOwner::pianoRoll, { .adaptive = false, .fixedLevel = 10 });
    phazite::rememberWindowPosition (song, plugin, { 140, 360 });
    phazite::test::dispatchPendingMessages();

    // No unsaved change, nothing to undo, and nothing for autosave to write.
    CHECK_FALSE (project->hasUnsavedChanges());
    CHECK_FALSE (song.getUndoManager().canUndo());
    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (project->getFile()).exists());

    // A notification that changes nothing, which makes the project compare itself with its saved copy, doesn't count them
    // either; a change to the song still counts.
    phazite::findTrackState (song, track).sendPropertyChangeMessage (schema::trackName);
    CHECK_FALSE (project->hasUnsavedChanges());
    song.getState().setProperty (schema::outline, "pop", nullptr);
    CHECK (project->hasUnsavedChanges());

    // Saved and opened again, they are all there.
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (song.getUndoManager().canUndo());
    const auto file = project->getFile();
    project = nullptr;
    project = openProject (file);
    auto& reopened = project->getSong();
    REQUIRE (phazite::findTrackState (reopened, track).isValid());
    REQUIRE (phazite::findTrackState (reopened, other).isValid());
    REQUIRE (phazite::isFolder (reopened, folder));
    CHECK (phazite::getSavedTrackHeight (reopened, track).value_or (0) == 200);
    CHECK (phazite::isCollapsed (reopened, other));
    CHECK (phazite::isCollapsed (reopened, folder));
    CHECK (phazite::isPinned (reopened, track));
    CHECK (phazite::isDrumTrack (reopened, track));
    CHECK (phazite::getDrumRowOrder (reopened, track) == std::vector<int> { 38, 36 });
    CHECK (phazite::getHiddenDrumRows (reopened, track) == std::vector<int> { 42 });
    CHECK (phazite::loadGridSettings (reopened, phazite::GridOwner::playlist).adaptiveOffset == 2);
    CHECK (phazite::loadGridSettings (reopened, phazite::GridOwner::pianoRoll).fixedLevel == 10);
    CHECK (phazite::getWindowPosition (reopened, plugin) == std::optional<juce::Point<int>> (juce::Point<int> (140, 360)));

    // Everything else is the song or state, which closing without saving would lose: each of these is an unsaved change.
    const auto isUnsavedChange = [&project] (const std::function<void()>& change)
    {
        REQUIRE (project->save().wasOk());
        REQUIRE_FALSE (project->hasUnsavedChanges());
        change();
        phazite::test::dispatchPendingMessages();
        return project->hasUnsavedChanges();
    };

    CHECK (isUnsavedChange ([&reopened] { reopened.getState().setProperty (schema::outline, "rock", nullptr); }));
    CHECK (isUnsavedChange ([&reopened] { reopened.getState().getOrCreateChildWithName (schema::LASTKEY, nullptr); }));
    CHECK (isUnsavedChange ([&reopened, track] { phazite::setTrackKey (reopened, track, phazite::Key { 2, phazite::Scale::dorian }); }));
    CHECK (isUnsavedChange ([&reopened, folder] { phazite::setMixedAsBus (reopened, folder, false); })); // a new folder is a bus

    // Those two are undo steps; written without one, they count too.
    CHECK (isUnsavedChange ([&reopened, track]
                            { phazite::findTrackState (reopened, track).setProperty (schema::keyScale, "lydian", nullptr); }));
    CHECK (isUnsavedChange ([&reopened, folder]
                            { phazite::findTrackState (reopened, folder).setProperty (schema::folderBus, true, nullptr); }));

    // A section's own properties, as the model changes them (Sections.h).
    const auto section = phazite::insertSection (reopened, {}, { .name = "Verse", .length = phazite::BeatDuration::fromBeats (16.0) });
    REQUIRE (section.isValid());
    CHECK (isUnsavedChange ([&reopened, section] { phazite::renameSection (reopened, section, "Chorus"); }));
    CHECK (isUnsavedChange ([&reopened, section] { phazite::setSectionColour (reopened, section, juce::Colours::orange); }));

    // Its role and its energy, which no model function changes once it is made: the song made like a copy of itself whole with
    // one of them changed (ApplySong.h), in an undo step, is an unsaved change; undone, the project is as it was saved.
    const auto changeInCopy = [&reopened, section] (const juce::Identifier& property, const juce::var& value)
    {
        auto copy = phazite::test::copyWholeSong (reopened);
        auto changed = false;

        for (auto child : copy.getChildWithName (schema::SECTIONS))
        {
            if ((std::uint64_t) (juce::int64) child[schema::sectionID] == section.getRaw())
            {
                child.setProperty (property, value, nullptr);
                changed = true;
            }
        }

        REQUIRE (changed);
        const phazite::UndoStep step (reopened, "Change Section");
        phazite::makeSongLike (reopened, copy);
    };

    CHECK (isUnsavedChange ([&changeInCopy] { changeInCopy (schema::sectionRole, "chorus"); }));
    CHECK (phazite::getSection (reopened, section).value_or (phazite::Section()).role == "chorus");
    phazite::undo (reopened); // as saved again, as closing the project finds it, comparing the whole song with the saved copy
    CHECK_FALSE (project->checkUnsavedChanges());

    CHECK (isUnsavedChange ([&changeInCopy] { changeInCopy (schema::sectionEnergy, 0.9); }));
    CHECK (phazite::getSection (reopened, section).value_or (phazite::Section()).energy == doctest::Approx (0.9));
    phazite::undo (reopened);
    CHECK_FALSE (project->checkUnsavedChanges());
}

TEST_CASE ("Saving stores plug-ins' own settings, without an undo step")
{
    using phazite::test::TestPlugin;
    TestPlugin testPlugin;
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Settings");
    auto& song = project->getSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // whose session plays the plug-in
    phazite::setSong (context, &song);
    const auto track = phazite::insertTrack (song, {}, nullptr);
    REQUIRE (track.isValid());
    const auto plugin = TestPlugin::add (song, track);

    // Saved without undo steps.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return phazite::getPluginsLoading (song).isEmpty(); })); // in its helper
    song.getUndoManager().clearUndoHistory();
    REQUIRE (project->save().wasOk());
    REQUIRE_FALSE (project->hasUnsavedChanges());
    REQUIRE_FALSE (song.getUndoManager().canUndo());

    // Its gain turned in its window: it is an unsaved change. Saved, it is in the project, with nothing to undo and nothing
    // unsaved.
    TestPlugin::setGain (song, plugin, 0.25f);
    CHECK (phazite::test::dispatchMessagesUntil ([&project] { return project->hasUnsavedChanges(); })); // as its helper tells
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (song.getUndoManager().canUndo());
    CHECK_FALSE (project->hasUnsavedChanges());

    // Nor do autosave and saving a copy add an undo step.
    TestPlugin::setGain (song, plugin, 0.5f);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&project] { return project->hasUnsavedChanges(); }));
    REQUIRE (project->autosave().wasOk());
    CHECK (phazite::Project::getAutosaveFile (project->getFile()).existsAsFile());
    juce::String error;
    const auto copy = project->saveCopyAs (temp.folder, "Copy", error);
    REQUIRE (copy.existsAsFile());
    CHECK_FALSE (song.getUndoManager().canUndo());

    // Opened again, the project plays the gain it was saved with, and the copy the one it had then.
    const auto file = project->getFile();
    phazite::setSong (context, nullptr);
    project = nullptr;

    for (const auto& [saved, gain] : { std::pair (file, 0.25f), std::pair (copy, 0.5f) })
    {
        auto opened = openProject (saved);
        REQUIRE (opened != nullptr);
        phazite::ProjectContext openedContext { phazite::test::getEngineContext() };
        phazite::setSong (openedContext, &opened->getSong());
        CHECK (TestPlugin::getGain (opened->getSong(), plugin) == doctest::Approx (gain));
        phazite::setSong (openedContext, nullptr);
    }
}

TEST_CASE ("Switching the loop on or off, or between the song and a range, is saved but isn't an unsaved change")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Loops");
    auto& song = project->getSong();
    phazite::test::dispatchPendingMessages();
    REQUIRE_FALSE (project->hasUnsavedChanges());

    // It is about playing the song rather than the song (design question 18), as where the loop is.
    phazite::toggleLoop (song);
    phazite::loopBeats (song, { beats (2.0), beats (6.0) });
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::isLoopOn (song));
    REQUIRE_FALSE (phazite::doesLoopFollowSong (song));
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (project->getFile()).exists());

    // Saved, the project opens with it as it was.
    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;
    project = openProject (file);
    CHECK (phazite::isLoopOn (project->getSong()));
    CHECK_FALSE (phazite::doesLoopFollowSong (project->getSong()));
    CHECK (phazite::getLoopBeats (project->getSong()).getStart().inBeats() == doctest::Approx (2.0));
}

TEST_CASE ("Autosave writes in the background, and says once when it fails")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Background");
    const auto autosaveFile = phazite::Project::getAutosaveFile (project->getFile());
    const auto waitForIt = [&project] { return phazite::test::runTimersUntil ([&project] { return ! project->isAutosaving(); }); };

    project->getSong().getState().setProperty (phazite::schema::outline, "pop", nullptr);
    project->autosaveInBackground();
    REQUIRE (waitForIt());
    CHECK (autosaveFile.existsAsFile());
    CHECK_FALSE (project->takeAutosaveFailure());
    CHECK_FALSE (project->hasChangesToAutosave());

    // A file where its folder goes: the write fails, which is told once, and the change is written again at the next.
    const auto backups = autosaveFile.getParentDirectory();
    REQUIRE (backups.deleteRecursively());
    REQUIRE (backups.create().wasOk());
    project->getSong().getState().setProperty (phazite::schema::outline, "rock", nullptr);
    project->autosaveInBackground();
    REQUIRE (waitForIt());
    CHECK (project->takeAutosaveFailure());
    CHECK_FALSE (project->takeAutosaveFailure());
    CHECK (project->hasChangesToAutosave());
    REQUIRE (backups.deleteFile());
}

TEST_CASE ("Autosave waits while the song plays")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Playing");
    auto& song = project->getSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // whose session plays the song
    phazite::setSong (context, &song);
    const auto autosaveFile = phazite::Project::getAutosaveFile (project->getFile());
    phazite::test::HeldClock clock; // which moves only as the test says, and ticks the timer as it moves it
    phazite::AutosaveTimer timer (*project, 120, 1);
    phazite::playOrStop (song);
    REQUIRE (phazite::isSongPlaying (song));

    // Past its first delay while the song plays: none.
    song.getState().setProperty (phazite::schema::outline, "pop", nullptr);
    timer.tickNow(); // which sees the change
    clock.advance (2500.0);
    timer.tickNow();
    CHECK_FALSE (project->isAutosaving());
    CHECK_FALSE (autosaveFile.exists());

    // Stopped: at once, not after its interval.
    phazite::stopPlaying (song);
    timer.tickNow();
    CHECK (phazite::test::runTimersUntil ([&autosaveFile] { return autosaveFile.existsAsFile(); }));
}

TEST_CASE ("Each of Phazite's devices keeps its parameters' values through a save and a reopen, and its type with them")
{
    phazite::test::TemporaryFolder temp;
    juce::File file;
    std::map<phazite::DeviceID, std::vector<float>> setTo; // each device, and the values its parameters were set to

    {
        juce::String error;
        const auto project = phazite::Project::create (temp.folder, "Devices", error);
        REQUIRE (project != nullptr);
        auto& song = project->getSong();
        const auto own = phazite::getOwnDevices();
        const auto tracks = phazite::ensureTracks (song, (int) own.size());

        for (size_t d = 0; d < own.size(); ++d)
        {
            const auto& info = *own[d];
            INFO (std::string (info.typeID));
            const auto device = phazite::makeOwnDeviceState (song, info.typeID);
            const auto id = info.kind == phazite::devices::DeviceKind::instrument ? phazite::setInstrument (song, tracks[d], device)
                                                                                  : phazite::insertDeviceState (song, tracks[d], device);
            REQUIRE (id.isValid());
            auto& values = setTo[id];

            // Each one away from its default, to the other end of its range: the filters' types to Sample & Hold.
            for (const auto& parameter : info.parameters)
            {
                const auto value = parameter.defaultValue < parameter.maximum ? parameter.maximum : parameter.minimum;
                phazite::setValueSetByHand (song, phazite::ParameterRef { id, juce::String (parameter.id.data(), parameter.id.size()) },
                                            value);
                values.push_back (value);
            }
        }

        REQUIRE (project->save().wasOk());
        file = project->getFile();
    }

    juce::String error;
    const auto project = phazite::Project::open (file, false, error);
    REQUIRE (project != nullptr);
    const auto& song = project->getSong();

    for (const auto& [id, values] : setTo)
    {
        const auto state = phazite::findDeviceState (song, id);
        const auto* info = phazite::findOwnDevice (state[phazite::schema::deviceType].toString().toStdString());
        REQUIRE (info != nullptr);
        INFO (std::string (info->typeID));
        REQUIRE (info->parameters.size() == values.size());

        for (size_t i = 0; i < values.size(); ++i)
        {
            const juce::String parameter (info->parameters[i].id.data(), info->parameters[i].id.size());
            INFO (parameter.toStdString());
            CHECK (phazite::getValueSetByHand (song, phazite::ParameterRef { id, parameter }) == doctest::Approx (values[i]));
        }
    }
}

TEST_CASE ("A save that can't write the project file leaves the file as it was last saved, whole, and the changes unsaved")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Held");
    const auto file = project->getFile();
    project->getSong().getState().setProperty ("take", 1, nullptr);
    REQUIRE (project->save().wasOk());
    juce::MemoryBlock saved;
    REQUIRE (file.loadFileAsData (saved));

    project->getSong().getState().setProperty ("take", 2, nullptr);

    {
        // Held by another program (or, outside Windows, in a folder that can't be written).
        const phazite::test::HeldFile held (file);

        if (! held.works())
        {
            MESSAGE ("The file can't be held from this user: nothing to fail.");
            return;
        }

        const auto result = project->save();
        CHECK (result.failed());
        CHECK (result.getErrorMessage().contains (file.getFullPathName()));
        CHECK (project->hasUnsavedChanges());
        CHECK (project->checkUnsavedChanges());
    }

    // The file is as it was, byte for byte, with nothing left beside it.
    juce::MemoryBlock after;
    REQUIRE (file.loadFileAsData (after));
    CHECK (after == saved);
    CHECK (file.getParentDirectory().findChildFiles (juce::File::findFiles, false, "*_temp*").isEmpty());

    // Once it can be written, the save works.
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());
    project = nullptr;
    CHECK ((int) openProject (file)->getSong().getState()["take"] == 2);
}

TEST_CASE ("A track's height and its collapse, changed between undo steps, stay through Undo and Redo, and are no unsaved change")
{
    phazite::test::TestSong song;
    const auto first = song.firstTrack;
    phazite::TrackID second;
    const auto row = [&song] (phazite::TrackID track)
    {
        const auto found = phazite::getTrackRow (song.context, track);
        REQUIRE (found.has_value());
        return *found;
    };

    {
        auto& project = song.getProject();
        phazite::test::UndoCheck check (song.context);
        check.checkSeveral ("a track", [&song, &second] { second = song.addTrack ("Bass"); });
        REQUIRE (project.save().wasOk());

        // Set between the steps, as the playlist sets them: no undo step, nothing unsaved.
        phazite::setSavedTrackHeight (song.context, first, 120);
        phazite::setCollapsed (song.context, first, true);
        phazite::setSavedTrackHeight (song.context, second, 90);
        CHECK_FALSE (project.hasUnsavedChanges());
        CHECK_FALSE (project.checkUnsavedChanges());

        check.checkSeveral ("a clip", [&song, &second] { song.addMidiClip (second, 0.0, 4.0); });
        phazite::setCollapsed (song.context, second, true);

        // Undone to before the track, and redone: the first track as it was set, and the second back as it was set too.
        auto& songState = *phazite::findSong (song.context);
        while (phazite::getTrackRow (song.context, second).has_value() && phazite::canUndo (songState))
            check.stepBackOrForth ("Undo", [&songState] { phazite::undo (songState); });

        CHECK_FALSE (phazite::getTrackRow (song.context, second).has_value());
        CHECK (row (first).savedHeight == std::optional<int> (120));
        CHECK (row (first).collapsed);

        while (phazite::canRedo (songState))
            check.stepBackOrForth ("Redo", [&songState] { phazite::redo (songState); });

        CHECK (row (first).savedHeight == std::optional<int> (120));
        CHECK (row (first).collapsed);
        CHECK (row (second).savedHeight == std::optional<int> (90));
        CHECK (row (second).collapsed);
        CHECK (phazite::getClips (song.context, second).size() == 1);
        check.walkBackAndForth();

        // Saved and opened again, the view state with it.
        REQUIRE (project.save().wasOk());
    }

    song.reopen();
    CHECK (row (first).savedHeight == std::optional<int> (120));
    CHECK (row (first).collapsed);
    CHECK (row (second).savedHeight == std::optional<int> (90));
}
