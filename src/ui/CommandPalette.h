// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "model/Commands.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// Search any command by name and run it: type, use the arrow keys, press Enter. Esc or a click
// elsewhere closes it.
class CommandPalette : public juce::Component,
                       private juce::TextEditor::Listener,
                       private juce::ListBoxModel
{
public:
    explicit CommandPalette (CommandRegistry&);

    // Opens a palette in a call-out pointing at `anchor`.
    static void show (CommandRegistry&, juce::Component& anchor);

    void resized() override;

private:
    // A search field that hands the up and down keys to the list (a single-line TextEditor would
    // otherwise use them to move its caret).
    struct SearchField : public juce::TextEditor
    {
        std::function<bool (const juce::KeyPress&)> onNavigationKey;

        bool keyPressed (const juce::KeyPress& key) override
        {
            return (onNavigationKey != nullptr && onNavigationKey (key)) || juce::TextEditor::keyPressed (key);
        }
    };

    CommandRegistry& registry;
    SearchField searchField;
    juce::ListBox list;
    juce::Array<const Command*> results;

    void updateResults();
    bool moveSelection (const juce::KeyPress&);
    void runSelected();
    void close();

    void textEditorTextChanged (juce::TextEditor&) override { updateResults(); }
    void textEditorReturnKeyPressed (juce::TextEditor&) override { runSelected(); }
    void textEditorEscapeKeyPressed (juce::TextEditor&) override { close(); }

    int getNumRows() override { return results.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int) override { runSelected(); }
    juce::String getNameForRow (int row) override;
};
} // namespace phazite::ui
