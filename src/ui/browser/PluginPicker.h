// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "services/Library.h"
#include "ui/components/Controls.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// The plug-in picker (F8, as in FL Studio; docs/design.md, section 11): type part of an instrument's or effect's name,
// or its maker's, use the arrow keys, and press Enter (or click one) to add it. It lists the built-in devices and the
// plug-ins found by scanning, in a call-out like the command palette's. Esc or a click elsewhere closes it.
class PluginPicker : public juce::Component,
                     private juce::TextEditor::Listener,
                     private juce::ListBoxModel
{
public:
    // onChosen gets the device picked, once the picker has closed.
    PluginPicker (const Library&, std::function<void (const LibraryItem&)> onChosen);

    // Opens a picker in a call-out pointing at anchor.
    static void show (const Library&, juce::Component& anchor, std::function<void (const LibraryItem&)> onChosen);

    void resized() override;

    // For tests: the devices listed, best first.
    const std::vector<LibraryItem>& getResults() const noexcept { return results; }
    juce::TextEditor& getSearchField() noexcept { return searchField; }

private:
    const Library& library;
    std::function<void (const LibraryItem&)> onChosen;
    SearchField searchField;
    juce::ListBox list;
    std::vector<LibraryItem> results;

    void updateResults();
    bool moveSelection (const juce::KeyPress&);
    void chooseSelected();
    void close();

    void textEditorTextChanged (juce::TextEditor&) override { updateResults(); }
    void textEditorReturnKeyPressed (juce::TextEditor&) override { chooseSelected(); }
    void textEditorEscapeKeyPressed (juce::TextEditor&) override { close(); }

    int getNumRows() override { return (int) results.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int) override { chooseSelected(); }
    juce::String getNameForRow (int row) override;
};
} // namespace phazite::ui
