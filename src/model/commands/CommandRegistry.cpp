// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CommandRegistry.h"
#include "Undo.h"

#include <algorithm>

namespace phazite
{
CommandRegistry::~CommandRegistry()
{
    jassert (listeners.isEmpty()); // a ui::CommandBridge must go before the registry it shows
}

bool CommandRegistry::add (Command command)
{
    if (command.id.isEmpty() || command.name.isEmpty() || command.perform == nullptr || indexByID.contains (command.id))
    {
        jassertfalse; // every command needs a unique id, a name and an action
        return false;
    }

    indexByID[command.id] = commands.size();
    commands.push_back (std::make_unique<Command> (std::move (command)));

    const auto& added = *commands.back();
    listeners.call ([&added] (Listener& listener) { listener.commandAdded (added); });
    return true;
}

bool CommandRegistry::addMacro (const juce::String& id, const juce::String& name, const juce::StringArray& commandIDs)
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

juce::Array<const Command*> CommandRegistry::getAll() const
{
    juce::Array<const Command*> all;

    for (const auto& command : commands)
        all.add (command.get());

    return all;
}

const Command* CommandRegistry::getCommand (int index) const
{
    return juce::isPositiveAndBelow (index, getNumCommands()) ? commands[(size_t) index].get() : nullptr;
}

int CommandRegistry::indexOf (const juce::String& id) const
{
    const auto found = indexByID.find (id);
    return found != indexByID.end() ? (int) found->second : -1;
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

    if (auto* edit = command->undoTarget != nullptr ? command->undoTarget() : nullptr)
    {
        const UndoStep step (*edit, getCurrentName (*command)); // e.g. "Delete 2 Tracks"
        command->perform();
    }
    else
    {
        command->perform();
    }

    listeners.call ([command] (Listener& l) { l.commandInvoked (*command); });
    return true;
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

void CommandRegistry::addListener (Listener* listener)
{
    listeners.add (listener);
}

void CommandRegistry::removeListener (Listener* listener)
{
    listeners.remove (listener);
}

//==============================================================================
juce::String getCurrentName (const Command& command)
{
    if (command.currentName != nullptr)
        if (const auto name = command.currentName(); name.isNotEmpty())
            return name;

    return command.name;
}

std::function<void()> withEdit (const te::SelectionManager& selection, std::function<void (te::Edit&)> action)
{
    return [&selection, action]
    {
        if (auto* edit = selection.getEdit())
            action (*edit);
    };
}

std::function<bool()> ifEdit (const te::SelectionManager& selection, std::function<bool (te::Edit&)> test)
{
    return [&selection, test]
    {
        auto* edit = selection.getEdit();
        return edit != nullptr && test (*edit);
    };
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
} // namespace phazite
