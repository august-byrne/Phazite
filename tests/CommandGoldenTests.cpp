// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden files of the model's commands (tests/golden/commands): their catalogue (each command's id, texts and switches,
// in the order the app registers them, which decides which of two commands on one key runs and the order of the
// shortcuts list), and a script that runs every model command where it does something: the test fails for a command the
// registry has that the script doesn't run (getCommandsNotRun() lists those that can't run in it, with why: none now), and
// for one it runs where it changes nothing. The script's golden files hold the project it leaves (script.xml), the undo
// step the Edit menu offers after each command (script-undo.txt), and, after each step, every command whose state changed
// (enabled or greyed out, ticked, and the name menus show), and what is selected (notes with their pitches, starts and
// lengths), where the playhead is, the loop's range and whether the song plays or records, each if it changed
// (script-states.txt). GoldenFiles.h says how to regenerate them.
//
// Every command the script runs, and every change it makes itself, goes through an UndoCheck (UndoCheck.h): Undo gives
// the song as it was before, and Redo as it was after; and at its end, the script's every step is undone and redone, the
// song at each as it was there.
#include "EnginePlayback.h"
#include "GoldenFiles.h"
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

// The model's commands, registered as the app registers them: addSongCommands(), which Main.cpp calls, then the note
// commands as AppShell adds them, with a grid of a quarter beat.
void addModelCommands (phazite::CommandRegistry& registry, phazite::ProjectContext& context)
{
    phazite::addSongCommands (registry, context, getRenders());
    phazite::addNoteCommands (registry, getSelection (context), [] { return 0.25; });
}

// The model's commands that the script can't run, each with why, so that a new command is either run by the script or
// listed here on purpose: none. (The commands that need the song to play, a device's input or notes played live run
// while the engine is driven a block at a time: EnginePlayback.h.)
const std::vector<std::pair<juce::String, juce::String>>& getCommandsNotRun()
{
    static const std::vector<std::pair<juce::String, juce::String>> commands {};
    return commands;
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
        addModelCommands (registry, context);
        resetAppSwitches();
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
        addModelCommands (registry, context);

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
