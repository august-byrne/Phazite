// SPDX-License-Identifier: AGPL-3.0-or-later
#include "GoldenFiles.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "devices/drums/DrumSampler.h"
#include "devices/eq/Equalizer.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Grid.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Devices.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

#include <functional>
#include <map>
#include <utility>

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

TEST_CASE ("Saving keeps the newest backups")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Backups");
    CHECK (getBackups (*project).isEmpty()); // the first save had nothing to back up

    for (int i = 0; i < phazite::Project::numBackupsToKeep + 3; ++i)
    {
        project->getSong().getState().setProperty ("version", i, nullptr);
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

    // As the app has it, but the first autosave a second after the first change rather than 15 s.
    phazite::AutosaveTimer timer (*project, 120, 1);
    phazite::test::runTimersUntil ([] { return false; }, 1500);
    CHECK_FALSE (autosaveFile.exists()); // nothing changed

    project->getSong().getState().setProperty (phazite::schema::outline, "pop", nullptr);
    CHECK (phazite::test::runTimersUntil ([&autosaveFile] { return autosaveFile.existsAsFile(); }, 4000));

    // The next change waits for the interval.
    const auto first = autosaveFile.loadFileAsString();
    project->getSong().getState().setProperty (phazite::schema::outline, "rock", nullptr);
    phazite::test::runTimersUntil ([] { return false; }, 2500);
    CHECK (autosaveFile.loadFileAsString() == first);
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
    REQUIRE (phazite::isAudioState (phazite::findClipState (song, clip)));
    REQUIRE (phazite::getClipFile (song, phazite::findClipState (song, clip)).isAChildOf (project->getAudioFolder()));
    REQUIRE (song.getUndoManager().canUndo());
    REQUIRE (project->autosave().wasOk());
    REQUIRE (phazite::Project::getAutosaveFile (oldFile).existsAsFile());

    // A folder of that name is there already: nothing changes.
    const auto projects = temp.folder.getChildFile ("Projects");
    REQUIRE (projects.getChildFile ("Taken").createDirectory().wasOk());
    CHECK (project->moveTo (projects, "Taken").failed());
    CHECK (project->getFile() == oldFile);
    CHECK (project->hasUnsavedChanges());

    REQUIRE (project->moveTo (projects, "Beat").wasOk());
    const auto newFile = projects.getChildFile ("Beat").getChildFile ("Beat.phazite");
    CHECK (project->getFile() == newFile);
    CHECK (newFile.existsAsFile());
    CHECK_FALSE (project->hasUnsavedChanges());
    CHECK (song.getUndoManager().canUndo());

    // The clip plays the new folder's copy of its audio. The old folder stays, for its owner to delete once the project
    // has closed, without the autosave, whose changes are saved now.
    const auto clipFile = phazite::getClipFile (song, phazite::findClipState (song, clip));
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
    const auto reopenedClip = phazite::findClipState (reopenedSong, clips.front());
    REQUIRE (phazite::isAudioState (reopenedClip));
    CHECK (phazite::getClipFile (reopenedSong, reopenedClip) == newFile.getSiblingFile ("Audio").getChildFile ("Kick.wav"));
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

    SUBCASE ("The First Beat example song, as Help > Example Songs makes it")
    {
        project = phazite::Project::create (
            temp.folder, "First", error,
            phazite::getExampleSongContent (phazite::test::getEngineContext(), "firstBeat", phazite::test::getFactoryPresetsFolder()));
        INFO (error.toStdString());
        REQUIRE (project != nullptr);
        const auto& song = project->getSong();
        const auto tracks = phazite::getTrackIDs (song);
        REQUIRE (tracks.size() == 2);
        CHECK (phazite::getTrackName (song, tracks[0]) == juce::String ("Drums"));
        CHECK (phazite::getTrackName (song, tracks[1]) == juce::String ("Synth"));

        // Each with its clip, played by one of Phazite's own instruments: the drum sampler, the synth.
        for (const auto& [track, instrumentType] :
             { std::pair (tracks[0], phazite::devices::DrumSampler::typeID), std::pair (tracks[1], phazite::devices::Synth::typeID) })
        {
            CHECK (phazite::getClips (song, track).size() == 1);
            const auto instrument = phazite::findDeviceState (song, phazite::getInstrument (song, track));
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
    phazite::test::settle();
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
    phazite::test::settle();
    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    // Once the message loop has run after it opened, nothing has changed.
    project = openProject (file);
    phazite::test::settle();
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
    CHECK (phazite::findCurveState (song, gainRef).isValid());
    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    // Opened, nothing can bring the device back: its curve goes, without an unsaved change, and the volume's stays.
    project = openProject (file);
    const auto& reopened = project->getSong();
    CHECK_FALSE (phazite::findCurveState (reopened, gainRef).isValid());
    REQUIRE (phazite::findCurveState (reopened, volumeRef).getNumChildren() == 1);
    CHECK (phazite::getPoints (reopened, volumeRef).size() == 1);
    phazite::test::settle();
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
        phazite::test::settle();
        REQUIRE (project->save().wasOk());
    }

    const auto file = project->getFile();
    project = nullptr;
    project = openProject (file);
    phazite::test::settle();
    REQUIRE_FALSE (project->hasUnsavedChanges());

    auto& song = project->getSong();
    auto trackState = phazite::findTrackState (song, trackID);
    auto clipState = phazite::findClipState (song, clipID);
    REQUIRE (trackState.isValid());
    REQUIRE (clipState.isValid());

    // Notifications that change nothing.
    clipState.sendPropertyChangeMessage (phazite::schema::clipMute);
    trackState.sendPropertyChangeMessage (phazite::schema::trackName);
    CHECK_FALSE (project->hasUnsavedChanges());

    // The same value written as another type (the file holds text).
    REQUIRE (trackState[phazite::schema::keyRoot].isString());
    trackState.setProperty (phazite::schema::keyRoot, 2, nullptr);
    CHECK_FALSE (project->hasUnsavedChanges());

    // A note taken out and put back as it was.
    const auto note = clipState.getChildWithName (phazite::schema::NOTE);
    REQUIRE (note.isValid());
    const auto noteIndex = clipState.indexOf (note);
    clipState.removeChild (note, nullptr);
    clipState.addChild (note.createCopy(), noteIndex, nullptr);
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

    // Real changes count, made without an undo step or with one; saving clears them.
    song.getState().setProperty (phazite::schema::outline, "pop", nullptr);
    CHECK (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK (phazite::Project::getAutosaveFile (file).existsAsFile());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    REQUIRE (phazite::test::addMidiNote (song, clipID, 67, beats (3.0), phazite::BeatDuration::fromBeats (0.5), 100).isValid());
    CHECK (project->hasUnsavedChanges());
    phazite::test::settle();
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    // With an undo step, as the editors make changes.
    REQUIRE (phazite::addNote (song, clipID, 69, beats (3.5), phazite::BeatDuration::fromBeats (0.25), 100).isValid());
    CHECK (project->hasUnsavedChanges());
    phazite::test::settle();
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

TEST_CASE ("A write mode, the master's too, is an unsaved change without an undo step, and the master's lanes are view state")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Modes");
    auto& song = project->getSong();
    const auto master = phazite::getMasterID (song);
    phazite::test::settle();
    REQUIRE_FALSE (project->hasUnsavedChanges());

    phazite::showLane (song, master, { phazite::getFader (song, master), phazite::getVolumeParameterID (song, master) });
    phazite::test::settle();
    CHECK_FALSE (project->hasUnsavedChanges());

    phazite::setAutomationMode (song, master, phazite::AutomationMode::latch);
    phazite::test::settle();
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
    phazite::test::settle();

    // Without undo steps, so that the view settings below can be seen to make none. The save stores the plug-in's
    // settings, but is no undo step.
    song.getUndoManager().clearUndoHistory();
    phazite::test::settle();
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
    phazite::test::settle();

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
        phazite::test::settle();
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

    REQUIRE (phazite::insertSection (reopened, {}, { .name = "Verse", .length = phazite::BeatDuration::fromBeats (16.0) }).isValid());
    auto section = reopened.getState().getChildWithName (schema::SECTIONS).getChild (0);
    REQUIRE (section.isValid());
    CHECK (isUnsavedChange ([&section] { section.setProperty (schema::sectionRole, "chorus", nullptr); }));
    CHECK (isUnsavedChange ([&section] { section.setProperty (schema::sectionEnergy, 0.9f, nullptr); }));
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
    phazite::test::settle();
    song.getUndoManager().clearUndoHistory();
    REQUIRE (project->save().wasOk());
    REQUIRE_FALSE (project->hasUnsavedChanges());
    REQUIRE_FALSE (song.getUndoManager().canUndo());

    // Its gain turned in its window: it is an unsaved change. Saved, it is in the project, with nothing to undo and nothing
    // unsaved.
    TestPlugin::setGain (song, plugin, 0.25f);
    phazite::test::settle (1000);
    CHECK (project->hasUnsavedChanges());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (song.getUndoManager().canUndo());
    CHECK_FALSE (project->hasUnsavedChanges());

    // Nor do autosave and saving a copy add an undo step.
    TestPlugin::setGain (song, plugin, 0.5f);
    phazite::test::settle (1000);
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
    phazite::test::settle();
    REQUIRE_FALSE (project->hasUnsavedChanges());

    // It is about playing the song rather than the song (design question 18), as where the loop is.
    phazite::toggleLoop (song);
    phazite::loopBeats (song, { beats (2.0), beats (6.0) });
    phazite::test::settle();
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
    const auto waitForIt = [&project] { return phazite::test::runTimersUntil ([&project] { return ! project->isAutosaving(); }, 5000); };

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
    phazite::AutosaveTimer timer (*project, 120, 1);
    phazite::playOrStop (song);
    REQUIRE (phazite::isSongPlaying (song));

    song.getState().setProperty (phazite::schema::outline, "pop", nullptr);
    phazite::test::runTimersUntil ([] { return false; }, 2500);
    CHECK_FALSE (autosaveFile.exists());

    phazite::stopPlaying (song);
    CHECK (phazite::test::runTimersUntil ([&autosaveFile] { return autosaveFile.existsAsFile(); }, 4000));
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
