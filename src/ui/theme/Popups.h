// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
// What pops up over the main window (menus, tooltips, the command search and the pickers) on frosted glass, as VS Code's
// popups are with its Modern UI (docs/vscode-look.md): what is behind a popup shows through the popup's own colour, which
// covers it at Popup::frostTint, blurred (Popup::frostBlur). For that these popups open inside the main window, as VS
// Code's are drawn inside its own. Popups that open on the desktop, from Phazite's other windows, are solid; so is every
// popup in the high contrast theme and where the operating system is set to reduce transparency, as in VS Code
// (LookAndFeel::isFrosted()).

// True where the operating system is set to reduce transparency: on Windows, "Transparency effects" turned off.
bool isTransparencyReducedBySystem();

// Marks a window whose popups open inside it: the main window. Another window's open on the desktop: a plug-in's editor,
// for one, is a window of the plug-in's helper inside Phazite's, which would cover what Phazite draws over it.
void setPopupsInside (juce::Component& window);

// The window that a popup opens inside: the marked window a component is in, or the one at a point of the screen; null
// for a popup on the desktop.
juce::Component* findWindowForPopups (const juce::Component&);
juce::Component* findWindowForPopups (juce::Point<int> screenPosition);

// What is behind a component in its parent, as the parent paints an area of its own there (an area which may reach beyond
// the component): the parent's own paint and its children's that come before the component, at so many pixels a unit, in
// a software image of that area.
juce::Image paintBehind (const juce::Component& front, juce::Rectangle<int> area, float scale);

// An ARGB image a whole number of times smaller, each of its pixels the average of those it covers.
juce::Image shrink (const juce::Image&, int factor);

// An ARGB image blurred in place, as a Gaussian of this standard deviation in its pixels blurs it: three box blurs each
// way, as browsers blur, the pixels at its edges repeated beyond them.
void blur (juce::Image&, float standardDeviation);

// The frosted glass behind a popup inside a window: a component in the popup's parent, just behind it, that shows what was
// behind the popup as it opened (or last moved). It is sharp at the popup's corners and blurred inside the rounded shape
// that the popup draws its colour in (Shape::rounded: a menu, a tooltip). Or it is sharp everywhere, for a popup that
// draws the blur inside its own outline with paintBlurred() (Shape::outlined: a call-out, which draws its shadow too).
// What changes behind it later, a meter or the playhead, wouldn't show through the tint. The glass is opaque, so that what
// is behind doesn't repaint with the popup. It takes no clicks and isn't announced. It follows the popup's place,
// visibility and order, and is deleted with it. It shows while the popup's look and feel frosts popups.
class FrostedGlass final : public juce::Component,
                           private juce::ComponentListener
{
public:
    enum class Shape
    {
        rounded,
        outlined
    };

    // Glass behind a popup inside a window (it must have a parent), owned by the popup: with the drop shadow that the
    // popup's window would have had on the desktop (a menu's, a tooltip's), if asked for.
    static FrostedGlass& putBehind (juce::Component& popup, Shape, bool withShadow);
    static FrostedGlass* findBehind (const juce::Component& popup);

    // The blurred image, inside a shape in the popup's coordinates.
    void paintBlurred (juce::Graphics&, const juce::Path& shape) const;

    // What was behind the popup: sharp, the popup's size at the screen's resolution; blurred, a little beyond the popup on
    // every side (getBlurredArea(), in the glass's coordinates), at a fraction of that resolution (Popup::frostResolution).
    const juce::Image& getSharp() const noexcept { return sharp; }
    const juce::Image& getBlurred() const noexcept { return blurred; }
    juce::Rectangle<int> getBlurredArea() const noexcept { return blurredArea; }

    void paint (juce::Graphics&) override;

private:
    FrostedGlass (juce::Component& popup, Shape);
    ~FrostedGlass() override;

    juce::Component& popup;
    const Shape shape;
    std::unique_ptr<juce::DropShadower> shadower;
    juce::Image sharp, blurred;
    juce::Rectangle<int> blurredArea;

    bool isWanted() const; // the popup's look and feel frosts popups
    void follow();         // the popup's bounds, visibility and order
    void take();           // what is behind the popup, again

    void componentMovedOrResized (juce::Component&, bool wasMoved, bool wasResized) override;
    void componentVisibilityChanged (juce::Component&) override;
    void componentBroughtToFront (juce::Component&) override;
    void componentBeingDeleted (juce::Component&) override;

    JUCE_DECLARE_NON_COPYABLE (FrostedGlass)
};

// The keys of a menu inside a window: the menu's own, and no others. JUCE gives a key that a menu doesn't use to the
// menu's parent, the window, whose shortcuts and computer keyboard would take it; a menu on the desktop has no parent.
void keepKeysInMenu (juce::Component& menuWindow);

// Phazite's tooltips, as VS Code's hovers. There are two windows of tips. One is inside the main window, on frosted glass,
// for what is in it. The other is on the desktop, solid, for the other windows (a plug-in's, a dialog's), and leaves the
// main window's tips to the first.
class Tooltips final : public juce::TooltipWindow
{
public:
    Tooltips (juce::Component& mainWindow, bool inside);

    juce::String getTipFor (juce::Component&) override;

private:
    juce::Component& mainWindow;
    const bool inside;
    juce::String tip; // what getTipFor() said last, which is what JUCE shows

    void paint (juce::Graphics&) override;

    JUCE_DECLARE_NON_COPYABLE (Tooltips)
};
} // namespace phazite::ui
