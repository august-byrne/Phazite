// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Commands.h"
#include "EngineSetup.h"

#include <PhaziteData.h>

#include <algorithm>

namespace phazite
{
namespace
{
    constexpr const char* keymapSettingKey = "phaziteKeymap";
    constexpr const char* defaultKeymapName = "Phazite";
    constexpr const char* keyChangesTag = "PHAZITEKEYCHANGES";

    juce::String getKeyChangesSettingKey (const juce::String& keymapName)
    {
        return "phaziteKeyChanges." + keymapName;
    }
} // namespace

//==============================================================================
std::optional<Keymap> Keymap::fromJSON (const juce::String& text)
{
    const auto json = juce::JSON::parse (text);
    Keymap keymap;
    keymap.name = json["name"].toString();
    keymap.description = json["description"].toString();
    keymap.rightClickDeletes = json["rightClickDeletes"];

    if (keymap.name.isEmpty())
        return {};

    if (auto* keys = json["keys"].getDynamicObject())
    {
        for (const auto& property : keys->getProperties())
        {
            juce::StringArray descriptions;

            if (auto* array = property.value.getArray())
                for (const auto& key : *array)
                    descriptions.add (key.toString());

            keymap.keys[property.name.toString()] = descriptions;
        }
    }

    return keymap;
}

juce::Array<Keymap> Keymap::getBuiltIn()
{
    juce::Array<Keymap> keymaps;

    for (const auto& [data, size] : { std::pair { PhaziteData::phazite_json, PhaziteData::phazite_jsonSize },
                                      std::pair { PhaziteData::fl_studio_json, PhaziteData::fl_studio_jsonSize } })
        if (auto keymap = fromJSON (juce::String::fromUTF8 (data, size)))
            keymaps.add (*keymap);

    return keymaps;
}

std::optional<Keymap> Keymap::findBuiltIn (const juce::String& name)
{
    for (const auto& keymap : getBuiltIn())
        if (keymap.name == name)
            return keymap;

    return {};
}

//==============================================================================
CommandRegistry::CommandRegistry()
{
    commandManager.setFirstCommandTarget (this);

    if (auto defaultKeymap = Keymap::findBuiltIn (defaultKeymapName))
        keymap = *defaultKeymap;
}

CommandRegistry::~CommandRegistry()
{
    commandManager.setFirstCommandTarget (nullptr);
}

juce::CommandID CommandRegistry::add (Command command)
{
    if (command.id.isEmpty() || command.name.isEmpty() || command.perform == nullptr || indexByID.contains (command.id))
    {
        jassertfalse; // every command needs a unique id, a name and an action
        return 0;
    }

    const auto index = commands.size();
    indexByID[command.id] = index;
    commands.push_back (std::make_unique<Command> (std::move (command)));

    // JUCE maps the command's keys from the current keymap (see getCommandInfo()).
    const auto commandID = firstCommandID + (juce::CommandID) index;
    juce::ApplicationCommandInfo info (commandID);
    getCommandInfo (commandID, info);
    commandManager.registerCommand (info);
    return commandID;
}

juce::CommandID CommandRegistry::addMacro (const juce::String& id, const juce::String& name, const juce::StringArray& commandIDs)
{
    Command macro;
    macro.id = id;
    macro.name = name;
    macro.category = "Macros";
    macro.description = "Runs " + commandIDs.joinIntoString (", ") + ".";

    macro.perform = [this, commandIDs]
    {
        for (const auto& commandID : commandIDs)
            if (auto* command = find (commandID))
                runWithoutUndoStep (*command);
    };

    // The macro is one undo step, in the Edit its first Edit-changing command uses.
    macro.undoTarget = [this, commandIDs]() -> te::Edit*
    {
        for (const auto& commandID : commandIDs)
            if (auto* command = find (commandID); command != nullptr && command->undoTarget != nullptr)
                return command->undoTarget();

        return nullptr;
    };

    return add (std::move (macro));
}

const Command* CommandRegistry::find (const juce::String& id) const
{
    const auto found = indexByID.find (id);
    return found != indexByID.end() ? commands[found->second].get() : nullptr;
}

const Command* CommandRegistry::find (juce::CommandID commandID) const
{
    const auto index = (size_t) (commandID - firstCommandID);
    return commandID >= firstCommandID && index < commands.size() ? commands[index].get() : nullptr;
}

juce::CommandID CommandRegistry::getCommandID (const juce::String& id) const
{
    const auto found = indexByID.find (id);
    return found != indexByID.end() ? firstCommandID + (juce::CommandID) found->second : 0;
}

juce::Array<const Command*> CommandRegistry::getAll() const
{
    juce::Array<const Command*> all;

    for (const auto& command : commands)
        all.add (command.get());

    return all;
}

bool CommandRegistry::isEnabled (const Command& command)
{
    return command.isEnabled == nullptr || command.isEnabled();
}

bool CommandRegistry::invoke (const juce::String& id)
{
    const auto* command = find (id);

    if (command == nullptr || ! isEnabled (*command))
        return false;

    if (command->undoTarget != nullptr)
        if (auto* edit = command->undoTarget())
            edit->getUndoManager().beginNewTransaction (getCurrentName (*command)); // e.g. "Delete 2 Tracks"

    command->perform();
    return true;
}

const Command* CommandRegistry::findCommandForKey (const juce::KeyPress& key) const
{
    const auto& mappings = *commandManager.getKeyMappings();
    const Command* found = nullptr;

    for (size_t i = 0; i < commands.size(); ++i)
    {
        const auto& command = *commands[i];

        if (found != nullptr && command.keyPriority <= found->keyPriority)
            continue;

        if (mappings.getKeyPressesAssignedToCommand (firstCommandID + (juce::CommandID) i).contains (key) && isEnabled (command))
            found = &command;
    }

    return found;
}

bool CommandRegistry::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    if (const auto* command = findCommandForKey (key))
        return runForKey (*command, key);

    return false;
}

bool CommandRegistry::runForKey (const Command& command, const juce::KeyPress& key)
{
    // A held key repeats. Only commands that take a step run again; for the others the repeat is used up.
    const auto held = std::find_if (heldKeys.begin(), heldKeys.end(),
                                    [&key, &command] (const auto& entry) { return entry.first == key && entry.second == command.id; });

    if (held != heldKeys.end())
        return command.repeatsWhileHeld ? invoke (command.id) : true;

    heldKeys.emplace_back (key, command.id);
    return invoke (command.id);
}

bool CommandRegistry::keyStateChanged (bool, juce::Component*)
{
    releaseHeldKeys ([] (const juce::KeyPress& key) { return ! juce::KeyPress::isKeyCurrentlyDown (key.getKeyCode()); });
    return false; // the other key listeners hear about it too
}

void CommandRegistry::releaseHeldKeys()
{
    releaseHeldKeys ([] (const juce::KeyPress&) { return true; });
}

void CommandRegistry::releaseHeldKeys (const std::function<bool (const juce::KeyPress&)>& isReleased)
{
    // The commands hear about it after the list is up to date, in case they press keys of their own.
    juce::StringArray released;

    for (auto it = heldKeys.begin(); it != heldKeys.end();)
    {
        if (isReleased (it->first))
        {
            released.add (it->second);
            it = heldKeys.erase (it);
        }
        else
        {
            ++it;
        }
    }

    for (const auto& id : released)
        if (const auto* command = find (id); command != nullptr && command->keyReleased != nullptr)
            command->keyReleased();
}

bool CommandRegistry::invokeWithoutShift (const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();

    if (! modifiers.isShiftDown() || modifiers.isCommandDown() || modifiers.isCtrlDown() || modifiers.isAltDown())
        return false;

    if (commandManager.getKeyMappings()->findCommandForKeyPress (key) != 0)
        return false; // Shift and the key have a command of their own

    if (const auto* command = findCommandForKey (juce::KeyPress (key.getKeyCode())))
        return runForKey (*command, key);

    return false;
}

void CommandRegistry::runWithoutUndoStep (const Command& command)
{
    if (isEnabled (command))
        command.perform();
}

juce::Array<const Command*> CommandRegistry::search (const juce::String& query, int maxResults) const
{
    std::vector<std::pair<int, const Command*>> matches;

    for (const auto& command : commands)
    {
        auto score = getMatchScore (query, command->name) * 2; // the name counts most
        score = std::max (score, getMatchScore (query, command->category + " " + command->name));

        for (const auto& synonym : command->synonyms)
            score = std::max (score, getMatchScore (query, synonym));

        if (score > 0)
            matches.emplace_back (score, command.get());
    }

    std::stable_sort (matches.begin(), matches.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });

    juce::Array<const Command*> results;

    for (const auto& match : matches)
        if (results.size() < maxResults)
            results.add (match.second);

    return results;
}

//==============================================================================
void CommandRegistry::setKeymap (const Keymap& newKeymap)
{
    keymap = newKeymap;

    // Re-register every command, so that JUCE's "default" keys are the new keymap's: the user's own
    // changes are then stored as differences from this keymap.
    commandManager.clearCommands();

    for (size_t i = 0; i < commands.size(); ++i)
    {
        juce::ApplicationCommandInfo info (firstCommandID + (juce::CommandID) i);
        getCommandInfo (info.commandID, info);
        commandManager.registerCommand (info);
    }
}

juce::String CommandRegistry::getKeyChanges() const
{
    // By command id: JUCE's own format names commands by number, and the numbers change when commands are added.
    juce::XmlElement changes (keyChangesTag);
    const auto& mappings = *commandManager.getKeyMappings();

    for (size_t i = 0; i < commands.size(); ++i)
    {
        const auto keys = mappings.getKeyPressesAssignedToCommand (firstCommandID + (juce::CommandID) i);
        juce::Array<juce::KeyPress> defaults;

        if (const auto found = keymap.keys.find (commands[i]->id); found != keymap.keys.end())
            for (const auto& description : found->second)
                if (const auto key = juce::KeyPress::createFromDescription (description); key.isValid())
                    defaults.add (key);

        const auto same =
            keys.size() == defaults.size()
            && std::all_of (keys.begin(), keys.end(), [&defaults] (const juce::KeyPress& key) { return defaults.contains (key); });

        if (same)
            continue;

        auto* change = changes.createNewChildElement ("COMMAND");
        change->setAttribute ("id", commands[i]->id);

        for (const auto& key : keys)
            change->createNewChildElement ("KEY")->setAttribute ("key", key.getTextDescription());
    }

    return changes.getNumChildElements() > 0 ? changes.toString() : juce::String();
}

void CommandRegistry::restoreKeyChanges (const juce::String& xml)
{
    // Changes that earlier versions saved in JUCE's numbered format can't be matched to commands reliably, so they
    // are left out.
    const auto parsed = juce::parseXML (xml);

    if (parsed == nullptr || ! parsed->hasTagName (keyChangesTag))
        return;

    auto& mappings = *commandManager.getKeyMappings();

    for (auto* change : parsed->getChildWithTagNameIterator ("COMMAND"))
    {
        const auto commandID = getCommandID (change->getStringAttribute ("id"));

        if (commandID == 0)
            continue;

        mappings.clearAllKeyPresses (commandID);

        for (auto* key : change->getChildWithTagNameIterator ("KEY"))
            if (const auto press = juce::KeyPress::createFromDescription (key->getStringAttribute ("key")); press.isValid())
                mappings.addKeyPress (commandID, press);
    }
}

void CommandRegistry::loadKeySettings (juce::PropertiesFile& settings)
{
    if (auto savedKeymap = Keymap::findBuiltIn (settings.getValue (keymapSettingKey, defaultKeymapName)))
        setKeymap (*savedKeymap);

    restoreKeyChanges (settings.getValue (getKeyChangesSettingKey (keymap.name)));
}

void CommandRegistry::saveKeySettings (juce::PropertiesFile& settings) const
{
    settings.setValue (keymapSettingKey, keymap.name);
    settings.setValue (getKeyChangesSettingKey (keymap.name), getKeyChanges());
    settings.saveIfNeeded();
}

juce::StringArray CommandRegistry::getShortcutDescriptions (const juce::String& id) const
{
    juce::StringArray descriptions;

    if (const auto commandID = getCommandID (id); commandID != 0)
        for (const auto& key : commandManager.getKeyMappings()->getKeyPressesAssignedToCommand (commandID))
            descriptions.add (key.getTextDescription());

    return descriptions;
}

//==============================================================================
void CommandRegistry::getAllCommands (juce::Array<juce::CommandID>& ids)
{
    for (size_t i = 0; i < commands.size(); ++i)
        ids.add (firstCommandID + (juce::CommandID) i);
}

void CommandRegistry::getCommandInfo (juce::CommandID commandID, juce::ApplicationCommandInfo& info)
{
    const auto* command = find (commandID);

    if (command == nullptr)
        return;

    info.setInfo (getCurrentName (*command), command->description, command->category, 0);
    info.setActive (isEnabled (*command));
    info.setTicked (command->isTicked != nullptr && command->isTicked());

    if (const auto keys = keymap.keys.find (command->id); keys != keymap.keys.end())
        for (const auto& description : keys->second)
            if (const auto key = juce::KeyPress::createFromDescription (description); key.isValid())
                info.addDefaultKeypress (key.getKeyCode(), key.getModifiers());
}

bool CommandRegistry::perform (const InvocationInfo& invocation)
{
    if (const auto* command = find (invocation.commandID))
        return invoke (command->id);

    return false;
}

//==============================================================================
juce::String getCurrentName (const Command& command)
{
    if (command.currentName != nullptr)
        if (const auto name = command.currentName(); name.isNotEmpty())
            return name;

    return command.name;
}

//==============================================================================
int getMatchScore (const juce::String& query, const juce::String& text)
{
    const auto q = query.trim().toLowerCase();
    const auto t = text.toLowerCase();

    if (q.isEmpty())
        return 1;

    int score = 0, run = 0, ti = 0;

    for (int qi = 0; qi < q.length(); ++qi)
    {
        const auto qc = q[qi];

        if (qc == ' ') // a space in the query only separates words
        {
            run = 0;
            continue;
        }

        bool found = false;

        while (ti < t.length())
        {
            const auto tc = t[ti];
            const bool startsWord = ti == 0 || ! juce::CharacterFunctions::isLetterOrDigit (t[ti - 1]);
            ++ti;

            if (tc == qc)
            {
                score += 1 + (startsWord ? 6 : 0) + 3 * run;
                ++run;
                found = true;
                break;
            }

            run = 0;
        }

        if (! found)
            return 0;
    }

    if (t.startsWith (q))
        score += 10;

    return score;
}

//==============================================================================
namespace
{
    // AutomatableParameter::valueTreePropertyChanged() updates a parameter's CachedValue when undo
    // changes it, but not its currentValue, which the audio thread reads. Automated parameters
    // follow their curves instead, so they are left alone.
    void playStoredParameterValues (te::Edit& edit)
    {
        for (auto* parameter : edit.getAllAutomatableParams (true))
            if (parameter->getCurve().getNumPoints() == 0)
                parameter->updateFromAttachedValue();
    }
} // namespace

void undo (te::Edit& edit)
{
    edit.undo();
    playStoredParameterValues (edit);
}

void redo (te::Edit& edit)
{
    edit.redo();
    playStoredParameterValues (edit);
}

//==============================================================================
void addEditCommands (CommandRegistry& registry, UIBehaviour& ui)
{
    const auto focusedEdit = [&ui] { return ui.getLastFocusedEdit(); };

    // "Undo Change Volume": the name of the step, when it has one.
    const auto nameWithStep = [focusedEdit] (bool isUndo)
    {
        const juce::String verb (isUndo ? "Undo" : "Redo");

        if (auto* edit = focusedEdit())
        {
            auto& undoManager = edit->getUndoManager();
            const auto step = isUndo ? undoManager.getUndoDescription() : undoManager.getRedoDescription();

            if ((isUndo ? undoManager.canUndo() : undoManager.canRedo()) && step.isNotEmpty())
                return verb + " " + step;
        }

        return verb;
    };

    registry.add ({ .id = "edit.undo",
                    .name = "Undo",
                    .category = "Edit",
                    .description = "Undoes the last change.",
                    .synonyms = { "back", "revert" },
                    .perform =
                        [focusedEdit]
                    {
                        if (auto* edit = focusedEdit())
                            undo (*edit);
                    },
                    .isEnabled =
                        [focusedEdit]
                    {
                        auto* edit = focusedEdit();
                        return edit != nullptr && edit->getUndoManager().canUndo();
                    },
                    .currentName = [nameWithStep] { return nameWithStep (true); },
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "edit.redo",
                    .name = "Redo",
                    .category = "Edit",
                    .description = "Redoes the change you last undid.",
                    .synonyms = { "again" },
                    .perform =
                        [focusedEdit]
                    {
                        if (auto* edit = focusedEdit())
                            redo (*edit);
                    },
                    .isEnabled =
                        [focusedEdit]
                    {
                        auto* edit = focusedEdit();
                        return edit != nullptr && edit->getUndoManager().canRedo();
                    },
                    .currentName = [nameWithStep] { return nameWithStep (false); },
                    .repeatsWhileHeld = true });

    registry.add ({ .id = "edit.undoOrRedo",
                    .name = "Undo or Redo",
                    .category = "Edit",
                    .description = "Undoes the last change, or redoes it if you just undid it, like Ctrl+Z in FL Studio.",
                    .synonyms = { "undo", "redo", "toggle" },
                    .perform =
                        [focusedEdit]
                    {
                        if (auto* edit = focusedEdit())
                        {
                            if (edit->getUndoManager().canRedo())
                                redo (*edit);
                            else
                                undo (*edit);
                        }
                    },
                    .isEnabled =
                        [focusedEdit]
                    {
                        auto* edit = focusedEdit();
                        return edit != nullptr && (edit->getUndoManager().canUndo() || edit->getUndoManager().canRedo());
                    },
                    .currentName =
                        [focusedEdit, nameWithStep]
                    {
                        auto* edit = focusedEdit();
                        return nameWithStep (! (edit != nullptr && edit->getUndoManager().canRedo()));
                    } });

    registry.add ({ .id = "transport.playStop",
                    .name = "Play / Stop",
                    .category = "Transport",
                    .description = "Starts or stops playback.",
                    .synonyms = { "start", "pause", "playback" },
                    .perform =
                        [focusedEdit]
                    {
                        if (auto* edit = focusedEdit())
                        {
                            auto& transport = edit->getTransport();

                            if (transport.isPlaying())
                                transport.stop (false, false);
                            else
                                transport.play (false);
                        }
                    },
                    .isEnabled = [focusedEdit] { return focusedEdit() != nullptr; } });

    registry.add ({ .id = "transport.goToStart",
                    .name = "Go to Start",
                    .category = "Transport",
                    .description = "Moves the playhead to the start of the song.",
                    .synonyms = { "home", "beginning", "rewind", "return" },
                    .perform =
                        [focusedEdit]
                    {
                        if (auto* edit = focusedEdit())
                            edit->getTransport().setPosition (te::TimePosition());
                    },
                    .isEnabled = [focusedEdit] { return focusedEdit() != nullptr; } });

    registry.add ({ .id = "transport.goToEnd",
                    .name = "Go to End",
                    .category = "Transport",
                    .description = "Moves the playhead to the end of the song, where its last clip ends.",
                    .synonyms = { "end", "last", "fast forward" },
                    .perform =
                        [focusedEdit]
                    {
                        if (auto* edit = focusedEdit())
                            edit->getTransport().setPosition (te::toPosition (edit->getLength()));
                    },
                    .isEnabled = [focusedEdit] { return focusedEdit() != nullptr; } });
}
} // namespace phazite
