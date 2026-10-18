// SPDX-License-Identifier: AGPL-3.0-or-later
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "TestPlugin.h"
#include "model/arrangement/Outlines.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Grid.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Outlines.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/StarterProject.h"
#include "model/transport/internal/Playback.h"
#include "ui/playlist/TrackHeights.h"

#include <functional>

namespace
{
te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
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

    // With Phazite's state, in memory and in the file.
    CHECK (phazite::findPhaziteState (project->getEdit()).isValid());
    CHECK (phazite::test::readXml (project->getFile())->getChildByName ("PHAZITE") != nullptr);

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

TEST_CASE ("A project's first save moves it to a folder of its own, where it goes on with its audio and its undo history")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder.getChildFile ("Unsaved"), "Untitled");
    const auto oldFile = project->getFile();
    auto& edit = project->getEdit();
    edit.ensureNumberOfAudioTracks (1);

    // Audio imported into it (the project's own copy, by its path relative to the project), unsaved and autosaved.
    const auto kick = phazite::test::writeSound (temp.folder.getChildFile ("Samples").getChildFile ("Kick.wav"), 0.5);
    auto* clip = dynamic_cast<te::WaveAudioClip*> (phazite::importFile (*te::getAudioTracks (edit)[0], kick, beats (0.0)));
    REQUIRE (clip != nullptr);
    REQUIRE (clip->getAudioFile().getFile().isAChildOf (project->getAudioFolder()));
    REQUIRE (edit.getUndoManager().canUndo());
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
    CHECK (edit.getUndoManager().canUndo());

    // The clip plays the new folder's copy of its audio. The old folder stays, for its owner to delete once the project
    // has closed, without the autosave, whose changes are saved now.
    CHECK (clip->getAudioFile().getFile() == newFile.getSiblingFile ("Audio").getChildFile ("Kick.wav"));
    CHECK (clip->getAudioFile().getFile().existsAsFile());
    CHECK (oldFile.existsAsFile());
    CHECK_FALSE (phazite::Project::getAutosaveFile (oldFile).exists());

    // It saves there from now on, and opens from there with its audio.
    project->getPhaziteState().setProperty (phazite::schema::outline, "pop", nullptr);
    REQUIRE (project->save().wasOk());
    project = nullptr;
    auto reopened = openProject (newFile);
    CHECK (reopened->getPhaziteState()[phazite::schema::outline].toString() == juce::String ("pop"));
    const auto clips = te::getAudioTracks (reopened->getEdit())[0]->getClips();
    REQUIRE (clips.size() == 1);
    auto* reopenedClip = dynamic_cast<te::WaveAudioClip*> (clips.getFirst());
    REQUIRE (reopenedClip != nullptr);
    CHECK (reopenedClip->getAudioFile().getFile() == newFile.getSiblingFile ("Audio").getChildFile ("Kick.wav"));
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

TEST_CASE ("A new project opens from its first save with what it starts with, without undo steps or unsaved changes")
{
    TemporaryFolder temp;
    juce::String error;
    std::unique_ptr<phazite::Project> project;

    SUBCASE ("The starter song, as the first start makes it")
    {
        project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "First", error, phazite::addStarterContent);
        INFO (error.toStdString());
        REQUIRE (project != nullptr);
        const auto tracks = te::getAudioTracks (project->getEdit());
        REQUIRE (tracks.size() == 2);
        CHECK (tracks[0]->getName() == juce::String ("Drums"));
        CHECK (tracks[1]->getName() == juce::String ("Synth"));

        for (auto* track : tracks)
        {
            CHECK (track->getClips().size() == 1);
            CHECK (dynamic_cast<phazite::DevicePlugin*> (phazite::getInstrument (*track)) != nullptr); // the drum sampler, the synth
        }
    }

    SUBCASE ("An outline's sections, as New Project lays them out")
    {
        project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Outlined", error,
                                            [] (te::Edit& e) { phazite::applyOutline (e, *phazite::findOutline ("pop")); });
        INFO (error.toStdString());
        REQUIRE (project != nullptr);
        CHECK (phazite::getAppliedOutline (project->getEdit()) == juce::String ("pop"));
        CHECK_FALSE (phazite::getSections (project->getEdit()).isEmpty());
    }

    // It is opened from its save: Tracktion put what was added in order while loading, and nothing of it is an undo step
    // or, once Tracktion's updates arrive, an unsaved change.
    CHECK (project->getFile().existsAsFile());
    CHECK_FALSE (project->getEdit().getUndoManager().canUndo());
    phazite::test::settle();
    CHECK_FALSE (project->hasUnsavedChanges());
    CHECK_FALSE (project->getEdit().getUndoManager().canUndo());
}

TEST_CASE ("A project with MIDI clips and an alias opens without unsaved changes")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Notes");
    auto* track = te::getAudioTracks (project->getEdit())[0];
    REQUIRE (track != nullptr);
    auto* clip = phazite::insertMidiClip (*track, { beats (0.0), beats (4.0) }, nullptr);
    REQUIRE (clip != nullptr);
    REQUIRE (phazite::addNote (*clip, 60, beats (0.0), te::BeatDuration::fromBeats (1.0), 100) != nullptr);
    REQUIRE (phazite::addNote (*clip, 64, beats (2.0), te::BeatDuration::fromBeats (1.0), 100) != nullptr);
    const auto aliases = phazite::copyClips ({ clip }, true);
    REQUIRE (aliases.size() == 1);
    phazite::moveClip (*aliases[0], beats (4.0), *track);

    // As the app's message loop does between edits: Tracktion sorts the track's clips, and the alias copies the notes.
    phazite::test::settle();
    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;

    // Once Tracktion's updates after loading arrive (each MIDI clip notifies its mute switch, and the alias copies its
    // notes again), nothing has changed.
    project = openProject (file);
    phazite::test::settle();
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (file).exists());
}

TEST_CASE ("Only a change to what the project holds is an unsaved change, made with an undo step or not")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Changes");

    {
        auto* track = te::getAudioTracks (project->getEdit())[0];
        REQUIRE (track != nullptr);
        auto* clip = phazite::insertMidiClip (*track, { beats (0.0), beats (4.0) }, nullptr);
        REQUIRE (clip != nullptr);
        REQUIRE (phazite::addNote (*clip, 60, beats (0.0), te::BeatDuration::fromBeats (1.0), 100) != nullptr);
        REQUIRE (phazite::addNote (*clip, 64, beats (2.0), te::BeatDuration::fromBeats (1.0), 100) != nullptr);
        const auto aliases = phazite::copyClips ({ clip }, true);
        REQUIRE (aliases.size() == 1);
        phazite::moveClip (*aliases[0], beats (4.0), *track);
        phazite::setTrackKey (*track, phazite::Key { 2, phazite::Scale::minor });
        phazite::test::settle();
        REQUIRE (project->save().wasOk());
    }

    const auto file = project->getFile();
    project = nullptr;
    project = openProject (file);
    phazite::test::settle();
    REQUIRE_FALSE (project->hasUnsavedChanges());

    auto& edit = project->getEdit();
    auto* track = te::getAudioTracks (edit)[0];
    REQUIRE (track != nullptr);
    auto* clip = dynamic_cast<te::MidiClip*> (track->getClips().getFirst()); // the original, at beat 0
    REQUIRE (clip != nullptr);

    // Notifications that change nothing.
    clip->state.sendPropertyChangeMessage (te::IDs::mute);
    track->state.sendPropertyChangeMessage (te::IDs::name);
    CHECK_FALSE (project->hasUnsavedChanges());

    // The same value written as another type (the file holds text).
    REQUIRE (track->state[phazite::schema::phaziteKeyRoot].isString());
    track->state.setProperty (phazite::schema::phaziteKeyRoot, 2, nullptr);
    CHECK_FALSE (project->hasUnsavedChanges());

    // A note taken out and put back as it was.
    auto sequence = clip->getSequence().state;
    const auto note = sequence.getChild (0);
    sequence.removeChild (note, nullptr);
    sequence.addChild (note.createCopy(), 0, nullptr);
    CHECK_FALSE (project->hasUnsavedChanges());

    // Where the playhead is.
    phazite::movePlayhead (edit, te::TimePosition::fromSeconds (2.0));
    track->state.sendPropertyChangeMessage (te::IDs::name);
    CHECK_FALSE (project->hasUnsavedChanges());

    // A change taken back before anything asked.
    track->state.setProperty (phazite::schema::phaziteKeyRoot, 5, nullptr);
    track->state.setProperty (phazite::schema::phaziteKeyRoot, 2, nullptr);
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (file).exists());

    // Real changes count, made without an undo step or with one; saving clears them.
    project->getPhaziteState().setProperty (phazite::schema::outline, "pop", nullptr);
    CHECK (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK (phazite::Project::getAutosaveFile (file).existsAsFile());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    REQUIRE (clip->getSequence().addNote (67, beats (3.0), te::BeatDuration::fromBeats (0.5), 100, 0, nullptr) != nullptr);
    CHECK (project->hasUnsavedChanges());
    phazite::test::settle(); // the alias copies the note
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    // With an undo step, as the editors make changes: Tracktion's listener for undo steps then marks the Edit changed
    // too, with its flag and its time stamp, lastSignificantChange.
    REQUIRE (phazite::addNote (*clip, 69, beats (3.5), te::BeatDuration::fromBeats (0.25), 100) != nullptr);
    CHECK (project->hasUnsavedChanges());
    phazite::test::settle();
    CHECK (edit.hasChangedSinceSaved());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    // A change Tracktion notices outside the project's values, such as a plug-in's own settings, reaches Phazite only as
    // that time stamp once Tracktion's flag is reset (as it is 200 ms after loading): it counts, and autosave writes it.
    // (The stamp is in milliseconds; the settle() above makes this one new.)
    edit.markAsChanged();
    edit.resetChangedStatus();
    CHECK (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK (phazite::Project::getAutosaveFile (file).existsAsFile());
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (project->hasUnsavedChanges());

    track->state.setProperty (phazite::schema::phaziteKeyRoot, 3, nullptr);
    CHECK (project->hasUnsavedChanges());
}

TEST_CASE ("View settings are saved, but changing them isn't an unsaved change")
{
    namespace schema = phazite::schema;
    phazite::test::TestPlugin testPlugin;
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Views");
    auto& edit = project->getEdit();

    auto* track = phazite::insertTrack (edit, nullptr, nullptr);
    REQUIRE (track != nullptr);
    auto* other = phazite::insertTrack (edit, track, nullptr);
    REQUIRE (other != nullptr);
    auto* folder = phazite::insertFolder (edit, other, nullptr);
    REQUIRE (folder != nullptr);
    auto* plugin = phazite::insertDevice (*track, phazite::test::TestPlugin::create (edit));
    REQUIRE (plugin != nullptr);
    phazite::test::settle();

    // A first save writes the plug-in's settings into the project, through the undo manager (docs/known-bugs.md, bug 2).
    // Then without undo steps, so that the view settings below can be seen to make none. Tracktion marks the Edit changed
    // when its undo history changes (once the message about it arrives), so the project is saved again after that.
    REQUIRE (project->save().wasOk());
    edit.getUndoManager().clearUndoHistory();
    phazite::test::settle();
    REQUIRE (project->save().wasOk());
    REQUIRE_FALSE (project->hasUnsavedChanges());

    // Every view setting: tracks' heights (one shown so short that it collapses), a collapsed folder, a pinned track, drum
    // mode and rows, both editors' grids, which gives the piano roll's its element, and where a device's window was (as the
    // window does when it moves).
    phazite::setSavedTrackHeight (*track, 200);
    phazite::setCollapsed (*other, true); // as dragging its edge well below the minimum does
    phazite::setCollapsed (*folder, true);
    phazite::setPinned (*track, true);
    phazite::setDrumTrack (*track, true);
    phazite::setDrumRowOrder (*track, { 38, 36 });
    phazite::hideDrumRow (*track, 42);
    phazite::hideDrumRow (*other, 42);
    phazite::showAllDrumRows (*other);
    phazite::saveGridSettings (edit, phazite::GridOwner::playlist, { .snap = false, .adaptiveOffset = 2 });
    phazite::saveGridSettings (edit, phazite::GridOwner::pianoRoll, { .adaptive = false, .fixedLevel = 10 });
    plugin->windowState->lastWindowBounds = juce::Rectangle<int> (140, 360, 300, 200);
    phazite::rememberWindowPosition (*plugin, { 140, 360 });
    phazite::test::settle();

    // No unsaved change, nothing to undo, and nothing for autosave to write.
    CHECK_FALSE (project->hasUnsavedChanges());
    CHECK_FALSE (edit.getUndoManager().canUndo());
    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (project->getFile()).exists());

    // A notification that changes nothing, which makes the project compare itself with its saved copy, doesn't count them
    // either; a change to the song still counts.
    track->state.sendPropertyChangeMessage (te::IDs::name);
    CHECK_FALSE (project->hasUnsavedChanges());
    project->getPhaziteState().setProperty (schema::outline, "pop", nullptr);
    CHECK (project->hasUnsavedChanges());

    // Saved and opened again, they are all there. Tracktion writes where the window was as it saves, through the undo
    // manager, and finds it written.
    const auto trackID = track->itemID, otherID = other->itemID, folderID = folder->itemID, pluginID = plugin->itemID;
    REQUIRE (project->save().wasOk());
    CHECK_FALSE (edit.getUndoManager().canUndo());
    const auto file = project->getFile();
    project = nullptr;
    project = openProject (file);
    auto& reopened = project->getEdit();
    auto* t = dynamic_cast<te::AudioTrack*> (te::findTrackForID (reopened, trackID));
    auto* o = te::findTrackForID (reopened, otherID);
    auto* f = dynamic_cast<te::FolderTrack*> (te::findTrackForID (reopened, folderID));
    REQUIRE (t != nullptr);
    REQUIRE (o != nullptr);
    REQUIRE (f != nullptr);
    CHECK (phazite::getSavedTrackHeight (*t).value_or (0) == 200);
    CHECK (phazite::isCollapsed (*o));
    CHECK (phazite::isCollapsed (*f));
    CHECK (phazite::isPinned (*t));
    CHECK (phazite::isDrumTrack (*t));
    CHECK (phazite::getDrumRowOrder (*t) == std::vector<int> { 38, 36 });
    CHECK (phazite::getHiddenDrumRows (*t) == std::vector<int> { 42 });
    CHECK (phazite::loadGridSettings (reopened, phazite::GridOwner::playlist).adaptiveOffset == 2);
    CHECK (phazite::loadGridSettings (reopened, phazite::GridOwner::pianoRoll).fixedLevel == 10);
    const auto reopenedPlugin = te::findPluginForID (reopened, pluginID);
    REQUIRE (reopenedPlugin != nullptr);
    REQUIRE (reopenedPlugin->windowState->lastWindowBounds.has_value());
    CHECK (reopenedPlugin->windowState->lastWindowBounds->getPosition() == juce::Point<int> (140, 360));

    // Everything else is the song or state, which closing without saving would lose: each of these is an unsaved change.
    const auto isUnsavedChange = [&project] (const std::function<void()>& change)
    {
        REQUIRE (project->save().wasOk());
        REQUIRE_FALSE (project->hasUnsavedChanges());
        change();
        phazite::test::settle();
        return project->hasUnsavedChanges();
    };

    CHECK (isUnsavedChange ([&project] { project->getPhaziteState().setProperty (schema::outline, "rock", nullptr); }));
    CHECK (isUnsavedChange ([&project] { project->getPhaziteState().getOrCreateChildWithName (schema::LASTKEY, nullptr); }));
    CHECK (isUnsavedChange ([t] { phazite::setTrackKey (*t, phazite::Key { 2, phazite::Scale::dorian }); }));
    CHECK (isUnsavedChange ([f] { phazite::setMixedAsBus (*f, false); })); // a new folder is a bus

    // Those two are undo steps, which Tracktion counts by itself; written without one, they count too.
    CHECK (isUnsavedChange ([t] { t->state.setProperty (schema::phaziteKeyScale, "lydian", nullptr); }));
    CHECK (isUnsavedChange ([f] { f->state.setProperty (schema::phaziteBus, true, nullptr); }));

    auto* section = phazite::insertSection (reopened, {}, { .name = "Verse", .length = te::BeatDuration::fromBeats (16.0) });
    REQUIRE (section != nullptr);
    CHECK (isUnsavedChange ([section] { section->state.setProperty (schema::phaziteRole, "chorus", nullptr); }));
    CHECK (isUnsavedChange ([section] { section->state.setProperty (schema::phaziteEnergy, 0.9f, nullptr); }));
}

TEST_CASE ("Switching the loop on or off, or between the song and a range, is saved but isn't an unsaved change")
{
    TemporaryFolder temp;
    auto project = createProject (temp.folder, "Loops");
    auto& edit = project->getEdit();
    phazite::test::settle();
    REQUIRE_FALSE (project->hasUnsavedChanges());

    // It is about playing the song rather than the song (design question 18), as where the loop is.
    phazite::toggleLoop (edit);
    phazite::loopRange (edit, { te::TimePosition::fromSeconds (1.0), te::TimePosition::fromSeconds (3.0) });
    phazite::test::settle();
    REQUIRE (phazite::isLoopOn (edit));
    REQUIRE_FALSE (phazite::doesLoopFollowSong (edit));
    CHECK_FALSE (project->hasUnsavedChanges());
    REQUIRE (project->autosave().wasOk());
    CHECK_FALSE (phazite::Project::getAutosaveFile (project->getFile()).exists());

    // Saved, the project opens with it as it was.
    REQUIRE (project->save().wasOk());
    const auto file = project->getFile();
    project = nullptr;
    project = openProject (file);
    CHECK (phazite::isLoopOn (project->getEdit()));
    CHECK_FALSE (phazite::doesLoopFollowSong (project->getEdit()));
    CHECK (project->getEdit().getTransport().getLoopRange().getStart().inSeconds() == doctest::Approx (1.0));
}
