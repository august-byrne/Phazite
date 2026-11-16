// SPDX-License-Identifier: AGPL-3.0-or-later
// The settings' Shortcuts tab's list, as VS Code's Keyboard Shortcuts editor: a search over every command, by its name or
// by a key (typed as text, or pressed while Record Keys is on), and a row for each of a command's keys (one for a command
// without), with the command, its key and where the key comes from (the keymap's name, or User for the user's own). A
// command is given a key in a card that asks for it and says what else the key does before it is assigned: the key stays
// with every other command that has it, as commands share keys on purpose, and the card says which of them runs when
// (KeyShares.h). A key is changed, added or removed, a command's keys reset to the keymap's, or all of them. The user's
// changes are saved as the bridge's key mappings tell of them (KeymapStore.h, from MainWindow).
#pragma once

#include "ui/components/Controls.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <optional>
#include <vector>

namespace phazite::ui
{
class ShortcutsEditor : public juce::Component,
                        private juce::ListBoxModel,
                        private juce::ChangeListener,
                        private juce::Timer
{
public:
    ShortcutsEditor (CommandBridge&, const ComputerKeyboard&);
    ~ShortcutsEditor() override;

    // A row: a command and one of its keys, or none for a command without keys.
    struct Row
    {
        const Command* command = nullptr;
        std::optional<juce::KeyPress> key;
    };

    // What the list shows now, best for the search first; the row of a command's key, or of the command without one; -1 if
    // the search hides it.
    const std::vector<Row>& getRows() const noexcept { return rows; }
    int findRow (const juce::String& commandID, std::optional<juce::KeyPress> = {}) const;
    juce::String getSource (const Row&) const; // the keymap's name, User, or nothing

    // Searches as a typed search or a key pressed while Record Keys is on does.
    void search (const juce::String& text);
    void searchByKey (const juce::KeyPress&);

    // The card that asks for a key: for the selected row's command, in place of its key or as another.
    class KeyCapture;
    void changeSelectedKey();
    void addKeyToSelected();
    void removeSelectedKey();
    void resetSelected();
    KeyCapture* getKeyCapture() const noexcept { return capture.get(); }

    juce::TextEditor& getSearchField() noexcept { return searchField; }
    juce::Button& getRecordButton() noexcept { return recordButton; }
    juce::ListBox& getList() noexcept { return list; }
    juce::Button& getChangeButton() noexcept { return changeButton; }
    juce::Button& getAddButton() noexcept { return addButton; }
    juce::Button& getRemoveButton() noexcept { return removeButton; }
    juce::Button& getResetButton() noexcept { return resetButton; }
    juce::Button& getResetAllButton() noexcept { return resetAllButton; }
    juce::String getHelpText() const { return help.getText(); } // the selected row's command, and what else its key does

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    CommandBridge& commands;
    const ComputerKeyboard& keyboard;
    KeyDispatcher keys; // to find commands by keys as the window's keys do, never to run them

    SearchField searchField;
    IconButton recordButton { "Record Keys", Icon::keyboard };
    juce::ListBox list;
    juce::Label help;
    juce::TextButton changeButton { "Change Key..." }, addButton { "Add Key..." }, removeButton { "Remove Key" }, resetButton { "Reset" },
        resetAllButton { "Reset All" };
    TintedIcon pencil { Icon::draw };
    std::unique_ptr<KeyCapture> capture;

    std::vector<Row> rows;
    std::optional<juce::KeyPress> searchedKey; // while the search is a key pressed
    bool settingSearchText = false;
    bool confirmingResetAll = false;

    void refreshRows();
    void showSelected(); // the buttons and the help, for the selected row
    void select (const juce::String& commandID, std::optional<juce::KeyPress>);
    const Row* getSelectedRow() const;
    void showKeyCapture (bool replacing);
    void closeKeyCapture();
    void assign (const juce::String& commandID, std::optional<juce::KeyPress> replacing, const juce::KeyPress&);
    void showMenu (int row);
    juce::Rectangle<int> getColumn (int index, int width) const; // 0 the pencil, 1 the command, 2 the key, 3 the source

    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int) override { showSelected(); }
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int) override { changeSelectedKey(); }
    void deleteKeyPressed (int) override { removeSelectedKey(); }
    juce::String getNameForRow (int row) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the keys changed, here or elsewhere
    void timerCallback() override;                                   // Reset All wasn't confirmed in time

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShortcutsEditor)
};

//==============================================================================
// Over the editor, a card that asks for a command's key: every key pressed while it has the keyboard focus is taken,
// Escape and Enter too, but Tab, which goes on to Assign and Cancel, the keys the system needs (Alt+F4), and a key JUCE
// gives no code; under the key, what else it does, which screen readers hear. Assign gives the command the key (in place
// of the one it changes, which goes only once the new one is there), Cancel or a click beside the card closes it, and so
// do Escape on its buttons and another keymap chosen meanwhile.
class ShortcutsEditor::KeyCapture : public juce::Component
{
public:
    KeyCapture (ShortcutsEditor&, const Command&, std::optional<juce::KeyPress> replacing);

    // The key pressed, as the command would keep it (getShortcutKey()).
    std::optional<juce::KeyPress> getKey() const noexcept { return key; }
    juce::String getKeymapName() const { return keymapName; } // the keymap it opened for
    juce::String getShareText() const { return share.getText(); }
    juce::Component& getShareLabel() noexcept { return share; }
    juce::Button& getAssignButton() noexcept { return assignButton; }
    juce::Button& getCancelButton() noexcept { return cancelButton; }

    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    ShortcutsEditor& editor;
    const Command& command;
    const std::optional<juce::KeyPress> replacing;
    const juce::String keymapName;
    std::optional<juce::KeyPress> key;
    juce::Label heading, share;
    juce::TextButton assignButton { "Assign" }, cancelButton { "Cancel" };

    static bool isSystemKey (const juce::KeyPress&); // Alt+F4, Alt+Tab, Alt+Esc, Alt+Space, Ctrl+Esc
    juce::Rectangle<int> getCard() const;
    juce::Rectangle<int> getKeyArea() const;
    void take (const juce::KeyPress&);
};
} // namespace phazite::ui
