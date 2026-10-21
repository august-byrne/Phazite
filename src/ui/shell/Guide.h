// SPDX-License-Identifier: AGPL-3.0-or-later
// The guide in the hint bar (docs/design.md, section 13; roadmap step 1.9): what a newcomer could try next, one step at a
// time, shown as the hint bar's tip while the pointer rests on nothing that has one of its own. A step is done once the
// user has done it, in any way (a key, a button, a menu or the command palette), and stays done: the settings keep the
// steps done, so the guide goes on from there in the next session. Help > Getting Started Tips turns it off and on.
#pragma once

#include "services/Settings.h"
#include "ui/input/CommandBridge.h"

#include <optional>

namespace phazite::ui
{
class Guide : private CommandRegistry::Listener
{
public:
    // The steps, in the order they are suggested.
    enum class Step
    {
        play,      // play the song
        playNotes, // play an instrument with the computer keyboard
        addSound,  // put a sound from the browser into the song
        record,    // record what is played (or capture it)
        exportSong // export the song
    };

    static constexpr int numSteps = 5;

    Guide (CommandBridge&, Settings&);
    ~Guide() override;

    // The first step not done yet, or nothing once every step is.
    std::optional<Step> getNextStep() const;
    bool isDone (Step) const;

    // The next step's tip, with the keys of the keymap in use; empty once every step is done, or while the guide is off.
    juce::String getHint() const;

    // A step that no command does was done: a note played on the computer keyboard, a sound put into the song from the
    // browser. The commands' steps are done when they run.
    void markDone (Step);

    bool isOn() const;
    void setOn (bool);
    void toggle();

    // "help.guide", the guide's switch.
    void addCommands (CommandRegistry&);

private:
    CommandBridge& commands;
    Settings& settings;
    juce::StringArray done; // the steps done, by name: the settings' list, read once (only the guide writes it)

    void commandAdded (const Command&) override {}
    void commandInvoked (const Command&) override; // play, record, capture and export are steps
};
} // namespace phazite::ui
