// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CommandPalette.h"
#include "LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int rowHeight = 30;
    constexpr int numVisibleRows = 10;
} // namespace

CommandPalette::CommandPalette (CommandRegistry& r)
    : registry (r),
      list ("Commands", this)
{
    setTitle ("Command palette");

    searchField.setTitle ("Search commands");
    searchField.setTextToShowWhenEmpty ("Type a command, e.g. undo", getPalette (*this).textMuted);
    searchField.setFont (juce::FontOptions (TextSize::title));
    searchField.addListener (this);
    searchField.setWantsKeyboardFocus (true);
    searchField.onNavigationKey = [this] (const juce::KeyPress& key) { return moveSelection (key); };

    list.setRowHeight (rowHeight);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

    addAndMakeVisible (searchField);
    addAndMakeVisible (list);

    updateResults();
    setSize (520, controlHeight + Space::s + rowHeight * numVisibleRows + Space::s * 2);
}

void CommandPalette::show (CommandRegistry& registry, juce::Component& anchor)
{
    auto palette = std::make_unique<CommandPalette> (registry);
    auto* searchField = &palette->searchField;

    auto* topLevel = anchor.getTopLevelComponent();
    juce::CallOutBox::launchAsynchronously (std::move (palette), topLevel->getLocalArea (&anchor, anchor.getLocalBounds()), topLevel);

    // Type straight away, once the call-out is on screen.
    juce::MessageManager::callAsync (
        [field = juce::Component::SafePointer<juce::TextEditor> (searchField)]
        {
            if (field != nullptr)
                field->grabKeyboardFocus();
        });
}

void CommandPalette::resized()
{
    auto area = getLocalBounds().reduced (Space::s);
    searchField.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::s);
    list.setBounds (area);
}

bool CommandPalette::moveSelection (const juce::KeyPress& key)
{
    // The search field keeps the focus; the arrow keys move through the list.
    const auto row = list.getSelectedRow();

    if (key == juce::KeyPress::downKey)
    {
        list.selectRow (juce::jmin (row + 1, results.size() - 1));
        return true;
    }

    if (key == juce::KeyPress::upKey)
    {
        list.selectRow (juce::jmax (row - 1, 0));
        return true;
    }

    return false;
}

void CommandPalette::updateResults()
{
    results = registry.search (searchField.getText(), 50);
    list.updateContent();
    list.selectRow (0);
    list.repaint();
}

void CommandPalette::runSelected()
{
    const auto row = list.getSelectedRow();

    if (! juce::isPositiveAndBelow (row, results.size()))
        return;

    // Closing deletes this palette, so take what is needed first, then run the command once the
    // palette has gone (focus and keyboard then go back to the window).
    const auto id = results[row]->id;
    auto& commands = registry;
    close();
    juce::MessageManager::callAsync ([&commands, id] { commands.invoke (id); });
}

void CommandPalette::close()
{
    if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
        box->dismiss();
}

void CommandPalette::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, results.size()))
        return;

    const auto& p = getPalette (*this);
    const auto* command = results[row];
    const bool enabled = command->isEnabled == nullptr || command->isEnabled();

    auto area = juce::Rectangle<int> (width, height);

    if (selected)
    {
        g.setColour (p.accent);
        g.fillRoundedRectangle (area.toFloat().reduced (1.0f), cornerRadius);
    }

    const auto textColour = selected ? p.textOnAccent : p.text;
    const auto mutedColour = selected ? p.textOnAccent : p.textMuted;
    area.reduce (Space::s, 0);

    // The shortcut on the right, then the name, then the category in what's left.
    const auto shortcuts = registry.getShortcutDescriptions (command->id);

    if (! shortcuts.isEmpty())
    {
        g.setColour (mutedColour);
        g.setFont (juce::FontOptions (TextSize::small));
        g.drawText (shortcuts[0], area.removeFromRight (140), juce::Justification::centredRight);
    }

    g.setColour (enabled ? textColour : textColour.withAlpha (0.5f));
    g.setFont (juce::FontOptions (TextSize::body));
    const auto name = getCurrentName (*command); // e.g. "Undo Change Volume"
    const auto nameWidth =
        juce::jmin (area.getWidth(), juce::roundToInt (juce::TextLayout::getStringWidth (g.getCurrentFont(), name)) + Space::m);
    g.drawText (name, area.removeFromLeft (nameWidth), juce::Justification::centredLeft, true);

    g.setColour (mutedColour);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (command->category, area, juce::Justification::centredLeft, true);
}

void CommandPalette::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    list.selectRow (row);
    runSelected();
}

juce::String CommandPalette::getNameForRow (int row)
{
    if (! juce::isPositiveAndBelow (row, results.size()))
        return {};

    const auto* command = results[row];
    const auto shortcuts = registry.getShortcutDescriptions (command->id);
    return getCurrentName (*command) + (shortcuts.isEmpty() ? juce::String() : ", " + shortcuts[0]);
}
} // namespace phazite::ui
