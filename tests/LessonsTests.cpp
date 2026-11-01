// SPDX-License-Identifier: AGPL-3.0-or-later
// Lessons (roadmap step 3.7, src/ui/shell/Lessons.h) and their card (LessonCard.h): Help > Lessons starts one, whose steps
// show one at a time with the keys of the keymap in use, each going on when its command runs, when the song shows it done,
// or with the card's Next or Skip; the card rings the control a step is about, and a lesson finished is kept as done.
#include "AppScreen.h"
#include "TestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/RecordingModel.h"
#include "services/Settings.h"
#include "ui/input/CommandBridge.h"
#include "ui/shell/LessonCard.h"
#include "ui/shell/Lessons.h"

namespace
{
// The lessons, with the commands their steps name doing nothing, in Phazite's keymap.
struct LessonSetup
{
    LessonSetup()
    {
        for (const auto* id : { "view.pianoRoll", "device.add", "transport.playStop", "edit.duplicate", "transport.metronome",
                                "transport.record", "track.add", "track.arm", "section.loop", "transport.punch", "view.commandPalette" })
            registry.add ({ .id = id, .name = id, .perform = [] {} });

        lessons.addCommands (registry);
        lessons.onChange = [this] { ++changes; };
    }

    phazite::Settings settings; // in memory
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::ui::Lessons lessons { commands, context, settings };
    int changes = 0;
};
} // namespace

TEST_CASE ("A lesson shows one step at a time with the keymap's keys, each going on when it is done, or with Next or Skip")
{
    LessonSetup s;
    CHECK (s.lessons.getText().isEmpty());
    CHECK_FALSE (phazite::CommandRegistry::isEnabled (*s.registry.find ("help.stopLesson")));

    // From an empty song: an instrument first, from the plug-in picker, which hangs from the command search.
    REQUIRE (s.registry.invoke ("help.lesson.beat"));
    CHECK (s.lessons.getRunningID() == juce::String ("beat"));
    CHECK (s.lessons.getText() == "Press F8, type Drum Sampler and press Enter: it plays on a new track.");
    CHECK (s.lessons.getHint().startsWith ("Make a Beat, 1 of 5: Press F8"));
    CHECK (s.lessons.getTarget() == juce::String ("view.commandPalette"));
    CHECK_FALSE (s.lessons.waitsForNext()); // the song shows it done
    CHECK (s.changes == 1);

    // Skip goes on past a step not done; another command leaves a step, and its own goes on, however it is run. Its control
    // is the one of the command that does it.
    s.lessons.next();
    CHECK (s.lessons.getText() == "Press F7 to show the track's notes in the piano roll.");
    CHECK (s.lessons.getTarget() == juce::String ("view.pianoRoll"));
    REQUIRE (s.registry.invoke ("transport.playStop"));
    CHECK (s.lessons.getStep() == 1);
    REQUIRE (s.registry.invoke ("view.pianoRoll"));
    CHECK (s.lessons.getStep() == 2);

    // A step that only the mouse does waits for Next; Back goes to the one before.
    CHECK (s.lessons.waitsForNext());
    CHECK (s.lessons.getTarget() == juce::String ("pianoRoll"));
    s.lessons.back();
    CHECK (s.lessons.getStep() == 1);
    s.lessons.next();
    s.lessons.next();
    CHECK (s.lessons.getText() == "Press Space to hear them, and again to stop.");

    for (const auto* id : { "transport.playStop", "edit.duplicate" })
        REQUIRE (s.registry.invoke (id));

    // Done: its closing words, until the card's Done ends it; and kept as done, as Help > Lessons says.
    CHECK (s.lessons.isFinished());
    CHECK (s.lessons.getText() == "That is a beat. Help > Lessons has more.");
    CHECK (s.lessons.getTarget().isEmpty());
    CHECK (s.lessons.isDone ("beat"));
    CHECK (s.settings.get (phazite::Settings::lessonsDone) == juce::StringArray { "beat" });
    CHECK (s.registry.find ("help.lesson.beat")->currentName() == "Lesson: Make a Beat (done)");
    CHECK (phazite::CommandRegistry::isEnabled (*s.registry.find ("help.stopLesson")));
    s.lessons.next();
    CHECK_FALSE (s.lessons.isRunning());
    CHECK (s.lessons.getText().isEmpty());
}

TEST_CASE ("A lesson's step that the song shows done goes on by itself, and a command without a key is found by its name")
{
    LessonSetup s;
    auto edit = phazite::createSongEdit (phazite::test::getEngine());
    setEdit (s.context, edit.get());
    const auto track = phazite::getTrackID (*te::getAudioTracks (*edit)[0]);

    REQUIRE (s.registry.invoke ("help.lesson.recording"));
    REQUIRE (s.registry.invoke ("track.add"));
    CHECK (s.lessons.getText().startsWith ("Right-click the track's arm button"));
    CHECK (s.lessons.getTarget() == juce::String ("track.arm"));

    // Armed from its button (no command): the card's next look goes on.
    phazite::setTrackInput (s.context, track, "1");
    phazite::setArmed (s.context, track, true);
    s.lessons.update();
    CHECK (s.lessons.getStep() == 2);

    // Phazite's keymap gives the metronome O and Record R; Loop Section has no key: found with the command search.
    CHECK (s.lessons.getText() == "Press O to hear the beat, then press R to record, and again to stop.");
    REQUIRE (s.registry.invoke ("transport.record"));
    CHECK (s.lessons.getText().contains ("search for section.loop with Ctrl+K"));
    CHECK (s.lessons.getTarget() == juce::String ("transport.record"));

    // Started again while a track is armed, its arming step is passed as soon as it is reached; Stop Lesson ends it.
    REQUIRE (s.registry.invoke ("help.lesson.recording"));
    CHECK (s.lessons.getStep() == 0);
    REQUIRE (s.registry.invoke ("track.add"));
    CHECK (s.lessons.getStep() == 2);
    REQUIRE (s.registry.invoke ("help.stopLesson"));
    CHECK_FALSE (s.lessons.isRunning());
    setEdit (s.context, nullptr);
}

TEST_CASE ("The lesson card shows the step and rings its control, and its buttons go back, on and stop the lesson")
{
    phazite::test::AppScreen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& card = screen.shell.getLessonCard();
    CHECK_FALSE (card.isVisible());

    REQUIRE (screen.registry.invoke ("help.lesson.effects"));
    phazite::test::dispatchPendingMessages();
    REQUIRE (card.isShowing());
    CHECK (card.getText().startsWith ("Press F9 to see the mixer"));
    phazite::test::checkChildrenApart (card);

    // In the window's corner, above the status bar; the hint bar keeps its hints.
    CHECK (screen.shell.getLocalBounds().contains (card.getBounds()));
    CHECK (card.getBottom() <= screen.shell.getStatusBar().getY());
    CHECK_FALSE (screen.shell.getHintBar().getHint().startsWith ("Add Effects"));

    // The ring goes round the Mixer tab, the control of the command the step names.
    auto* mixerTab = screen.shell.findLessonTarget ("view.mixer");
    REQUIRE (mixerTab != nullptr);
    const auto ring = card.getRing().getTarget();
    REQUIRE (ring.has_value());
    CHECK (*ring == screen.shell.getLocalArea (mixerTab, mixerTab->getLocalBounds()));

    // Skip, then Back; the close button stops the lesson, the ring with it.
    phazite::test::Mouse (card.getNextButton()).click (card.getNextButton().getLocalBounds().getCentre());
    CHECK (screen.shell.getLessons().getStep() == 1);
    CHECK (card.getText().startsWith ("Click a strip to select its track"));
    REQUIRE (card.getBackButton().isShowing());
    phazite::test::Mouse (card.getBackButton()).click (card.getBackButton().getLocalBounds().getCentre());
    CHECK (screen.shell.getLessons().getStep() == 0);

    phazite::test::Mouse (card.getCloseButton()).click (card.getCloseButton().getLocalBounds().getCentre());
    CHECK_FALSE (screen.shell.getLessons().isRunning());
    CHECK_FALSE (card.isVisible());
    CHECK_FALSE (card.getRing().isVisible());
}
