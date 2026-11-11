// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DesignTokens.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// Phazite's look: the design tokens applied to JUCE's components, and the Inter typeface; JUCE's controls drawn as VS Code's
// Modern UI draws its own (docs/vscode-look.md). While it lives (the app's, for the whole session), it keeps every one of
// JUCE's standard mouse pointers: JUCE keeps a pointer only while something holds it, so a view that switches between two (a
// clip's halves, select and move) would make one again at each switch, the hand's from an image (docs/performance-audit.md,
// section 8). JUCE workaround W15 (docs/juce-workarounds.md).
class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel(); // with Inter from fonts/Inter next to the app, where the build copies it

    // With Inter's files from that folder: the tests have none next to them, so the README's picture
    // (tests/ReadmePicture.cpp) takes the source tree's.
    explicit LookAndFeel (const juce::File& interFolder);

    void setTheme (ThemeName);
    ThemeName getTheme() const noexcept { return theme; }
    const Palette& getPalette() const noexcept { return palette; }

    // A font for numbers that change (tempo, time, levels): fixed-width digits, so they don't jitter.
    static juce::Font getNumberFont (float height);

    // True if Inter's files were found; otherwise the system font is used.
    bool hasInter() const noexcept { return regular != nullptr; }

    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    // Knobs, faders and buttons in the design tokens' colors, with a visible keyboard focus.
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float startAngle, float endAngle,
                           juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override;

    // A bar slider is a number field (ui::NumberField): a box with its value, in the number font, typed in after a click. Its
    // box fills it, as a button's or a combo box's does.
    juce::Label* createSliderTextBox (juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout (juce::Slider&) override;

    // Buttons as VS Code's (ButtonTint): one filled in the accent while it is on (or its colour is the accent, the main one),
    // else clear in a border; the hover wash under the pointer; the focus outline, the fill inside it.
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour, bool isHighlighted,
                               bool isDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isHighlighted, bool isDown) override;
    void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float width, float height, bool ticked, bool isEnabled,
                      bool isHighlighted, bool isDown) override;

    // Scroll bars as VS Code's: a rounded thumb without a track or buttons, lighter under the pointer and while dragged.
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isVertical, int thumbStart,
                        int thumbSize, bool isMouseOver, bool isMouseDown) override;
    int getDefaultScrollbarWidth() override { return ScrollTint::width; }
    bool areScrollbarButtonsVisible() override { return false; }

    // Fields and lists to pick from as VS Code's inputs and dropdowns: rounded, in the control border, outlined in the
    // focus colour while they have the keyboard focus.
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonWidth,
                       int buttonHeight, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    // What pops up over the window, as VS Code's: inside the main window on frosted glass (ui/theme/Popups.h), its colour a
    // tint over what is behind it, blurred; solid in the high contrast theme, where the operating system is set to reduce
    // transparency (setTransparencyReduced()), and on the desktop. Each rounded, outlined and over a soft shadow. A call-out
    // is frosted with FrostedGlass::Shape::outlined glass behind it (the command search, the pickers).
    bool isFrosted() const noexcept { return theme != ThemeName::highContrast && ! transparencyReduced; }
    void setTransparencyReduced (bool);
    float getPopupAlpha() const noexcept; // the popups' colour over their glass: Popup::frostTint, or 1 where not frosted
    void drawCallOutBoxBackground (juce::CallOutBox&, juce::Graphics&, const juce::Path&, juce::Image& cachedImage) override;
    float getCallOutBoxCornerSize (const juce::CallOutBox&) override { return cardCornerRadius; }
    std::unique_ptr<juce::DropShadower> createDropShadowerForComponent (juce::Component&) override;

    // Menus as VS Code's (Popup): inside the window that they are opened from, where it is marked for it
    // (findWindowForPopups()), over their glass and with their keys to themselves (keepKeysInMenu()); rounded items inset
    // from the menu's sides, the highlighted one in a wash of the focus colour outlined in it, a tick at the left of one
    // that is ticked, its shortcut fainter at the right.
    juce::Component* getParentComponentForMenuOptions (const juce::PopupMenu::Options&) override;
    void preparePopupMenuWindow (juce::Component&) override;
    void drawPopupMenuBackgroundWithOptions (juce::Graphics&, int width, int height, const juce::PopupMenu::Options&) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                            const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth,
                                    int& idealHeight) override;
    int getPopupMenuBorderSize() override { return Popup::itemInset; }
    juce::Font getPopupMenuFont() override;

    // Menu bars: each name lit under the pointer and while its menu is open; the title bar's (ui/shell/TitleBar.h), with
    // onBarProperty set, on the bar's own background, and others, such as a plug-in window's, on the raised surface.
    static constexpr const char* onBarProperty = "phaziteMenuBarOnBar";
    void drawMenuBarBackground (juce::Graphics&, int width, int height, bool isMouseOverBar, juce::MenuBarComponent&) override;
    void drawMenuBarItem (juce::Graphics&, int width, int height, int itemIndex, const juce::String& itemText, bool isMouseOverItem,
                          bool isMenuOpen, bool isMouseOverBar, juce::MenuBarComponent&) override;
    juce::Font getMenuBarFont (juce::MenuBarComponent&, int itemIndex, const juce::String& itemText) override;
    int getMenuBarItemWidth (juce::MenuBarComponent&, int itemIndex, const juce::String& itemText) override;

    // Tooltips as VS Code's hovers: rounded and outlined on the raised surface, solid, or as a tint over frosted glass (inside
    // the main window: ui::Tooltips, which paints them with paintTooltip()). Their windows aren't opaque, so that their
    // corners show what is behind.
    juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPosition,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
    void paintTooltip (juce::Graphics&, const juce::String& text, juce::Rectangle<float> bounds, bool frosted);

    // Tabs as VS Code's panels' (TabTint): pills, the one that shows filled, the one under the pointer washed, on the bar's own
    // background, with no line under them.
    int getTabButtonBestWidth (juce::TabBarButton&, int tabDepth) override;
    int getTabButtonOverlap (int) override { return 0; }
    void drawTabButton (juce::TabBarButton&, juce::Graphics&, bool isMouseOver, bool isMouseDown) override;
    juce::Font getTabButtonFont (juce::TabBarButton&, float height) override;
    void drawTabbedButtonBarBackground (juce::TabbedButtonBar&, juce::Graphics&) override {}
    void drawTabAreaBehindFrontButton (juce::TabbedButtonBar&, juce::Graphics&, int, int) override {}

private:
    ThemeName theme = ThemeName::dark;
    bool transparencyReduced = false;
    Palette palette;
    juce::Typeface::Ptr regular, medium, semiBold;
    std::vector<juce::MouseCursor> standardPointers;
};

// The palette of the LookAndFeel a component uses (the dark theme if that isn't Phazite's).
const Palette& getPalette (const juce::Component&);

// One of a component's colours in a role of the palette's (&Palette::textMuted, say), at that much of its alpha: set now, and
// again from the palette of each theme switched to later (refreshTheme()), unless something else set that colour since. A
// colour that comes straight from the palette needs no lookAndFeelChanged() of its component, then.
void setThemeColour (juce::Component&, int colourId, juce::Colour Palette::* role, float alpha = 1.0f);

// After the theme changed (LookAndFeel::setTheme()), in a window and everything in it: the colours set by role in the new
// palette's; the text already in each field in the field's text colour, which a juce::TextEditor otherwise keeps from when it
// was typed, and its placeholder in the muted one; then the look-and-feel change, which each component's lookAndFeelChanged()
// takes for the colours it works out itself (an attention tint, a track's ink).
void refreshTheme (juce::Component& window);

// A row of a list as VS Code's lists draw theirs: under the pointer, the hover wash; selected, the selection's wash (or, where
// the keyboard isn't in the list, the inactive selection's), rounded and inset from the list's sides (ListRow). The colour its
// text is drawn in: the bright text's on a selection.
juce::Colour drawListRow (juce::Graphics&, const juce::Component& list, juce::Rectangle<float> row, bool selected, bool hovered,
                          bool listHasFocus = true);

// A shortcut as VS Code's command search shows one, each key in a little box ("Ctrl", "Shift", "P"), in the middle of the area
// and ending at its right, its text in that colour: the width it took.
float drawKeyCaps (juce::Graphics&, const Palette&, const juce::String& shortcut, juce::Rectangle<float> area, juce::Colour text);

// A place, or a rectangle's edges, moved to the nearest of the screen's pixels at the context's scale, so that a shape filled
// there, or an outline drawn half its width inside it, isn't smeared across two pixels at its edges.
float snapToPixels (juce::Graphics&, float);
juce::Rectangle<float> snapToPixels (juce::Graphics&, juce::Rectangle<float>);
} // namespace phazite::ui
