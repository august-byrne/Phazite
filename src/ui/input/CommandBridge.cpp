// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CommandBridge.h"

namespace phazite::ui
{
CommandBridge::CommandBridge (CommandRegistry& r)
    : registry (r)
{
    commandManager.setFirstCommandTarget (this);

    if (auto defaultKeymap = Keymap::findBuiltIn (Keymap::defaultName))
        keymap = *defaultKeymap;

    // The commands there already, then each one added after (commandAdded()).
    for (int i = 0; i < registry.getNumCommands(); ++i)
        registerWithManager (firstCommandID + i);

    registry.addListener (this);
}

CommandBridge::~CommandBridge()
{
    registry.removeListener (this);
    commandManager.setFirstCommandTarget (nullptr);
}

juce::CommandID CommandBridge::getCommandID (const juce::String& id) const
{
    const auto index = registry.indexOf (id);
    return index >= 0 ? firstCommandID + index : 0;
}

const Command* CommandBridge::find (juce::CommandID commandID) const
{
    return commandID >= firstCommandID ? registry.getCommand (commandID - firstCommandID) : nullptr;
}

void CommandBridge::registerWithManager (juce::CommandID commandID)
{
    // JUCE maps the command's keys from the current keymap (see getCommandInfo()).
    juce::ApplicationCommandInfo info (commandID);
    getCommandInfo (commandID, info);
    commandManager.registerCommand (info);
}

void CommandBridge::commandAdded (const Command& command)
{
    registerWithManager (getCommandID (command.id));
}

void CommandBridge::setKeymap (const Keymap& newKeymap)
{
    keymap = newKeymap;

    // Re-register every command, so that JUCE's "default" keys are the new keymap's: the user's own
    // changes are then stored as differences from this keymap.
    commandManager.clearCommands();

    for (int i = 0; i < registry.getNumCommands(); ++i)
        registerWithManager (firstCommandID + i);
}

juce::Array<juce::KeyPress> CommandBridge::getKeyPresses (const juce::String& id) const
{
    if (const auto commandID = getCommandID (id); commandID != 0)
        return getKeyMappings().getKeyPressesAssignedToCommand (commandID);

    return {};
}

juce::StringArray CommandBridge::getShortcutDescriptions (const juce::String& id) const
{
    juce::StringArray descriptions;

    for (const auto& key : getKeyPresses (id))
        descriptions.add (key.getTextDescription());

    return descriptions;
}

//==============================================================================
void CommandBridge::getAllCommands (juce::Array<juce::CommandID>& ids)
{
    for (int i = 0; i < registry.getNumCommands(); ++i)
        ids.add (firstCommandID + i);
}

void CommandBridge::getCommandInfo (juce::CommandID commandID, juce::ApplicationCommandInfo& info)
{
    const auto* command = find (commandID);

    if (command == nullptr)
        return;

    info.setInfo (getCurrentName (*command), command->description, command->category, 0);
    info.setActive (CommandRegistry::isEnabled (*command));
    info.setTicked (command->isTicked != nullptr && command->isTicked());

    if (const auto keys = keymap.keys.find (command->id); keys != keymap.keys.end())
        for (const auto& description : keys->second)
            if (const auto key = juce::KeyPress::createFromDescription (description); key.isValid())
                info.addDefaultKeypress (key.getKeyCode(), key.getModifiers());
}

bool CommandBridge::perform (const InvocationInfo& invocation)
{
    if (const auto* command = find (invocation.commandID))
        return registry.invoke (command->id);

    return false;
}
} // namespace phazite::ui
