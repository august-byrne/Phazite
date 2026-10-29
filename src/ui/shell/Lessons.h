// SPDX-License-Identifier: AGPL-3.0-or-later
// Short lessons in the hint bar (docs/design.md, section 12; roadmap step 3.7): Help > Lessons starts one, and the hint bar
// shows its steps one at a time, where the guide's tips show (while the pointer rests on nothing with a tip of its own),
// with the keys of the keymap in use. A step is done when the user does it, in any way: its command runs, however it is
// run, or the song shows it (a track armed). A click on the hint bar goes on to the next step too, past one that only the
// mouse does, and after the last it ends the lesson, whose closing words show until then. As Ableton Live teaches in its
// lessons, but in the bar along the window's bottom, so that nothing covers the song. Message thread only.
#pragma once

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
    // result shows in the song, whether it is done.
    struct Step
    {
        const char* text;
        std::vector<const char*> doneBy {};
        std::function<bool (const ProjectContext&)> isDone {};
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

    Lessons (CommandBridge&, const ProjectContext&);
    ~Lessons() override;

    // A lesson, from its first step; and stopping it.
    void start (const juce::String& id);
    void stop();
    bool isRunning() const noexcept { return lesson != nullptr; }
    juce::String getRunningID() const;
    int getStep() const noexcept { return step; } // from 0; as many as the lesson has once it is finished

    // What the hint bar shows while a lesson runs ("Record Audio, 2 of 4: ..."), or nothing.
    juce::String getHint() const;

    // A click on the hint bar: the next step, or the end of a finished lesson.
    void next();

    // Goes on if the step shows in the song that it is done (the hint bar asks, ten times a second).
    void update();

    // "help.lesson." and each lesson's id, and "help.stopLesson".
    void addCommands (CommandRegistry&);

private:
    CommandBridge& commands;
    const ProjectContext& context;
    const Lesson* lesson = nullptr;
    int step = 0;

    void commandAdded (const Command&) override {}
    void commandInvoked (const Command&) override; // a step's command
};
} // namespace phazite::ui
