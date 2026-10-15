// SPDX-License-Identifier: AGPL-3.0-or-later
// Golden files of the model's commands (tests/golden/commands): their catalogue (each command's id, texts and switches,
// in the order the app registers them, which decides which of two commands on one key runs and the order of the
// shortcuts list), and a script that runs every model command where it does something. The script's golden files hold
// the project it leaves (script.xml), the undo step the Edit menu offers after each command (script-undo.txt), and,
// after each step, every command whose state changed (enabled or greyed out, ticked, and the name menus show), and what
// is selected (notes with their pitches, starts and lengths), where the playhead is and the loop's range, each if it
// changed (script-states.txt). GoldenFiles.h says how to regenerate them.
#include "GoldenFiles.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "app/AppCommands.h"
#include "model/arrangement/Clips.h"
#include "model/arrangement/Import.h"
#include "model/arrangement/Selection.h"
#include "model/arrangement/Tracks.h"
#include "model/commands/CommandRegistry.h"
#include "model/notes/Notes.h"
#include "model/project/Project.h"
#include "model/transport/Playback.h"

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
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
    "when greyed out; ticked; the name menus show), and what is selected, where the playhead is and the loop's range (in "
    "beats), each if it changed. Regenerate with PHAZITE_UPDATE_GOLDEN=1 (CLAUDE.md).";

constexpr const char* aboutTheProject = "The project the command script leaves (every model command run where it does something), "
                                        "saved: tests/CommandGoldenTests.cpp";

te::BeatPosition beats (double b)
{
    return te::BeatPosition::fromBeats (b);
}

// The model's commands, registered as the app registers them: addSongCommands(), which Main.cpp calls, then the note
// commands as AppShell adds them, with a grid of a quarter beat.
void addModelCommands (phazite::CommandRegistry& registry, te::SelectionManager& selection, phazite::RangeSelection& range)
{
    phazite::addSongCommands (registry, selection, range);
    phazite::addNoteCommands (registry, selection, [] { return 0.25; });
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

// What is selected, in the order it was selected, and the time range: "selected: track One, track Two", or
// "selected: nothing".
juce::String describeSelected (const te::SelectionManager& selection, const phazite::RangeSelection& range)
{
    juce::StringArray items;

    for (auto* selected : selection.getSelectedObjects())
    {
        if (auto* section = dynamic_cast<te::ArrangerClip*> (selected))
        {
            items.add ("section " + section->getName());
        }
        else if (auto* clip = dynamic_cast<te::Clip*> (selected))
        {
            auto* clipTrack = clip->getTrack();
            items.add ("clip on " + (clipTrack != nullptr ? clipTrack->getName() : juce::String ("no track")) + " at "
                       + juce::String (clip->getStartBeat().inBeats(), 3));
        }
        else if (auto* track = dynamic_cast<te::Track*> (selected))
        {
            items.add ("track " + track->getName());
        }
        else if (dynamic_cast<te::SelectedMidiEvents*> (selected) != nullptr)
        {
            if (const auto notes = phazite::getNoteData (phazite::getSelectedNotes (selection)); ! notes.empty())
                items.add (describeNotes (notes));
        }
        else if (selected != nullptr)
        {
            items.add ("other: " + selected->getSelectableDescription());
        }
    }

    if (! range.isEmpty())
    {
        juce::StringArray names;

        for (auto* track : range.getTracks())
            names.add (track->getName());

        items.add ("range " + juce::String (range.getRange().getStart().inBeats(), 3) + "-"
                   + juce::String (range.getRange().getEnd().inBeats(), 3) + " beats on " + names.joinIntoString (" and "));
    }

    return "selected: " + (items.isEmpty() ? juce::String ("nothing") : items.joinIntoString (", "));
}

// Where the playhead is, in beats: "playhead at 2.000".
juce::String describePlayhead (const te::Edit& edit)
{
    return "playhead at " + juce::String (edit.tempoSequence.toBeats (edit.getTransport().getPosition()).inBeats(), 3);
}

// The loop's range in beats, whether looping is on or not (Loop's tick says which): "loop 2.000-6.000 beats".
juce::String describeLoop (const te::Edit& edit)
{
    const auto loop = edit.tempoSequence.toBeats (edit.getTransport().getLoopRange());
    return "loop " + juce::String (loop.getStart().inBeats(), 3) + "-" + juce::String (loop.getEnd().inBeats(), 3) + " beats";
}

//==============================================================================
juce::String quoted (const juce::String& text)
{
    return "\"" + text + "\"";
}

// The lines in which a golden text and Phazite's differ, by number, at most maxListedDifferences:
//   line 31: expected "track.pin | ...", got "track.collapse | ..."
// Lines are compared exactly; a line one text has and the other hasn't is missing or unexpected.
juce::StringArray compareLines (const juce::String& expected, const juce::String& actual)
{
    const auto expectedLines = juce::StringArray::fromLines (expected);
    const auto actualLines = juce::StringArray::fromLines (actual);
    juce::StringArray differences;

    for (int i = 0; i < std::max (expectedLines.size(), actualLines.size()); ++i)
    {
        const auto where = "line " + juce::String (i + 1) + ": ";

        if (i >= actualLines.size())
            differences.add (where + "missing (expected " + quoted (expectedLines[i]) + ")");
        else if (i >= expectedLines.size())
            differences.add (where + "unexpected " + quoted (actualLines[i]));
        else if (expectedLines[i] != actualLines[i])
            differences.add (where + "expected " + quoted (expectedLines[i]) + ", got " + quoted (actualLines[i]));
    }

    phazite::test::limitDifferences (differences, maxListedDifferences);
    return differences;
}

// A golden text file: a line saying what it is, then the lines, each ending in "\n".
juce::String makeGoldenText (const char* about, const juce::StringArray& lines)
{
    return juce::String (about) + "\n" + lines.joinIntoString ("\n") + "\n";
}

std::unique_ptr<phazite::Project> createProject (const juce::File& parentFolder)
{
    juce::String error;
    auto project = phazite::Project::create (phazite::test::getEngine(), parentFolder, "Script", error);
    INFO (error.toStdString());
    REQUIRE (project != nullptr);
    return project;
}

//==============================================================================
// A project with the model's commands, as the app has them, which a script runs one after another. It records the undo
// step the Edit menu offers after each command (undoLines) and, after each step of the script, the commands whose
// state changed, and what is selected, the playhead and the loop, each if it changed (stateLines). The script's own
// changes (setup()) are undo steps of their own, named "Script: ...", so that they never join a command's.
struct Script
{
    Script()
    {
        one = te::getAudioTracks (edit)[0];
        REQUIRE (one != nullptr);
        one->setName ("One");
        edit.getUndoManager().clearUndoHistory();

        selection.edit = &edit;
        addModelCommands (registry, selection, range);
        snapshot ("start");
    }

    // Runs commands, each of which must be enabled where the script runs it, recording the undo step and the states each
    // leaves.
    void run (std::initializer_list<const char*> ids)
    {
        for (const auto* id : ids)
            record (id);
    }

    void record (const juce::String& id)
    {
        REQUIRE_MESSAGE (registry.invoke (id), ("The script runs " + id + ", which is unknown or greyed out there.").toStdString());

        const auto step = edit.getUndoManager().getUndoDescription();
        undoLines.add (id + ": " + (step.isEmpty() ? juce::String ("(nothing to undo)") : step));
        snapshot (id);
    }

    // A change the script makes itself, as an undo step of its own.
    void setup (const juce::String& what, const std::function<void()>& change)
    {
        edit.getUndoManager().beginNewTransaction ("Script: " + what);
        change();
        snapshot (what);
    }

    // Selects the items alone, in this order: the first with selectOnly(), the others added to it.
    void select (const juce::String& names, std::initializer_list<te::Selectable*> items)
    {
        auto isFirst = true;

        for (auto* item : items)
        {
            if (isFirst)
                selection.selectOnly (item);
            else
                selection.addToSelection (item);

            isFirst = false;
        }

        snapshot ("select " + names);
    }

    void selectNothing()
    {
        selection.deselectAll();
        snapshot ("select nothing");
    }

    // The one selected track, of a type.
    template <typename TrackType>
    TrackType& getSelectedTrack() const
    {
        const auto tracks = phazite::getSelectedTracks (selection);
        REQUIRE (tracks.size() == 1);
        auto* track = dynamic_cast<TrackType*> (tracks.getFirst());
        REQUIRE (track != nullptr);
        return *track;
    }

    // Adds 'after <what>:', the commands whose state changed since the last snapshot (all of them the first time), and
    // what is selected, where the playhead is and the loop's range, each if it changed.
    void snapshot (const juce::String& what)
    {
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
        addIfChanged (playhead, describePlayhead (edit));
        addIfChanged (loop, describeLoop (edit));
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
    te::Edit& edit = project->getEdit();
    te::AudioTrack* one = nullptr;
    te::SelectionManager selection { phazite::test::getEngine() };
    phazite::RangeSelection range;
    phazite::CommandRegistry registry;
    std::unique_ptr<te::SelectedMidiEvents> events; // the selected notes, while the script edits them

    std::map<juce::String, juce::String> states; // each command's state at the last snapshot
    juce::String selected, playhead, loop;       // the lines the last snapshot had for them
    juce::StringArray undoLines, stateLines;
};
} // namespace

//==============================================================================
TEST_SUITE ("golden")
{
    TEST_CASE ("Golden commands: the model's commands keep their ids, texts, switches and order")
    {
        te::SelectionManager selection { phazite::test::getEngine() };
        phazite::RangeSelection range;
        phazite::CommandRegistry registry;
        addModelCommands (registry, selection, range);

        juce::StringArray lines;

        for (const auto* command : registry.getAll())
            lines.add (describeCommand (*command));

        phazite::test::checkGoldenText ("commands/catalogue.txt", makeGoldenText (aboutTheCatalogue, lines), compareLines);
    }

    TEST_CASE ("Golden commands: a script of every model command, with the undo step and the states each leaves")
    {
        Script s;
        auto* one = s.one;

        // Tracks: add, name and group them, change their heights, pin, collapse, mute and solo them, move them and move
        // through them. One, inside a muted folder, isn't muted itself, so Mute Track isn't ticked for it.
        s.select ("One", { one });
        s.run ({ "track.add" });

        te::AudioTrack* two = nullptr;
        s.setup ("name the new track Two",
                 [&s, &two]
                 {
                     two = &s.getSelectedTrack<te::AudioTrack>();
                     two->setName ("Two");
                 });

        s.run ({ "track.addFolder" });
        s.setup ("name the new folder Empty", [&s] { s.getSelectedTrack<te::FolderTrack>().setName ("Empty"); });
        s.run ({ "track.ungroup" });

        s.select ("One", { one });
        s.run ({ "track.taller", "track.shorter" });
        s.selectNothing();
        s.run ({ "track.taller" }); // every track

        s.select ("One", { one });
        s.run ({ "track.pin", "track.collapse", "track.collapse" });

        s.select ("One and Two", { one, two });
        s.run ({ "track.groupIntoFolder" });

        te::FolderTrack* group = nullptr;
        s.setup ("name the new folder Group",
                 [&s, &group]
                 {
                     group = &s.getSelectedTrack<te::FolderTrack>();
                     group->setName ("Group");
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

        // Sections and outlines.
        s.selectNothing();
        s.run ({ "section.add", "edit.duplicate", "edit.duplicateAsAlias", "section.loop", "edit.delete", "section.outline.pop",
                 "section.outline.rock" });

        // A MIDI clip on One, and its notes: half a beat long, a beat apart, so that legato makes the first one longer.
        te::MidiClip* clip = nullptr;
        s.setup ("add a MIDI clip on One, with two notes",
                 [&s, &clip, one]
                 {
                     clip = phazite::insertMidiClip (*one, { beats (0.0), beats (4.0) }, &s.selection);
                     REQUIRE (clip != nullptr);
                     REQUIRE (s.selection.isSelected (clip));
                     REQUIRE (phazite::addNote (*clip, 60, beats (0.0), te::BeatDuration::fromBeats (0.5), 100) != nullptr);
                     REQUIRE (phazite::addNote (*clip, 64, beats (1.0), te::BeatDuration::fromBeats (0.5), 100) != nullptr);
                 });

        s.run ({ "edit.mute", "edit.mute", "edit.duplicate", "edit.delete" });
        s.select ("the clip", { clip });
        s.run ({ "edit.duplicateAsAlias", "clip.makeUnique" });
        s.select ("the clip", { clip });
        s.run ({ "transport.goToEnd", "transport.goToStart" });
        s.setup ("put the playhead at 1 s", [&s] { phazite::movePlayhead (s.edit, te::TimePosition::fromSeconds (1.0)); });
        s.run ({ "edit.split" });

        s.setup ("select the clip's notes",
                 [&s, &clip]
                 {
                     s.selection.selectOnly (clip);
                     s.events = std::make_unique<te::SelectedMidiEvents> (*clip);
                     s.events->setSelected (s.selection, clip->getSequence().getNotes(), false);
                 });

        s.run ({ "note.transposeUp", "note.transposeDown", "note.octaveUp", "note.octaveDown", "note.lengthen", "note.shorten",
                 "note.legato", "note.chop", "edit.mute", "edit.mute", "edit.duplicate", "edit.delete" });

        s.setup ("select nothing, and drop the notes' selection",
                 [&s]
                 {
                     s.selection.deselectAll();
                     s.events = nullptr;
                 });

        // An audio clip on Two: its fade shapes (linear last, as a new clip's fades are linear already), and deleting it
        // with the gap closed.
        s.setup ("import a tone onto Two at beat 8, and select it",
                 [&s, two]
                 {
                     const auto tone = phazite::test::writeSound (s.temp.folder.getChildFile ("Tone.wav"), 2.0);
                     auto* audio = dynamic_cast<te::WaveAudioClip*> (phazite::importFile (*two, tone, beats (8.0)));
                     REQUIRE (audio != nullptr);
                     s.selection.selectOnly (audio);
                 });

        s.run ({ "clip.fadeConvex", "clip.fadeConcave", "clip.fadeSCurve", "clip.fadeLinear", "edit.deleteAndCloseGap" });

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

        // The range is looped: Loop turns looping off and on again, Loop the Song makes the loop follow the song again,
        // and, with looping off, Loop Selection without a range loops the song.
        s.setup ("clear the range", [&s] { s.range.clear(); });
        s.run ({ "transport.loop", "transport.loop", "transport.loopSong", "transport.loop", "transport.loopSelection" });

        // Selecting everything: clips, or tracks while tracks are selected.
        s.selectNothing();
        s.run ({ "edit.selectAll" });
        s.select ("One", { one });
        s.run ({ "edit.selectAll", "edit.deselectAll" });

        // Undo and redo. (Play / Stop and Play / Pause aren't run: the tests have no audio device.)
        s.run ({ "edit.undo", "edit.redo", "edit.undoOrRedo", "edit.undoOrRedo" });

        s.range.clear();
        s.selection.deselectAll();
        s.events = nullptr;
        phazite::test::settle();

        REQUIRE (s.project->save().wasOk());
        phazite::test::checkProjectGolden ("commands/script.xml", *phazite::test::readNormalisedProject (s.project->getFile()),
                                           aboutTheProject);
        phazite::test::checkGoldenText ("commands/script-undo.txt", makeGoldenText (aboutTheUndoSteps, s.undoLines), compareLines);
        phazite::test::checkGoldenText ("commands/script-states.txt", makeGoldenText (aboutTheStates, s.stateLines), compareLines);
    }
}
