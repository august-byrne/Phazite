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
struct Command
{
    juce::String id;            // stable, e.g. "transport.playStop": keymaps and macros refer to it
    juce::String name;          // shown in menus, the palette and as the undo step's name
    juce::String category;      // "Transport", "Edit", "Project", "View", ...
    juce::String description;   // one sentence, for hover help and the hint bar
    juce::StringArray synonyms; // other words the palette's search should match
    std::function<void()> perform;
    std::function<bool()> isEnabled; // optional: when false the command is greyed out and doesn't run
    // Optional: the Edit this command changes. Running the command then starts a new undo step
    // named after it, so the whole command undoes in one go.
    std::function<te::Edit*()> undoTarget;
};

//==============================================================================
// A set of shortcuts: command id -> key descriptions in JUCE's format ("command + Z", "spacebar",
// "F5"). "command" is Ctrl on Windows and Linux and Cmd on macOS.
struct Keymap
{
    juce::String name, description;
    std::map<juce::String, juce::StringArray> keys;

    static std::optional<Keymap> fromJSON (const juce::String&);

    // The keymaps that ship with Phazite (resources/keymaps): "Phazite" (the default) and "FL Studio".
    static juce::Array<Keymap> getBuiltIn();
    static std::optional<Keymap> findBuiltIn (const juce::String& name);
};

//==============================================================================
class CommandRegistry : public juce::ApplicationCommandTarget
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

    const Command* find (juce::CommandID) const;
    void runWithoutUndoStep (const Command&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CommandRegistry)
};

// How well a search matches some text: 0 for no match, higher for better matches. Every character
// of the query must appear in the text in order; the start of words and runs of characters score
// higher, and a match of the whole beginning scores highest.
int getMatchScore (const juce::String& query, const juce::String& text);

// The commands that act on the focused Edit (UIBehaviour::getLastFocusedEdit): undo, redo,
// FL Studio's undo-or-redo, and play/stop.
void addEditCommands (CommandRegistry&, UIBehaviour&);
} // namespace phazite
