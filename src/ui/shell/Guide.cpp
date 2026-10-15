// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Guide.h"
#include "ui/input/CommandUI.h"

namespace phazite::ui
{
namespace
{
    // A step's name, as the settings keep it: never rename one.
    const char* getName (Guide::Step step)
    {
        switch (step)
        {
            case Guide::Step::play:
                return "play";
            case Guide::Step::playNotes:
                return "playNotes";
            case Guide::Step::addSound:
                return "addSound";
            case Guide::Step::record:
                return "record";
            case Guide::Step::exportSong:
                return "export";
        }

        return "";
    }

    // "press Space", or without a key, the other way to do it.
    juce::String pressOr (const CommandBridge& commands, const juce::String& commandID, const juce::String& otherwise)
    {
        const auto key = getKeyText (commands, commandID);
        return key.isNotEmpty() ? "press " + key : otherwise;
    }

    bool isComputerKeyboardOff (const CommandBridge& commands)
    {
        const auto* command = commands.getRegistry().find ("input.computerKeyboard");
        return command != nullptr && command->isTicked != nullptr && ! command->isTicked();
    }
} // namespace

Guide::Guide (CommandBridge& bridge, Settings& s)
    : commands (bridge),
      settings (s)
{
    commands.getRegistry().addListener (this);
}

Guide::~Guide()
{
    commands.getRegistry().removeListener (this);
}

bool Guide::isDone (Step step) const
{
    return settings.get (Settings::guideStepsDone).contains (getName (step));
}

std::optional<Guide::Step> Guide::getNextStep() const
{
    for (int i = 0; i < numSteps; ++i)
        if (! isDone ((Step) i))
            return (Step) i;

    return {};
}

void Guide::markDone (Step step)
{
    if (isDone (step))
        return;

    auto done = settings.get (Settings::guideStepsDone);
    done.add (getName (step));
    settings.set (Settings::guideStepsDone, done);
}

void Guide::commandInvoked (const Command& command)
{
    const auto& id = command.id;

    if (id == "transport.playStop" || id == "transport.playPause")
    {
        markDone (Step::play);
    }
    else if (id == "transport.record" || id == "transport.capture")
    {
        markDone (Step::playNotes); // recording what was played: playing came first, on whatever keyboard
        markDone (Step::record);
    }
    else if (id == "project.export")
    {
        markDone (Step::exportSong);
    }
}

juce::String Guide::getHint() const
{
    const auto next = getNextStep();

    if (! isOn() || ! next.has_value())
        return {};

    switch (*next)
    {
        case Step::play:
        {
            const auto key = getKeyText (commands, "transport.playStop");
            return "Try this: "
                   + (key.isNotEmpty() ? "press " + key + " to play the song, and " + key + " again to stop it."
                                       : juce::String ("click the play button at the top left to play the song."));
        }

        case Step::playNotes:
            if (isComputerKeyboardOff (commands))
                return "Try this: " + pressOr (commands, "input.computerKeyboard", "click the keyboard button at the top")
                       + " to play notes on your computer keyboard, then A S D F G H J K L on a track with an instrument.";

            return "Try this: select a track with an instrument and play it on your computer keyboard: A S D F G H J K L are the "
                   "white keys, W E T Y U O P the black ones.";

        case Step::addSound:
            return "Try this: drag a sound from the browser on the left onto a track, or below the tracks for a new track.";

        case Step::record:
        {
            auto text = "Try this: " + pressOr (commands, "transport.record", "click the red record button")
                        + " to record what you play onto the selected track, and again to stop.";

            if (const auto capture = getKeyText (commands, "transport.capture"); capture.isNotEmpty())
                text << " Played something good without recording? " << capture << " captures it.";

            return text;
        }

        case Step::exportSong:
            return "Try this: " + pressOr (commands, "project.export", "choose File > Export Song")
                   + " to export the song as a WAV file that plays anywhere.";
    }

    return {};
}

bool Guide::isOn() const
{
    return settings.get (Settings::guide);
}

void Guide::setOn (bool on)
{
    settings.set (Settings::guide, on);
}

void Guide::toggle()
{
    setOn (! isOn());
}

void Guide::addCommands (CommandRegistry& registry)
{
    registry.add ({ .id = "help.guide",
                    .name = "Getting Started Tips",
                    .category = "Help",
                    .description = "Shows what to try next in the hint bar, such as playing the song or recording what you play, until "
                                   "you have done each.",
                    .synonyms = { "guide", "tips", "tutorial", "hints", "learn", "next step", "onboarding" },
                    .perform = [this] { toggle(); },
                    .isTicked = [this] { return isOn(); } });
}
} // namespace phazite::ui
