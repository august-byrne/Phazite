// SPDX-License-Identifier: AGPL-3.0-or-later
// Lessons in the hint bar (roadmap step 3.7, src/ui/shell/Lessons.h): Help > Lessons starts one, whose steps show one at a
// time with the keys of the keymap in use, each going on when its command runs, when the song shows it done, or with a
// click on the hint bar.
#include "AppScreen.h"
#include "TestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "ui/input/CommandBridge.h"
#include "ui/shell/Lessons.h"

namespace
{
// The lessons, with the commands their steps name doing nothing, in Phazite's keymap.
struct LessonSetup
{
    LessonSetup()
    {
        for (const auto* id :
             { "view.pianoRoll", "tool.draw", "transport.playStop", "edit.quantize", "edit.duplicate", "transport.metronome",
               "transport.record", "track.arm", "section.loop", "transport.punch", "view.commandPalette" })
            registry.add ({ .id = id, .name = id, .perform = [] {} });

        lessons.addCommands (registry);
    }

    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::ui::Lessons lessons { commands, context };
};
} // namespace

TEST_CASE ("A lesson shows one step at a time with the keymap's keys, each going on when its command runs or with a click")
{
    LessonSetup s;
    CHECK (s.lessons.getHint().isEmpty());
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*s.registry.find ("help.stopLesson")));

    REQUIRE (s.registry.invoke ("help.lesson.beat"));
    CHECK (s.lessons.getRunningID() == juce::String ("beat"));
    CHECK (s.lessons.getHint()
           == "Make a Beat, 1 of 5: Click the name of a track with an instrument, such as the drums, then press F7 to show its notes.");

    // Another command leaves the step; its own goes on, however it is run.
    REQUIRE (s.registry.invoke ("transport.playStop"));
    CHECK (s.lessons.getStep() == 0);
    REQUIRE (s.registry.invoke ("view.pianoRoll"));
    CHECK (s.lessons.getStep() == 1);
    CHECK (s.lessons.getHint().startsWith ("Make a Beat, 2 of 5: Press P for the Draw tool"));

    // A step that only the mouse does goes on with a click on the hint bar.
    s.lessons.next();
    CHECK (s.lessons.getHint() == "Make a Beat, 3 of 5: Press Space to hear them, and again to stop.");

    for (const auto* id : { "transport.playStop", "edit.quantize", "edit.duplicate" })
        REQUIRE (s.registry.invoke (id));

    // Done: its closing words, until a click ends it.
    CHECK (s.lessons.getHint().startsWith ("Make a Beat, done: That is a beat."));
    CHECK (phazite::CommandRegistry::isEnabled (*s.registry.find ("help.stopLesson")));
    s.lessons.next();
    CHECK_FALSE (s.lessons.isRunning());
    CHECK (s.lessons.getHint().isEmpty());
}

TEST_CASE ("A lesson's step that the song shows done goes on by itself, and a command without a key is found by its name")
{
    LessonSetup s;
    auto edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    setEdit (s.context, edit.get());
    const auto track = phazite::getTrackID (*te::getAudioTracks (*edit)[0]);

    REQUIRE (s.registry.invoke ("help.lesson.recording"));
    CHECK (s.lessons.getHint().startsWith ("Record Audio, 1 of 4: Right-click the round button"));

    // Armed from its button (no command): the hint bar's next look goes on.
    phazite::setTrackInput (s.context, track, "1");
    phazite::setArmed (s.context, track, true);
    s.lessons.update();
    CHECK (s.lessons.getStep() == 1);

    // Phazite's keymap gives the metronome O; Loop Section has no key: found with the command search.
    CHECK (s.lessons.getHint() == "Record Audio, 2 of 4: Press O to hear the beat as you play.");
    REQUIRE (s.registry.invoke ("transport.metronome"));
    REQUIRE (s.registry.invoke ("transport.record"));
    CHECK (s.lessons.getHint().contains ("search for section.loop with Ctrl+K"));

    // Started again while armed, its first step is passed at once; Stop Lesson ends it.
    REQUIRE (s.registry.invoke ("help.lesson.recording"));
    CHECK (s.lessons.getStep() == 1);
    REQUIRE (s.registry.invoke ("help.stopLesson"));
    CHECK_FALSE (s.lessons.isRunning());
    setEdit (s.context, nullptr);
}

TEST_CASE ("The hint bar shows the running lesson in place of the guide, and a click on it goes on")
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& hints = screen.shell.getHintBar();

    REQUIRE (screen.registry.invoke ("help.lesson.arranging"));
    CHECK (phazite::test::dispatchMessagesUntil ([&hints] { return hints.getHint().startsWith ("Arrange a Song, 1 of 4:"); }));

    // The alert that no audio output plays takes a click itself: without it, the click goes to the lesson.
    hints.setAlert ({}, nullptr);
    phazite::test::Mouse (hints).click (hints.getLocalBounds().getCentre());
    CHECK (screen.shell.getLessons().getStep() == 1);
    CHECK (phazite::test::dispatchMessagesUntil ([&hints] { return hints.getHint().startsWith ("Arrange a Song, 2 of 4:"); }));

    REQUIRE (screen.registry.invoke ("help.stopLesson"));
    CHECK (phazite::test::dispatchMessagesUntil ([&hints] { return ! hints.getHint().startsWith ("Arrange a Song"); }));
}
