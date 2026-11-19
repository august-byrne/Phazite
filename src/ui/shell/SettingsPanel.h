// SPDX-License-Identifier: AGPL-3.0-or-later
// The Settings window's content, as VS Code's Settings editor (docs/vscode-look.md, "Settings"): a search field across the
// top; under it, at the left, the table of contents (the groups, and Shortcuts set apart at its foot), at the right one
// scrolling list of every setting under its group's title. A setting is a row: its name, a sentence or two of what it does,
// and its control (a tick box with its sentence beside it, a list to pick from, a number, a button). The row under the
// pointer or holding the keyboard focus is washed; one changed from its default has the modified line at its left, and its
// menu (its gear, a right-click, or Shift+F10) has Reset Setting. Shortcuts shows the keymap and every command's keys
// (ShortcutsEditor.h) in the list's place.
#pragma once

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
// the warning colour; a warning row says all of it so.
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
    // word in its name, its description or its group.
    void setAvailable (bool);
    bool isAvailable() const noexcept { return available; }
    bool matches (const juce::StringArray& words) const;

    int getHeightForWidth (int width) const;
    juce::PopupMenu getMenu(); // Reset Setting, enabled while it is modified
    juce::Button& getGearButton() noexcept;
    bool isHighlighted() const noexcept { return hovered || focused; }

    // As the keyboard focus comes to one of its controls: the list scrolls it into view.
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
    bool warning = false, available = true, switchRow = false, hovered = false, focused = false, shownModified = false;
    std::unique_ptr<juce::Component> control;
    int controlWidth = 0;
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

    void paint (juce::Graphics&) override;
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
    // left in each group; with none, one line says so.
    void search (const juce::String& text);

    juce::TextEditor& getSearchField() noexcept { return searchField; }
    juce::Viewport& getList() noexcept;
    juce::Component& getContents() noexcept;
    ShortcutSettings& getShortcuts() noexcept { return *shortcutPage; }

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

    SearchField searchField;
    juce::Label countLabel, noResults;
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
    juce::StringArray searchWords;
    juce::String clickedGroup; // the last entry clicked, marked while the list can't scroll its title to the top
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
    void markGroupInView();
    void scrollRowIntoView (SettingRow&);

    void settingChanged (const juce::String& key) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the devices changed
    void timerCallback() override;                                   // so might the song's devices' delays

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPanel)
};
} // namespace phazite::ui
