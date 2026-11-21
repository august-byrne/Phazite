// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden files of the model's commands (tests/golden/commands): their catalogue (each command's id, texts and switches,
// in the order the app registers them, which decides which of two commands on one key runs and the order of the
// shortcuts list), and a script that runs every model command where it does something: the test fails for a command the
// registry has that the script doesn't run (getCommandsNotRun() lists those it doesn't, with why: versions' commands that
// change the song outside the undo history, need what one project can't make, or change no song), and for one it runs where
// it changes nothing. And the whole window's commands beyond the model's: each that changes the song or the project run once
// through an UndoCheck, every other listed with why it changes no song (getWindowCommandsNotRun()). The script's golden
// files hold the project it leaves (script.xml), the undo
// step the Edit menu offers after each command (script-undo.txt), and, after each step, every command whose state changed
// (enabled or greyed out, ticked, and the name menus show), and what is selected (notes with their pitches, starts and
// lengths), where the playhead is, the loop's range and whether the song plays or records, each if it changed
// (script-states.txt). GoldenFiles.h says how to regenerate them.
//
// Every command the script runs, and every change it makes itself, goes through an UndoCheck (UndoCheck.h): Undo gives
// the song as it was before, and Redo as it was after; and at its end, the script's every step is undone and redone, the
// song at each as it was there.
#include "AppScreen.h"
#include "EnginePlayback.h"
#include "GoldenFiles.h"
#include "SongCheck.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "UndoCheck.h"
#include "app/AppCommands.h"
#include "devices/reverb/Reverb.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Sections.h"
#include "model/arrangement/internal/Selection.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/LiveInput.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/notes/internal/Notes.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "model/transport/internal/Metronome.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/playlist/Playlist.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace
{
constexpr int maxListedDifferences = 40; // the most lines a comparison lists

constexpr const char* aboutTheCatalogue =
    "# The model's commands, in the order the app registers them (tests/CommandGoldenTests.cpp): id | name | category | "
    "description | synonyms | switches. Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).";

constexpr const char* aboutTheUndoSteps =
    "# The command script (tests/CommandGoldenTests.cpp): each command it runs, and the undo step the Edit menu offers "
    "after it (the one before, if the command made none). Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).";

constexpr const char* aboutTheStates =
    "# The command script (tests/CommandGoldenTests.cpp): after each step, every command whose state changed (on, or off "
    "when greyed out; ticked; the name menus show), and what is selected, where the playhead is, the loop's range (in "
    "beats) and whether the song plays or records, each if it changed. Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).";

constexpr const char* aboutTheProject = "The project the command script leaves (every model command run where it does something), "
                                        "saved: tests/CommandGoldenTests.cpp";

phazite::BeatPosition beats (double b)
{
    return phazite::BeatPosition::fromBeats (b);
}

// What runs the freezes and bounces the commands start: the main window's part, which outlives the registry as the
// window does.
phazite::test::RenderHost& getRenders()
{
    static phazite::test::RenderHost renders;
    return renders;
}

// The model's commands, registered as the app registers them: addSongCommands(), which Main.cpp calls, the versions'
// commands as the project's session adds them (ProjectSession::addCommands(), the project saved as the app's save saves it),
// then the note commands as AppShell adds them, with a grid of a quarter beat.
void addModelCommands (phazite::CommandRegistry& registry, phazite::ProjectContext& context, phazite::SaveProjectThen saveProject)
{
    phazite::addSongCommands (registry, context, getRenders());
    phazite::addVersionCommands (registry, context, std::move (saveProject), getRenders());
    phazite::addNoteCommands (registry, getSelection (context), [] { return 0.25; });
}

// The model's commands that the script doesn't run, each with why, so that a new command is either run by the script or
// listed here on purpose. (The commands that need the song to play, a device's input or notes played live run while the
// engine is driven a block at a time: EnginePlayback.h.) Among the versions' commands, those that change the song in an
// undo step are run; these change it outside the undo history by design, need what one project's script can't make, name
// their step by the time of day, or change no song at all, each with the test that runs it.
const std::vector<std::pair<juce::String, juce::String>>& getCommandsNotRun()
{
    static const std::vector<std::pair<juce::String, juce::String>> commands {
        { "version.takeNewer",
          "needs versions of the current branch that the song didn't grow from, saved on another computer or after the project "
          "file was saved, which one project's script can't make; VersionsTiedTests.cpp runs it through an UndoCheck (\"A "
          "version another computer saved on the branch under a closed project is offered to be taken, as one undo step\"), "
          "and the merge it starts where the song has changes of its own (\"The newer versions merged with the song's own "
          "changes: its version follows both lines, and nothing is lost\")" },
        { "version.switchBranch",
          "a switch starts the song's undo history again (VersionsModel.h: Branches), which no UndoCheck can walk back through, "
          "so it has no step to check; VersionsBranchTests.cpp checks what it leaves (\"Switching keeps each branch's song as it "
          "was left there, unsaved changes included, and starts the undo history again\")" },
        { "version.merge",
          "changes no song itself: it lists what a version of another line changed (its step is version.finishMerge's), and "
          "that version is on another branch, which needs a switch (above); VersionsBranchTests.cpp checks its list (\"A branch "
          "that changed other tracks merges with no clashes, and the version after follows both lines\")" },
        { "version.finishMerge",
          "takes the list version.merge shows, which the script can't make (above); VersionsGoingBackTests.cpp runs it through "
          "an UndoCheck (\"A merge of note edits made through an alias on both branches is one exact undo step, the group whole "
          "after it, its Undo and its Redo\"), and VersionsTiedTests.cpp too (\"The newer versions merged with the song's own "
          "changes: its version follows both lines, and nothing is lost\")" },
        { "version.cancelMerge",
          "cancels a merge version.finishMerge took, which the script can't make (above); no UndoCheck runs it: "
          "VersionsBranchTests.cpp checks the song it leaves and its step's name (\"Merge Branch merges another branch's last "
          "commit, and Cancel Merge makes the song what it was before it\") and its Undo (\"Undo after a merge takes the merge "
          "back with the song, Redo brings it back, and a merge waits still as the project opens again\")" },
        { "version.open",
          "a version open in the song's place is a trial held open until Back to Now (VersionsModel.cpp: showVersion()), no "
          "undo step, which an UndoCheck around the command sees as a change without a step (VersionsOpenTests.cpp)" },
        { "version.backToNow", "as version.open, which it ends (VersionsOpenTests.cpp)" },
        { "version.compare", "opens the last version in the song's place, as version.open (VersionsOpenTests.cpp)" },
        { "version.restoreSnapshot",
          "changes the song in one undo step whose name holds the local time of the automatic save it restores (\"Restore the "
          "Automatic Save of ...\"), which the script's golden file of undo steps can't hold the same on every machine; "
          "VersionsGoingBackTests.cpp runs it through an UndoCheck (\"Every way of going back is one exact undo step on a song "
          "of notes and aliases, which holds together after it, its Undo and its Redo\")" },
        { "version.verify",
          "changes neither the song nor a command's state once it is done: what it found shows in the Versions view alone "
          "(VersionsKeptWholeTests.cpp)" },
        { "version.resetToReadable",
          "needs a branch whose last version can't be read from the Versions folder (VersionsKeptWholeTests.cpp)" },
        { "version.bounce", "changes no song: it renders a version into the history (VersionsHearingTests.cpp)" },
        { "version.hear", "changes no song: it plays a version's bounce in the song's place (VersionsHearingTests.cpp)" },
        { "version.deleteBounce", "changes no song: it deletes a version's bounce (VersionsHearingTests.cpp)" },
        { "version.exportDawProject", "changes no song: it writes a file into the Renders folder (DawProjectTests.cpp)" },
    };
    return commands;
}

// What a command of the versions began on the history's thread done, and what came of it (TestSong.h: finished()).
void waitForVersions (phazite::ProjectContext& context)
{
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&context]
        {
            const auto* history = phazite::getVersionHistory (context);
            return history == nullptr || ! history->isReadingForCommand();
        },
        phazite::test::versionHistoryTimeout));
    REQUIRE (phazite::test::waitForVersionHistory (context));
}

// The recordings' options and the transport's switches that the app keeps for every project, as they are at first: the
// script starts from them whatever the tests before it left, and leaves them so.
void resetAppSwitches()
{
    auto& engine = phazite::test::getEngineContext();
    auto& audio = phazite::getAudioIO (engine);
    phazite::setOverdubOn (audio, true);
    phazite::setRecordQuantizeOn (audio, false);
    phazite::setCountInOn (audio, false);
    phazite::getPlaybackHost (engine).setLowLatency (false);
}

//==============================================================================
// A command as the catalogue lists it: id | name | category | description | synonyms | switches, where the switches are
// those of its optional members that are set, or '-' if none is.
juce::String describeCommand (const phazite::Command& command)
{
    juce::StringArray switches;

    if (command.isEnabled != nullptr)
        switches.add ("enabled-when");

    if (command.undoTarget != nullptr)
        switches.add ("undo");

    if (command.isTicked != nullptr)
        switches.add ("ticked");

    if (command.currentName != nullptr)
        switches.add ("current-name");

    if (command.keyReleased != nullptr)
        switches.add ("key-released");

    if (command.keyPriority != 0)
        switches.add ("priority=" + juce::String (command.keyPriority));

    if (command.repeatsWhileHeld)
        switches.add ("repeats");

    if (command.overPlugins)
        switches.add ("over-plugins");

    if (command.needsPluginStates)
        switches.add ("plugin-states");

    if (command.readsPluginStates != nullptr)
        switches.add ("plugin-states-when");

    if (command.changesLater)
        switches.add ("changes-later");

    const auto synonyms = command.synonyms.isEmpty() ? juce::String ("-") : command.synonyms.joinIntoString (", ");
    const auto flags = switches.isEmpty() ? juce::String ("-") : switches.joinIntoString (" ");
    return command.id + " | " + command.name + " | " + command.category + " | " + command.description + " | " + synonyms + " | " + flags;
}

// A command's state as menus and the palette show it: on, or off when greyed out; ticked; and its name now.
juce::String describeState (const phazite::Command& command)
{
    const auto enabled = command.isEnabled == nullptr || command.isEnabled();
    const auto ticked = command.isTicked != nullptr && command.isTicked();
    return juce::String (enabled ? "on" : "off") + (ticked ? ", ticked" : "") + " \"" + phazite::getCurrentName (command) + "\"";
}

// Notes, in the order they were selected, each with its pitch and its start and length in beats, so that what a note
// command did shows: "2 notes (60 at 0.000 for 0.500, 64 at 1.000 for 0.500)".
juce::String describeNotes (const std::vector<phazite::NoteData>& notes)
{
    juce::StringArray described;

    for (const auto& note : notes)
        described.add (juce::String (note.pitch) + " at " + juce::String (note.start.inBeats(), 3) + " for "
                       + juce::String (note.length.inBeats(), 3));

    return juce::String ((int) notes.size()) + (notes.size() == 1 ? " note (" : " notes (") + described.joinIntoString (", ") + ")";
}

// What is selected: clips, then tracks, each in the order they were selected, then sections in the song's order, then notes;
// and the time range: "selected: track One, track Two", or "selected: nothing".
juce::String describeSelected (const phazite::Selection& selection, const phazite::RangeSelection& range)
{
    juce::StringArray items;

    auto* song = selection.getSong();

    if (song != nullptr)
    {
        for (const auto clip : selection.getClips())
        {
            const auto clipTrack = phazite::getClipTrack (*song, clip);
            items.add ("clip on " + (clipTrack.isValid() ? phazite::getTrackName (*song, clipTrack) : juce::String ("no track")) + " at "
                       + juce::String (phazite::getClipRange (*song, clip).getStart().inBeats(), 3));
        }

        for (const auto track : selection.getTracks())
            items.add ("track " + phazite::getTrackName (*song, track));
    }

    for (const auto& section : phazite::getSelectedSections (selection))
        items.add ("section " + section.name);

    if (song != nullptr)
    {
        if (const auto notes = phazite::getNoteData (*song, phazite::getSelectedNotes (selection)); ! notes.empty())
            items.add (describeNotes (notes));
    }

    if (! range.isEmpty() && song != nullptr)
    {
        juce::StringArray names;

        for (const auto track : range.getTracks())
            names.add (phazite::getTrackName (*song, track));

        items.add ("range " + juce::String (range.getRange().getStart().inBeats(), 3) + "-"
                   + juce::String (range.getRange().getEnd().inBeats(), 3) + " beats on " + names.joinIntoString (" and "));
    }

    return "selected: " + (items.isEmpty() ? juce::String ("nothing") : items.joinIntoString (", "));
}

// Where the playhead is, in beats: "playhead at 2.000".
juce::String describePlayhead (const phazite::Song& song)
{
    return "playhead at " + juce::String (phazite::getPlayheadBeat (song).inBeats(), 3);
}

// The loop's range in beats, whether looping is on or not (Loop's tick says which): "loop 2.000-6.000 beats".
juce::String describeLoop (const phazite::Song& song)
{
    const auto loop = phazite::getLoopBeats (song);
    return "loop " + juce::String (loop.getStart().inBeats(), 3) + "-" + juce::String (loop.getEnd().inBeats(), 3) + " beats";
}

// Whether the song plays: "stopped", "playing" or "recording".
juce::String describeTransport (const phazite::Song& song)
{
    return phazite::isRecording (song) ? "recording" : phazite::isSongPlaying (song) ? "playing" : "stopped";
}

//==============================================================================
// The lines in which a golden text and Phazite's differ (phazite::test::compareLines()), at most maxListedDifferences.
juce::StringArray compareLines (const juce::String& expected, const juce::String& actual)
{
    return phazite::test::compareLines (expected, actual, maxListedDifferences);
}

// A golden text file: a line saying what it is, then the lines, each ending in "\n".
juce::String makeGoldenText (const char* about, const juce::StringArray& lines)
{
    return juce::String (about) + "\n" + lines.joinIntoString ("\n") + "\n";
}

std::unique_ptr<phazite::Project> createProject (const juce::File& parentFolder)
{
    juce::String error;
    auto project = phazite::Project::create (parentFolder, "Script", error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project;
}

//==============================================================================
// A project with the model's commands, as the app has them, which a script runs one after another. It records the undo
// step the Edit menu offers after each command (undoLines) and, after each step of the script, the commands whose
// state changed, and what is selected, the playhead, the loop and the transport, each if it changed (stateLines). The
// script's own changes (setup()) are undo steps of their own, named "Script: ...", so that they never join a command's.
// Each command and each change is checked to undo and redo exactly (undoCheck).
struct Script
{
    Script()
    {
        REQUIRE_FALSE (phazite::getTrackIDs (song).empty());
        one = phazite::getTrackIDs (song).front();
        phazite::setTrackName (song, one, "One");
        song.getUndoManager().clearUndoHistory();

        setSong (context, &song);
        addModelCommands (registry, context, [this] (std::function<void (bool)> then) { then (project->save().wasOk()); });
        resetAppSwitches();

        // Its history read, and its initial version made, as a project's are as it starts: the versions' commands' states
        // follow from there, the same in every run.
        REQUIRE (phazite::test::waitForVersionHistory (context));
        undoCheck.emplace (song);
        snapshot ("start");
    }

    ~Script() { resetAppSwitches(); }

    // Runs commands, each of which must be enabled where the script runs it, recording the undo step and the states each
    // leaves.
    void run (std::initializer_list<const char*> ids)
    {
        for (const auto* id : ids)
            record (id);
    }

    // Runs a command, then what must happen before its change is whole (a render it started ends), if anything. Undo
    // and Redo themselves step through the history; any other command's change is checked to undo and redo exactly. A
    // command must change something where the script runs it: the song, a command's state, or what a snapshot holds.
    void record (const juce::String& id, const std::function<void()>& then = nullptr)
    {
        const auto wholeBefore = phazite::test::writeSong (song, phazite::test::SongPart::whole);
        const auto linesBefore = stateLines.size();

        const auto invoke = [this, &id, &then]
        {
            REQUIRE_MESSAGE (registry.invoke (id), ("The script runs " + id + ", which is unknown or greyed out there.").toStdString());

            if (then != nullptr)
                then();
        };

        if (id == "edit.undo" || id == "edit.redo" || id == "edit.undoOrRedo")
            undoCheck->stepBackOrForth (id, invoke);
        else
            undoCheck->check (id, invoke);

        const auto step = song.getUndoManager().getUndoDescription();
        undoLines.add (id + ": " + (step.isEmpty() ? juce::String ("(nothing to undo)") : step));
        snapshot (id);
        commandsRun.addIfNotAlreadyThere (id);

        const auto changedSomething =
            stateLines.size() > linesBefore + 1 || phazite::test::writeSong (song, phazite::test::SongPart::whole) != wholeBefore;
        CHECK_MESSAGE (changedSomething, ("The script runs " + id + " where it changes nothing.").toStdString());
    }

    // Runs a command that starts a render (a freeze, a bounce), and waits for the render: what it makes of the song is
    // the command's change.
    void runRender (const juce::String& id)
    {
        auto& renders = getRenders();
        renders.finished.clear();

        record (id,
                [&renders]
                {
                    REQUIRE (phazite::test::dispatchMessagesUntil ([&renders] { return ! renders.finished.isEmpty(); }));
                    CHECK (renders.finished[0].endsWith (": ok"));
                });
    }

    // Runs a command whose work reads audio in the background (model/arrangement/internal/AudioWork.h), and waits for its
    // change, which it makes once what it read is in.
    void runReading (const juce::String& id)
    {
        record (id, [this] { REQUIRE (phazite::test::dispatchMessagesUntil ([this] { return ! phazite::isReadingAudio (context); })); });
    }

    // Runs a command of the versions, and waits for what it began on the history's thread (a version saved, a song read
    // for a restore) and what came of it.
    void runVersions (const juce::String& id)
    {
        record (id, [this] { waitForVersions (context); });
    }

    // Selects in the Versions view, as a click there does (none of it an undo step): a version, or Now with none; a line of
    // the changes the view lists; a line of what a version changed.
    void selectVersion (const juce::String& what, const juce::String& versionID)
    {
        phazite::selectVersion (context, versionID);
        snapshot ("select " + what);
    }

    void selectChange (const juce::String& what, const phazite::VersionChange& change)
    {
        phazite::selectChange (context, change);
        snapshot ("select " + what);
    }

    void selectVersionChange (const juce::String& what, const juce::String& versionID, const phazite::VersionChange& change)
    {
        phazite::selectVersionChange (context, versionID, change);
        snapshot ("select " + what);
    }

    // The line of the changes listed now (since the last version, or against the version compared) that names a track.
    phazite::VersionChange findListedChange (phazite::TrackID track)
    {
        phazite::test::withChangesIn (context);
        const auto changes = phazite::getListedChanges (context);
        const auto found =
            std::find_if (changes.begin(), changes.end(), [track] (const phazite::VersionChange& change) { return change.track == track; });
        REQUIRE (found != changes.end());
        return *found;
    }

    // A change the script makes itself, as an undo step of its own (or several, which undo and redo exactly together).
    void setup (const juce::String& what, const std::function<void()>& change)
    {
        undoCheck->checkSeveral ("Script: " + what, change);
        snapshot (what);
    }

    // The engine plays on for so long, as the audio device would have it.
    void play (double seconds)
    {
        REQUIRE (engine != nullptr);
        engine->play (seconds);
        snapshot ("playing for " + juce::String (seconds, 2) + " s");
    }

    // Selects the tracks alone, in this order: the first with selectOnly(), the others added to it.
    void select (const juce::String& names, std::initializer_list<phazite::TrackID> tracks)
    {
        auto isFirst = true;

        for (const auto track : tracks)
        {
            if (isFirst)
                selection.selectOnly (track);
            else
                selection.addToSelection (track);

            isFirst = false;
        }

        snapshot ("select " + names);
    }

    // Selects a clip alone.
    void select (const juce::String& names, phazite::ClipID clip)
    {
        selection.selectOnly (clip);
        snapshot ("select " + names);
    }

    void selectNothing()
    {
        selection.deselectAll();
        snapshot ("select nothing");
    }

    // The one selected track: a folder, or a track that isn't.
    phazite::TrackID getSelectedTrack (bool folder) const
    {
        const auto tracks = phazite::getSelectedTracks (selection);
        REQUIRE (tracks.size() == 1);
        REQUIRE (phazite::isFolder (song, tracks.front()) == folder);
        return tracks.front();
    }

    // Adds 'after <what>:', the commands whose state changed since the last snapshot (all of them the first time), and
    // what is selected, where the playhead is and the loop's range, each if it changed.
    void snapshot (const juce::String& what)
    {
        // The versions' commands as they stand once the changes since the last version are worked out, in the background.
        phazite::test::withChangesIn (context);
        stateLines.add ("after " + what + ":");

        for (const auto* command : registry.getAll())
        {
            const auto state = describeState (*command);

            if (const auto last = states.find (command->id); last == states.end() || last->second != state)
            {
                stateLines.add ("  " + command->id + ": " + state);
                states[command->id] = state;
            }
        }

        addIfChanged (selected, describeSelected (selection, range));

        // While the song plays, the playhead between two blocks is told by the clock, which no golden file can hold: it is
        // noted once the song has stopped, after the engine has played a block, in which it takes the stop it was told of.
        if (! phazite::isSongPlaying (song))
        {
            if (engine != nullptr)
                engine->play (phazite::test::EnginePlayback::blockSize / phazite::test::EnginePlayback::sampleRate);

            addIfChanged (playhead, describePlayhead (song));
        }

        addIfChanged (loop, describeLoop (song));
        addIfChanged (transport, describeTransport (song));
    }

    // Fails (CHECK) for each command the registry has that the script didn't run, unless getCommandsNotRun() says why it
    // can't; and for each one listed there that the script ran after all, or that isn't a command any more.
    void checkEveryCommandRan() const
    {
        juce::StringArray notRun;

        for (const auto& [id, why] : getCommandsNotRun())
        {
            notRun.add (id);
            CHECK_MESSAGE (why.isNotEmpty(), (id + " is listed as a command the script can't run, without why").toStdString());
            CHECK_MESSAGE (registry.find (id) != nullptr,
                           (id + " is listed as a command the script can't run, but is no command").toStdString());
            CHECK_MESSAGE (! commandsRun.contains (id),
                           (id + " is listed as a command the script can't run, but it runs it").toStdString());
        }

        for (const auto* command : registry.getAll())
            CHECK_MESSAGE ((commandsRun.contains (command->id) || notRun.contains (command->id)),
                           ("The command script doesn't run " + command->id
                            + ": run it there where it does something (tests/CommandGoldenTests.cpp), or say in getCommandsNotRun() "
                              "why it can't")
                               .toStdString());
    }

    // Adds a line of state unless the last snapshot added the same one.
    void addIfChanged (juce::String& last, const juce::String& now)
    {
        if (now != last)
        {
            stateLines.add ("  " + now);
            last = now;
        }
    }

    // Destroyed in the reverse order: the notes' selection first, then the commands, the selection and the project.
    phazite::test::TemporaryFolder temp;
    std::unique_ptr<phazite::Project> project = createProject (temp.folder);
    phazite::Song& song = project->getSong();
    phazite::TrackID one;
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // the open project, as the app has it
    phazite::Selection& selection = getSelection (context);
    phazite::RangeSelection& range = getRangeSelection (context);
    phazite::CommandRegistry registry;
    std::optional<phazite::test::UndoCheck> undoCheck; // from the script's start
    phazite::test::EnginePlayback* engine = nullptr;   // what drives the engine, while the script has it play

    std::map<juce::String, juce::String> states;      // each command's state at the last snapshot
    juce::String selected, playhead, loop, transport; // the lines the last snapshot had for them
    juce::StringArray undoLines, stateLines;
    juce::StringArray commandsRun; // the ids of the commands run
};
} // namespace

//==============================================================================
TEST_SUITE ("golden")
{
    TEST_CASE ("Golden commands: the model's commands keep their ids, texts, switches and order")
    {
        phazite::ProjectContext context { phazite::test::getEngineContext() };
        phazite::CommandRegistry registry;
        addModelCommands (registry, context, [] (std::function<void (bool)> then) { then (true); });

        juce::StringArray lines;

        for (const auto* command : registry.getAll())
            lines.add (describeCommand (*command));

        phazite::test::checkGoldenText ("commands/catalogue.txt", makeGoldenText (aboutTheCatalogue, lines), compareLines);
    }

    TEST_CASE ("Golden commands: a script of every model command, with the undo step and the states each leaves")
    {
        Script s;
        const auto one = s.one;

        // Tracks: add, name and group them, change their heights, pin, collapse, mute and solo them, move them and move
        // through them. One, inside a muted folder, isn't muted itself, so Mute Track isn't ticked for it.
        s.select ("One", { one });
        s.run ({ "track.add" });

        phazite::TrackID two;
        s.setup ("name the new track Two",
                 [&s, &two]
                 {
                     two = s.getSelectedTrack (false);
                     phazite::setTrackName (s.song, two, "Two");
                 });

        s.run ({ "track.addFolder" });
        s.setup ("name the new folder Empty", [&s] { phazite::setTrackName (s.song, s.getSelectedTrack (true), "Empty"); });
        s.run ({ "track.ungroup" });

        s.select ("One", { one });
        s.run ({ "track.taller", "track.shorter" });
        s.selectNothing();
        s.run ({ "track.taller" }); // every track

        s.select ("One", { one });
        s.run ({ "track.pin", "track.collapse", "track.collapse" });
        s.selectNothing();
        s.run ({ "track.collapseAll", "track.collapseAll" }); // every row a thin one, and at full height again

        s.select ("One and Two", { one, two });
        s.run ({ "track.groupIntoFolder" });

        phazite::TrackID group;
        s.setup ("name the new folder Group",
                 [&s, &group]
                 {
                     group = s.getSelectedTrack (true);
                     phazite::setTrackName (s.song, group, "Group");
                 });

        s.run ({ "track.mute" }); // the folder
        s.select ("One", { one });
        s.run ({ "track.mute", "track.mute", "track.solo", "track.solo" });
        s.select ("Group", { group });
        s.run ({ "track.mute" });

        s.select ("Two", { two });
        s.run ({ "track.moveUp", "track.moveDown" });
        s.selectNothing();
        s.run ({ "track.selectNext" }); // the first track
        s.selectNothing();
        s.run ({ "track.selectPrevious" }); // the last track

        s.select ("One", { one });
        s.run ({ "track.addReturn" });
        s.select ("One", { one });
        s.run ({ "edit.duplicate", "edit.delete" }); // the copy goes, and the track in its place is selected

        // Sections and outlines, each outline in place of the one before.
        s.selectNothing();
        s.run ({ "section.add", "edit.duplicate", "edit.duplicateAsAlias", "section.loop", "edit.delete", "section.outline.electronic",
                 "section.outline.hipHop", "section.outline.lofi", "section.outline.rnb", "section.outline.drumAndBass",
                 "section.outline.techno", "section.outline.pop", "section.outline.rock" });

        // A MIDI clip on One, and its notes: half a beat long, a beat apart, so that legato makes the first one longer.
        phazite::ClipID clip;
        s.setup ("add a MIDI clip on One, with two notes",
                 [&s, &clip, one]
                 {
                     auto& song = s.song;
                     clip = phazite::insertMidiClip (song, one, { beats (0.0), beats (4.0) }, &s.selection);
                     REQUIRE (clip.isValid());
                     REQUIRE (s.selection.isSelected (clip));
                     REQUIRE (phazite::addNote (song, clip, 60, beats (0.0), phazite::BeatDuration::fromBeats (0.5), 100).isValid());
                     REQUIRE (phazite::addNote (song, clip, 64, beats (1.0), phazite::BeatDuration::fromBeats (0.5), 100).isValid());
                 });

        s.run ({ "edit.mute", "edit.mute", "edit.duplicate", "edit.delete" });
        s.select ("the clip", clip);
        s.run ({ "edit.duplicateAsAlias", "clip.makeUnique" });
        s.select ("the clip", clip);
        s.run ({ "transport.goToEnd", "transport.goToStart" });
        s.setup ("put the playhead at 1 s", [&s] { phazite::movePlayhead (s.song, beats (2.0)); });
        s.run ({ "edit.split" });

        const auto selectTheClipsNotes = [&s, &clip]
        {
            const auto notes = phazite::getRefs (phazite::getClipNotes (s.song, clip));

            s.selection.selectOnly (clip);
            s.selection.setNoteClips ({ clip });
            s.selection.selectNotes (notes);
        };

        s.setup ("select the clip's notes", selectTheClipsNotes);

        s.run ({ "note.transposeUp", "note.transposeDown", "note.octaveUp", "note.octaveDown", "note.lengthen", "note.shorten",
                 "note.legato", "note.chop", "note.slide", "note.slide", "note.portamento", "note.portamento", "edit.mute", "edit.mute",
                 "edit.duplicate", "edit.delete" });

        // The notes copied, pasted at the playhead into the clip the piano roll shows, and the pasted ones cut.
        s.setup ("select the clip's notes again, and put the playhead at beat 1",
                 [&s, &selectTheClipsNotes]
                 {
                     selectTheClipsNotes();
                     phazite::movePlayhead (s.song, beats (1.0));
                 });

        s.run ({ "edit.copy", "edit.paste", "edit.cut" });

        s.setup ("select nothing, and drop the notes' selection",
                 [&s]
                 {
                     s.selection.deselectAll();
                     s.selection.setNoteClips ({});
                 });

        // An audio clip on Two: its fade shapes (linear last, as a new clip's fades are linear already), and deleting it
        // with the gap closed.
        s.setup ("import a tone onto Two at beat 8, and select it",
                 [&s, two]
                 {
                     auto& song = s.song;
                     const auto tone = phazite::test::writeSound (s.temp.folder.getChildFile ("Tone.wav"), 2.0);
                     const auto audio = phazite::importFile (song, two, tone, beats (8.0));
                     REQUIRE (phazite::hasAudioClips (song, { audio }));
                     s.selection.selectOnly (audio);
                 });

        s.run ({ "clip.fadeConvex", "clip.fadeConcave", "clip.fadeSCurve", "clip.fadeLinear" });

        // What it plays: backwards and forwards again, following the song's tempo and at its own speed again (by its own
        // switch, and by the song's for every sample), and normalized.
        s.run ({ "clip.reverse", "clip.reverse", "clip.followTempo", "clip.followTempo", "transport.samplesFollowTempo",
                 "transport.samplesFollowTempo" });
        s.runReading ("clip.normalize");

        // Copied, pasted at the playhead onto its track, and the pasted one cut.
        s.run ({ "edit.copy" });
        s.setup ("put the playhead at beat 16", [&s] { phazite::movePlayhead (s.song, beats (16.0)); });
        s.run ({ "edit.paste", "edit.cut" });

        // Its track frozen and live again, and bounced in place: the bounce is a new track under it, selected.
        s.select ("Two", { two });
        s.runRender ("track.freeze");
        s.run ({ "track.freeze" });
        s.runRender ("track.bounceInPlace");

        s.setup ("select the tone on Two",
                 [&s, two]
                 {
                     const auto clips = phazite::getClips (s.context, two);
                     REQUIRE (clips.size() == 1);
                     s.selection.selectOnly (clips.front().id);
                 });

        s.run ({ "edit.deleteAndCloseGap" });

        // A time range on One, which ends inside a clip, so that Split cuts it there: time inserted and removed, the range
        // looped and split, duplicated (the copy replaces the part Split cut off, and becomes the range) and deleted (the
        // copy goes).
        s.setup ("select beats 2 to 6 on One",
                 [&s, one]
                 {
                     s.selection.deselectAll();
                     s.range.set ({ beats (2.0), beats (6.0) }, { one });
                 });

        s.run ({ "edit.insertTime", "edit.removeTime", "transport.loopSelection", "edit.split", "edit.duplicate", "edit.delete" });

        // The range is looped: Punch In punches at it, and not; Loop turns looping off and on again, Loop the Song makes
        // the loop follow the song again, and, with looping off, Loop Selection without a range loops the song.
        s.setup ("clear the range", [&s] { s.range.clear(); });
        s.run ({ "transport.punch", "transport.punch", "transport.loop", "transport.loop", "transport.loopSong", "transport.loop",
                 "transport.loopSelection" });

        // Devices: two reverbs on One, grouped into a container, back on the track, and grouped again.
        s.setup ("add two reverbs to One, and select them",
                 [&s, one]
                 {
                     using phazite::test::makeBuiltInDevice;
                     const auto first =
                         phazite::insertDeviceState (s.song, one, makeBuiltInDevice (s.song, phazite::devices::Reverb::typeID));
                     const auto second =
                         phazite::insertDeviceState (s.song, one, makeBuiltInDevice (s.song, phazite::devices::Reverb::typeID));
                     REQUIRE (first.isValid());
                     REQUIRE (second.isValid());
                     s.selection.selectOnly (first);
                     s.selection.addToSelection (second);
                 });

        s.run ({ "device.group", "device.ungroup", "device.group" });

        // Automation: One's lanes shown (it gets its volume's) and collapsed, how it writes its automation, points of its
        // volume raised and lowered, and a lane for the pan, turned last.
        const phazite::ParameterRef volume { phazite::getFader (s.song, one), phazite::getVolumeParameterID (s.song, one) };
        s.select ("One", { one });
        s.run ({ "automation.showLanes", "automation.showLanes", "automation.touch", "automation.latch", "automation.write",
                 "automation.read" });

        s.setup ("add two points to One's volume, and select them",
                 [&s, &volume]
                 {
                     REQUIRE (volume.isValid());
                     phazite::addPoint (s.song, volume, { beats (0.0), 0.5f, 0.0f });
                     phazite::addPoint (s.song, volume, { beats (4.0), 0.625f, 0.0f });
                     phazite::selectPoints (s.selection, volume, { 0, 1 });
                 });

        s.run ({ "automation.raisePoints", "automation.lowerPoints", "automation.raisePointsMore", "automation.lowerPointsMore" });
        s.setup ("turn One's pan",
                 [&s, one]
                 {
                     phazite::getPan (s.context, one).setValue (0.25f);
                     phazite::test::dispatchPendingMessages(); // the parameter touched last is known a message later
                 });
        s.run ({ "automation.automateLastTouched" });

        // MIDI learn for it, then cancelled: neither changes the song (the link is made as a controller comes).
        s.run ({ "midi.learnLastTouched", "midi.learnLastTouched" });

        // A lane removed between two undo steps: lanes come and go outside the undo history, which must still find what
        // each step changed on the track (the walk at the script's end undoes the clip).
        const auto addClipWithNote = [&s, one] (double beat)
        {
            const auto added = phazite::insertMidiClip (s.song, one, { beats (beat), beats (beat + 4.0) }, nullptr);
            REQUIRE (added.isValid());
            REQUIRE (phazite::addNote (s.song, added, 60, beats (beat), phazite::BeatDuration::fromBeats (1.0), 100).isValid());
            return added;
        };

        phazite::ClipID third, fourth;
        s.setup ("add a clip to One at beat 32", [&third, &addClipWithNote] { third = addClipWithNote (32.0); });
        s.setup ("remove the lane of One's pan",
                 [&s, one] { phazite::removeAutomationLane (s.context, phazite::getPan (s.context, one).getRef()); });
        s.setup ("add a clip to One at beat 40, and select both",
                 [&s, &third, &fourth, &addClipWithNote]
                 {
                     fourth = addClipWithNote (40.0);
                     s.selection.selectOnly (third);
                     s.selection.addToSelection (fourth);
                 });

        // The two clips joined into one, which lasts from the first's start to the second's end.
        s.run ({ "clip.consolidate" });

        // The switches: the metronome and the patterns' tempo are the song's, the others the app's. And a tempo change at
        // the playhead, taken away, and added again.
        s.run ({ "transport.metronome", "transport.metronome", "transport.countIn", "transport.countIn", "transport.overdub",
                 "transport.overdub", "transport.quantizeRecordings", "transport.quantizeRecordings", "transport.lowLatency",
                 "transport.lowLatency", "transport.patternsFollowTempo", "transport.patternsFollowTempo" });
        s.setup ("put the playhead at beat 8", [&s] { phazite::movePlayhead (s.song, beats (8.0)); });
        s.run ({ "transport.addTempoChange", "transport.removeTempoChanges", "transport.addTempoChange" });

        // What needs the song to play: the engine driven a block at a time, its output coming back into its two inputs.
        {
            phazite::test::EnginePlayback playback { phazite::test::EnginePlayback::Input::loopback };
            s.engine = &playback;

            // A track that records input 1, armed; recorded round a looped range of two beats, twice and a half: a clip of
            // three takes, the second playing, then the first, then the second again.
            phazite::TrackID take;
            s.setup ("add a track Take, which records input 1 unheard, loop beats 0 to 2, and put the playhead at the start",
                     [&s, &take]
                     {
                         take = phazite::addTrackAtEnd (s.song, false, nullptr);
                         REQUIRE (take.isValid());
                         phazite::setTrackName (s.song, take, "Take");
                         phazite::setTrackInput (s.context, take, "1");
                         phazite::setInputMonitoring (s.context, take, phazite::InputMonitoring::never);
                         phazite::loopBeatRange (s.song, 0.0, 2.0);
                         phazite::movePlayhead (s.song, beats (0.0));
                     });

            s.select ("Take", { take });
            s.run ({ "track.arm", "transport.record" });
            s.play (2.5);
            s.run ({ "transport.record" });

            s.setup ("select the clip recorded",
                     [&s, take]
                     {
                         const auto clips = phazite::getClips (s.context, take);
                         REQUIRE (clips.size() == 1);
                         s.selection.selectOnly (clips.front().id);
                     });

            s.run ({ "clip.previousTake", "clip.nextTake" });
            s.select ("Take", { take });
            s.run ({ "track.arm" });

            // Play / Stop goes back to where playback started; Play / Pause stays where it stopped.
            s.run ({ "transport.playStop" });
            s.play (0.5);
            s.run ({ "transport.playStop", "transport.playPause" });
            s.play (0.5);
            s.run ({ "transport.playPause" });

            // Capture: four notes played live on a track with an instrument, while the song is stopped, become a clip. Each
            // note is where the clock had it within its block, which differs from run to run: the clip is deleted again, so
            // that the project the script leaves is the same every time.
            s.setup ("add a track Keys with a synth, which live MIDI plays",
                     [&s]
                     {
                         const auto keys = phazite::addTrackAtEnd (s.song, false, nullptr);
                         REQUIRE (keys.isValid());
                         phazite::setTrackName (s.song, keys, "Keys");
                         REQUIRE (phazite::setInstrument (s.song, keys,
                                                          phazite::test::makeBuiltInDevice (s.song, phazite::devices::Synth::typeID))
                                      .isValid());
                         s.selection.selectOnly (keys);
                         phazite::routeLiveMidiInput (s.song);
                         REQUIRE (phazite::getLiveMidiTrack (s.song) == keys);
                     });

            auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());

            for (const auto pitch : { 60, 64, 67, 72 })
            {
                playback.play (0.25);
                audio.playLive (juce::MidiMessage::noteOn (1, pitch, 0.8f));
                playback.play (0.25);
                audio.playLive (juce::MidiMessage::noteOff (1, pitch));
            }

            s.run ({ "transport.capture", "edit.delete" });
            s.engine = nullptr;
        }

        // The versions: the song so far saved as a version; Two renamed and its change discarded, all of it and as a line;
        // renamed again and saved as a second version; renamed a third time, and its line restored from the second version;
        // compared with the first, and the comparison closed; the whole song restored from the first; the second version
        // undone (the song stays as it is); and a branch made from Now, where the song goes on.
        const auto renameTwo = [&s, two] (const juce::String& name)
        { s.setup ("name Two " + name, [&s, two, name] { phazite::setTrackName (s.song, two, name); }); };

        s.runVersions ("version.save");
        const auto firstVersion = phazite::getLastVersion (s.context)->id;
        renameTwo ("Second");
        s.runVersions ("version.discardAll");
        renameTwo ("Second");
        s.selectChange ("the change of Two", s.findListedChange (two));
        s.runVersions ("version.putBack");
        renameTwo ("Second");
        s.runVersions ("version.save");
        const auto secondVersion = phazite::getLastVersion (s.context)->id;
        const auto secondsLine = [&s, &secondVersion, two]
        {
            // Read in the background the first time they are asked for.
            REQUIRE (phazite::test::dispatchMessagesUntil (
                [&]
                {
                    return ! phazite::getVersionChanges (s.context, secondVersion).empty()
                           && ! phazite::isReadingVersionChanges (s.context, secondVersion);
                },
                phazite::test::versionHistoryTimeout));
            const auto changes = phazite::getVersionChanges (s.context, secondVersion);
            const auto found =
                std::find_if (changes.begin(), changes.end(), [two] (const phazite::VersionChange& change) { return change.track == two; });
            REQUIRE (found != changes.end());
            return *found;
        };

        renameTwo ("Third");
        s.selectVersionChange ("the second version's change of Two", secondVersion, secondsLine());
        s.runVersions ("version.restoreChange");
        s.selectVersion ("the first version", firstVersion);
        s.runVersions ("version.takeFrom");
        s.runVersions ("version.showChanges");
        s.runVersions ("version.bringBack");
        s.runVersions ("version.undoLast");
        s.selectVersion ("Now", {});
        s.runVersions ("version.newBranch");

        // Selecting everything: clips, or tracks while tracks are selected.
        s.selectNothing();
        s.run ({ "edit.selectAll" });
        s.select ("One", { one });
        s.run ({ "edit.selectAll", "edit.deselectAll" });

        // Undo and redo.
        s.run ({ "edit.undo", "edit.redo", "edit.undoOrRedo", "edit.undoOrRedo" });

        s.range.clear();
        s.selection.deselectAll();
        s.selection.setNoteClips ({});
        phazite::test::dispatchPendingMessages();

        // Every command ran, and every step of the script undoes and redoes to the song as it was there.
        s.checkEveryCommandRan();
        s.undoCheck->walkBackAndForth();
        phazite::test::dispatchPendingMessages();

        REQUIRE (s.project->save().wasOk());
        phazite::test::checkProjectGolden ("commands/script.xml", *phazite::test::readNormalisedProject (s.project->getFile()),
                                           aboutTheProject);
        phazite::test::checkGoldenText ("commands/script-undo.txt", makeGoldenText (aboutTheUndoSteps, s.undoLines), compareLines);
        phazite::test::checkGoldenText ("commands/script-states.txt", makeGoldenText (aboutTheStates, s.stateLines), compareLines);
    }
}

//==============================================================================
namespace
{
// Why the window's commands below change no song: what each does instead.
constexpr const char* showsView = "shows, hides, zooms or arranges a part of the window: view state outside the song";
constexpr const char* opensField = "opens a field or a menu, and changes nothing until what is typed or chosen there is "
                                   "applied, by a model function that has tests of its own";
constexpr const char* selects = "selects or deselects: no undo step";
constexpr const char* projectFile = "opens, saves or writes a project or a file, or opens a dialog for one: no edit of the song";
constexpr const char* history = "changes the versions' history or how the Versions view shows it, not the song (VersionsTests.cpp)";
constexpr const char* setting = "switches a setting of the app or of a view, outside the song";
constexpr const char* help = "shows help or a lesson over the window, or opens an example song as another project "
                             "(LessonsTests.cpp, ExampleSongTests.cpp)";

// The commands the whole window registers beyond the model's (the project session's, the shell's and the editors':
// AppScreen.h) that the case below doesn't run, each with why: none of them changes the song in an undo step (one, Import
// DAWproject, switches it to a branch it makes, as a switch does). A new command of the window is either run there or listed
// here on purpose.
const std::vector<std::pair<juce::String, juce::String>>& getWindowCommandsNotRun()
{
    static const std::vector<std::pair<juce::String, juce::String>> commands {
        { "help.example.exodus", help },
        { "help.example.loFiBeat", help },
        { "help.example.houseGroove", help },
        { "project.new", projectFile },
        { "project.open", projectFile },
        { "project.openRecent", projectFile },
        { "project.saveAs", projectFile },
        { "project.save", projectFile },
        { "project.autoSave", setting },
        { "project.export", projectFile },
        { "project.cleanUpAudio", "opens the Project view's list of the audio files no version plays, to delete: files, not the song "
                                  "(CleanUpTests.cpp)" },
        { "project.importDawProject",
          "opens a DAWproject file chosen in a dialog: as a new project in the open one's place, or, one exported from a version "
          "of this project, as a new branch of its versions from that version, which the song then switches to as "
          "version.switchBranch does (its undo history started again, so no UndoCheck can walk back through it); "
          "DawProjectTests.cpp runs the branch (\"Changed in another DAW and imported back: a branch from its version with just "
          "those changes, which merges\", and \"A DAWproject imported back unpacks its audio off the message thread, and one "
          "cancelled leaves none of it\")" },
        { "view.commandPalette", showsView },
        { "view.menus", showsView },
        { "view.searchBrowser", showsView },
        { "view.browser", showsView },
        { "view.project", showsView },
        { "view.versions", showsView },
        { "version.pickBranch", opensField },
        { "version.renameBranch", history },
        { "version.mergeBranch", opensField },
        { "version.deleteBranch", history },
        { "version.recover", history },
        { "version.editMessage", history },
        { "version.bounceEach", history },
        { "version.showAutomatic", history },
        { "device.add", opensField },
        { "device.savePreset", projectFile },
        { "track.savePreset", projectFile },
        { "view.toggleLeftPanel", showsView },
        { "view.maximizeLeftPanel", showsView },
        { "view.toggleSong", showsView },
        { "view.maximizeSong", showsView },
        { "view.toggleEditor", showsView },
        { "view.maximizeEditor", showsView },
        { "view.toggleInspector", showsView },
        { "view.maximizeInspector", showsView },
        { "view.maximizePanel", showsView },
        { "view.pianoRoll", showsView },
        { "view.instrument", showsView },
        { "view.effects", showsView },
        { "device.show", showsView },
        { "view.playlist", showsView },
        { "view.mixer", showsView },
        { "view.layoutBeat", showsView },
        { "view.layoutMix", showsView },
        { "view.layoutFull", showsView },
        { "view.scrollContinuously", setting },
        { "view.resetViewPlaces", showsView },
        { "view.detachLeftPanel", showsView },
        { "view.detachInspector", showsView },
        { "view.detachEditor", showsView },
        { "view.zoomIn", showsView },
        { "view.zoomOut", showsView },
        { "view.zoomToFit", showsView },
        { "view.zoomToSelection", showsView },
        { "view.zoomBack", showsView },
        { "view.centreOnPlayhead", showsView },
        { "view.followPlayback", setting },
        { "tool.smart", setting },
        { "tool.draw", setting },
        { "tool.paint", setting },
        { "tool.slice", setting },
        { "tool.slip", setting },
        { "tool.mute", setting },
        { "tool.delete", setting },
        { "tool.select", setting },
        { "tool.zoom", setting },
        { "automation.typeValue", opensField },
        { "edit.rename", opensField },
        { "track.color", opensField },
        { "view.focusMode", showsView },
        { "note.selectAll", selects },
        { "note.deselectAll", selects },
        { "pianoRoll.ghostNotes", setting },
        { "pianoRoll.snapToKey", setting },
        { "help.guide", help },
        { "help.lesson.beat", help },
        { "help.lesson.recording", help },
        { "help.lesson.arranging", help },
        { "help.lesson.effects", help },
        { "help.lesson.chords", help },
        { "help.stopLesson", help },
    };
    return commands;
}
} // namespace

TEST_CASE ("Every command of the whole window that changes the song, run once there, undoes and redoes exactly")
{
    // The window's commands beyond the model's, which the script above runs: on a song of a synth's track with a MIDI clip
    // of a chord and a note off the grid, and a track with an audio clip, each run once through an UndoCheck where it does
    // something (the playlist's nudges and quantize on the MIDI clip, the grid's switches, Quantize Audio on the audio clip;
    // the piano roll's transforms, each applied, and one cancelled, its drum mode and its key on every track), and every
    // step walked back and forth at the end. Every other command of the window is listed with why it changes no song.
    using phazite::test::dispatchPendingMessages;
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    dispatchPendingMessages();
    auto& context = screen.session.getContext();
    auto& song = *phazite::findSong (context);
    auto& selection = getSelection (context);

    const auto tracks = phazite::ensureTracks (song, 2);
    REQUIRE (phazite::setInstrument (song, tracks[0], phazite::test::makeBuiltInDevice (song, phazite::devices::Synth::typeID)).isValid());
    const auto clip = phazite::insertMidiClip (song, tracks[0], { beats (4.3), beats (8.3) }, nullptr);
    REQUIRE (clip.isValid());

    for (const auto& [pitch, start] : { std::pair { 60, 0.0 }, std::pair { 64, 0.0 }, std::pair { 67, 0.0 }, std::pair { 72, 2.1 } })
        REQUIRE (phazite::addNote (song, clip, pitch, beats (4.3 + start), phazite::BeatDuration::fromBeats (1.0), 100).isValid());

    phazite::setTrackKey (song, tracks[0], phazite::Key { 2, phazite::Scale::minor });
    const auto tone = phazite::test::writeSound (screen.temp.folder.getChildFile ("Tone.wav"), 2.0);
    const auto audio = phazite::importFile (song, tracks[1], tone, beats (0.0));
    REQUIRE (phazite::hasAudioClips (song, { audio }));
    song.getUndoManager().clearUndoHistory();
    dispatchPendingMessages();

    phazite::test::UndoCheck check (context);
    juce::StringArray run;

    // What a command changes: the song, in an undo step; the project's view state alone; or nothing the project keeps (a
    // view's own setting, a change previewed and taken back).
    enum class Changes
    {
        theSong,
        theView,
        nothing
    };

    const auto runChecked = [&] (const juce::String& what, Changes changes, const std::function<void()>& change)
    {
        CAPTURE (what.toStdString());
        const auto undoable = phazite::test::writeSong (song, phazite::test::SongPart::undoable);
        const auto whole = phazite::test::writeSong (song, phazite::test::SongPart::whole);
        check.check (what,
                     [&]
                     {
                         change();
                         dispatchPendingMessages();
                     });
        const auto undoableAfter = phazite::test::writeSong (song, phazite::test::SongPart::undoable);
        const auto wholeAfter = phazite::test::writeSong (song, phazite::test::SongPart::whole);
        CHECK ((undoableAfter != undoable) == (changes == Changes::theSong));

        if (changes == Changes::theView)
            CHECK (wholeAfter != whole);
        else if (changes == Changes::nothing)
            CHECK (wholeAfter == whole);
    };

    const auto command = [&] (const juce::String& id, Changes changes)
    {
        runChecked (id, changes, [&] { REQUIRE (screen.registry.invoke (id)); });
        run.add (id);
    };

    // The playlist's, on the MIDI clip, which starts off the grid: a step of the grid later and earlier, and its start to the
    // grid; and the grid's switches, each to and fro, which the project keeps for the editor (its view state).
    screen.showView<phazite::ui::Playlist> ("view.playlist");
    selection.selectOnly (clip);
    dispatchPendingMessages();

    for (const auto* id : { "edit.nudgeRight", "edit.nudgeLeft", "edit.quantize" })
        command (id, Changes::theSong);

    for (const auto* id :
         { "grid.snap", "grid.snap", "grid.adaptive", "grid.adaptive", "grid.finer", "grid.coarser", "grid.triplets", "grid.triplets" })
        command (id, Changes::theView);

    // Quantize Audio, whose step comes once the hits are found in the background.
    selection.selectOnly (audio);
    dispatchPendingMessages();
    runChecked ("clip.quantizeAudio", Changes::theSong,
                [&]
                {
                    REQUIRE (screen.registry.invoke ("clip.quantizeAudio"));
                    REQUIRE (phazite::test::runTimersUntil ([&context] { return ! phazite::isReadingAudio (context); }));
                });
    run.add ("clip.quantizeAudio");

    // The piano roll's, on the MIDI clip's notes: each transform opened and applied as one step; one opened and cancelled; its
    // drum mode (the track's view state) on and off; the track's key given to every track.
    selection.selectOnly (clip);
    dispatchPendingMessages();
    auto& roll = screen.showView<phazite::ui::PianoRoll> ("view.pianoRoll");
    roll.setActive (true);
    dispatchPendingMessages();

    const auto transform =
        [&] (const juce::String& open, const juce::String& close, Changes changes, const std::function<void()>& adjust = nullptr)
    {
        REQUIRE (screen.registry.invoke ("note.selectAll"));
        dispatchPendingMessages();
        runChecked (open + ", then " + close, changes,
                    [&]
                    {
                        REQUIRE (screen.registry.invoke (open));
                        dispatchPendingMessages();

                        if (adjust != nullptr)
                            adjust();

                        REQUIRE (screen.registry.invoke (close));
                    });
        run.addIfNotAlreadyThere (open);
        run.addIfNotAlreadyThere (close);
    };

    for (const auto* id : { "note.quantizeSettings", "note.legatoSettings", "note.chopSettings", "note.strumSettings",
                            "note.humanizeSettings", "note.arpeggiateSettings" })
        transform (id, "note.applyTransform", Changes::theSong);

    // Transpose, which moves nothing until its amount is set: two steps of the track's key up.
    transform ("note.transposeSettings", "note.applyTransform", Changes::theSong,
               [&roll]
               {
                   auto* steps = dynamic_cast<juce::Slider*> (phazite::test::findComponentWithTitle (roll, "Steps"));

                   if (steps == nullptr)
                       steps = dynamic_cast<juce::Slider*> (phazite::test::findComponentWithTitle (roll, "Semitones"));

                   REQUIRE (steps != nullptr);
                   steps->setValue (2.0, juce::sendNotificationSync);
               });

    transform ("note.quantizeSettings", "note.cancelTransform", Changes::nothing);
    command ("pianoRoll.drumMode", Changes::theView);
    command ("pianoRoll.drumMode", Changes::theView);
    command ("pianoRoll.keyForAllTracks", Changes::theSong);

    check.walkBackAndForth();
    phazite::test::checkSong (song);

    // Every command of the window that the model's script doesn't run, run here or listed with why.
    phazite::ProjectContext modelContext { phazite::test::getEngineContext() };
    phazite::CommandRegistry model;
    addModelCommands (model, modelContext, [] (std::function<void (bool)> then) { then (true); });
    juce::StringArray notRun;

    for (const auto& [id, why] : getWindowCommandsNotRun())
    {
        notRun.add (id);
        CHECK_MESSAGE (screen.registry.find (id) != nullptr,
                       (id + " is listed as a window command not run, but is no command").toStdString());
        CHECK_MESSAGE (! run.contains (id), (id + " is listed as a window command not run, but the case runs it").toStdString());
    }

    for (const auto* each : screen.registry.getAll())
        if (model.find (each->id) == nullptr)
            CHECK_MESSAGE ((run.contains (each->id) || notRun.contains (each->id)),
                           ("The whole window's case doesn't run " + each->id
                            + ": run it there through its UndoCheck (tests/CommandGoldenTests.cpp), or say in "
                              "getWindowCommandsNotRun() why it changes no song")
                               .toStdString());
}
