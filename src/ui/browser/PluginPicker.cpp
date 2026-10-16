// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PluginPicker.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
namespace
{
    constexpr int rowHeight = 30;
    constexpr int numVisibleRows = 10;

    // What a row says after the device's name: what it is, and who makes it.
    juce::String describe (const LibraryItem& item)
    {
        if (! item.device.has_value())
            return {};

        const auto maker = item.device->isBuiltIn ? juce::String ("Built in") : item.device->description.manufacturerName;
        return item.device->category + (maker.isNotEmpty() ? " - " + maker : juce::String());
    }
} // namespace

PluginPicker::PluginPicker (const Library& l, std::function<void (const LibraryItem&)> chosen)
    : library (l),
      onChosen (std::move (chosen)),
      list ("Devices", this)
{
    setTitle ("Plug-in picker");

    searchField.setTitle ("Search instruments and effects");
    searchField.setTextToShowWhenEmpty ("Type an instrument or effect, e.g. reverb", getPalette (*this).textMuted);
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

void PluginPicker::show (const Library& library, juce::Component& anchor, std::function<void (const LibraryItem&)> onChosen)
{
    auto picker = std::make_unique<PluginPicker> (library, std::move (onChosen));
    auto* searchField = &picker->searchField;

    auto* topLevel = anchor.getTopLevelComponent();
    juce::CallOutBox::launchAsynchronously (std::move (picker), topLevel->getLocalArea (&anchor, anchor.getLocalBounds()), topLevel);

    // Type straight away, once the call-out is on screen.
    juce::MessageManager::callAsync (
        [field = juce::Component::SafePointer<juce::TextEditor> (searchField)]
        {
            if (field != nullptr)
                field->grabKeyboardFocus();
        });
}

void PluginPicker::resized()
{
    auto area = getLocalBounds().reduced (Space::s);
    searchField.setBounds (area.removeFromTop (controlHeight));
    area.removeFromTop (Space::s);
    list.setBounds (area);
}

bool PluginPicker::moveSelection (const juce::KeyPress& key)
{
    // The search field keeps the focus; the arrow keys move through the list.
    const auto row = list.getSelectedRow();

    if (key == juce::KeyPress::downKey)
    {
        list.selectRow (juce::jmin (row + 1, getNumRows() - 1));
        return true;
    }

    if (key == juce::KeyPress::upKey)
    {
        list.selectRow (juce::jmax (row - 1, 0));
        return true;
    }

    return false;
}

void PluginPicker::updateResults()
{
    results = library.searchDevices (searchField.getText());
    list.updateContent();
    list.selectRow (0);
    list.repaint();
}

void PluginPicker::chooseSelected()
{
    const auto row = list.getSelectedRow();

    if (! juce::isPositiveAndBelow (row, getNumRows()))
        return;

    // Closing deletes this picker, so take what is needed first, then add the device once the picker has gone (focus
    // and keyboard then go back to the window).
    if (onChosen != nullptr)
        juce::MessageManager::callAsync ([add = onChosen, device = results[(size_t) row]] { add (device); });

    close();
}

void PluginPicker::close()
{
    if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
        box->dismiss();
}

void PluginPicker::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, getNumRows()))
        return;

    const auto& p = getPalette (*this);
    const auto& item = results[(size_t) row];
    auto area = juce::Rectangle<int> (width, height);

    if (selected)
    {
        g.setColour (p.accent);
        g.fillRoundedRectangle (area.toFloat().reduced (1.0f), cornerRadius);
    }

    area.reduce (Space::s, 0);

    // What it is and who makes it on the right, the name in what's left.
    g.setColour (selected ? p.textOnAccent : p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (describe (item), area.removeFromRight (200), juce::Justification::centredRight, true);

    g.setColour (selected ? p.textOnAccent : p.text);
    g.setFont (juce::FontOptions (TextSize::body));
    g.drawText (item.name, area.withTrimmedRight (Space::s), juce::Justification::centredLeft, true);
}

void PluginPicker::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    list.selectRow (row);
    chooseSelected();
}

juce::String PluginPicker::getNameForRow (int row)
{
    if (! juce::isPositiveAndBelow (row, getNumRows()))
        return {};

    const auto& item = results[(size_t) row];
    return item.name + ", " + describe (item);
}
} // namespace phazite::ui
