// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Keymap.h"
#include "model/commands/CommandRegistry.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// The command registry as JUCE's menus, key mappings and shortcuts editor see it: every command in JUCE's
// ApplicationCommandManager, with the keys of the current keymap and the user's changes to them. A command's JUCE
// CommandID is its place in the registry's order, from 0x1000; commands added later are registered as they come.
class CommandBridge : public juce::ApplicationCommandTarget,
                      private CommandRegistry::Listener
{
public:
    explicit CommandBridge (CommandRegistry&); // Phazite's keymap, until setKeymap()
    ~CommandBridge() override;

    CommandRegistry& getRegistry() noexcept { return registry; }
    const CommandRegistry& getRegistry() const noexcept { return registry; }

    // Runs a command if it exists and is enabled (CommandRegistry::invoke()).
    bool invoke (const juce::String& id) { return registry.invoke (id); }

    juce::CommandID getCommandID (const juce::String& id) const; // 0 if unknown

    // Shortcuts: a keymap, plus the user's own changes to it (KeymapStore.h keeps them).
    void setKeymap (const Keymap&);
    const Keymap& getKeymap() const noexcept { return keymap; }

    // A command's keys now, and as shown to the user ("ctrl + Z").
    juce::Array<juce::KeyPress> getKeyPresses (const juce::String& id) const;
    juce::StringArray getShortcutDescriptions (const juce::String& id) const;

    // A command's keys as the keymap gives them, before the user's changes.
    juce::Array<juce::KeyPress> getDefaultKeyPresses (const juce::String& id) const;

    // The user's changes, as the shortcuts editor makes them. A key given to a command stays with every other command that
    // has it: commands share keys on purpose, the one with the highest keyPriority running (KeyDispatcher). The settings
    // keep each change as the bridge's key mappings tell of it (KeymapStore.h).
    bool addKey (const juce::String& id, const juce::KeyPress&); // whether the command has the key now (not one not valid)
    void removeKey (const juce::String& id, const juce::KeyPress&);
    void resetKeys (const juce::String& id); // the keymap's keys again
    void resetAllKeys();

    juce::ApplicationCommandManager& getCommandManager() noexcept { return commandManager; }
    juce::KeyPressMappingSet& getKeyMappings() const noexcept { return *commandManager.getKeyMappings(); }

    //==============================================================================
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

private:
    static constexpr juce::CommandID firstCommandID = 0x1000;

    CommandRegistry& registry;
    juce::ApplicationCommandManager commandManager;
    Keymap keymap;

    const Command* find (juce::CommandID) const;
    void registerWithManager (juce::CommandID);
    void commandAdded (const Command&) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CommandBridge)
};
} // namespace phazite::ui
