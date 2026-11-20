// SPDX-License-Identifier: AGPL-3.0-or-later
// The Settings, as VS Code's Settings editor (docs/vscode-look.md, "Settings"), in a card over the main window as VS Code's
// modal editor (ui/components/ModalCard.h; MainWindow::showSettings()): a search field across the top, with its count, Clear
// and Filter at its right end; under it, at the left, the table of contents (the groups, and Shortcuts set apart at its foot),
// a sash that sizes it, and at the right one scrolling list of every setting under its group's title. A setting is a row: its
// group and name ("Audio: Driver"), a sentence or two of what it does, and its control (a tick box with its sentence beside
// it, a list to pick from, a number, a button), in the settings' own colours. The row under the pointer or focused last is
// washed; one changed from its default has the modified line at its left, and its menu (its gear, a right-click, Shift+F9 or
// Shift+F10) has Reset Setting. Shortcuts shows the keymap and every command's keys (ShortcutsEditor.h) in the list's place.
// Narrower than the table of contents and the list at their least, the table of contents hides, and a button at the search's
// left (Contents, Phazite's own) lists the groups and Shortcuts in a menu instead. It is opaque, on the editor's background,
// as the card's body is (ModalCard.h).
#pragma once

#include "PanelLayout.h"
#include "ShortcutsEditor.h"
#include "model/engine/EngineContext.h"
#include "model/mixer/MixerModel.h"
#include "services/Settings.h"
#include "ui/components/Controls.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/ComputerKeyboard.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace phazite
{
class ProjectContext;
} // namespace phazite

namespace phazite::ui
{
//==============================================================================
// What the audio settings say about delay, a sentence a line (docs/design.md, section 20): how long a note played takes to
// sound, from the output's buffer and latency and the song's delay compensation; whether low latency is on; and that an
// audio interface's ASIO driver gives less. And, apart, the warning that the output is Bluetooth, if it is.
struct DelayDescription
{
    juce::StringArray lines;
    juce::String warning;
};

DelayDescription describeDelay (const EngineContext::AudioOutput&, const DelayCompensation&, bool lowLatency, bool hasAsioDrivers);

// The sizes the interface takes (MainWindow: View > Larger Interface and Smaller Interface step through them, and the
// Settings window's Interface Size picks one).
inline constexpr std::array<float, 8> interfaceScales { 0.75f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f };

//==============================================================================
// One setting's row (SettingsPanel). Its group, name and description; its control, which it owns: a tick box (made with
// makeSwitch(), its sentence the description, beside it, a click on which switches it too), or any other control under the
// description; or none, for a row that only says something. A note under the control says why the last choice failed, in
// the warning colour; a warning row says all of it so. Its control takes the settings' colours (Palette::settingsDropdown,
// settingsField, checkbox...), set again as the theme changes.
class SettingRow : public juce::Component
{
public:
    SettingRow (const juce::String& group, const juce::String& title, const juce::String& description);
    ~SettingRow() override;

    const juce::String group, title;

    juce::String getDescriptionText() const { return description; }
    void setDescriptionText (const juce::String&);
    juce::String getNote() const { return note; }
    void setNote (const juce::String&);
    void setWarning (bool); // the description in the warning colour
    bool isWarning() const noexcept { return warning; }

    juce::ToggleButton& makeSwitch();
    void setControl (std::unique_ptr<juce::Component>, int width);
    juce::Component* getControl() const noexcept { return control.get(); }
    bool isSwitch() const noexcept { return switchRow; }

    // What it shows now, from what it is a setting of; whether it differs from its default; and Reset Setting, which puts the
    // default back (none: no menu, no modified line).
    std::function<void()> refresh;
    std::function<bool()> isModified;
    std::function<void()> reset;

    // Calls refresh, and repaints the row where its modified line comes or goes.
    void update();

    // Whether the row is there at all (a device's channels are only where it has them), and whether a search finds it: each
    // word in its name, its description or its group; and whether it differs from its default now.
    void setAvailable (bool);
    bool isAvailable() const noexcept { return available; }
    bool matches (const juce::StringArray& words) const;
    bool isModifiedNow() const { return isModified != nullptr && isModified(); }

    // How far in its contents start from the list's left side: SettingsTint::rowInset, or narrowRowInset where the table of
    // contents is hidden. Its height for a width.
    void setLeftInset (int);
    int getHeightForWidth (int width) const;

    juce::PopupMenu getMenu(); // Reset Setting, enabled while it is modified
    juce::Button& getGearButton() noexcept;

    // How many times its menu has shown, from its gear, a right-click or the keys: for tests, as a menu shown is gone again
    // at once where the program isn't the foreground one.
    int getMenusShown() const noexcept { return menusShown; }

    // The row focused last (VS Code's list's focused row): washed, its gear shown, until another row is focused; outlined while
    // the keyboard focus is in it. Under the pointer, washed less.
    void setFocusedRow (bool);
    bool isFocusedRow() const noexcept { return focusedRow; }
    bool isHighlighted() const noexcept { return hovered || focusedRow; }

    // As the keyboard focus comes to one of its controls: the panel makes it the focused row and scrolls it into view.
    std::function<void (SettingRow&)> onFocused;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusOfChildComponentChanged (FocusChangeType) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    juce::String description, note;
    bool warning = false, available = true, switchRow = false, hovered = false, focused = false, focusedRow = false, shownModified = false;
    std::unique_ptr<juce::Component> control;
    int controlWidth = 0, leftInset = SettingsTint::rowInset, menusShown = 0;
    std::unique_ptr<juce::Button> gear;
    struct HoverWatch; // hears the pointer come and go over its controls too, for its wash
    std::unique_ptr<HoverWatch> hoverWatch;

    juce::Rectangle<int> getContents() const;        // inside the list's sides
    juce::Rectangle<int> getDescriptionArea() const; // beside a switch's tick box, or under the name
    int getTextHeight (const juce::String&, int width) const;
    void updateHover();
    void showMenu();
    void controlsChanged(); // its control's title and description, as the row's
};

//==============================================================================
// The Shortcuts page: the keymap, and every command's keys (ShortcutsEditor.h).
class ShortcutSettings : public juce::Component
{
public:
    ShortcutSettings (CommandBridge&, const ComputerKeyboard&, std::function<void (const juce::String& keymapName)> chooseKeymap);

    ShortcutsEditor& getEditor() noexcept { return editor; }
    juce::ComboBox& getKeymapBox() noexcept { return keymapBox; }

    void resized() override;

private:
    juce::Label keymapLabel;
    juce::ComboBox keymapBox;
    ShortcutsEditor editor;
};

//==============================================================================
class SettingsPanel : public juce::Component,
                      private Settings::Listener,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    // The groups, in the order the table of contents lists them; Shortcuts is a page of its own, last.
    static constexpr const char* audio = "Audio";
    static constexpr const char* midi = "MIDI";
    static constexpr const char* recording = "Recording";
    static constexpr const char* appearance = "Appearance";
    static constexpr const char* help = "Help";
    static constexpr const char* editing = "Editing";
    static constexpr const char* files = "Files";
    static constexpr const char* versions = "Versions";
    static constexpr const char* startup = "Startup";
    static constexpr const char* updates = "Updates";
    static constexpr const char* shortcuts = "Shortcuts";

    // What the window's owner does for it: another keymap (MainWindow::setKeymap()), and another interface size (its
    // setInterfaceScale()); without the second, there is no Interface Size row. The preferences that have a command (a
    // theme, Hover Help, Auto Save...) run it, so that the menus and the row show the same; a row of a command that isn't
    // registered isn't there.
    struct Actions
    {
        std::function<void (const juce::String& keymapName)> chooseKeymap;
        std::function<void (float scale)> setInterfaceScale;
    };

    SettingsPanel (EngineContext&, CommandBridge&, ComputerKeyboard&, const ProjectContext&, Settings&, Actions);
    ~SettingsPanel() override;

    // The list scrolled to a group's title, or the Shortcuts page.
    void showGroup (const juce::String& group);
    bool showsShortcuts() const noexcept { return onShortcuts; }

    // Filters the list to the settings whose name, description or group have each word, the table of contents counting those
    // left in each group, the count at the field's end saying how many in all; with none, one line says so. "@modified" among
    // the words (Filter's Modified puts it there) keeps only the settings that differ from their defaults.
    void search (const juce::String& text);
    static constexpr const char* modifiedFilter = "@modified";

    // The search's end: its count ("3 Settings Found", or nothing while it doesn't search), Clear (enabled while there is
    // something to clear) and Filter, whose menu has Modified. Clear and Filter are one Tab stop after the search, as VS Code's
    // action bar is: Clear, or Filter while Clear is off; Left and Right go from one to the other, Enter and Space press.
    juce::TextEditor& getSearchField() noexcept { return searchField; }
    juce::String getCountText() const;
    juce::Button& getClearButton() noexcept { return clearButton; }
    juce::Button& getFilterButton() noexcept { return filterButton; }
    juce::PopupMenu getFilterMenu();

    // In the narrow layout alone, at the search's left (before it in Tab's order): Contents, whose menu lists what the table of
    // contents lists (the groups, with their counts while searching, and Shortcuts), the one shown ticked; an item goes there
    // as a click on its entry does.
    juce::Button& getContentsButton() noexcept { return contentsButton; }
    juce::PopupMenu getContentsMenu();

    juce::Viewport& getList() noexcept;
    juce::Component& getContents() noexcept;
    ShortcutSettings& getShortcuts() noexcept { return *shortcutPage; }

    // The table of contents' width as it shows, with the list's left side there (0 while it is hidden, narrower than both at
    // their least); and the sash between them, which a drag moves (each at least its least) and a double-click puts back at
    // SettingsTint::tocWidth. The settings keep its width (Settings::settingsContentsWidth).
    int getContentsWidth() const noexcept { return shownContentsWidth; }
    bool isNarrow() const noexcept { return shownContentsWidth == 0; }
    PanelDivider& getSash() noexcept { return sash; }

    // The groups the table of contents lists now (not Shortcuts), with the count of each while searching; the one marked
    // as the list scrolls (or Shortcuts); where a group's entry is, in the panel's coordinates.
    juce::StringArray getListedGroups() const;
    int getCount (const juce::String& group) const;
    juce::String getMarkedGroup() const;
    juce::Rectangle<int> getContentsEntry (const juce::String& group) const;

    // A group's title's place in the list; a row by its name; the rows the list shows; whether "No Settings Found" shows.
    int getGroupTop (const juce::String& group) const;
    SettingRow* findRow (const juce::String& title) const;
    juce::Array<SettingRow*> getShownRows() const;
    bool showsNoResults() const;

    // For tests: the delay's sentences, under the buffer's row; the Bluetooth warning, or nothing.
    juce::String getDelayText() const;
    juce::String getWarning() const;

    void paint (juce::Graphics&) override;
    void resized() override;
    std::unique_ptr<juce::ComponentTraverser> createKeyboardFocusTraverser() override; // the search first, as it opens

private:
    class Contents;
    class ListView;
    class GroupTitle;
    struct AudioRows;

    EngineContext& engine;
    CommandBridge& commands;
    ComputerKeyboard& computerKeyboard;
    const ProjectContext& projectContext;
    Settings& settings;
    Actions actions;

    // Clear or Filter, which tells as the keys leave it (resetSearchStop()).
    struct SearchButton : public IconButton
    {
        using IconButton::IconButton;
        std::function<void()> onFocusLost;

        void focusLost (FocusChangeType cause) override
        {
            IconButton::focusLost (cause);

            if (onFocusLost != nullptr)
                onFocusLost();
        }
    };

    SearchField searchField;
    juce::Label countLabel, noResults;
    SearchButton clearButton { "Clear Settings Search Input", Icon::clearSearch }, filterButton { "Filter Settings", Icon::filter };
    IconButton contentsButton { "Contents", Icon::menu };
    PanelDivider sash { "Table of contents' edge", true };
    int contentsWidth = SettingsTint::tocWidth, shownContentsWidth = SettingsTint::tocWidth, widthAtDragStart = 0;
    std::unique_ptr<Contents> contents;
    std::unique_ptr<ListView> listView;
    std::unique_ptr<juce::Component> list; // the rows, under their groups' titles, which the list view scrolls
    std::unique_ptr<ShortcutSettings> shortcutPage;

    juce::StringArray groups;
    juce::OwnedArray<GroupTitle> titles;
    std::vector<std::unique_ptr<SettingRow>> rows;
    std::unique_ptr<AudioRows> audioRows;
    juce::OwnedArray<juce::ToggleButton> midiSwitches; // made again as MIDI inputs come and go
    juce::StringArray shownMidiInputs;
    juce::StringArray searchWords; // without "@modified"
    bool searching = false, modifiedOnly = false;
    juce::String clickedGroup; // the last entry clicked, marked while the list can't scroll its title to the top
    juce::Component::SafePointer<SettingRow> focusedRow;
    bool onShortcuts = false;

    SettingRow& addRow (const char* group, const juce::String& title, const juce::String& description);
    void addSwitch (const char* group, const juce::String& title, const juce::String& description, const juce::String& commandID,
                    bool defaultValue);
    void addAudioRows();
    void addMidiRows();
    void addPreferenceRows();
    void updateAudio();
    void updateMidi();
    void refreshAll();
    void layOutList();
    void placeSearchEnd();   // the count and the buttons inside the field's right end, its text stopping before them
    void resetSearchStop();  // Clear the Tab stop of the search's buttons while it is on, else Filter
    struct SearchButtonKeys; // the arrows and Space on Clear and Filter
    std::unique_ptr<SearchButtonKeys> searchButtonKeys;
    void markGroupInView();
    void rowFocused (SettingRow&); // the row focused last, scrolled into view
    void scrollRowIntoView (SettingRow&);
    void addFilter (const juce::String& filter);
    void setContentsWidth (int); // as the sash sets it

    void settingChanged (const juce::String& key) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the devices changed
    void timerCallback() override;                                   // so might the song's devices' delays

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPanel)
};
} // namespace phazite::ui
