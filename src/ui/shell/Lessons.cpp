// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Lessons.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/chords/ChordModel.h"
#include "model/transport/RecordingModel.h"
#include "ui/input/CommandUI.h"

#include <algorithm>

namespace phazite::ui
{
namespace
{
    // "press F7", or how to find a command that has no key.
    juce::String describeRun (const CommandBridge& commands, const juce::String& id)
    {
        if (const auto key = getKeyText (commands, id); key.isNotEmpty())
            return "press " + key;

        const auto* command = commands.getRegistry().find (id);
        const auto palette = getKeyText (commands, "view.commandPalette");
        return "search for " + (command != nullptr ? command->name : id)
               + (palette.isNotEmpty() ? " with " + palette : juce::String (" in the command search"));
    }

    // A step's text with its commands' keys, each sentence starting with a capital letter.
    juce::String fill (const CommandBridge& commands, juce::String text)
    {
        juce::String filled;

        while (text.contains ("{run:"))
        {
            filled << text.upToFirstOccurrenceOf ("{run:", false, false);
            text = text.fromFirstOccurrenceOf ("{run:", false, false);
            filled << describeRun (commands, text.upToFirstOccurrenceOf ("}", false, false));
            text = text.fromFirstOccurrenceOf ("}", false, false);
        }

        filled << text;
        juce::String capitalized;

        for (int i = 0; i < filled.length(); ++i)
        {
            const auto startsSentence = i == 0 || (i >= 2 && filled[i - 1] == ' ' && (filled[i - 2] == '.' || filled[i - 2] == '?'));
            capitalized << juce::String::charToString (startsSentence ? juce::CharacterFunctions::toUpperCase (filled[i]) : filled[i]);
        }

        return capitalized;
    }

    bool matches (const juce::String& pattern, const juce::String& id)
    {
        return pattern.endsWithChar ('*') ? id.startsWith (pattern.dropLastCharacters (1)) : id == pattern;
    }

    // A track that plays notes through an instrument, which a beat needs first.
    bool hasInstrument (const ProjectContext& project)
    {
        const auto rows = getPlaylistRows (project);
        return std::any_of (rows.begin(), rows.end(), [] (const TrackRow& row) { return row.hasInstrument; });
    }
} // namespace

const std::vector<Lessons::Lesson>& Lessons::getAll()
{
    // Each starts from an empty project, as a new one is (the owner, 2026-11-01), and points at the control of each step.
    static const std::vector<Lesson> lessons {
        { "beat",
          "Make a Beat",
          "From an empty song to a beat: a drum sampler, drum hits clicked into the piano roll, played and repeated.",
          { { .text = "{run:device.add}, type Drum Sampler and press Enter: it plays on a new track.",
              .isDone = hasInstrument,
              .target = "view.commandPalette" },
            { .text = "{run:view.pianoRoll} to show the track's notes in the piano roll.", .doneBy = { "view.pianoRoll" } },
            { .text = "Click in the piano roll to add drum hits, a row for each drum; a right-click takes one away. Click Next when you "
                      "have a few.",
              .target = "pianoRoll" },
            { .text = "{run:transport.playStop} to hear them, and again to stop.",
              .doneBy = { "transport.playStop", "transport.playPause" } },
            { .text = "Click the beat's clip in the playlist, then {run:edit.duplicate} to repeat it after itself.",
              .doneBy = { "edit.duplicate", "edit.duplicateAsAlias" },
              .target = "playlist" } },
          "That is a beat. Help > Lessons has more." },

        { "recording",
          "Record Audio",
          "Records a voice or an instrument through the audio interface: its input, the metronome, a take, and punching in to do a "
          "part again.",
          { { .text = "{run:track.add} to add a track for it: without an instrument, a track records audio.", .doneBy = { "track.add" } },
            { .text = "Right-click the track's arm button, the hollow circle in its header, and choose the input your microphone or "
                      "instrument is on: the track is armed.",
              .doneBy = { "track.arm" },
              .isDone = [] (const ProjectContext& project) { return ! getArmedTracks (project).empty(); },
              .target = "track.arm" },
            { .text = "{run:transport.metronome} to hear the beat, then {run:transport.record} to record, and again to stop.",
              .doneBy = { "transport.record" } },
            { .text = "To do a part again, click its section's name above the tracks and {run:section.loop}, then right-click the "
                      "record button, turn on Punch In and record again: only the looped part changes.",
              .doneBy = { "transport.punch" },
              .target = "transport.record" } },
          "Recorded. Each time round a looped part is a take: right-click the clip to choose one." },

        { "arranging",
          "Arrange a Song",
          "Lays out a song in sections, repeats one with everything in it, and exports the song.",
          { { .text = "Choose an outline in Add > Song Outline: it lays out sections, such as Intro, Verse and Chorus, above the tracks.",
              .doneBy = { "section.outline.*", "section.add" },
              .target = "view.menus" },
            { .text = "Click a section's name to select it, then {run:edit.duplicate} to repeat it, with everything in it, right after it.",
              .doneBy = { "edit.duplicate", "edit.duplicateAsAlias" },
              .target = "sections" },
            { .text = "{run:transport.goToStart} to go back to the start, and {run:transport.playStop} to play the song.",
              .doneBy = { "transport.playStop", "transport.playPause" } },
            { .text = "{run:project.export} to export the song as a WAV file that plays anywhere.",
              .doneBy = { "project.export" },
              .target = "view.menus" } },
          "That is the song's shape: fill its sections in the playlist." },

        { "effects",
          "Add Effects",
          "Adds an effect to a track in the mixer, and draws how one of its knobs changes over the song.",
          { { .text = "{run:view.mixer} to see the mixer: a strip for each track. No tracks yet? Help > Example Songs has songs to try "
                      "this on.",
              .doneBy = { "view.mixer" } },
            { .text = "Click a strip to select its track, then {run:device.add}, type an effect such as Reverb, and press Enter.",
              .doneBy = { "device.add" },
              .target = "mixer" },
            { .text = "Turn one of the effect's knobs, then {run:automation.automateLastTouched} and draw how it changes in the lane "
                      "that opens under the track.",
              .doneBy = { "automation.automateLastTouched" },
              .target = "view.effects" },
            { .text = "{run:transport.playStop} to hear it.", .doneBy = { "transport.playStop", "transport.playPause" } } },
          "Any knob can change over the song this way." },

        { "chords",
          "Write Chords",
          "Puts a chord progression on the song's chord lane, and writes a part that plays it.",
          { { .text = "{run:chords.progressions} to open Chord Progressions: listen to a few, and put one on the song. Its chords show "
                      "in the chord lane, under the bar numbers.",
              .isDone = [] (const ProjectContext& project) { return hasLaneChords (project); } },
            { .text = "Click a chord in the lane under the bar numbers to change it. Click Next when you like it.", .target = "chordLane" },
            { .text = "Double-click a row of a track with an instrument, under the chords, to add a clip. In the piano roll, the rows of "
                      "the chord's notes are tinted: draw notes there. Click Next when you have some.",
              .target = "pianoRoll" },
            { .text = "{run:transport.playStop} to hear them.", .doneBy = { "transport.playStop", "transport.playPause" } } },
          "The chord lane is a guide: change a chord, and the piano roll tints its notes." },
    };

    return lessons;
}

Lessons::Lessons (CommandBridge& bridge, const ProjectContext& projectContext, Settings& s)
    : commands (bridge),
      context (projectContext),
      settings (s)
{
    commands.getRegistry().addListener (this);
}

Lessons::~Lessons()
{
    commands.getRegistry().removeListener (this);
}

void Lessons::start (const juce::String& id)
{
    for (const auto& each : getAll())
    {
        if (id == each.id)
        {
            lesson = &each;
            step = -1;
            setStep (0);
            update(); // a step done already is passed
            return;
        }
    }
}

void Lessons::stop()
{
    if (lesson == nullptr)
        return;

    lesson = nullptr;
    step = 0;

    if (onChange != nullptr)
        onChange();
}

juce::String Lessons::getRunningID() const
{
    return lesson != nullptr ? juce::String (lesson->id) : juce::String();
}

bool Lessons::isFinished() const noexcept
{
    return lesson != nullptr && step >= (int) lesson->steps.size();
}

juce::String Lessons::getText() const
{
    if (lesson == nullptr)
        return {};

    if (isFinished())
        return lesson->finished;

    return fill (commands, lesson->steps[(size_t) step].text);
}

juce::String Lessons::getHint() const
{
    if (lesson == nullptr)
        return {};

    if (isFinished())
        return juce::String (lesson->name) + ", done: " + lesson->finished;

    return juce::String (lesson->name) + ", " + juce::String (step + 1) + " of " + juce::String ((int) lesson->steps.size()) + ": "
           + getText();
}

juce::String Lessons::getTarget() const
{
    if (lesson == nullptr || isFinished())
        return {};

    const auto& current = lesson->steps[(size_t) step];

    if (current.target != nullptr)
        return current.target;

    // The control of the command that does it; not of a pattern ("section.outline.*").
    if (! current.doneBy.empty() && ! juce::String (current.doneBy.front()).endsWithChar ('*'))
        return current.doneBy.front();

    return {};
}

bool Lessons::waitsForNext() const
{
    if (lesson == nullptr || isFinished())
        return false;

    const auto& current = lesson->steps[(size_t) step];
    return current.doneBy.empty() && current.isDone == nullptr;
}

void Lessons::next()
{
    if (lesson == nullptr)
        return;

    if (isFinished())
        stop();
    else
        setStep (step + 1);
}

void Lessons::back()
{
    if (lesson != nullptr && step > 0)
        setStep (step - 1);
}

void Lessons::setStep (int newStep)
{
    if (lesson == nullptr || newStep == step)
        return;

    step = juce::jlimit (0, (int) lesson->steps.size(), newStep);

    // Finished: kept as done, for Help > Lessons.
    if (isFinished() && ! isDone (lesson->id))
    {
        auto done = settings.get (Settings::lessonsDone);
        done.add (lesson->id);
        settings.set (Settings::lessonsDone, done);
    }

    if (onChange != nullptr)
        onChange();
}

void Lessons::update()
{
    while (lesson != nullptr && ! isFinished() && lesson->steps[(size_t) step].isDone != nullptr
           && lesson->steps[(size_t) step].isDone (context))
        setStep (step + 1);
}

bool Lessons::isDone (const juce::String& id) const
{
    return settings.get (Settings::lessonsDone).contains (id);
}

void Lessons::commandInvoked (const Command& command)
{
    if (lesson == nullptr || isFinished())
        return;

    for (const auto* pattern : lesson->steps[(size_t) step].doneBy)
    {
        if (matches (pattern, command.id))
        {
            setStep (step + 1);
            update();
            return;
        }
    }
}

void Lessons::addCommands (CommandRegistry& registry)
{
    for (const auto& each : getAll())
    {
        const juce::String id (each.id), name ("Lesson: " + juce::String (each.name));
        registry.add ({ .id = "help.lesson." + id,
                        .name = name,
                        .category = "Help",
                        .description = each.description,
                        .synonyms = { "lesson", "tutorial", "learn", "how to", "teach me" },
                        .perform = [this, id] { start (id); },
                        .isTicked = [this, id] { return getRunningID() == id; },
                        .currentName = [this, id, name] { return isDone (id) ? name + " (done)" : juce::String(); } });
    }

    registry.add ({ .id = "help.stopLesson",
                    .name = "Stop Lesson",
                    .category = "Help",
                    .description = "Ends the lesson that is showing.",
                    .synonyms = { "lesson", "tutorial", "close lesson" },
                    .perform = [this] { stop(); },
                    .isEnabled = [this] { return isRunning(); } });
}
} // namespace phazite::ui
