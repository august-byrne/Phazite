// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SettingsPanel.h"
#include "model/mixer/MixerModel.h"
#include "model/transport/TransportModel.h"
#include "ui/theme/Icons.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/theme/Popups.h"

#include <algorithm>
#include <cmath>

namespace phazite::ui
{
namespace
{
    juce::Font getBodyFont()
    {
        return juce::FontOptions (TextSize::body);
    }

    juce::Font getNameFont()
    {
        return juce::FontOptions (TextSize::body, juce::Font::bold); // Inter's SemiBold (LookAndFeel)
    }

    // Text wrapped to a width, a line every SettingsTint::lineHeight, as VS Code's settings write theirs (1.4 em).
    juce::TextLayout layOutText (const juce::String& text, const juce::Font& font, juce::Colour colour, int width)
    {
        juce::AttributedString string (text);
        string.setFont (font);
        string.setColour (colour);
        string.setLineSpacing ((float) SettingsTint::lineHeight - font.getHeight());
        string.setWordWrap (juce::AttributedString::byWord);
        juce::TextLayout layout;
        layout.createLayout (string, (float) juce::jmax (1, width));
        return layout;
    }

    int getLinesHeight (const juce::String& text, const juce::Font& font, int width)
    {
        if (text.isEmpty())
            return 0;

        return layOutText (text, font, {}, width).getNumLines() * SettingsTint::lineHeight;
    }

    void drawLines (juce::Graphics& g, const juce::String& text, const juce::Font& font, juce::Colour colour, juce::Rectangle<int> area)
    {
        if (text.isEmpty())
            return;

        // Each line in the middle of its own line's height.
        const auto leading = ((float) SettingsTint::lineHeight - font.getHeight()) / 2.0f;
        layOutText (text, font, colour, area.getWidth()).draw (g, area.toFloat().translated (0.0f, leading));
    }

    // The items of a list to pick from, made again only where they changed; the one chosen shown without a change message.
    void showItems (juce::ComboBox& box, const juce::StringArray& items, int selectedIndex)
    {
        juce::StringArray shown;

        for (int i = 0; i < box.getNumItems(); ++i)
            shown.add (box.getItemText (i));

        if (shown != items)
        {
            box.clear (juce::dontSendNotification);

            for (int i = 0; i < items.size(); ++i)
                box.addItem (items[i], i + 1);
        }

        box.setSelectedId (juce::isPositiveAndBelow (selectedIndex, items.size()) ? selectedIndex + 1 : 0, juce::dontSendNotification);
    }

    juce::String describeBuffer (int samples, double sampleRate)
    {
        auto text = juce::String (samples) + " samples";

        if (sampleRate > 0.0)
            text << " (" << juce::String (samples * 1000.0 / sampleRate, 1) << " ms)";

        return text;
    }

    // The words of a search, each to be found.
    juce::StringArray getWords (const juce::String& text)
    {
        auto words = juce::StringArray::fromTokens (text.toLowerCase(), " \t", {});
        words.removeEmptyStrings();
        return words;
    }

    const juce::StringArray themeValues { "dark", "light", "highContrast", "system" }; // as Settings::theme keeps them
    const juce::StringArray themeNames { "Dark", "Light", "High Contrast", "Follow System" };
    const juce::StringArray themeCommands { "options.theme.dark", "options.theme.light", "options.theme.highcontrast",
                                            "options.theme.followsystem" };
} // namespace

//==============================================================================
DelayDescription describeDelay (const EngineContext::AudioOutput& output, const DelayCompensation& compensation, bool lowLatency,
                                bool hasAsioDrivers)
{
    DelayDescription description;

    if (! output.isOpen)
    {
        description.lines.add ("No audio output is open, so nothing can be heard: choose one above.");
        return description;
    }

    const auto ms = [] (double seconds)
    {
        const auto milliseconds = seconds * 1000.0;
        return juce::String (milliseconds, milliseconds < 100.0 ? 1 : 0) + " ms";
    };

    const auto shortest = output.latencySeconds + (lowLatency ? 0.0 : compensation.seconds);
    auto& lines = description.lines;
    lines.add ("A note you play sounds " + ms (shortest) + " to " + ms (shortest + output.bufferSeconds) + " later"
               + (output.isBluetooth ? ", and Bluetooth adds more." : "."));
    lines.add ("The buffer: " + juce::String (output.bufferSamples) + " samples, " + ms (output.bufferSeconds)
               + ", which a note waits for at most.");
    lines.add ("The output: " + ms (output.latencySeconds) + ", as its driver reports it.");

    if (lowLatency)
    {
        lines.add ("Delay compensation: off, as Low Latency is on; Phazite's devices that look ahead are bypassed.");
    }
    else if (compensation.seconds > 0.0)
    {
        juce::StringArray late;

        for (const auto& [name, seconds] : compensation.devices)
            if (late.size() < 3)
                late.add (name + " (" + ms (seconds) + ")");

        lines.add ("Delay compensation: " + ms (compensation.seconds) + ", for " + late.joinIntoString (", ")
                   + ". Low Latency, in the status bar, turns it off while you play.");
    }
    else
    {
        lines.add ("Delay compensation: none, as no device in this song looks ahead.");
    }

    if (hasAsioDrivers && ! output.type.equalsIgnoreCase ("ASIO"))
        lines.add ("With an audio interface, choose ASIO above, then its driver: sound comes with less delay.");

    if (output.isBluetooth)
        description.warning = "This output is Bluetooth, which adds about 150 to 250 ms that no driver reports: fine for listening, "
                              "too slow for playing and recording. For those, plug in wired headphones or speakers and choose them "
                              "above.";

    return description;
}

struct SettingRow::HoverWatch : public juce::MouseListener
{
    explicit HoverWatch (SettingRow& r)
        : row (r)
    {
    }

    void mouseEnter (const juce::MouseEvent&) override { row.updateHover(); }
    void mouseExit (const juce::MouseEvent&) override { row.updateHover(); }

    SettingRow& row;
};

//==============================================================================
SettingRow::SettingRow (const juce::String& groupName, const juce::String& name, const juce::String& text)
    : group (groupName),
      title (name),
      description (text)
{
    setTitle (title);
    setDescription (description);
    setWantsKeyboardFocus (false);
    hoverWatch = std::make_unique<HoverWatch> (*this);
    addMouseListener (hoverWatch.get(), true);

    // The gear, in the gutter at its left, which shows under the pointer or with the keyboard focus (VS Code's
    // setting-toolbar-container): the keys reach its menu with Shift+F10 instead.
    auto button = std::make_unique<IconButton> ("More Actions for " + title, Icon::settings);
    button->setQuiet (true);
    button->setWantsKeyboardFocus (false);
    button->setTooltip ("Reset Setting");
    button->onClick = [this] { showMenu(); };
    gear = std::move (button);
    addChildComponent (*gear);
}

SettingRow::~SettingRow()
{
    removeMouseListener (hoverWatch.get());
}

juce::Button& SettingRow::getGearButton() noexcept
{
    return *gear;
}

void SettingRow::setDescriptionText (const juce::String& text)
{
    if (text == description)
        return;

    description = text;
    juce::Component::setDescription (text);
    controlsChanged();
    repaint();
}

void SettingRow::setNote (const juce::String& text)
{
    if (text != note)
    {
        note = text;
        repaint();
    }
}

void SettingRow::setWarning (bool isWarning)
{
    warning = isWarning;
    repaint();
}

juce::ToggleButton& SettingRow::makeSwitch()
{
    auto button = std::make_unique<juce::ToggleButton>();
    auto& made = *button;
    switchRow = true;
    setControl (std::move (button), (int) ButtonTint::tickBoxSize);
    return made;
}

void SettingRow::setControl (std::unique_ptr<juce::Component> newControl, int width)
{
    control = std::move (newControl);
    controlWidth = width;
    addAndMakeVisible (*control);
    controlsChanged();
}

void SettingRow::controlsChanged()
{
    if (control != nullptr)
    {
        control->setTitle (title);
        control->setDescription (description);
    }
}

void SettingRow::update()
{
    if (refresh != nullptr)
        refresh();

    if (isModified != nullptr && isModified() != shownModified)
        repaint();
}

void SettingRow::setAvailable (bool isAvailable)
{
    available = isAvailable;
}

bool SettingRow::matches (const juce::StringArray& words) const
{
    const auto text = (title + " " + description + " " + group).toLowerCase();

    for (const auto& word : words)
        if (! text.contains (word))
            return false;

    return true;
}

juce::Rectangle<int> SettingRow::getContents() const
{
    return getLocalBounds().reduced (SettingsTint::rowInset, 0);
}

int SettingRow::getTextHeight (const juce::String& text, int width) const
{
    return getLinesHeight (text, getBodyFont(), width);
}

juce::Rectangle<int> SettingRow::getDescriptionArea() const
{
    auto area = getContents().reduced (SettingsTint::padSide, 0);
    area.setY (SettingsTint::padTop + SettingsTint::lineHeight + SettingsTint::titleGap);

    if (switchRow)
        area.removeFromLeft ((int) ButtonTint::tickBoxSize + SettingsTint::tickGap);

    area.setHeight (getTextHeight (description, area.getWidth()));
    return area;
}

int SettingRow::getHeightForWidth (int width) const
{
    const auto textWidth = width - 2 * (SettingsTint::rowInset + SettingsTint::padSide);
    auto height = SettingsTint::padTop + SettingsTint::lineHeight + SettingsTint::titleGap;

    if (switchRow)
    {
        const auto beside = textWidth - (int) ButtonTint::tickBoxSize - SettingsTint::tickGap;
        height += juce::jmax (SettingsTint::lineHeight, getTextHeight (description, beside));
    }
    else
    {
        height += getTextHeight (description, textWidth);

        if (control != nullptr)
            height += SettingsTint::valueGap + (control->getHeight() > 0 ? control->getHeight() : SettingsTint::controlHeight);
    }

    if (note.isNotEmpty())
        height += Space::xs + getTextHeight (note, textWidth);

    return height + (switchRow ? SettingsTint::switchPadBottom : SettingsTint::padBottom);
}

void SettingRow::resized()
{
    const auto contents = getContents();
    gear->setBounds (contents.getX() - SettingsTint::gearSize, SettingsTint::gearTop, SettingsTint::gearSize, SettingsTint::gearSize);

    if (control == nullptr)
        return;

    const auto text = getDescriptionArea();

    if (switchRow)
    {
        const auto box = (int) ButtonTint::tickBoxSize;
        control->setBounds (contents.getX() + SettingsTint::padSide, text.getY() + (SettingsTint::lineHeight - box) / 2, box, box);
        return;
    }

    const auto height = control->getHeight() > 0 ? control->getHeight() : SettingsTint::controlHeight;
    const auto width = juce::jmin (controlWidth, contents.getWidth() - 2 * SettingsTint::padSide);
    control->setBounds (contents.getX() + SettingsTint::padSide, text.getBottom() + SettingsTint::valueGap, width, height);
}

void SettingRow::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    const auto contents = getContents();

    // Washed under the pointer, more with the keyboard focus, and then outlined (settings.rowHoverBackground,
    // focusedRowBackground, focusedRowBorder).
    if (focused)
    {
        g.setColour (p.hover.withMultipliedAlpha (SettingsTint::focusWash));
        g.fillRect (contents);
        g.setColour (p.focus);
        g.drawRect (contents, 1);
    }
    else if (hovered)
    {
        g.setColour (p.hover.withMultipliedAlpha (SettingsTint::hoverWash));
        g.fillRect (contents);
    }

    // Changed from its default: the line at its left.
    shownModified = isModified != nullptr && isModified();

    if (shownModified)
    {
        const auto bottom = switchRow ? SettingsTint::switchModifiedBottom : SettingsTint::modifiedBottom;
        g.setColour (p.modified);
        g.fillRect (contents.getX() + SettingsTint::modifiedLeft, contents.getY() + SettingsTint::modifiedTop, SettingsTint::modifiedWidth,
                    contents.getHeight() - SettingsTint::modifiedTop - bottom);
    }

    const auto inner = contents.reduced (SettingsTint::padSide, 0);
    g.setColour (p.heading);
    g.setFont (getNameFont());
    g.drawText (title, inner.withY (SettingsTint::padTop).withHeight (SettingsTint::lineHeight), juce::Justification::centredLeft, true);

    const auto text = getDescriptionArea();
    drawLines (g, description, getBodyFont(), warning ? p.warning : p.text.withMultipliedAlpha (SettingsTint::descriptionAlpha), text);

    if (note.isNotEmpty())
    {
        const auto top = (control != nullptr && ! switchRow ? control->getBottom() : text.getBottom()) + Space::xs;
        drawLines (g, note, getBodyFont(), p.warning, inner.withY (top).withHeight (getTextHeight (note, inner.getWidth())));
    }
}

void SettingRow::updateHover()
{
    const auto over = isMouseOver (true);

    if (over != hovered)
    {
        hovered = over;
        gear->setVisible (reset != nullptr && isHighlighted());
        repaint();
    }
}

void SettingRow::focusOfChildComponentChanged (FocusChangeType)
{
    const auto has = hasKeyboardFocus (true);

    if (has != focused)
    {
        focused = has;
        gear->setVisible (reset != nullptr && isHighlighted());
        repaint();

        if (focused && onFocused != nullptr)
            onFocused (*this);
    }
}

juce::PopupMenu SettingRow::getMenu()
{
    juce::PopupMenu menu;

    if (reset == nullptr)
        return menu;

    juce::PopupMenu::Item item ("Reset Setting");
    item.isEnabled = isModified == nullptr || isModified();
    item.action = [safeThis = juce::Component::SafePointer<SettingRow> (this)]
    {
        if (safeThis != nullptr && safeThis->reset != nullptr)
            safeThis->reset();
    };
    menu.addItem (std::move (item));
    return menu;
}

void SettingRow::showMenu()
{
    auto menu = getMenu();

    if (menu.getNumItems() == 0)
        return;

    // From the gear, under it, as a menu a button drops; from a right-click or the keys, at the pointer or the row.
    if (gear->isMouseButtonDown() || gear->isMouseOver())
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (gear.get()));
    else
        menu.showMenuAsync (getContextMenuOptions (*this));
}

void SettingRow::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && (e.eventComponent == this || e.eventComponent == gear.get()))
        showMenu();
}

void SettingRow::mouseUp (const juce::MouseEvent& e)
{
    // A switch's sentence switches it, as VS Code's does.
    if (e.eventComponent != this || e.mods.isPopupMenu() || ! switchRow || e.mouseWasDraggedSinceMouseDown())
        return;

    if (auto* button = dynamic_cast<juce::Button*> (control.get());
        button != nullptr && button->isEnabled() && getDescriptionArea().expanded (0, Space::xs).contains (e.getPosition()))
        button->setToggleState (! button->getToggleState(), juce::sendNotificationSync);
}

bool SettingRow::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress (juce::KeyPress::F10Key, juce::ModifierKeys::shiftModifier, 0) && reset != nullptr)
    {
        showMenu();
        return true;
    }

    return false;
}

std::unique_ptr<juce::AccessibilityHandler> SettingRow::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group);
}

//==============================================================================
// A group's title in the list (settings-group-title-label): big and semibold.
class SettingsPanel::GroupTitle : public juce::Component
{
public:
    explicit GroupTitle (const juce::String& name)
    {
        setTitle (name);
        setInterceptsMouseClicks (false, false);
    }

    static int getTitleHeight() { return 2 * SettingsTint::groupTitlePadding + SettingsTint::groupTitleLine; }

    void paint (juce::Graphics& g) override
    {
        g.setColour (getPalette (*this).heading);
        g.setFont (juce::FontOptions (SettingsTint::groupTitleSize, juce::Font::bold));
        g.drawText (getTitle(), getLocalBounds().withTrimmedLeft (SettingsTint::rowInset + SettingsTint::groupTitleLeft),
                    juce::Justification::centredLeft, true);
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::staticText);
    }
};

//==============================================================================
// The table of contents (VS Code's settings-toc): an entry for each group with settings, which scrolls the list to it, the one
// in view marked; while searching, each with how many it has left; and Shortcuts at its foot, apart. Each entry is a button,
// which Tab reaches; Up and Down go from one to the next.
class SettingsPanel::Contents : public juce::Component
{
    struct Entry;

public:
    explicit Contents (SettingsPanel& owner)
        : panel (owner)
    {
        setTitle ("Table of contents");
        setFocusContainerType (FocusContainerType::none);
    }

    void setEntries (const juce::StringArray& groups)
    {
        entries.clear();

        for (const auto& name : groups)
            add (name);

        add (shortcuts);
        resized();
    }

    void show (const juce::String& group, bool listed, int count)
    {
        if (auto* entry = find (group))
        {
            entry->setVisible (listed);
            entry->count = count;
            entry->repaint();
        }
    }

    void mark (const juce::String& group)
    {
        if (group == marked)
            return;

        marked = group;

        for (auto* entry : entries)
        {
            entry->setToggleState (entry->getName() == marked, juce::dontSendNotification);
            entry->repaint();
        }
    }

    juce::String getMarked() const { return marked; }

    Entry* find (const juce::String& group) const
    {
        for (auto* entry : entries)
            if (entry->getName() == group)
                return entry;

        return nullptr;
    }

    int getCount (const juce::String& group) const
    {
        for (auto* entry : entries)
            if (entry->getName() == group)
                return entry->count;

        return 0;
    }

    void resized() override
    {
        auto y = 0;

        for (auto* entry : entries)
        {
            if (entry->getName() == juce::String (shortcuts))
                y += 2 * SettingsTint::tocSeparator + 1; // the line above it

            if (entry->isVisible() || entry->getName() == juce::String (shortcuts))
            {
                entry->setBounds (0, y, getWidth(), SettingsTint::tocRowHeight);
                y += SettingsTint::tocRowHeight;
            }
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (auto* last = find (shortcuts))
        {
            g.setColour (getPalette (*this).border);
            g.fillRect (0, last->getY() - SettingsTint::tocSeparator - 1, getWidth() - SettingsTint::sideInset, 1);
        }
    }

private:
    struct Entry : public juce::Button
    {
        Entry (const juce::String& name, SettingsPanel& p)
            : juce::Button (name),
              panel (p)
        {
            setTitle (name);
            setDescription (name == juce::String (SettingsPanel::shortcuts) ? "Shows every command's keys"
                                                                            : "Scrolls the settings to " + name);
            onClick = [this] { panel.showGroup (getName()); };
        }

        void paintButton (juce::Graphics& g, bool isHighlighted, bool) override
        {
            // VS Code's: the entry in view semibold in the heading colour; the others in the text colour at 90 %, the heading
            // colour at 70 % under the pointer; the keyboard focus outlined.
            const auto& p = getPalette (*this);
            const auto isMarked = getToggleState();
            auto area = getLocalBounds().withTrimmedLeft (Space::s).withTrimmedRight (Space::xs);
            const auto font = isMarked ? juce::Font (juce::FontOptions (TextSize::body, juce::Font::bold)) : getBodyFont();
            g.setFont (font);
            g.setColour (isMarked        ? p.heading
                         : isHighlighted ? p.heading.withMultipliedAlpha (SettingsTint::tocHoverAlpha)
                                         : p.text.withMultipliedAlpha (SettingsTint::tocOtherAlpha));
            const auto countText = count > 0 ? " (" + juce::String (count) + ")" : juce::String();
            const auto countWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (getBodyFont(), countText));
            const auto name = area.withTrimmedRight (countWidth);
            g.drawText (getName(), name, juce::Justification::centredLeft, true);

            if (countText.isNotEmpty())
            {
                const auto nameWidth = juce::roundToInt (juce::GlyphArrangement::getStringWidth (font, getName()));
                g.setFont (getBodyFont());
                g.setColour (p.text.withMultipliedAlpha (SettingsTint::tocCountAlpha));
                g.drawText (countText, area.withTrimmedLeft (juce::jmin (nameWidth, name.getWidth())), juce::Justification::centredLeft,
                            false);
            }

            if (hasKeyboardFocus (false))
            {
                g.setColour (p.focus);
                g.drawRect (getLocalBounds().withTrimmedRight (Space::xs), 1);
            }
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key != juce::KeyPress::upKey && key != juce::KeyPress::downKey)
                return juce::Button::keyPressed (key);

            if (auto* owner = dynamic_cast<Contents*> (getParentComponent()))
                owner->moveFocus (this, key == juce::KeyPress::downKey ? 1 : -1);

            return true;
        }

        void focusGained (FocusChangeType) override { repaint(); }
        void focusLost (FocusChangeType) override { repaint(); }

        SettingsPanel& panel;
        int count = 0;
    };

    SettingsPanel& panel;
    juce::OwnedArray<Entry> entries;
    juce::String marked;

    void add (const juce::String& name)
    {
        auto* entry = entries.add (std::make_unique<Entry> (name, panel));
        entry->setClickingTogglesState (false);
        addAndMakeVisible (entry);
    }

    void moveFocus (Entry* from, int step)
    {
        auto index = entries.indexOf (from);

        for (index += step; juce::isPositiveAndBelow (index, entries.size()); index += step)
        {
            if (entries[index]->isVisible())
            {
                entries[index]->grabKeyboardFocus();
                return;
            }
        }
    }
};

//==============================================================================
// The list's scrolling view: as it scrolls, the group in view is marked in the table of contents.
class SettingsPanel::ListView : public juce::Viewport
{
public:
    explicit ListView (SettingsPanel& owner)
        : panel (owner)
    {
        setTitle ("Settings");
        setScrollBarsShown (true, false);
    }

    void visibleAreaChanged (const juce::Rectangle<int>&) override { panel.markGroupInView(); }

private:
    SettingsPanel& panel;
};

//==============================================================================
// A device's channels, two by two, a tick box for each pair, under each other.
namespace
{
    class ChannelPairs : public juce::Component
    {
    public:
        std::function<void (int pair, bool on)> onSwitch;

        void show (const juce::StringArray& pairs, const juce::Array<bool>& on)
        {
            if (pairs != names)
            {
                names = pairs;
                buttons.clear();

                for (int i = 0; i < names.size(); ++i)
                {
                    auto* button = buttons.add (std::make_unique<juce::ToggleButton> (names[i]));
                    button->setTitle (names[i]);
                    button->onClick = [this, i, button]
                    {
                        if (onSwitch != nullptr)
                            onSwitch (i, button->getToggleState());
                    };
                    addAndMakeVisible (button);
                }

                setSize (getWidth(), juce::jmax (1, names.size()) * ListRow::height);
            }

            for (int i = 0; i < buttons.size(); ++i)
                buttons[i]->setToggleState (on[i], juce::dontSendNotification);
        }

        juce::ToggleButton* getButton (int pair) const { return buttons[pair]; }

        void resized() override
        {
            for (int i = 0; i < buttons.size(); ++i)
                buttons[i]->setBounds (0, i * ListRow::height, getWidth(), ListRow::height);
        }

    private:
        juce::StringArray names;
        juce::OwnedArray<juce::ToggleButton> buttons;
    };
} // namespace

// The Audio group's rows and their controls, shown from what the engine context says of the device.
struct SettingsPanel::AudioRows
{
    SettingRow *driver = nullptr, *output = nullptr, *input = nullptr, *outputChannels = nullptr, *inputChannels = nullptr;
    SettingRow *sampleRate = nullptr, *bufferSize = nullptr, *delay = nullptr, *bluetooth = nullptr, *testSound = nullptr;
    SettingRow* controlPanel = nullptr;
    juce::ComboBox *driverBox = nullptr, *outputBox = nullptr, *inputBox = nullptr, *rateBox = nullptr, *bufferBox = nullptr;
    ChannelPairs *outputPairs = nullptr, *inputPairs = nullptr;
    EngineContext::AudioChoices shown;
};

//==============================================================================
SettingsPanel::SettingsPanel (EngineContext& e, CommandBridge& c, ComputerKeyboard& keyboard, const ProjectContext& context, Settings& s,
                              Actions a)
    : engine (e),
      commands (c),
      computerKeyboard (keyboard),
      projectContext (context),
      settings (s),
      actions (std::move (a))
{
    setTitle ("Settings");
    setOpaque (true);
    const auto& p = getPalette (*this);

    // The search, as VS Code's: its count at its right end while it searches.
    searchField.setTitle ("Search settings");
    searchField.setDescription ("Shows only the settings whose name, description or group have each word typed");
    searchField.setTextToShowWhenEmpty ("Search settings", p.textMuted);
    searchField.setFont (getBodyFont());
    searchField.setIndents (Space::s - 2, (SettingsTint::searchHeight - (int) TextSize::body) / 2 - 1);
    searchField.onTextChange = [this] { search (searchField.getText()); };

    // Escape clears a search; with none, it goes to the window, which closes.
    searchField.onEscapeKey = [this]
    {
        if (searchField.isEmpty())
        {
            if (auto* top = getTopLevelComponent(); top != nullptr && top != this)
                top->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));

            return;
        }

        searchField.setText ({}, true);
    };
    addAndMakeVisible (searchField);

    countLabel.setFont (getBodyFont());
    countLabel.setJustificationType (juce::Justification::centredRight);
    countLabel.setInterceptsMouseClicks (false, false);
    setThemeColour (countLabel, juce::Label::textColourId, &Palette::textMuted);
    addAndMakeVisible (countLabel);

    noResults.setText ("No Settings Found", juce::dontSendNotification);
    noResults.setFont (getBodyFont());
    noResults.setBorderSize ({});
    setThemeColour (noResults, juce::Label::textColourId, &Palette::text);
    addChildComponent (noResults);

    contents = std::make_unique<Contents> (*this);
    addAndMakeVisible (*contents);
    listView = std::make_unique<ListView> (*this);
    list = std::make_unique<juce::Component>();
    list->setTitle ("Settings list");
    listView->setViewedComponent (list.get(), false);
    addAndMakeVisible (*listView);

    shortcutPage = std::make_unique<ShortcutSettings> (commands, computerKeyboard, actions.chooseKeymap);
    addChildComponent (*shortcutPage);

    addAudioRows();
    addMidiRows();
    addPreferenceRows();

    // The groups that have rows, in their order.
    for (const auto* group : { audio, midi, recording, appearance, help, editing, files, versions, startup, updates })
    {
        if (std::any_of (rows.begin(), rows.end(), [group] (const auto& row) { return row->group == group; }))
        {
            groups.add (group);
            list->addAndMakeVisible (titles.add (std::make_unique<GroupTitle> (group)));
        }
    }

    contents->setEntries (groups);

    for (auto& row : rows)
    {
        row->onFocused = [this] (SettingRow& focusedRow) { scrollRowIntoView (focusedRow); };
        list->addAndMakeVisible (*row);
    }

    settings.addListener (this);
    engine.addChangeListener (this);
    refreshAll();
    startTimer (1000);
}

SettingsPanel::~SettingsPanel()
{
    engine.removeChangeListener (this);
    settings.removeListener (this);
}

juce::Viewport& SettingsPanel::getList() noexcept
{
    return *listView;
}

juce::Component& SettingsPanel::getContents() noexcept
{
    return *contents;
}

SettingRow& SettingsPanel::addRow (const char* group, const juce::String& title, const juce::String& description)
{
    rows.push_back (std::make_unique<SettingRow> (group, title, description));
    return *rows.back();
}

void SettingsPanel::addSwitch (const char* group, const juce::String& title, const juce::String& description, const juce::String& commandID,
                               bool defaultValue)
{
    // A preference that a command switches (and the menus tick): the command runs, so that both show the same.
    if (commands.getRegistry().find (commandID) == nullptr)
        return;

    auto& row = addRow (group, title, description);
    auto& button = row.makeSwitch();
    const auto isOn = [this, commandID]
    {
        const auto* command = commands.getRegistry().find (commandID);
        return command != nullptr && command->isTicked != nullptr && command->isTicked();
    };
    const auto setOn = [this, commandID, isOn] (bool on)
    {
        if (isOn() != on)
            commands.invoke (commandID);

        refreshAll();
    };

    button.onClick = [&button, setOn] { setOn (button.getToggleState()); };
    row.refresh = [&button, isOn] { button.setToggleState (isOn(), juce::dontSendNotification); };
    row.isModified = [isOn, defaultValue] { return isOn() != defaultValue; };
    row.reset = [setOn, defaultValue] { setOn (defaultValue); };
}

//==============================================================================
void SettingsPanel::addAudioRows()
{
    audioRows = std::make_unique<AudioRows>();
    auto& a = *audioRows;

    const auto addChoice = [this] (const juce::String& title, const juce::String& description, juce::ComboBox*& box)
    {
        auto& row = addRow (audio, title, description);
        auto made = std::make_unique<juce::ComboBox>();
        box = made.get();
        row.setControl (std::move (made), SettingsTint::choiceWidth);
        return &row;
    };

    // Each choice opens the device so; where it doesn't open, its row says why and the lists show what plays again.
    const auto choose = [this] (SettingRow* row, std::function<juce::String()> change)
    {
        const auto error = change();

        for (auto* each : { audioRows->driver, audioRows->output, audioRows->input, audioRows->outputChannels, audioRows->inputChannels,
                            audioRows->sampleRate, audioRows->bufferSize })
            each->setNote (each == row ? error : juce::String());

        updateAudio();
        layOutList();
    };

    a.driver = addChoice ("Driver", "How Phazite reaches the audio devices. An audio interface's own ASIO driver gives the least delay.",
                          a.driverBox);
    a.driverBox->onChange = [this, choose]
    { choose (audioRows->driver, [this] { return engine.chooseAudioDriver (audioRows->driverBox->getText()); }); };

    a.output = addChoice ("Output Device", "Where Phazite plays.", a.outputBox);
    a.outputBox->onChange = [this, choose]
    { choose (audioRows->output, [this] { return engine.chooseAudioOutput (audioRows->outputBox->getText()); }); };

    a.input = addChoice ("Input Device", "Where audio tracks record from, through the inputs their tracks choose.", a.inputBox);
    a.inputBox->onChange = [this, choose]
    {
        const auto index = audioRows->inputBox->getSelectedItemIndex();
        const auto device = index > 0 ? audioRows->inputBox->getText() : juce::String();
        choose (audioRows->input, [this, device] { return engine.chooseAudioInput (device); });
    };

    const auto addPairs = [this, choose] (const juce::String& title, const juce::String& description, bool input, ChannelPairs*& pairs)
    {
        auto& row = addRow (audio, title, description);
        auto made = std::make_unique<ChannelPairs>();
        pairs = made.get();
        pairs->onSwitch = [this, choose, input, rowPointer = &row] (int pair, bool on)
        { choose (rowPointer, [this, input, pair, on] { return engine.setChannelPairOn (input, pair, on); }); };
        row.setControl (std::move (made), SettingsTint::choiceWidth);
        return &row;
    };

    a.outputChannels = addPairs ("Output Channels", "The device's outputs Phazite plays through, two by two.", false, a.outputPairs);
    a.inputChannels = addPairs ("Input Channels", "The device's inputs Phazite opens, two by two.", true, a.inputPairs);

    a.sampleRate = addChoice ("Sample Rate", "How many samples a second the device plays.", a.rateBox);
    a.rateBox->onChange = [this, choose]
    {
        const auto index = audioRows->rateBox->getSelectedItemIndex();

        if (juce::isPositiveAndBelow (index, audioRows->shown.sampleRates.size()))
            choose (audioRows->sampleRate, [this, rate = audioRows->shown.sampleRates[index]] { return engine.chooseSampleRate (rate); });
    };

    a.bufferSize = addChoice ("Buffer Size",
                              "How many samples the device takes at a time: fewer let what you play sound sooner, more keep a busy song "
                              "from crackling.",
                              a.bufferBox);
    a.bufferBox->onChange = [this, choose]
    {
        const auto index = audioRows->bufferBox->getSelectedItemIndex();

        if (juce::isPositiveAndBelow (index, audioRows->shown.bufferSizes.size()))
            choose (audioRows->bufferSize, [this, size = audioRows->shown.bufferSizes[index]] { return engine.chooseBufferSize (size); });
    };

    // What the choices mean for delay (describeDelay()), under them, and the Bluetooth warning.
    a.delay = &addRow (audio, "Delay", {});
    a.bluetooth = &addRow (audio, "Bluetooth Output", {});
    a.bluetooth->setWarning (true);

    a.testSound = &addRow (audio, "Test Sound", "Plays a short tone through the output, to hear that it works.");
    auto test = std::make_unique<juce::TextButton> ("Test Sound");
    test->changeWidthToFitText (SettingsTint::controlHeight);
    const auto testWidth = test->getWidth();
    test->onClick = [this] { engine.playTestSound(); };
    a.testSound->setControl (std::move (test), testWidth);

    a.controlPanel = &addRow (audio, "Driver Control Panel", "The driver's own settings, in its own window.");
    auto panelButton = std::make_unique<juce::TextButton> ("Control Panel");
    panelButton->changeWidthToFitText (SettingsTint::controlHeight);
    const auto panelWidth = panelButton->getWidth();
    panelButton->onClick = [this]
    {
        // The driver's window is modal and the system's: Phazite's windows take no input until it closes, as JUCE's device
        // settings keep them.
        juce::Component blocker;
        blocker.setOpaque (true);
        blocker.addToDesktop (0);
        blocker.enterModalState();
        engine.showDriverControlPanel();
        blocker.exitModalState();
        getTopLevelComponent()->toFront (true);
        updateAudio();
    };
    a.controlPanel->setControl (std::move (panelButton), panelWidth);
}

void SettingsPanel::updateAudio()
{
    auto& a = *audioRows;
    const auto choices = engine.getAudioChoices();
    a.shown = choices;

    showItems (*a.driverBox, choices.drivers, choices.drivers.indexOf (choices.driver));
    showItems (*a.outputBox, choices.outputs, choices.outputs.indexOf (choices.output));
    a.output->setAvailable (! choices.drivers.isEmpty());
    a.driver->setAvailable (! choices.drivers.isEmpty());

    juce::StringArray inputs ("None");
    inputs.addArray (choices.inputs);
    showItems (*a.inputBox, inputs, choices.input.isEmpty() ? 0 : inputs.indexOf (choices.input));
    a.input->setAvailable (choices.inputsApart && ! choices.inputs.isEmpty());

    a.outputPairs->show (choices.outputPairs, choices.outputPairsOn);
    a.inputPairs->show (choices.inputPairs, choices.inputPairsOn);
    a.outputChannels->setAvailable (choices.isOpen && ! choices.outputPairs.isEmpty());
    a.inputChannels->setAvailable (choices.isOpen && ! choices.inputPairs.isEmpty());

    juce::StringArray rates, sizes;

    for (const auto rate : choices.sampleRates)
        rates.add (juce::String (juce::roundToInt (rate)) + " Hz");

    for (const auto size : choices.bufferSizes)
        sizes.add (describeBuffer (size, choices.sampleRate));

    showItems (*a.rateBox, rates, choices.sampleRates.indexOf (choices.sampleRate));
    showItems (*a.bufferBox, sizes, choices.bufferSizes.indexOf (choices.bufferSize));
    a.sampleRate->setAvailable (choices.isOpen);
    a.bufferSize->setAvailable (choices.isOpen);
    a.testSound->setAvailable (choices.isOpen);
    a.controlPanel->setAvailable (choices.isOpen && choices.hasControlPanel);

    const auto description =
        describeDelay (engine.getAudioOutput(), getDelayCompensation (projectContext), engine.isLowLatency(), engine.hasAsioDrivers());
    a.delay->setDescriptionText (description.lines.joinIntoString ("\n"));
    a.bluetooth->setDescriptionText (description.warning);
    a.bluetooth->setAvailable (description.warning.isNotEmpty());
}

juce::String SettingsPanel::getDelayText() const
{
    return audioRows->delay->getDescriptionText();
}

juce::String SettingsPanel::getWarning() const
{
    return audioRows->bluetooth->isAvailable() ? audioRows->bluetooth->getDescriptionText() : juce::String();
}

//==============================================================================
void SettingsPanel::addMidiRows()
{
    // The MIDI inputs' rows come and go with them (updateMidi()), before these.
    auto& none = addRow (midi, "MIDI Inputs", "No MIDI devices found. Connect a MIDI keyboard: it shows up here within a few seconds.");
    none.setAvailable (false);

    auto& keyboardRow = addRow (midi, "Computer Keyboard", {});
    auto& button = keyboardRow.makeSwitch();
    button.onClick = [this, &button]
    {
        computerKeyboard.setEnabled (button.getToggleState());
        refreshAll();
    };
    keyboardRow.refresh = [this, &button, &keyboardRow]
    {
        button.setToggleState (computerKeyboard.isEnabled(), juce::dontSendNotification);

        // The keys as the layout in use types them: it may have changed since.
        keyboardRow.setDescriptionText ("Plays the selected track's instrument from the typing keyboard: "
                                        + computerKeyboard.getWhiteKeysText() + " play the white keys from middle C, and "
                                        + computerKeyboard.getBlackKeysText() + " the black keys. " + computerKeyboard.getOctaveKeysText()
                                        + " change the octave, " + computerKeyboard.getVelocityKeysText()
                                        + " the velocity. Hold Shift for single-key shortcuts.");
    };
    keyboardRow.isModified = [this] { return computerKeyboard.isEnabled() != Settings::computerKeyboard.defaultValue; };
    keyboardRow.reset = [this]
    {
        computerKeyboard.setEnabled (Settings::computerKeyboard.defaultValue);
        refreshAll();
    };

    auto& routing = addRow (midi, "Live MIDI", {});
    routing.refresh = [this, &routing]
    {
        if (const auto track = getChannel (projectContext, getLiveMidiTrack (projectContext)))
            routing.setDescriptionText ("What the MIDI inputs and the computer keyboard play goes to the track \"" + track->name
                                        + "\", the one selected last with an instrument.");
        else
            routing.setDescriptionText ("Live MIDI plays no track yet: it goes to the first track with an instrument.");
    };
}

void SettingsPanel::updateMidi()
{
    const auto inputs = engine.getMidiInputs();
    juce::StringArray ids;

    for (const auto& input : inputs)
        ids.add (input.id + "\n" + input.name);

    // Made again only when the inputs connected change.
    if (ids != shownMidiInputs)
    {
        shownMidiInputs = ids;
        std::erase_if (rows, [] (const std::unique_ptr<SettingRow>& row)
                       { return row->group == juce::String (midi) && row->getProperties().contains ("midiInput"); });

        // Before the computer keyboard's row, in the inputs' order.
        auto at = std::find_if (rows.begin(), rows.end(), [] (const auto& row) { return row->group == juce::String (midi); });

        for (const auto& input : inputs)
        {
            auto row =
                std::make_unique<SettingRow> (midi, input.name, "Plays into the track live MIDI plays, as the computer keyboard does.");
            row->getProperties().set ("midiInput", true);
            auto& button = row->makeSwitch();
            const auto id = input.id;
            button.onClick = [this, id, &button] { engine.setMidiInputOn (id, button.getToggleState()); };

            const auto isOn = [this, id]
            {
                for (const auto& each : engine.getMidiInputs())
                    if (each.id == id)
                        return each.isOn;

                return false;
            };

            row->refresh = [&button, isOn] { button.setToggleState (isOn(), juce::dontSendNotification); };
            row->isModified = [isOn] { return ! isOn(); }; // every MIDI input plays, until it is switched off
            row->reset = [this, id] { engine.setMidiInputOn (id, true); };
            row->onFocused = [this] (SettingRow& focusedRow) { scrollRowIntoView (focusedRow); };
            list->addAndMakeVisible (*row);
            at = std::next (rows.insert (at, std::move (row)));
        }
    }

    for (auto& row : rows)
        if (row->group == juce::String (midi) && row->title == "MIDI Inputs")
            row->setAvailable (inputs.empty());
}

//==============================================================================
void SettingsPanel::addPreferenceRows()
{
    // Recording: the options the transport's record menu switches, and the inputs' correction.
    addSwitch (recording, "Overdub", "Recording adds what you play to the clips already there; off, it takes their place.",
               "transport.overdub", Settings::recordOverdub.defaultValue);
    addSwitch (recording, "Quantize Recordings", "Moves the notes you record to the nearest sixteenth as they go in.",
               "transport.quantizeRecordings", Settings::recordQuantize.defaultValue);
    addSwitch (recording, "Count-In", "Plays a bar of clicks before recording starts from a stop, even with the metronome off.",
               "transport.countIn", Settings::recordCountIn.defaultValue);

    {
        auto& row = addRow (recording, "Recording Correction",
                            "Moves what the audio inputs record earlier by this much, after the delays the driver reports. Leave it at "
                            "0 ms unless recordings come out late with this audio interface.");
        auto field = std::make_unique<NumberField> ("Recording Correction", -100.0, 100.0, 0.1, 0.0);
        field->wheelNeedsFocus = true;
        auto& made = *field;
        made.textFromValueFunction = [] (double milliseconds) { return juce::String (milliseconds, 1) + " ms"; };
        made.valueFromTextFunction = [] (const juce::String& text) { return text.retainCharacters ("-0123456789.").getDoubleValue(); };
        made.updateText();
        made.onValueChange = [this, &made] { engine.setRecordingCorrection (made.getValue()); };
        row.refresh = [this, &made]
        {
            if (! made.isMouseButtonDown (true))
                made.setValue (engine.getRecordingCorrection(), juce::dontSendNotification);
        };
        row.isModified = [this]
        { return ! juce::exactlyEqual (engine.getRecordingCorrection(), Settings::recordingCorrection.defaultValue); };
        row.reset = [this]
        {
            engine.setRecordingCorrection (Settings::recordingCorrection.defaultValue);
            refreshAll();
        };
        row.setControl (std::move (field), SettingsTint::numberWidth);
    }

    // Appearance: the theme, as its commands set it, and the interface's size.
    if (commands.getRegistry().find (themeCommands[0]) != nullptr)
    {
        auto& row = addRow (appearance, "Theme", "Dark or light, high contrast, or as the operating system is set, changing when it does.");
        auto box = std::make_unique<juce::ComboBox>();
        auto& made = *box;
        made.addItemList (themeNames, 1);
        made.onChange = [this, &made]
        {
            const auto index = made.getSelectedItemIndex();

            if (juce::isPositiveAndBelow (index, themeCommands.size()))
                commands.invoke (themeCommands[index]);

            refreshAll();
        };
        row.refresh = [this, &made]
        { made.setSelectedItemIndex (juce::jmax (0, themeValues.indexOf (settings.get (Settings::theme))), juce::dontSendNotification); };
        row.isModified = [this] { return settings.get (Settings::theme) != Settings::theme.defaultValue; };
        row.reset = [this]
        {
            commands.invoke (themeCommands[themeValues.indexOf (Settings::theme.defaultValue)]);
            refreshAll();
        };
        row.setControl (std::move (box), SettingsTint::choiceWidth);
    }

    if (actions.setInterfaceScale != nullptr)
    {
        auto& row = addRow (appearance, "Interface Size",
                            "Makes everything in Phazite's windows bigger or smaller. View > Larger Interface and Smaller Interface step "
                            "through these sizes.");
        auto box = std::make_unique<juce::ComboBox>();
        auto& made = *box;

        for (size_t i = 0; i < interfaceScales.size(); ++i)
            made.addItem (juce::String (juce::roundToInt (interfaceScales[i] * 100.0f)) + " %", (int) i + 1);

        const auto nearest = [] (double scale)
        {
            size_t best = 0;

            for (size_t i = 1; i < interfaceScales.size(); ++i)
                if (std::abs (interfaceScales[i] - scale) < std::abs (interfaceScales[best] - scale))
                    best = i;

            return best;
        };

        made.onChange = [this, &made]
        {
            const auto index = made.getSelectedItemIndex();

            if (juce::isPositiveAndBelow (index, (int) interfaceScales.size()))
                actions.setInterfaceScale (interfaceScales[(size_t) index]);
        };
        row.refresh = [this, &made, nearest]
        { made.setSelectedItemIndex ((int) nearest (settings.get (Settings::interfaceScale)), juce::dontSendNotification); };
        row.isModified = [this, nearest]
        { return nearest (settings.get (Settings::interfaceScale)) != nearest (Settings::interfaceScale.defaultValue); };
        row.reset = [this] { actions.setInterfaceScale ((float) Settings::interfaceScale.defaultValue); };
        row.setControl (std::move (box), SettingsTint::choiceWidth);
    }

    // Help, editing, files and versions: what the View, File and Help menus switch.
    addSwitch (help, "Hover Help",
               "Shows what a control does when the pointer rests on it. The hint bar at the window's foot shows it either way.",
               "view.hoverHelp", Settings::hoverHelp.defaultValue);
    addSwitch (help, "Getting Started Tips",
               "Shows what to try next in the hint bar, such as playing the song or recording what you play, until you have done each.",
               "help.guide", Settings::guide.defaultValue);

    addSwitch (editing, "Scroll Continuously",
               "While the song plays, keeps the playhead in the middle and scrolls the song under it, rather than turning the page at "
               "the edge.",
               "view.scrollContinuously", Settings::scrollContinuously.defaultValue);

    {
        auto& row = addRow (editing, "Undo Steps",
                            "How many steps back each project's Undo can go, from 10 to 100,000, for the projects opened from now on. Each "
                            "step is kept whatever its size.");
        auto field = std::make_unique<NumberField> ("Undo Steps", 10.0, 100000.0, 10.0, Settings::undoSteps.defaultValue);
        field->wheelNeedsFocus = true;
        auto& made = *field;
        made.textFromValueFunction = [] (double steps) { return juce::String (juce::roundToInt (steps)); };
        made.valueFromTextFunction = [] (const juce::String& text) { return text.retainCharacters ("0123456789").getDoubleValue(); };
        made.updateText();
        made.onValueChange = [this, &made] { settings.set (Settings::undoSteps, (double) juce::roundToInt (made.getValue())); };
        row.refresh = [this, &made]
        {
            if (! made.isMouseButtonDown (true))
                made.setValue (settings.get (Settings::undoSteps), juce::dontSendNotification);
        };
        row.isModified = [this]
        { return juce::roundToInt (settings.get (Settings::undoSteps)) != juce::roundToInt (Settings::undoSteps.defaultValue); };
        row.reset = [this] { settings.set (Settings::undoSteps, Settings::undoSteps.defaultValue); };
        row.setControl (std::move (field), SettingsTint::numberWidth);
    }

    addSwitch (files, "Auto Save",
               "Saves the project a moment after each change, so that its changes show in the Versions view as they are made; a new "
               "project once its first save puts it somewhere.",
               "project.autoSave", Settings::autoSave.defaultValue);
    addSwitch (versions, "Bounce Each Version",
               "Bounces the song to audio as each version is saved, so that it can be played later against the song.", "version.bounceEach",
               Settings::versionBounce.defaultValue);
    addSwitch (versions, "Show Automatic Saves",
               "Lists the automatic saves in the history, between the versions: the song as it was at times while it had changes.",
               "version.showAutomatic", Settings::versionShowAutomatic.defaultValue);

    addSwitch (startup, "Startup Sound",
               "Plays Phazite's sound once it has opened: a quick blip with echoes, at the browser's preview volume.",
               "options.startupSound", Settings::startupSound.defaultValue);
    addSwitch (updates, "Check for Updates When Phazite Opens",
               "Off until you turn it on. Each time Phazite opens, it then asks its releases on GitHub for one small file, to see "
               "whether a newer version is out.",
               "options.updateCheck", Settings::updateCheck.defaultValue);
}

//==============================================================================
void SettingsPanel::refreshAll()
{
    updateAudio();
    updateMidi();

    for (auto& row : rows)
    {
        row->update();
    }

    layOutList();
}

void SettingsPanel::settingChanged (const juce::String&)
{
    refreshAll();
}

void SettingsPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAll();
}

void SettingsPanel::timerCallback()
{
    // The delay depends on the song's devices, and live MIDI on its selection; and what a menu switched without telling the
    // settings (an option the engine keeps) shows here within a second. The list is laid out again where a description changed.
    const auto describe = [this]
    {
        juce::String all;

        for (auto& row : rows)
            all << row->getDescriptionText() << '\n';

        return all;
    };

    const auto before = describe();
    updateAudio();

    for (auto& row : rows)
        row->update();

    if (describe() != before)
        layOutList();
}

void SettingsPanel::search (const juce::String& text)
{
    if (searchField.getText() != text)
        searchField.setText (text, false);

    searchWords = getWords (text);

    if (onShortcuts)
    {
        onShortcuts = false;
        shortcutPage->setVisible (false);
        listView->setVisible (true);
    }

    layOutList();
    listView->setViewPosition (0, 0);
    markGroupInView();
}

void SettingsPanel::showGroup (const juce::String& group)
{
    onShortcuts = group == juce::String (shortcuts);
    shortcutPage->setVisible (onShortcuts);
    listView->setVisible (! onShortcuts);

    if (onShortcuts)
    {
        noResults.setVisible (false);
        contents->mark (shortcuts);
        return;
    }

    // A group the search hides: the search goes.
    if (getGroupTop (group) < 0 && ! searchWords.isEmpty())
        search ({});

    layOutList();
    clickedGroup = group;
    listView->setViewPosition (0, juce::jmax (0, getGroupTop (group)));
    markGroupInView();
}

juce::StringArray SettingsPanel::getListedGroups() const
{
    juce::StringArray listed;

    for (int i = 0; i < groups.size(); ++i)
        if (titles[i]->isVisible())
            listed.add (groups[i]);

    return listed;
}

int SettingsPanel::getCount (const juce::String& group) const
{
    return contents->getCount (group);
}

juce::String SettingsPanel::getMarkedGroup() const
{
    return contents->getMarked();
}

juce::Rectangle<int> SettingsPanel::getContentsEntry (const juce::String& group) const
{
    if (auto* entry = contents->find (group); entry != nullptr && entry->isVisible())
        return getLocalArea (entry, entry->getLocalBounds());

    return {};
}

int SettingsPanel::getGroupTop (const juce::String& group) const
{
    const auto index = groups.indexOf (group);
    return index >= 0 && titles[index]->isVisible() ? titles[index]->getY() : -1;
}

SettingRow* SettingsPanel::findRow (const juce::String& title) const
{
    for (const auto& row : rows)
        if (row->title == title)
            return row.get();

    return nullptr;
}

juce::Array<SettingRow*> SettingsPanel::getShownRows() const
{
    juce::Array<SettingRow*> shown;

    for (const auto& row : rows)
        if (row->isVisible())
            shown.add (row.get());

    return shown;
}

bool SettingsPanel::showsNoResults() const
{
    return noResults.isVisible();
}

void SettingsPanel::layOutList()
{
    const auto width = juce::jmax (0, listView->getWidth() - listView->getScrollBarThickness());
    auto y = 0;
    auto found = 0;

    for (int i = 0; i < groups.size(); ++i)
    {
        auto count = 0;
        auto top = y + GroupTitle::getTitleHeight();

        for (auto& row : rows)
        {
            if (row->group != groups[i])
                continue;

            const auto shown = row->isAvailable() && row->matches (searchWords);
            row->setVisible (shown);

            if (! shown)
                continue;

            const auto height = row->getHeightForWidth (width);
            row->setBounds (0, top, width, height);
            top += height;
            ++count;
        }

        titles[i]->setVisible (count > 0);
        contents->show (groups[i], count > 0, searchWords.isEmpty() ? 0 : count);

        if (count > 0)
        {
            titles[i]->setBounds (0, y, width, GroupTitle::getTitleHeight());
            y = top;
        }

        found += count;
    }

    list->setSize (width, y);
    contents->resized();

    const auto none = ! searchWords.isEmpty() && found == 0;
    noResults.setVisible (none && ! onShortcuts);
    countLabel.setText (searchWords.isEmpty() ? juce::String()
                        : found == 0          ? juce::String ("No Settings Found")
                        : found == 1          ? juce::String ("1 Setting Found")
                                              : juce::String (found) + " Settings Found",
                        juce::dontSendNotification);
    markGroupInView();
}

void SettingsPanel::markGroupInView()
{
    if (contents == nullptr || listView == nullptr)
        return;

    if (onShortcuts)
    {
        contents->mark (shortcuts);
        return;
    }

    const auto top = listView->getViewPositionY();
    const auto atEnd = top >= list->getHeight() - listView->getViewHeight() - 1;
    juce::String inView;

    for (int i = 0; i < groups.size(); ++i)
        if (titles[i]->isVisible() && (inView.isEmpty() || titles[i]->getY() <= top + 1))
            inView = groups[i];

    // The last group clicked, while the list, at its end, can't bring its title to the top.
    if (! atEnd || getGroupTop (clickedGroup) < 0)
        clickedGroup = {};

    contents->mark (clickedGroup.isNotEmpty() ? clickedGroup : inView);
}

void SettingsPanel::scrollRowIntoView (SettingRow& row)
{
    const auto top = listView->getViewPositionY();
    const auto height = listView->getViewHeight();

    if (row.getY() < top)
        listView->setViewPosition (0, row.getY());
    else if (row.getBottom() > top + height)
        listView->setViewPosition (0, juce::jmin (row.getY(), row.getBottom() - height));
}

void SettingsPanel::paint (juce::Graphics& g)
{
    // On the editor's background, as VS Code's Settings editor; a line under the search.
    const auto& p = getPalette (*this);
    g.fillAll (p.background);
    g.setColour (p.border);
    g.fillRect (SettingsTint::sideInset, searchField.getBottom() + SettingsTint::lineGap, getWidth() - 2 * SettingsTint::sideInset, 1);
}

void SettingsPanel::resized()
{
    const auto width = getWidth();
    searchField.setBounds (SettingsTint::sideInset, SettingsTint::headerTop, width - 2 * SettingsTint::sideInset,
                           SettingsTint::searchHeight);
    countLabel.setBounds (searchField.getBounds().withTrimmedRight (Space::s).withTrimmedLeft (searchField.getWidth() / 2));

    const auto line = searchField.getBottom() + SettingsTint::lineGap;
    const auto body = line + 1 + SettingsTint::bodyTop;
    noResults.setBounds (SettingsTint::sideInset, line + 1 + SettingsTint::noResultsTop, width - 2 * SettingsTint::sideInset,
                         SettingsTint::lineHeight);
    contents->setBounds (SettingsTint::sideInset, body, SettingsTint::tocWidth, juce::jmax (0, getHeight() - body));

    const auto right =
        juce::Rectangle<int> (SettingsTint::sideInset + SettingsTint::tocWidth, body,
                              juce::jmax (0, width - SettingsTint::sideInset - SettingsTint::tocWidth), juce::jmax (0, getHeight() - body));
    listView->setBounds (right);
    shortcutPage->setBounds (right);
    layOutList();
}

//==============================================================================
ShortcutSettings::ShortcutSettings (CommandBridge& commands, const ComputerKeyboard& computerKeyboard,
                                    std::function<void (const juce::String&)> chooseKeymap)
    : editor (commands, computerKeyboard)
{
    setTitle ("Shortcuts");
    keymapLabel.setText ("Keymap", juce::dontSendNotification);
    keymapLabel.setFont (getNameFont());
    keymapLabel.setBorderSize ({});
    setThemeColour (keymapLabel, juce::Label::textColourId, &Palette::heading);

    const auto keymaps = Keymap::getBuiltIn();

    for (int i = 0; i < keymaps.size(); ++i)
        keymapBox.addItem (keymaps[i].name, i + 1);

    keymapBox.setTitle ("Keymap");
    keymapBox.setDescription ("Which app's shortcuts to start from. Your own changes are kept for each keymap.");
    keymapBox.setText (commands.getKeymap().name, juce::dontSendNotification);
    keymapBox.setTooltip ("Which app's shortcuts to start from. Your own changes are kept for each keymap.");
    keymapBox.onChange = [this, choose = std::move (chooseKeymap)]
    {
        if (choose != nullptr)
            choose (keymapBox.getText());
    };

    addAndMakeVisible (keymapLabel);
    addAndMakeVisible (keymapBox);
    addAndMakeVisible (editor);
}

void ShortcutSettings::paint (juce::Graphics& g)
{
    g.fillAll (getPalette (*this).background);
}

void ShortcutSettings::resized()
{
    // In the settings' list's place, its sides where the rows' contents are.
    auto area =
        getLocalBounds().withTrimmedLeft (SettingsTint::rowInset).withTrimmedRight (SettingsTint::sideInset).withTrimmedBottom (Space::l);
    auto top = area.removeFromTop (SettingsTint::controlHeight);
    keymapLabel.setBounds (top.removeFromLeft (ShortcutsTint::keymapLabelWidth));
    keymapBox.setBounds (top.withWidth (juce::jmin (top.getWidth(), ShortcutsTint::keymapBoxWidth)));
    area.removeFromTop (Space::m);
    editor.setBounds (area);
}
} // namespace phazite::ui
