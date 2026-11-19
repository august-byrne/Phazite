// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ShortcutsEditor.h"
#include "ui/input/CommandUI.h"
#include "ui/input/KeyShares.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/theme/Popups.h"

#include <algorithm>

namespace phazite::ui
{
namespace
{
    constexpr int confirmMilliseconds = 4000; // how long Reset All waits for its second click

    juce::String describe (const juce::KeyPress& key)
    {
        return formatKey (key.getTextDescription());
    }

    bool sameKeys (const juce::Array<juce::KeyPress>& a, const juce::Array<juce::KeyPress>& b)
    {
        return a.size() == b.size() && std::all_of (a.begin(), a.end(), [&b] (const juce::KeyPress& key) { return b.contains (key); });
    }

    enum MenuItem
    {
        changeItem = 1,
        addItem,
        removeItem,
        resetItem
    };
} // namespace

ShortcutsEditor::ShortcutsEditor (CommandBridge& bridge, const ComputerKeyboard& computerKeyboard)
    : commands (bridge),
      keyboard (computerKeyboard),
      keys (bridge, computerKeyboard.getLayout()),
      list ("Shortcuts", this)
{
    setTitle ("Keyboard shortcuts");
    const auto& p = getPalette (*this);

    searchField.setTitle ("Search shortcuts");
    searchField.setTooltip ("Finds commands by name or by key: type a name or Ctrl+Z, or turn on Record Keys and press the key");
    searchField.setTextToShowWhenEmpty ("Type a command or a key, e.g. undo or Ctrl+Z", p.textMuted);
    searchField.setFont (juce::FontOptions (TextSize::body));
    searchField.onTextChange = [this]
    {
        searchedKey.reset();
        refreshRows();
    };
    searchField.onNavigationKey = [this] (const juce::KeyPress& key)
    {
        // While Record Keys is on, a key pressed is what to search for; Tab still moves on.
        if (recordButton.getToggleState())
        {
            if (key.getKeyCode() == juce::KeyPress::tabKey)
                return false;

            searchByKey (key);
            return true;
        }

        const auto row = list.getSelectedRow();

        if (key == juce::KeyPress::downKey || key == juce::KeyPress::upKey)
        {
            if (rows.empty())
                return true;

            list.selectRow (juce::jlimit (0, (int) rows.size() - 1, row + (key == juce::KeyPress::downKey ? 1 : -1)));
            return true;
        }

        if (key == juce::KeyPress::returnKey)
        {
            changeSelectedKey();
            return true;
        }

        return false;
    };

    recordButton.setClickingTogglesState (true);
    recordButton.setTooltip ("Record Keys: a key pressed in the search finds the commands it runs; click again to type a search");
    recordButton.onClick = [this]
    {
        search ({});
        searchField.grabKeyboardFocus();
    };

    list.setTitle ("Shortcuts");
    list.setTooltip ("Every command, a row for each of its keys, and where the key comes from: Enter or the pencil changes a key, "
                     "Delete removes it, a right-click shows the rest");
    list.setRowHeight (ListRow::height);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

    help.setJustificationType (juce::Justification::topLeft);
    help.setFont (juce::FontOptions (TextSize::small));
    help.setMinimumHorizontalScale (1.0f);
    setThemeColour (help, juce::Label::textColourId, &Palette::textMuted);

    changeButton.setTooltip ("Changes the selected key, in a card that asks for the new one (Enter)");
    changeButton.onClick = [this] { changeSelectedKey(); };
    addButton.setTooltip ("Gives the selected command another key, in a card that asks for it");
    addButton.onClick = [this] { addKeyToSelected(); };
    removeButton.setTooltip ("Takes the selected key from its command; other commands that have it keep it (Delete)");
    removeButton.onClick = [this] { removeSelectedKey(); };
    resetButton.setTooltip ("Gives the selected command its keymap's keys again");
    resetButton.onClick = [this] { resetSelected(); };
    resetAllButton.setTooltip ("Gives every command its keymap's keys again, taking away your changes to this keymap: click twice");
    resetAllButton.onClick = [this]
    {
        if (! confirmingResetAll)
        {
            confirmingResetAll = true;
            resetAllButton.setButtonText ("Click Again to Reset All");
            startTimer (confirmMilliseconds);
            resized();
            return;
        }

        timerCallback();
        commands.resetAllKeys();
        refreshRows();
    };

    for (auto* c : std::initializer_list<juce::Component*> { &searchField, &recordButton, &list, &help, &changeButton, &addButton,
                                                             &removeButton, &resetButton, &resetAllButton })
        addAndMakeVisible (c);

    commands.getKeyMappings().addChangeListener (this);
    refreshRows();
    list.selectRow (0);
}

ShortcutsEditor::~ShortcutsEditor()
{
    commands.getKeyMappings().removeChangeListener (this);
    capture = nullptr;
}

//==============================================================================
juce::String ShortcutsEditor::getSource (const Row& row) const
{
    const auto defaults = commands.getDefaultKeyPresses (row.command->id);

    if (row.key.has_value())
        return defaults.contains (*row.key) ? commands.getKeymap().name : juce::String ("User");

    return defaults.isEmpty() ? juce::String() : juce::String ("User"); // the keymap's keys taken away
}

int ShortcutsEditor::findRow (const juce::String& commandID, std::optional<juce::KeyPress> key) const
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].command->id == commandID && rows[i].key == key)
            return (int) i;

    return -1;
}

void ShortcutsEditor::search (const juce::String& text)
{
    searchedKey.reset();
    searchField.setText (text, false);
    refreshRows();
}

void ShortcutsEditor::searchByKey (const juce::KeyPress& key)
{
    searchedKey = key;
    searchField.setText ("\"" + describe (getShortcutKey (key, keys.getLayout())) + "\"", false);
    refreshRows();
}

void ShortcutsEditor::refreshRows()
{
    std::optional<Row> selected;

    if (const auto* row = getSelectedRow())
        selected = *row;

    // The commands by name, each with a row for each of its keys, as VS Code lists them; a search holds every row whose
    // command's name, category or id, key or source has each of its words, or, for a key pressed, every key it runs.
    const auto& registry = commands.getRegistry();
    std::vector<const Command*> sorted;

    for (int i = 0; i < registry.getNumCommands(); ++i)
        sorted.push_back (registry.getCommand (i));

    std::stable_sort (sorted.begin(), sorted.end(),
                      [] (const Command* a, const Command* b) { return a->name.compareNatural (b->name) < 0; });

    const auto words = juce::StringArray::fromTokens (searchField.getText(), " \"", {});
    const auto matches = searchedKey.has_value() ? keys.findMatches (*searchedKey) : std::vector<KeyDispatcher::Match>();
    const auto shows = [this, &words, &matches] (const Row& row)
    {
        // A key pressed: the shortcuts it matches the way that decides what it runs (KeyDispatcher).
        if (searchedKey.has_value())
            return row.key.has_value()
                   && std::any_of (matches.begin(), matches.end(),
                                   [&row] (const KeyDispatcher::Match& m) { return m.command == row.command && m.shortcut == *row.key; });

        const auto text = row.command->name + " " + row.command->category + " " + row.command->id + " "
                          + (row.key.has_value() ? describe (*row.key) + " " + row.key->getTextDescription() : juce::String()) + " "
                          + getSource (row);
        return std::all_of (words.begin(), words.end(),
                            [&text] (const juce::String& word) { return word.isEmpty() || text.containsIgnoreCase (word); });
    };

    rows.clear();

    for (const auto* command : sorted)
    {
        const auto commandKeys = commands.getKeyPresses (command->id);

        if (commandKeys.isEmpty())
        {
            if (const Row row { command, {} }; shows (row))
                rows.push_back (row);
        }

        for (const auto& key : commandKeys)
            if (const Row row { command, key }; shows (row))
                rows.push_back (row);
    }

    list.updateContent();

    if (selected.has_value())
        select (selected->command->id, selected->key);

    list.repaint();
    showSelected();
}

const ShortcutsEditor::Row* ShortcutsEditor::getSelectedRow() const
{
    const auto row = list.getSelectedRow();
    return juce::isPositiveAndBelow (row, rows.size()) ? &rows[(size_t) row] : nullptr;
}

void ShortcutsEditor::select (const juce::String& commandID, std::optional<juce::KeyPress> key)
{
    auto row = findRow (commandID, key);

    for (size_t i = 0; row < 0 && i < rows.size(); ++i)
        if (rows[i].command->id == commandID)
            row = (int) i; // the command's key went: one of its others

    if (row >= 0)
    {
        list.selectRow (row);
        list.scrollToEnsureRowIsOnscreen (row);
    }
}

void ShortcutsEditor::showSelected()
{
    const auto* row = getSelectedRow();
    const auto changed =
        row != nullptr && ! sameKeys (commands.getKeyPresses (row->command->id), commands.getDefaultKeyPresses (row->command->id));

    changeButton.setEnabled (row != nullptr);
    addButton.setEnabled (row != nullptr && row->key.has_value());
    removeButton.setEnabled (row != nullptr && row->key.has_value());
    resetButton.setEnabled (changed);

    if (row == nullptr)
    {
        help.setText (rows.empty() ? "No command matches the search." : "Select a command to change its keys.", juce::dontSendNotification);
        return;
    }

    auto text = row->command->name + ": " + row->command->description;

    if (row->key.has_value())
        if (const auto shares = describeKeyShare (commands, keys, keyboard, row->command->id, *row->key); ! shares.isEmpty())
            text << "\n" << shares.joinIntoString (" ");

    help.setText (text, juce::dontSendNotification);
}

//==============================================================================
void ShortcutsEditor::changeSelectedKey()
{
    if (const auto* row = getSelectedRow())
        showKeyCapture (row->key.has_value());
}

void ShortcutsEditor::addKeyToSelected()
{
    if (getSelectedRow() != nullptr)
        showKeyCapture (false);
}

void ShortcutsEditor::removeSelectedKey()
{
    const auto* row = getSelectedRow();

    if (row == nullptr || ! row->key.has_value())
        return;

    const auto id = row->command->id;
    commands.removeKey (id, *row->key);
    refreshRows();
    select (id, {});
}

void ShortcutsEditor::resetSelected()
{
    if (const auto* row = getSelectedRow())
    {
        const auto id = row->command->id;
        commands.resetKeys (id);
        refreshRows();
        const auto defaults = commands.getDefaultKeyPresses (id);
        select (id, defaults.isEmpty() ? std::optional<juce::KeyPress>() : std::optional<juce::KeyPress> (defaults.getFirst()));
    }
}

void ShortcutsEditor::assign (const juce::String& commandID, std::optional<juce::KeyPress> replacing, const juce::KeyPress& key)
{
    // The new key first: the one it takes the place of goes only once the command has the new one.
    if (! commands.addKey (commandID, key))
        return;

    if (replacing.has_value() && *replacing != key)
        commands.removeKey (commandID, *replacing);

    refreshRows();
    select (commandID, key);
}

void ShortcutsEditor::showKeyCapture (bool replacing)
{
    const auto* row = getSelectedRow();

    if (row == nullptr)
        return;

    capture = std::make_unique<KeyCapture> (*this, *row->command, replacing ? row->key : std::nullopt);
    addAndMakeVisible (*capture);
    capture->setBounds (getLocalBounds());
    capture->grabKeyboardFocus();
}

void ShortcutsEditor::closeKeyCapture()
{
    // From the card's own button, which goes on after its click: the card goes once that is over.
    if (capture == nullptr)
        return;

    capture->setVisible (false);
    juce::MessageManager::callAsync ([closing = std::shared_ptr<KeyCapture> (capture.release())] {});
    list.grabKeyboardFocus();
}

void ShortcutsEditor::showMenu (int row)
{
    if (! juce::isPositiveAndBelow (row, rows.size()))
        return;

    const auto& shown = rows[(size_t) row];
    juce::PopupMenu menu;
    menu.addItem (changeItem, shown.key.has_value() ? "Change Key..." : "Give It a Key...");
    menu.addItem (addItem, "Add Key...", shown.key.has_value());
    menu.addItem (removeItem, "Remove Key", shown.key.has_value());
    menu.addItem (resetItem, "Reset", resetButton.isEnabled());

    menu.showMenuAsync (getContextMenuOptions (list),
                        [safe = juce::Component::SafePointer<ShortcutsEditor> (this)] (int result)
                        {
                            if (safe == nullptr)
                                return;

                            switch (result)
                            {
                                case changeItem:
                                    safe->changeSelectedKey();
                                    break;
                                case addItem:
                                    safe->addKeyToSelected();
                                    break;
                                case removeItem:
                                    safe->removeSelectedKey();
                                    break;
                                case resetItem:
                                    safe->resetSelected();
                                    break;
                                default:
                                    break;
                            }
                        });
}

//==============================================================================
juce::Rectangle<int> ShortcutsEditor::getColumn (int index, int width) const
{
    auto area = juce::Rectangle<int> (width, ListRow::height).reduced (Space::s, 0);
    const auto pencilArea = area.removeFromLeft (ShortcutsTint::pencil + Space::s);
    const auto source = area.removeFromRight (ShortcutsTint::sourceWidth);
    const auto command = area.removeFromLeft (juce::roundToInt ((float) area.getWidth() * ShortcutsTint::commandShare));
    const auto key = area.withTrimmedRight (Space::m);

    switch (index)
    {
        case 0:
            return pencilArea;
        case 1:
            return command;
        case 2:
            return key;
        default:
            return source;
    }
}

void ShortcutsEditor::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, rows.size()))
        return;

    const auto& p = getPalette (*this);
    const auto& shown = rows[(size_t) row];

    if (selected)
    {
        g.setColour (p.selected);
        g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (ListRow::inset, 0.0f), cornerRadius);
    }

    const auto textColour = selected ? p.textBright : p.text;

    // The pencil, on the row selected, as VS Code shows it on the row under the pointer.
    if (selected)
        pencil.draw (g, getColumn (0, width).toFloat().withSizeKeepingCentre ((float) ShortcutsTint::pencil, (float) ShortcutsTint::pencil),
                     textColour);

    g.setColour (textColour);
    g.setFont (juce::FontOptions (TextSize::body));
    g.drawText (shown.command->name, getColumn (1, width), juce::Justification::centredLeft, true);

    if (shown.key.has_value())
        drawKeyCaps (g, p, describe (*shown.key), getColumn (2, width).toFloat(), textColour);

    g.setColour (selected ? textColour : p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    g.drawText (getSource (shown), getColumn (3, width), juce::Justification::centredLeft, true);
}

void ShortcutsEditor::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        list.selectRow (row);
        showMenu (row);
        return;
    }

    if (getColumn (0, list.getVisibleRowWidth()).contains (e.getPosition().withY (ListRow::height / 2)))
    {
        list.selectRow (row);
        changeSelectedKey();
    }
}

void ShortcutsEditor::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    list.selectRow (row);
    changeSelectedKey();
}

juce::String ShortcutsEditor::getNameForRow (int row)
{
    if (! juce::isPositiveAndBelow (row, rows.size()))
        return {};

    const auto& shown = rows[(size_t) row];
    const auto source = getSource (shown);
    return shown.command->name + ", " + (shown.key.has_value() ? describe (*shown.key) : juce::String ("no key"))
           + (source.isNotEmpty() ? ", " + source : juce::String());
}

void ShortcutsEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Another keymap: the card, asking for a key of the one before, closes, and gives nothing.
    if (capture != nullptr && capture->getKeymapName() != commands.getKeymap().name)
        closeKeyCapture();

    refreshRows();
}

void ShortcutsEditor::timerCallback()
{
    stopTimer();
    confirmingResetAll = false;
    resetAllButton.setButtonText ("Reset All");
    resized();
}

//==============================================================================
void ShortcutsEditor::paint (juce::Graphics& g)
{
    // The columns' headings, over the list's.
    const auto& p = getPalette (*this);
    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small));
    const auto headings = list.getBounds().withHeight (ListRow::height).translated (0, -ListRow::height);
    const juce::StringArray names { "", "Command", "Key", "Source" };

    for (int i = 1; i < names.size(); ++i)
        g.drawText (names[i], getColumn (i, list.getVisibleRowWidth()) + headings.getPosition(),
                    i == 2 ? juce::Justification::centredRight : juce::Justification::centredLeft, true);
}

void ShortcutsEditor::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop (SettingsTint::controlHeight);
    const auto resetWidth = resetAllButton.getBestWidthForHeight (SettingsTint::controlHeight);
    resetAllButton.setBounds (top.removeFromRight (resetWidth));
    top.removeFromRight (Space::s);
    recordButton.setBounds (top.removeFromRight (ShortcutsTint::recordWidth));
    top.removeFromRight (Space::xs);
    searchField.setBounds (top);

    area.removeFromTop (Space::s + ListRow::height); // the headings
    auto buttons = area.removeFromBottom (SettingsTint::controlHeight);
    area.removeFromBottom (Space::s);
    help.setBounds (area.removeFromBottom (ShortcutsTint::helpLines * ListRow::height));
    area.removeFromBottom (Space::s);
    list.setBounds (area);

    for (auto* button : { &changeButton, &addButton, &removeButton, &resetButton })
    {
        button->setBounds (buttons.removeFromLeft (button->getBestWidthForHeight (SettingsTint::controlHeight)));
        buttons.removeFromLeft (Space::s);
    }

    if (capture != nullptr)
        capture->setBounds (getLocalBounds());
}

//==============================================================================
ShortcutsEditor::KeyCapture::KeyCapture (ShortcutsEditor& owner, const Command& c, std::optional<juce::KeyPress> keyReplaced)
    : editor (owner),
      command (c),
      replacing (keyReplaced),
      keymapName (owner.commands.getKeymap().name)
{
    setTitle ("Key for " + command.name);
    setDescription ("Press the keys to give " + command.name + ", then Tab to Assign");
    setWantsKeyboardFocus (true);

    heading.setText ((replacing.has_value() ? "Press the keys for " + command.name + ", in place of " + describe (*replacing)
                                            : "Press the keys to give " + command.name)
                         + ". Tab goes on to Assign.",
                     juce::dontSendNotification);
    heading.setFont (juce::FontOptions (TextSize::body));
    heading.setJustificationType (juce::Justification::topLeft);

    share.setFont (juce::FontOptions (TextSize::small));
    share.setJustificationType (juce::Justification::topLeft);
    share.setMinimumHorizontalScale (1.0f);
    setThemeColour (share, juce::Label::textColourId, &Palette::textMuted);

    // A click on the card's words gives the card the keys back, as a click beside them does.
    heading.setInterceptsMouseClicks (false, false);
    share.setInterceptsMouseClicks (false, false);

    assignButton.setTooltip ("Gives " + command.name + " the key pressed; the other commands that have it keep it");
    assignButton.setEnabled (false);
    setPrimary (assignButton, true);
    assignButton.onClick = [this]
    {
        // Never into another keymap than the one the card opened for.
        if (key.has_value() && keymapName == editor.commands.getKeymap().name)
            editor.assign (command.id, replacing, *key);

        editor.closeKeyCapture();
    };

    cancelButton.setTooltip ("Closes this without changing a key (Escape, from the buttons)");
    cancelButton.onClick = [this] { editor.closeKeyCapture(); };

    for (auto* c2 : std::initializer_list<juce::Component*> { &heading, &share, &assignButton, &cancelButton })
        addAndMakeVisible (c2);
}

bool ShortcutsEditor::KeyCapture::keyPressed (const juce::KeyPress& pressed)
{
    // Tab and Shift+Tab go round the card's parts, the key, Assign (once there is a key) and Cancel, and stay in the card;
    // with the focus on a button, Escape closes the card, and Enter and Space are the button's. Every other key is taken,
    // and goes nowhere else.
    // The keys the system needs stay the system's: Alt+F4 closes the window, Alt+Tab and the like switch.
    if (isSystemKey (pressed))
        return false;

    if (pressed.getKeyCode() == juce::KeyPress::tabKey)
    {
        std::vector<juce::Component*> parts { this };

        if (assignButton.isEnabled())
            parts.push_back (&assignButton);

        parts.push_back (&cancelButton);
        const auto now = std::find_if (parts.begin(), parts.end(), [] (juce::Component* c) { return c->hasKeyboardFocus (false); });
        const auto index = now != parts.end() ? (int) (now - parts.begin()) : 0;
        const auto step = pressed.getModifiers().isShiftDown() ? -1 : 1;
        parts[(size_t) ((index + step + (int) parts.size()) % (int) parts.size())]->grabKeyboardFocus();
        return true;
    }

    if (! hasKeyboardFocus (false))
    {
        if (pressed == juce::KeyPress::escapeKey)
            editor.closeKeyCapture();

        return true;
    }

    take (pressed);
    return true;
}

bool ShortcutsEditor::KeyCapture::isSystemKey (const juce::KeyPress& pressed)
{
    const auto modifiers = pressed.getModifiers();
    const auto code = pressed.getKeyCode();

    if (modifiers.isAltDown() && ! modifiers.isCtrlDown())
        return code == juce::KeyPress::F4Key || code == juce::KeyPress::tabKey || code == juce::KeyPress::escapeKey
               || code == juce::KeyPress::spaceKey;

    return modifiers.isCtrlDown() && ! modifiers.isAltDown() && code == juce::KeyPress::escapeKey;
}

void ShortcutsEditor::KeyCapture::take (const juce::KeyPress& pressed)
{
    // A key JUCE gives no code (Pause, the volume and browser keys) can't be a shortcut: Assign waits for another.
    if (! pressed.isValid())
        return;

    // A key AltGr typed a character with runs no shortcut (KeyDispatcher): not taken, and the card says what to press.
    if (isTypedWithAltGr (pressed, editor.keys.getLayout()))
    {
        share.setText ("AltGr types " + juce::String::charToString (pressed.getTextCharacter())
                           + " with this key, so it can't run a command: press it with the left Alt instead, or another key.",
                       juce::dontSendNotification);
        juce::AccessibilityHandler::postAnnouncement (share.getText(), juce::AccessibilityHandler::AnnouncementPriority::medium);
        return;
    }

    // As the command would keep it (getShortcutKey()); what else it does, by the key as it came.
    key = getShortcutKey (pressed, editor.keys.getLayout());
    const auto shares = describeKeyShare (editor.commands, editor.keys, editor.keyboard, command.id, pressed);
    share.setText (shares.isEmpty() ? juce::String ("No other command has this key.") : shares.joinIntoString ("\n"),
                   juce::dontSendNotification);
    assignButton.setEnabled (true);
    setDescription (describe (*key) + ". " + share.getText());
    juce::AccessibilityHandler::postAnnouncement (describe (*key) + ". " + share.getText(),
                                                  juce::AccessibilityHandler::AnnouncementPriority::medium);
    repaint();
}

void ShortcutsEditor::KeyCapture::mouseDown (const juce::MouseEvent& e)
{
    if (! getCard().contains (e.getPosition()))
        editor.closeKeyCapture();
    else
        grabKeyboardFocus();
}

juce::Rectangle<int> ShortcutsEditor::KeyCapture::getCard() const
{
    const auto width = juce::jmin (ShortcutsTint::captureWidth, getWidth() - 2 * Space::s);
    const auto height = 2 * ShortcutsTint::captureInset + 2 * ListRow::height + Space::s + controlHeight + Space::s
                        + ShortcutsTint::helpLines * ListRow::height + Space::s + controlHeight;
    return getLocalBounds().withSizeKeepingCentre (width, juce::jmin (height, getHeight()));
}

juce::Rectangle<int> ShortcutsEditor::KeyCapture::getKeyArea() const
{
    return getCard().reduced (ShortcutsTint::captureInset).withTrimmedTop (2 * ListRow::height + Space::s).withHeight (controlHeight);
}

void ShortcutsEditor::KeyCapture::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.background.withAlpha (ShortcutsTint::scrim));

    const auto card = getCard().toFloat();
    g.setColour (p.surfaceRaised);
    g.fillRoundedRectangle (card, cardCornerRadius);
    g.setColour (p.border);
    g.drawRoundedRectangle (card.reduced (0.5f), cardCornerRadius, 1.0f);

    // Where the key shows, outlined in the focus colour while the keys come here.
    const auto keyArea = getKeyArea().toFloat();
    g.setColour (p.background);
    g.fillRoundedRectangle (keyArea, cornerRadius);
    g.setColour (hasKeyboardFocus (false) ? p.focus : p.controlBorder);
    g.drawRoundedRectangle (keyArea.reduced (0.5f), cornerRadius, 1.0f);

    if (key.has_value())
    {
        drawKeyCaps (g, p, describe (*key), keyArea.reduced ((float) Space::s, 0.0f), p.text);
    }
    else
    {
        g.setColour (p.textMuted);
        g.setFont (juce::FontOptions (TextSize::body));
        g.drawText ("Waiting for a key...", keyArea.reduced ((float) Space::s, 0.0f), juce::Justification::centredLeft, true);
    }
}

void ShortcutsEditor::KeyCapture::resized()
{
    auto area = getCard().reduced (ShortcutsTint::captureInset);
    heading.setBounds (area.removeFromTop (2 * ListRow::height));
    area.removeFromTop (Space::s + controlHeight + Space::s); // the key
    auto buttons = area.removeFromBottom (controlHeight);
    area.removeFromBottom (Space::s);
    share.setBounds (area);

    cancelButton.setBounds (buttons.removeFromRight (cancelButton.getBestWidthForHeight (controlHeight)));
    buttons.removeFromRight (Space::s);
    assignButton.setBounds (buttons.removeFromRight (assignButton.getBestWidthForHeight (controlHeight)));
}
} // namespace phazite::ui
