// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's commands: every user action as a named command that menus, shortcuts, the command
// palette and macros run (and, later, scripts and an assistant). The registry has no UI code: menus,
// keys and the shortcuts editor reach it through ui::CommandBridge and ui::KeyDispatcher (src/ui/input). No Tracktion here
// (architecture phase A3): a command that changes a project names the project's undo manager.
#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace phazite
{
//==============================================================================
// Written with designated initializers that leave out what a command doesn't need. Every member has
// a default ({}), because GCC's -Wmissing-field-initializers warns about left-out members without one.
struct Command
{
    juce::String id {};            // stable, e.g. "transport.playStop": keymaps and macros refer to it
    juce::String name {};          // shown in menus, the palette and as the undo step's name
    juce::String category {};      // "Transport", "Edit", "Project", "View", ...
    juce::String description {};   // one sentence, for hover help and the hint bar
    juce::StringArray synonyms {}; // other words the palette's search should match
    std::function<void()> perform {};
    std::function<bool()> isEnabled {}; // optional: when false the command is greyed out and doesn't run
    // Optional: the undo manager of the project this command changes (Undo.h: undoTargetOf(); ProjectContext's
    // getUndoManager()). Running the command then begins an undo step named after it (an UndoStep, Undo.h), so the
    // whole command undoes in one go.
    std::function<juce::UndoManager*()> undoTarget {};
    std::function<bool()> isTicked {}; // optional: for on/off settings, shown with a tick in menus
    // Optional: the name as it is now, which menus and the palette show, such as "Undo Change Volume".
    std::function<juce::String()> currentName {};
    // Optional: when commands share a key, the enabled one with the highest priority runs. Commands for
    // selected notes use notePriority, so the arrow keys move notes while notes are selected and select
    // tracks otherwise.
    int keyPriority {};
    // Optional: whether holding the command's key runs it again each time the key repeats, for commands
    // that take a step (nudging, zooming, moving through tracks, undo). Others run once per press, so that
    // holding Space doesn't start and stop playing over and over.
    bool repeatsWhileHeld {};
    // Optional: called when the key that ran the command is let go. The tools use it to spring back.
    std::function<void()> keyReleased {};
    // Optional: whether the command's keys work while a plug-in's window has the keyboard focus, so that a sound can be
    // played while it is tweaked (play and stop). The plug-in gets every other key.
    bool overPlugins {};

    static constexpr int notePriority = 1;
};

// The command's name as menus and the palette show it now (its currentName, if it has one).
juce::String getCurrentName (const Command&);

//==============================================================================
class CommandRegistry
{
public:
    CommandRegistry() = default;
    ~CommandRegistry(); // its listeners (a ui::CommandBridge) go first

    // Adds a command; its id must be new, and it needs a name and an action. False if not.
    bool add (Command);

    // A command that runs other commands in order, as one undo step (named after the macro).
    bool addMacro (const juce::String& id, const juce::String& name, const juce::StringArray& commandIDs);

    const Command* find (const juce::String& id) const;
    juce::Array<const Command*> getAll() const;

    // The commands in the order they were added: JUCE's command numbers and the order in which keys find commands
    // follow it.
    int getNumCommands() const noexcept { return (int) commands.size(); }
    const Command* getCommand (int index) const; // nullptr if there is none
    int indexOf (const juce::String& id) const;  // -1 if there is none

    // Runs a command if it exists and is enabled, in one undo step named after it if it changes an Edit.
    bool invoke (const juce::String& id);
    static bool isEnabled (const Command&);

    // The command palette's search: matches on the name, category and synonyms, best first.
    juce::Array<const Command*> search (const juce::String& query, int maxResults = 20) const;

    // Told about each command added from then on (the ones added before are getCommand()'s), and about each command that
    // invoke() ran, once it has run.
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void commandAdded (const Command&) = 0;
        virtual void commandInvoked (const Command&) {}
    };

    void addListener (Listener*);
    void removeListener (Listener*);

private:
    std::vector<std::unique_ptr<Command>> commands;
    std::map<juce::String, size_t> indexByID;
    juce::ListenerList<Listener> listeners;

    void runWithoutUndoStep (const Command&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CommandRegistry)
};

// How well a search matches some text: 0 for no match, higher for better matches. Every character
// of the query must appear in the text in order; the start of words and runs of characters score
// higher, and a match of the whole beginning scores highest.
int getMatchScore (const juce::String& query, const juce::String& text);
} // namespace phazite
