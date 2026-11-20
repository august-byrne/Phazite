// SPDX-License-Identifier: AGPL-3.0-or-later
// A card over the whole main window, as VS Code's modal editor, which its Settings open in (docs/vscode-look.md, "Settings";
// ModalTint): the window, its title bar and window buttons included, under black at 50 %, and in its middle a card with a
// header (an icon, a title, Maximize and Close) over the content it is given. Its size follows the window's: 80 % of its width
// and of its height under the title bar, at most 1400 by 900, or maximized (Maximize, or a double-click on the header), the
// window less a margin; it can't be moved or resized by hand.
//
// It is two components side by side in its parent, which it covers (it places both as its parent's size changes): behind, its
// backdrop, see-through (the black at 50 %, the card's shadow, and the card where its rounded corners are: the rows at its top
// that they cut into, its foot as tall as their radius, and its edge down its sides); in front, this component, opaque, the
// rest of the card inside its edge (the header's lower part with its buttons, which reach higher than the corners' radius,
// and the content under it). JUCE paints nothing behind an opaque component that is a sibling in front, so that a repaint in
// the card (a row's wash, the list's scroll, a caret's blink, a button's hover) paints nothing of the window behind it. The
// content is opaque too and paints its own background, the editor's: square, so it stops where the corners' curve starts.
//
// While it shows this component is modal (JUCE's modal state, as a dialog is), and the dialog screen readers find, named by
// its title, with every control of the card in it and nothing behind reachable; a press on the backdrop (the dimmed window
// around the card) closes it and reaches nothing behind. The keys stay in it: Tab and Shift+Tab go round its controls; Escape
// that none of them takes closes it; the window's close key (Alt+F4) does what closing the window does (Quit); and every other
// key that none takes goes to passKey (the few commands that run over it: KeyDispatcher::keyPressedOverModal()), never to the
// window's shortcuts or the computer keyboard. As another of Phazite's windows is clicked meanwhile, the main window comes
// forward, as a dialog does, the keys where they were in the card.
#pragma once

#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace phazite::ui
{
class ModalCard : public juce::Component
{
public:
    struct Options
    {
        juce::String title;         // in its header, and what screen readers call the dialog
        Icon icon = Icon::settings; // before the title
        int titleBarHeight = 0;     // of the window's own title bar under it (Windows'), which the card keeps clear of
        std::function<bool (const juce::KeyPress&)> passKey; // a key no control took: whether a command ran
        std::function<void (bool isKeyDown)> keysChanged;    // a key went down or up, which the commands' keys hear
        std::function<void()> onClose;                       // Close, Escape or a press around it: the owner takes it away
        std::function<void()> closeWindow;                   // the window's close key (Alt+F4), which no control took
    };

    ModalCard (std::unique_ptr<juce::Component> content, Options);
    ~ModalCard() override;

    // Modal from now on, with the keyboard focus in the content (where it puts it first: the settings' search). And, as its
    // owner takes it away, modal no more, the focus given back to where it was before (if that still shows).
    void open();
    void finish();

    // Asks its owner to take it away (Options::onClose), as Close does.
    void close();

    // The window it is in to the front and active, the keyboard focus left on the control of the card that had it last: as
    // another of Phazite's windows is clicked while it shows, or Phazite is started again (app/Main.cpp).
    void bringWindowForward();

    // Its backdrop and it in front of the other children of its parent (AppShell::keepOverlaysInFront()).
    void keepInFront();

    juce::Component& getContent() noexcept { return *content; }
    const juce::String& getCardTitle() const noexcept { return options.title; }

    bool isMaximized() const noexcept { return maximized; }
    void setMaximized (bool);

    // Where the whole card is in its parent (this component is the card less its edge and where its corners are), and where it
    // is for an area so big with a title bar so tall at its top, at its default size or maximized.
    juce::Rectangle<int> getCardBounds() const;
    static juce::Rectangle<int> getCardBounds (juce::Rectangle<int> area, int titleBarHeight, bool maximized);

    // Its parts, for tests.
    juce::Component& getBackdrop() noexcept;
    juce::Button& getMaximizeButton() noexcept { return maximizeButton; }
    juce::Button& getCloseButton() noexcept { return closeButton; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    void parentSizeChanged() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    void inputAttemptWhenModal() override;
    bool canModalEventBeSentToComponent (const juce::Component*) override;
    std::unique_ptr<juce::ComponentTraverser> createKeyboardFocusTraverser() override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    class Backdrop;
    class FocusTraverser;

    static constexpr int edge = 1; // the card's border, inside its bounds

    const Options options;
    std::unique_ptr<juce::Component> content;
    std::unique_ptr<Backdrop> backdrop;
    TintedIcon titleIcon;
    IconButton maximizeButton { "Maximize", Icon::maximizePanel }, closeButton { "Close", Icon::close };
    juce::Component::SafePointer<juce::Component> focusBefore;
    juce::Image shadow; // the shadow of a card just big enough for its corners and a pixel between them, drawn once
    bool maximized = false;

    static int getCornerRows();                                                       // at its top, which its corners cut into
    static int getFootRows();                                                         // at its foot, from where its corners start
    static juce::Rectangle<int> getHeader (juce::Rectangle<int> card);                // in the card's coordinates as given
    static juce::Rectangle<int> getButton (juce::Rectangle<int> card, int fromRight); // Close (0), then Maximize (1)
    juce::Rectangle<int> getCardHere() const;                                         // the whole card in this component's coordinates
    void place();                                                                     // it and its backdrop, in its parent
    void paintCard (juce::Graphics&, juce::Rectangle<int> card);                      // its body, header and edge, the card where given
    void paintShadow (juce::Graphics&, juce::Rectangle<int> card);
    void updateMaximizeButton();
};

// Whether a component is a modal card: the main window's modal component then, but no dialog with a question to finish first
// (a quit doesn't wait for it, app/Main.cpp).
bool isModalCard (const juce::Component*);
} // namespace phazite::ui
