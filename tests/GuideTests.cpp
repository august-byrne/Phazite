// SPDX-License-Identifier: AGPL-3.0-or-later
// The guide in the hint bar (roadmap step 1.9, src/ui/shell/Guide.h): the next step a newcomer could try, with the keys of
// the keymap in use, each step gone once it is done in any way, and kept done in the settings.
#include "TestHelpers.h"
#include "model/commands/CommandRegistry.h"
#include "services/Settings.h"
#include "ui/input/CommandBridge.h"
#include "ui/shell/Guide.h"

using Step = phazite::ui::Guide::Step;

namespace
{
// The commands the guide watches and names, doing nothing, with Phazite's keymap.
struct GuideSetup
{
    GuideSetup()
    {
        for (const auto* id : { "transport.playStop", "transport.record", "transport.capture", "project.export" })
            registry.add ({ .id = id, .name = id, .perform = [] {} });

        registry.add ({ .id = "input.computerKeyboard",
                        .name = "Computer Keyboard",
                        .perform = [this] { keyboardOn = ! keyboardOn; },
                        .isTicked = [this] { return keyboardOn; } });
    }

    phazite::Settings settings; // in memory
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    bool keyboardOn = true;
};
} // namespace

TEST_CASE ("The guide suggests one step at a time, each gone once it is done, in any way")
{
    GuideSetup s;
    phazite::ui::Guide guide (s.commands, s.settings);
    CHECK (guide.getNextStep() == Step::play);
    CHECK (guide.getHint() == "Try this: press Space to play the song, and Space again to stop it.");

    // The play button, Space, the menu and the palette all run the command.
    s.registry.invoke ("transport.playStop");
    CHECK (guide.isDone (Step::play));
    CHECK (guide.getNextStep() == Step::playNotes);
    CHECK (guide.getHint().contains ("play it on your computer keyboard: A S D F G H J K L"));

    // Played on the computer keyboard (the main window tells the guide).
    guide.markDone (Step::playNotes);
    CHECK (guide.getNextStep() == Step::addSound);
    CHECK (guide.getHint().contains ("drag a sound from the browser"));

    guide.markDone (Step::addSound);
    CHECK (guide.getNextStep() == Step::record);
    CHECK (guide.getHint().startsWith ("Try this: press R to record what you play"));
    CHECK (guide.getHint().contains ("+Shift+C captures it."));

    s.registry.invoke ("transport.capture"); // a capture records too
    CHECK (guide.getNextStep() == Step::exportSong);
    CHECK (guide.getHint().contains ("+Shift+R to export the song"));

    s.registry.invoke ("project.export");
    CHECK_FALSE (guide.getNextStep().has_value());
    CHECK (guide.getHint().isEmpty());

    // The settings keep what was done, for the next session.
    phazite::ui::Guide later (s.commands, s.settings);
    CHECK_FALSE (later.getNextStep().has_value());
    CHECK (s.settings.get (phazite::Settings::guideStepsDone) == juce::StringArray { "play", "playNotes", "addSound", "record", "export" });
}

TEST_CASE ("The guide skips what recording shows was done, says how to turn the computer keyboard on, and can be turned off")
{
    GuideSetup s;
    phazite::ui::Guide guide (s.commands, s.settings);
    guide.addCommands (s.registry);

    s.registry.invoke ("transport.playStop");
    s.keyboardOn = false;
    CHECK (guide.getHint().startsWith ("Try this: press M to play notes on your computer keyboard"));

    // Recording what was played (on a MIDI keyboard, say): playing notes was done too.
    s.registry.invoke ("transport.record");
    CHECK (guide.isDone (Step::playNotes));
    CHECK (guide.isDone (Step::record));
    CHECK (guide.getNextStep() == Step::addSound);

    // Help > Getting Started Tips.
    const auto* tips = s.registry.find ("help.guide");
    REQUIRE (tips != nullptr);
    CHECK (tips->isTicked());
    s.registry.invoke ("help.guide");
    CHECK_FALSE (guide.isOn());
    CHECK_FALSE (tips->isTicked());
    CHECK (guide.getHint().isEmpty());
    CHECK (guide.getNextStep() == Step::addSound); // still there for when it is on again

    s.registry.invoke ("help.guide");
    CHECK (guide.getHint().contains ("drag a sound from the browser"));
}
