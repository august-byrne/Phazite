// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ModalCard.h"
#include "ui/theme/LookAndFeel.h"

#include <cmath>

namespace phazite::ui
{
// Behind the card, covering its parent: the window dimmed, the card's shadow, and the card where the opaque part doesn't cover
// it (the rows its rounded corners cut into, at its top and foot, and its edge down its sides). A press on it outside the card
// closes the card; a double-click on the card's header rows maximizes it, as one on the rest of the header does.
class ModalCard::Backdrop : public juce::Component
{
public:
    explicit Backdrop (ModalCard& cardToShow)
        : card (cardToShow)
    {
        setWantsKeyboardFocus (false);

        // A press on it leaves the keys in the card: with none of the card's controls holding them, the focus it would ask
        // for goes to its parent, which hands it to what is behind the card.
        setMouseClickGrabsKeyboardFocus (false);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (ModalTint::backdrop));
        const auto area = card.getCardBounds() - getPosition();

        // The shadow and the card only where a repaint reaches them: most repaints under the backdrop are the window's own,
        // away from the card (a meter, the playhead).
        if (g.clipRegionIntersects (area.expanded (ModalTint::shadowRadius)))
            card.paintShadow (g, area);

        if (g.clipRegionIntersects (area))
            card.paintCard (g, area);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // A press on the window around the card, as VS Code's modal takes one on its backdrop.
        if (! (card.getCardBounds() - getPosition()).contains (e.getPosition()))
            card.close(); // which deletes this
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (getHeader (card.getCardBounds() - getPosition()).contains (e.getPosition()))
            card.setMaximized (! card.isMaximized());
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        // Nothing for a screen reader: the window behind is out of reach while the card shows, and the card is the dialog.
        return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::ignored);
    }

private:
    ModalCard& card;
};

// The card's controls in their order; and where the card itself is given the keys while none of its controls has them (as its
// window comes forward again: bringWindowForward()), the control that had them as the window went to the back, as the
// window's peer keeps it, rather than its first.
class ModalCard::FocusTraverser : public juce::KeyboardFocusTraverser
{
public:
    explicit FocusTraverser (ModalCard& cardToFocus)
        : card (cardToFocus)
    {
    }

    juce::Component* getDefaultComponent (juce::Component* parent) override
    {
        if (auto* peer = card.getPeer(); peer != nullptr && parent != nullptr)
            if (auto* last = peer->getLastFocusedSubcomponent();
                parent->isParentOf (last) && last->isShowing() && last->isEnabled() && last->getWantsKeyboardFocus())
                return last;

        return juce::KeyboardFocusTraverser::getDefaultComponent (parent);
    }

private:
    ModalCard& card;
};

//==============================================================================
ModalCard::ModalCard (std::unique_ptr<juce::Component> contentToShow, Options optionsToUse)
    : options (std::move (optionsToUse)),
      content (std::move (contentToShow)),
      titleIcon (options.icon)
{
    setTitle (options.title);
    setOpaque (true);
    setWantsKeyboardFocus (false);
    setFocusContainerType (FocusContainerType::keyboardFocusContainer); // Tab goes round its controls

    backdrop = std::make_unique<Backdrop> (*this);
    backdrop->setVisible (true);

    for (auto* button : { &maximizeButton, &closeButton })
    {
        button->setToolbarStyle (true);
        addAndMakeVisible (*button);
    }

    maximizeButton.onClick = [this] { setMaximized (! maximized); };
    closeButton.setTooltip ("Close");
    closeButton.onClick = [this] { close(); };
    updateMaximizeButton();

    addAndMakeVisible (*content);
}

ModalCard::~ModalCard()
{
    if (isCurrentlyModal (false))
        exitModalState (0);
}

void ModalCard::open()
{
    focusBefore = juce::Component::getCurrentlyFocusedComponent();
    enterModalState (false, nullptr, false);
    content->grabKeyboardFocus(); // where the content puts it first
}

void ModalCard::finish()
{
    if (isCurrentlyModal (false))
        exitModalState (0);

    if (auto* before = focusBefore.getComponent(); before != nullptr && before->isShowing())
        before->grabKeyboardFocus();
}

void ModalCard::close()
{
    if (options.onClose != nullptr)
        options.onClose(); // which deletes this
}

void ModalCard::bringWindowForward()
{
    // The window active: as the system gives it the focus, JUCE gives the keys back to the control that had them as it went to
    // the back (ComponentPeer::handleFocusGain()); where they aren't back yet, this takes them, and its traverser finds that
    // control (FocusTraverser). (Component::toFront (true) on this gave them to its first control.)
    if (auto* peer = getPeer())
        peer->toFront (true);

    if (! hasKeyboardFocus (true))
        grabKeyboardFocus();
}

void ModalCard::keepInFront()
{
    backdrop->toFront (false);
    toFront (false);
}

juce::Component& ModalCard::getBackdrop() noexcept
{
    return *backdrop;
}

void ModalCard::setMaximized (bool shouldBeMaximized)
{
    if (maximized == shouldBeMaximized)
        return;

    maximized = shouldBeMaximized;
    updateMaximizeButton();
    place();
}

void ModalCard::updateMaximizeButton()
{
    maximizeButton.setIcon (maximized ? Icon::restorePanels : Icon::maximizePanel);
    maximizeButton.setTitle (maximized ? "Restore" : "Maximize");
    maximizeButton.setTooltip (maximized ? "Restore" : "Maximize");
}

juce::Rectangle<int> ModalCard::getCardBounds (juce::Rectangle<int> area, int titleBarHeight, bool isMaximized)
{
    // As VS Code lays out its modal editor (modalEditorPart.ts): centred in the whole window, never over its title bar.
    const auto width = area.getWidth(), height = area.getHeight();
    const auto available = juce::jmax (0, height - titleBarHeight);
    int cardWidth = 0, cardHeight = 0;

    if (isMaximized)
    {
        cardWidth = juce::jmax (0, width - ModalTint::maximizedMargin);
        cardHeight = juce::jmax (0, available - juce::jmax (titleBarHeight, ModalTint::maximizedMargin));
    }
    else
    {
        cardWidth = juce::jmin (juce::roundToInt ((float) width * ModalTint::share), ModalTint::maximumWidth, width);
        cardHeight = juce::jmin (juce::roundToInt ((float) available * ModalTint::share), ModalTint::maximumHeight, available);
    }

    const auto x = (width - cardWidth) / 2;
    const auto y = juce::jmax (titleBarHeight, (height - cardHeight) / 2);
    return { area.getX() + x, area.getY() + y, cardWidth, cardHeight };
}

juce::Rectangle<int> ModalCard::getCardBounds() const
{
    return getBounds().withTop (getY() - getCornerRows()).withBottom (getBottom() + getFootRows()).expanded (edge, 0);
}

juce::Rectangle<int> ModalCard::getCardHere() const
{
    return getLocalBounds().withTop (-getCornerRows()).withBottom (getHeight() + getFootRows()).expanded (edge, 0);
}

int ModalCard::getCornerRows()
{
    // The first row from the card's top at which its rounded corner leaves every pixel inside the edge whole: above it the
    // corner's curve passes inside the edge's column, and what is behind the card shows there in part. Below it this paints
    // every pixel as the whole card is painted (paintCard()), as the header is the card's own.
    const auto radius = (double) cardCornerRadius, inside = radius - edge;
    return (int) std::ceil (radius - std::sqrt (radius * radius - inside * inside));
}

int ModalCard::getFootRows()
{
    // At the foot the content meets the corners, which, painting its own background square, it would cover: it stops where the
    // corners' curve starts, and the backdrop paints the card from there.
    return (int) std::ceil (cardCornerRadius);
}

juce::Rectangle<int> ModalCard::getHeader (juce::Rectangle<int> card)
{
    return { card.getX() + edge, card.getY() + edge, card.getWidth() - 2 * edge, ModalTint::headerHeight };
}

juce::Rectangle<int> ModalCard::getButton (juce::Rectangle<int> card, int fromRight)
{
    const auto header = getHeader (card);
    const auto right = header.getRight() - ModalTint::headerRight - fromRight * (ModalTint::buttonSize + ModalTint::buttonGap);
    return { right - ModalTint::buttonSize, header.getY() + (header.getHeight() - ModalTint::buttonSize) / 2, ModalTint::buttonSize,
             ModalTint::buttonSize };
}

void ModalCard::place()
{
    auto* parent = getParentComponent();

    if (parent == nullptr)
        return;

    const auto area = parent->getLocalBounds();
    backdrop->setBounds (area);
    setBounds (getCardBounds (area, options.titleBarHeight, maximized)
                   .withTrimmedTop (getCornerRows())
                   .withTrimmedBottom (getFootRows())
                   .reduced (edge, 0));
    backdrop->repaint(); // the card's shadow and corners where it is now
}

void ModalCard::paintCard (juce::Graphics& g, juce::Rectangle<int> card)
{
    const auto& p = getPalette (*this);
    const auto bounds = card.toFloat();
    const auto header = getHeader (card);
    const auto inner = cardCornerRadius - (float) edge;

    // The body, on the editor's background; the header on the title bar's colour, its top corners rounded inside the edge,
    // with its line under it.
    g.setColour (p.background);
    g.fillRoundedRectangle (bounds, cardCornerRadius);

    juce::Path top;
    top.addRoundedRectangle ((float) header.getX(), (float) header.getY(), (float) header.getWidth(), (float) header.getHeight(), inner,
                             inner, true, true, false, false);
    g.setColour (p.surface);
    g.fillPath (top);
    g.setColour (p.panelBorder);
    g.fillRect (header.getX(), header.getBottom(), header.getWidth(), 1);

    // The icon and the title, in the title bar's text colour.
    auto area = header.withTrimmedLeft (ModalTint::headerLeft).withRight (getButton (card, 1).getX() - ModalTint::buttonGap);
    const auto iconArea = area.removeFromLeft (juce::roundToInt (ModalTint::iconSize)).toFloat();
    titleIcon.draw (g, iconArea.withSizeKeepingCentre (ModalTint::iconSize, ModalTint::iconSize), p.textMuted);
    area.removeFromLeft (ModalTint::iconGap);
    g.setColour (p.textMuted);
    g.setFont (juce::FontOptions (TextSize::small, juce::Font::bold)); // Inter's SemiBold (LookAndFeel)
    g.drawText (options.title, area, juce::Justification::centredLeft, true);

    g.setColour (p.border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), cardCornerRadius, 1.0f);
}

void ModalCard::paintShadow (juce::Graphics& g, juce::Rectangle<int> card)
{
    // Drawn once into a picture of a card just big enough for its corners' blur and one pixel between them, then painted from
    // it: its corners as they are, the pixel between them stretched along the card's sides (a straight side's shadow is the
    // same all along it). Under the card itself, nothing: the card covers it.
    const auto radius = ModalTint::shadowRadius;
    const auto corner = 2 * radius + (int) std::ceil (cardCornerRadius); // a corner of the picture: as far as the blur reaches
    const auto outer = card.expanded (radius);

    if (outer.getWidth() <= 2 * corner || outer.getHeight() <= 2 * corner)
    {
        juce::Path shape;
        shape.addRoundedRectangle (card.toFloat(), cardCornerRadius);
        juce::DropShadow (juce::Colour (ModalTint::shadow), radius, {}).drawForPath (g, shape);
        return;
    }

    if (shadow.isNull())
    {
        const auto side = 2 * corner + 1;
        shadow = juce::Image (juce::Image::ARGB, side, side, true);
        juce::Graphics picture (shadow);
        juce::Path shape;
        shape.addRoundedRectangle ((float) radius, (float) radius, (float) (side - 2 * radius), (float) (side - 2 * radius),
                                   cardCornerRadius);
        juce::DropShadow (juce::Colour (ModalTint::shadow), radius, {}).drawForPath (picture, shape);
    }

    const auto across = outer.getWidth() - 2 * corner, down = outer.getHeight() - 2 * corner;
    const auto left = outer.getX(), top = outer.getY(), right = outer.getRight() - corner, bottom = outer.getBottom() - corner;
    const auto draw = [&g, this] (int x, int y, int width, int height, int fromX, int fromY, int fromWidth, int fromHeight)
    { g.drawImage (shadow, x, y, width, height, fromX, fromY, fromWidth, fromHeight); };

    draw (left, top, corner, corner, 0, 0, corner, corner);
    draw (right, top, corner, corner, corner + 1, 0, corner, corner);
    draw (left, bottom, corner, corner, 0, corner + 1, corner, corner);
    draw (right, bottom, corner, corner, corner + 1, corner + 1, corner, corner);
    draw (left + corner, top, across, corner, corner, 0, 1, corner);
    draw (left + corner, bottom, across, corner, corner, corner + 1, 1, corner);
    draw (left, top + corner, corner, down, 0, corner, corner, 1);
    draw (right, top + corner, corner, down, corner + 1, corner, corner, 1);
}

void ModalCard::paint (juce::Graphics& g)
{
    // Every pixel of it: it is inside the card's rounded edge, where the card paints the body over all of it first.
    paintCard (g, getCardHere());
}

void ModalCard::resized()
{
    const auto card = getCardHere();
    closeButton.setBounds (getButton (card, 0));
    maximizeButton.setBounds (getButton (card, 1));

    // The content under the header's line, inside the edge, down to where the corners start.
    content->setBounds (getLocalBounds().withTop (getHeader (card).getBottom() + 1));
}

void ModalCard::parentHierarchyChanged()
{
    // The backdrop goes where this goes, just behind it.
    auto* parent = getParentComponent();

    if (backdrop->getParentComponent() != parent)
    {
        if (auto* before = backdrop->getParentComponent())
            before->removeChildComponent (backdrop.get());

        if (parent != nullptr)
            parent->addChildComponent (*backdrop, parent->getIndexOfChildComponent (this));
    }

    place();
}

void ModalCard::parentSizeChanged()
{
    place(); // the card follows the window's size
}

void ModalCard::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.eventComponent == this && getHeader (getCardHere()).contains (e.getPosition()))
        setMaximized (! maximized);
}

bool ModalCard::keyPressed (const juce::KeyPress& key)
{
    // Here, no control of the card took the key.
    if (key == juce::KeyPress::escapeKey)
    {
        close();
        return true;
    }

    // Tab and Shift+Tab go round its controls, here rather than after the window's keys have been asked (where JUCE moves the
    // focus with a Tab no one took): from its last to its first, as it is a focus container; with none of them focused (the
    // focus went with a control that hid, or a press gave it away), to its first or its last.
    if (key == juce::KeyPress::tabKey || key == juce::KeyPress (juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0))
    {
        const auto forwards = ! key.getModifiers().isShiftDown();

        if (auto* focused = juce::Component::getCurrentlyFocusedComponent(); focused != nullptr && isParentOf (focused))
        {
            focused->moveKeyboardFocusToSibling (forwards);
        }
        else
        {
            const auto controls = juce::KeyboardFocusTraverser().getAllComponents (this);

            if (! controls.empty())
                (forwards ? controls.front() : controls.back())->grabKeyboardFocus();
        }

        return true;
    }

    // The window's own close key, which JUCE keeps from the system while a component is modal: Quit, as closing the window is,
    // and as VS Code's modal lets its window close. (On macOS, closing the window reaches MainWindow::closeButtonPressed().)
#if ! JUCE_MAC
    if (key == juce::KeyPress (juce::KeyPress::F4Key, juce::ModifierKeys::altModifier, 0) && options.closeWindow != nullptr)
    {
        options.closeWindow();
        return true;
    }
#endif

    if (options.passKey != nullptr)
        options.passKey (key);

    return true; // the window's other keys wait
}

bool ModalCard::keyStateChanged (bool isKeyDown)
{
    if (options.keysChanged != nullptr)
        options.keysChanged (isKeyDown);

    return true;
}

void ModalCard::inputAttemptWhenModal()
{
    // Another of Phazite's windows clicked while it shows: the main window forward, as a dialog comes, without JUCE's beep.
    bringWindowForward();
}

bool ModalCard::canModalEventBeSentToComponent (const juce::Component* target)
{
    // Its backdrop, the dimmed window around it, takes a press: to close it.
    return target == backdrop.get();
}

std::unique_ptr<juce::ComponentTraverser> ModalCard::createKeyboardFocusTraverser()
{
    return std::make_unique<FocusTraverser> (*this);
}

std::unique_ptr<juce::AccessibilityHandler> ModalCard::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::dialogWindow);
}

bool isModalCard (const juce::Component* component)
{
    return dynamic_cast<const ModalCard*> (component) != nullptr;
}
} // namespace phazite::ui
