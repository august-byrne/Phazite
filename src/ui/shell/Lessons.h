// SPDX-License-Identifier: AGPL-3.0-or-later
// Short lessons (docs/design.md, section 12; roadmap step 3.7): Help > Lessons, or the welcome window's last page, starts
// one, and the lesson card (LessonCard.h) shows its steps one at a time, with the keys of the keymap in use, and rings the
// control each step is about. A step is done when the user does it, in any way: its command runs, however it is run, or
// the song shows it (a track armed); the card's Next goes on past a step that only the mouse does, and Skip past any.
// After the last step its closing words show until the card's Done ends it. A lesson finished once is kept as done in the
// settings, for Help > Lessons to say so. Message thread only.
#pragma once

#include "services/Settings.h"
#include "ui/input/CommandBridge.h"

#include <functional>
#include <vector>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
class Lessons : private CommandRegistry::Listener
{
public:
    // A step: what to do, where "{run:command.id}" becomes "press F7", or "search for Piano Roll with Ctrl+K" for a command
    // without a key; the commands that do it (an id ending in "*" stands for every id it starts); and, for a step whose
    // result shows in the song, whether it is done. Its target is the control the card rings, by its component ID: a
    // control that runs a command has the command's ID ("view.mixer", "transport.record"), others a name of their own
    // ("chordLane", "pianoRoll"); "pianoRoll/tool.draw" is one inside another. Without one, the first of doneBy's commands'.
    struct Step
    {
        const char* text;
        std::vector<const char*> doneBy {};
        std::function<bool (const ProjectContext&)> isDone {};
        const char* target = nullptr;
    };

    struct Lesson
    {
        const char* id; // its command is "help.lesson." and this
        const char* name;
        const char* description;
        std::vector<Step> steps;
        const char* finished; // what shows after the last step
    };

    // The lessons, in the order Help > Lessons lists them.
    static const std::vector<Lesson>& getAll();

    Lessons (CommandBridge&, const ProjectContext&, Settings&);
    ~Lessons() override;

    // A lesson, from its first step; and stopping it.
    void start (const juce::String& id);
    void stop();
    bool isRunning() const noexcept { return lesson != nullptr; }
    juce::String getRunningID() const;
    const Lesson* getLesson() const noexcept { return lesson; }
    int getStep() const noexcept { return step; } // from 0; as many as the lesson has once it is finished
    bool isFinished() const noexcept;             // its last step done: its closing words show

    // The step's text with the keymap's keys, or the closing words once the lesson is finished; nothing while none runs.
    juce::String getText() const;

    // The same with the lesson's name and how far it has come ("Record Audio, 2 of 4: ..."), as screen readers hear it.
    juce::String getHint() const;

    // The control to ring for the step (Step::target), or nothing.
    juce::String getTarget() const;

    // Whether the step only goes on when told to (Next): nothing the user does shows it done.
    bool waitsForNext() const;

    // The next step, past one that isn't done yet (Next, Skip), or the end of a finished lesson; and the step before.
    void next();
    void back();

    // Goes on if the step shows in the song that it is done (the card asks, a few times a second).
    void update();

    // Whether a lesson was finished once (the settings keep it).
    bool isDone (const juce::String& id) const;

    // Called when a lesson starts or stops, or goes to another step.
    std::function<void()> onChange;

    // "help.lesson." and each lesson's id, and "help.stopLesson".
    void addCommands (CommandRegistry&);

private:
    CommandBridge& commands;
    const ProjectContext& context;
    Settings& settings;
    const Lesson* lesson = nullptr;
    int step = 0;

    void setStep (int);
    void commandAdded (const Command&) override {}
    void commandInvoked (const Command&) override; // a step's command
};
} // namespace phazite::ui
