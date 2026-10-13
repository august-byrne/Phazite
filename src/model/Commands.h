// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's commands: every user action as a named command that menus, shortcuts, the command
// palette and macros run (and, later, scripts and an assistant).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include <map>
#include <optional>

namespace phazite
{
namespace te = tracktion;
class UIBehaviour;

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
    // Optional: the Edit this command changes. Running the command then starts a new undo step
    // named after it, so the whole command undoes in one go.
    std::function<te::Edit*()> undoTarget {};
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

    static constexpr int notePriority = 1;
};

// The command's name as menus and the palette show it now (its currentName, if it has one).
juce::String getCurrentName (const Command&);

//==============================================================================
// A set of shortcuts: command id -> key descriptions in JUCE's format ("command + Z", "spacebar",
// "F5"). "command" is Ctrl on Windows and Linux and Cmd on macOS.
struct Keymap
{
    juce::String name, description;
    std::map<juce::String, juce::StringArray> keys;
    bool rightClickDeletes = false; // in the playlist, as in FL Studio; otherwise right-click opens a menu

    static std::optional<Keymap> fromJSON (const juce::String&);

    // The keymaps that ship with Phazite (resources/keymaps): "Phazite" (the default) and "FL Studio".
    static juce::Array<Keymap> getBuiltIn();
    static std::optional<Keymap> findBuiltIn (const juce::String& name);
};

//==============================================================================
class CommandRegistry : public juce::ApplicationCommandTarget,
                        public juce::KeyListener
{
public:
    CommandRegistry();
    ~CommandRegistry() override;

    // Adds a command; its id must be new. Returns its JUCE CommandID, or 0 if the id was taken.
    juce::CommandID add (Command);

    // A command that runs other commands in order, as one undo step (named after the macro).
    juce::CommandID addMacro (const juce::String& id, const juce::String& name, const juce::StringArray& commandIDs);

    const Command* find (const juce::String& id) const;
    juce::CommandID getCommandID (const juce::String& id) const; // 0 if unknown
    juce::Array<const Command*> getAll() const;

    // Runs a command if it exists and is enabled.
    bool invoke (const juce::String& id);

    // The command a key runs: of the enabled commands it is a shortcut of, the one with the highest
    // keyPriority (the one registered first, if several have it). nullptr if there is none.
    const Command* findCommandForKey (const juce::KeyPress&) const;

    // Runs the key's command (findCommandForKey()). Windows add the registry as a key listener, so the
    // shortcuts work in them. While the key is held, its repeats run the command again only if it
    // repeatsWhileHeld; keyStateChanged() sees the key let go.
    bool keyPressed (const juce::KeyPress&, juce::Component*) override;
    bool keyStateChanged (bool isKeyDown, juce::Component*) override;

    // Forgets the held keys, as if they were let go: when the window loses the keyboard, their release goes
    // elsewhere.
    void releaseHeldKeys();

    // While the computer keyboard plays notes, single-key shortcuts are typed with Shift held, as in
    // Ableton Live: for Shift and a key, this runs the command of the key alone, unless Shift and the
    // key have a command of their own. Returns true if it ran a command.
    bool invokeWithoutShift (const juce::KeyPress&);

    // The command palette's search: matches on the name, category and synonyms, best first.
    juce::Array<const Command*> search (const juce::String& query, int maxResults = 20) const;

    //==============================================================================
    // Shortcuts: a keymap, plus the user's own changes to it.
    void setKeymap (const Keymap&);
    const Keymap& getKeymap() const noexcept { return keymap; }

    // The user's changes to the current keymap, as XML, and back.
    juce::String getKeyChanges() const;
    void restoreKeyChanges (const juce::String& xml);

    // The keymap and the user's changes, kept in Phazite's settings.
    void loadKeySettings (juce::PropertiesFile&);
    void saveKeySettings (juce::PropertiesFile&) const;

    // The shortcuts of a command, as shown to the user ("Ctrl + Z").
    juce::StringArray getShortcutDescriptions (const juce::String& id) const;

    juce::ApplicationCommandManager& getCommandManager() noexcept { return commandManager; }

    //==============================================================================
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

private:
    static constexpr juce::CommandID firstCommandID = 0x1000;

    juce::ApplicationCommandManager commandManager;
    std::vector<std::unique_ptr<Command>> commands; // commands[i] has CommandID firstCommandID + i
    std::map<juce::String, size_t> indexByID;
    Keymap keymap;
    // The keys that ran commands, while they are down, with those commands' ids.
    std::vector<std::pair<juce::KeyPress, juce::String>> heldKeys;

    const Command* find (juce::CommandID) const;
    bool runForKey (const Command&, const juce::KeyPress&);
    void releaseHeldKeys (const std::function<bool (const juce::KeyPress&)>& isReleased);
    void runWithoutUndoStep (const Command&);
    static bool isEnabled (const Command&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CommandRegistry)
};

// How well a search matches some text: 0 for no match, higher for better matches. Every character
// of the query must appear in the text in order; the start of words and runs of characters score
// higher, and a match of the whole beginning scores highest.
int getMatchScore (const juce::String& query, const juce::String& text);

// The commands that act on the focused Edit (UIBehaviour::getLastFocusedEdit): undo, redo,
// FL Studio's undo-or-redo, play/stop, and going to the start or end.
void addEditCommands (CommandRegistry&, UIBehaviour&);

// Undo and redo for an Edit. Use these, not Edit::undo() and redo(): Tracktion's undo puts back a
// parameter's stored value (a track's volume, say) but not the value the engine plays with, so
// these then set every parameter that isn't automated to its stored value.
void undo (te::Edit&);
void redo (te::Edit&);
} // namespace phazite
