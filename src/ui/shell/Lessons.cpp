// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Lessons.h"
#include "model/transport/RecordingModel.h"
#include "ui/input/CommandUI.h"

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
            const auto startsSentence = i == 0 || (i >= 2 && filled[i - 1] == ' ' && filled[i - 2] == '.');
            capitalized << juce::String::charToString (startsSentence ? juce::CharacterFunctions::toUpperCase (filled[i]) : filled[i]);
        }

        return capitalized;
    }

    bool matches (const juce::String& pattern, const juce::String& id)
    {
        return pattern.endsWithChar ('*') ? id.startsWith (pattern.dropLastCharacters (1)) : id == pattern;
    }
} // namespace

const std::vector<Lessons::Lesson>& Lessons::getAll()
{
    static const std::vector<Lesson> lessons {
        { "beat",
          "Make a Beat",
          "Five steps from an empty pattern to a beat: notes drawn in the piano roll, played, put on the grid and repeated.",
          { { .text = "Click the name of a track with an instrument, such as the drums, then {run:view.pianoRoll} to show its notes.",
              .doneBy = { "view.pianoRoll" } },
            { .text = "{run:tool.draw} for the Draw tool, and click in the piano roll to add a few notes. Then click here to go on." },
            { .text = "{run:transport.playStop} to hear them, and again to stop.",
              .doneBy = { "transport.playStop", "transport.playPause" } },
            { .text = "Drag around the notes with Ctrl held to select them, then {run:edit.quantize} to put them on the grid.",
              .doneBy = { "edit.quantize" } },
            { .text = "Click the pattern's clip in the playlist, then {run:edit.duplicate} to repeat it after itself.",
              .doneBy = { "edit.duplicate", "edit.duplicateAsAlias" } } },
          "That is a beat. Help > Lessons has more." },

        { "recording",
          "Record Audio",
          "Records a voice or an instrument through the audio interface: its input, the metronome, a take, and punching in to do a "
          "part again.",
          { { .text = "Right-click the round button beside the M and S of a track without an instrument, and choose the input your "
                      "microphone or instrument is on: the track is armed.",
              .doneBy = { "track.arm" },
              .isDone = [] (const ProjectContext& project) { return ! getArmedTracks (project).empty(); } },
            { .text = "{run:transport.metronome} to hear the beat as you play.", .doneBy = { "transport.metronome" } },
            { .text = "{run:transport.record} to record, and again to stop.", .doneBy = { "transport.record" } },
            { .text = "To do a part again, click its section's name above the tracks and {run:section.loop}, then right-click the "
                      "record button, turn on Punch In and record again: only the looped part changes.",
              .doneBy = { "transport.punch" } } },
          "Recorded. Each time round a looped part is a take: right-click the clip to choose one." },

        { "arranging",
          "Arrange a Song",
          "Lays out a song in sections, repeats one with everything in it, and exports the song.",
          { { .text = "Choose an outline in Add > Song Outline: it lays out sections, such as Intro, Verse and Chorus, above the tracks.",
              .doneBy = { "section.outline.*", "section.add" } },
            { .text = "Click a section's name to select it, then {run:edit.duplicate} to repeat it, with everything in it, right after it.",
              .doneBy = { "edit.duplicate", "edit.duplicateAsAlias" } },
            { .text = "{run:transport.goToStart} to go back to the start, and {run:transport.playStop} to play the song.",
              .doneBy = { "transport.playStop", "transport.playPause" } },
            { .text = "{run:project.export} to export the song as a WAV file that plays anywhere.", .doneBy = { "project.export" } } },
          "That is the song's shape: fill its sections in the playlist." },

        { "effects",
          "Add Effects",
          "Adds an effect to a track in the mixer, and draws how one of its knobs changes over the song.",
          { { .text = "{run:view.mixer} to see the mixer: a strip for each track.", .doneBy = { "view.mixer" } },
            { .text = "Click a strip to select its track, then {run:device.add}, type an effect such as Reverb, and press Enter.",
              .doneBy = { "device.add" } },
            { .text = "Turn one of the effect's knobs, then {run:automation.automateLastTouched} and draw how it changes in the lane "
                      "that opens under the track.",
              .doneBy = { "automation.automateLastTouched" } },
            { .text = "{run:transport.playStop} to hear it.", .doneBy = { "transport.playStop", "transport.playPause" } } },
          "Any knob can change over the song this way." },

        { "chords",
          "Write Chords",
          "Puts a chord progression on the chord lane, and makes parts that play it.",
          { { .text = "{run:chords.progressions} to open Chord Progressions: listen to a few, and put one on the chord lane.",
              .doneBy = { "chords.progressions" } },
            { .text = "Click a chord in the lane under the bar numbers to change it. Then click here to go on." },
            { .text = "{run:chords.makeChordPart} for a track that plays the chords, or {run:chords.makeBassLine} for a bass line.",
              .doneBy = { "chords.make*" } },
            { .text = "{run:transport.playStop} to hear them.", .doneBy = { "transport.playStop", "transport.playPause" } } },
          "The parts follow the chords until you change their notes." },
    };

    return lessons;
}

Lessons::Lessons (CommandBridge& bridge, const ProjectContext& projectContext)
    : commands (bridge),
      context (projectContext)
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
            step = 0;
            update(); // a step done already is passed
            return;
        }
    }
}

void Lessons::stop()
{
    lesson = nullptr;
    step = 0;
}

juce::String Lessons::getRunningID() const
{
    return lesson != nullptr ? juce::String (lesson->id) : juce::String();
}

juce::String Lessons::getHint() const
{
    if (lesson == nullptr)
        return {};

    const auto numSteps = (int) lesson->steps.size();

    if (step >= numSteps)
        return juce::String (lesson->name) + ", done: " + lesson->finished + " Click here to close the lesson.";

    return juce::String (lesson->name) + ", " + juce::String (step + 1) + " of " + juce::String (numSteps) + ": "
           + fill (commands, lesson->steps[(size_t) step].text);
}

void Lessons::next()
{
    if (lesson == nullptr)
        return;

    if (step < (int) lesson->steps.size())
        ++step;
    else
        stop();
}

void Lessons::update()
{
    while (lesson != nullptr && step < (int) lesson->steps.size() && lesson->steps[(size_t) step].isDone != nullptr
           && lesson->steps[(size_t) step].isDone (context))
        ++step;
}

void Lessons::commandInvoked (const Command& command)
{
    if (lesson == nullptr || step >= (int) lesson->steps.size())
        return;

    for (const auto* pattern : lesson->steps[(size_t) step].doneBy)
    {
        if (matches (pattern, command.id))
        {
            ++step;
            update();
            return;
        }
    }
}

void Lessons::addCommands (CommandRegistry& registry)
{
    for (const auto& each : getAll())
    {
        const juce::String id (each.id);
        registry.add ({ .id = "help.lesson." + id,
                        .name = "Lesson: " + juce::String (each.name),
                        .category = "Help",
                        .description = each.description,
                        .synonyms = { "lesson", "tutorial", "learn", "how to", "teach me" },
                        .perform = [this, id] { start (id); },
                        .isTicked = [this, id] { return getRunningID() == id; } });
    }

    registry.add ({ .id = "help.stopLesson",
                    .name = "Stop Lesson",
                    .category = "Help",
                    .description = "Ends the lesson the hint bar shows.",
                    .synonyms = { "lesson", "tutorial", "close lesson" },
                    .perform = [this] { stop(); },
                    .isEnabled = [this] { return isRunning(); } });
}
} // namespace phazite::ui
