// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UiTestHelpers.h"
#include "ui/theme/DesignTokens.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/theme/Popups.h"

using namespace phazite::ui;

namespace
{
// A component filled in one colour, or, with stripes, in vertical lines of black and white two pixels wide.
class Fill : public juce::Component
{
public:
    explicit Fill (juce::Colour c, bool s = false)
        : colour (c),
          stripes (s)
    {
        setOpaque (colour.isOpaque());
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colour);

        if (stripes)
        {
            g.setColour (juce::Colours::black);

            for (int x = 0; x < getWidth(); x += 4)
                g.fillRect (x, 0, 2, getHeight());
        }
    }

private:
    juce::Colour colour;
    bool stripes;
};

// The brightness of a pixel's red, 0 to 255.
int redAt (const juce::Image& image, int x, int y)
{
    return image.getPixelAt (x, y).getRed();
}

// How far apart the brightest and darkest pixels are along part of a row.
int spreadAlong (const juce::Image& image, int y, int fromX, int toX)
{
    auto low = 255, high = 0;

    for (int x = fromX; x < toX; ++x)
    {
        low = juce::jmin (low, redAt (image, x, y));
        high = juce::jmax (high, redAt (image, x, y));
    }

    return high - low;
}

// The window's children of a kind.
template <typename Kind>
int countChildren (const juce::Component& window)
{
    return (int) std::count_if (window.getChildren().begin(), window.getChildren().end(),
                                [] (const juce::Component* c) { return dynamic_cast<const Kind*> (c) != nullptr; });
}

juce::Component* findMenuWindow (const juce::Component& window)
{
    for (auto* child : window.getChildren())
        if (child->getName() == "menu")
            return child;

    return nullptr;
}
} // namespace

TEST_CASE ("Frosted glass blurs as a Gaussian of its standard deviation does, the image's edges repeated beyond them")
{
    // Black on the left half, white on the right: across the edge, the blur's cumulative curve, which reaches about 84 % a
    // standard deviation past the edge; the image's own edges keep their colours.
    juce::Image image (juce::Image::ARGB, 60, 8, true, juce::SoftwareImageType());
    {
        juce::Graphics g (image);
        g.fillAll (juce::Colours::black);
        g.setColour (juce::Colours::white);
        g.fillRect (30, 0, 30, 8);
    }

    blur (image, 4.0f);

    CHECK (redAt (image, 0, 4) == 0);
    CHECK (redAt (image, 59, 4) == 255);
    CHECK (redAt (image, 29, 4) == doctest::Approx (113).epsilon (0.15));
    CHECK (redAt (image, 30, 4) == doctest::Approx (142).epsilon (0.15));
    CHECK (redAt (image, 34, 4) == doctest::Approx (0.86 * 255).epsilon (0.08));
    CHECK (redAt (image, 25, 4) == doctest::Approx (0.14 * 255).epsilon (0.35));

    for (int x = 1; x < 60; ++x)
        CHECK (redAt (image, x, 4) >= redAt (image, x - 1, 4));

    // A colour all over stays as it is.
    juce::Image grey (juce::Image::ARGB, 20, 20, true, juce::SoftwareImageType());
    grey.clear (grey.getBounds(), juce::Colour (0xff808080));
    blur (grey, 3.0f);
    CHECK (grey.getPixelAt (0, 0) == juce::Colour (0xff808080));
    CHECK (grey.getPixelAt (10, 10) == juce::Colour (0xff808080));
}

TEST_CASE ("Shrinking an image makes each of its pixels the average of those it covers")
{
    // A checkerboard of single pixels, four times smaller: grey, the last row and column of a 9-pixel image averaging the
    // pixels there are.
    juce::Image board (juce::Image::ARGB, 9, 9, true, juce::SoftwareImageType());

    for (int y = 0; y < 9; ++y)
        for (int x = 0; x < 9; ++x)
            board.setPixelAt (x, y, (x + y) % 2 == 0 ? juce::Colours::white : juce::Colours::black);

    const auto small = shrink (board, 4);
    CHECK (small.getWidth() == 3);
    CHECK (small.getHeight() == 3);
    CHECK (redAt (small, 0, 0) == doctest::Approx (128).epsilon (0.02));
    CHECK (redAt (small, 1, 1) == doctest::Approx (128).epsilon (0.02));
    CHECK (redAt (small, 2, 2) == 255); // the one pixel at (8, 8), white
}

TEST_CASE ("What is behind a popup is what its parent paints there, the popup and what is in front of it left out")
{
    juce::Component parent;
    parent.setSize (100, 100);
    Fill behind (juce::Colours::red), popup (juce::Colours::green), above (juce::Colours::blue);
    behind.setBounds (parent.getLocalBounds());
    popup.setBounds (20, 20, 40, 30);
    above.setBounds (30, 30, 10, 10);

    for (auto* child : { &behind, &popup, &above })
        parent.addAndMakeVisible (child);

    // At twice the resolution, as on a screen at 200 %; beyond the parent, nothing.
    const auto image = paintBehind (popup, popup.getBounds(), 2.0f);
    CHECK (image.getWidth() == 80);
    CHECK (image.getHeight() == 60);
    CHECK (image.getPixelAt (0, 0) == juce::Colours::red);
    CHECK (image.getPixelAt (30, 30) == juce::Colours::red);
    CHECK (image.getPixelAt (79, 59) == juce::Colours::red);

    const auto beyond = paintBehind (popup, { 90, 90, 20, 20 }, 1.0f);
    CHECK (beyond.getPixelAt (5, 5) == juce::Colours::red);
    CHECK (beyond.getPixelAt (15, 15).isTransparent());
}

TEST_CASE ("The glass behind a popup follows it, is opaque, takes no clicks and goes with it; not in the high contrast theme")
{
    LookAndFeel lookAndFeel;
    Fill content (juce::Colours::red, true);
    phazite::test::TestWindow window (content, 400, 300);
    window.setLookAndFeel (&lookAndFeel);
    const auto before = window.getNumChildComponents();

    auto popup = std::make_unique<Fill> (juce::Colours::transparentBlack);
    popup->setBounds (50, 40, 120, 80);
    window.addAndMakeVisible (*popup);
    auto& glass = FrostedGlass::putBehind (*popup, FrostedGlass::Shape::rounded, true);

    CHECK (FrostedGlass::findBehind (*popup) == &glass);
    CHECK (&FrostedGlass::putBehind (*popup, FrostedGlass::Shape::rounded, true) == &glass); // one is enough
    CHECK (glass.getBounds() == popup->getBounds());
    CHECK (glass.isVisible());
    CHECK (glass.isOpaque());
    CHECK (window.getIndexOfChildComponent (&glass) < window.getIndexOfChildComponent (popup.get()));
    CHECK (window.getComponentAt (60, 50) == popup.get());

    bool clicksChildren = true, clicks = true;
    glass.getInterceptsMouseClicks (clicks, clicksChildren);
    CHECK_FALSE (clicks);

    // What was behind, sharp at the popup's size, and blurred a little beyond it; again where it moves.
    const auto scale = juce::Component::getApproximateScaleFactorForComponent (&glass);
    CHECK (glass.getSharp().getWidth() == juce::roundToInt (120 * scale));
    CHECK (glass.getBlurredArea().contains (glass.getLocalBounds()));
    popup->setBounds (100, 100, 80, 60);
    CHECK (glass.getBounds() == popup->getBounds());
    CHECK (glass.getSharp().getWidth() == juce::roundToInt (80 * scale));

    popup->setVisible (false);
    CHECK_FALSE (glass.isVisible());
    popup->setVisible (true);
    CHECK (glass.isVisible());

    // In front of something else, still just behind the popup.
    Fill other (juce::Colours::yellow);
    other.setBounds (0, 0, 10, 10);
    window.addAndMakeVisible (other);
    popup->toFront (false);
    CHECK (window.getIndexOfChildComponent (&glass) < window.getIndexOfChildComponent (popup.get()));
    window.removeChildComponent (&other);

    // Solid in the high contrast theme: no glass shows.
    lookAndFeel.setTheme (ThemeName::highContrast);
    popup->setBounds (100, 100, 90, 60);
    CHECK_FALSE (glass.isVisible());
    lookAndFeel.setTheme (ThemeName::dark);

    // Gone with the popup, its shadow too.
    popup = nullptr;
    CHECK (window.getNumChildComponents() == before);
    window.setLookAndFeel (nullptr);
}

TEST_CASE ("Through frosted glass, what is behind a popup shows blurred under the popup's tint")
{
    // Black and white lines behind; inside the popup's rounded shape (away from its corners), nearly the same grey all along.
    LookAndFeel lookAndFeel;
    Fill content (juce::Colours::white, true);
    phazite::test::TestWindow window (content, 300, 200);
    window.setLookAndFeel (&lookAndFeel);
    Fill popup (juce::Colours::transparentBlack);
    popup.setBounds (60, 40, 160, 100);
    window.addAndMakeVisible (popup);
    FrostedGlass::putBehind (popup, FrostedGlass::Shape::rounded, false);

    const auto painted = window.paintEverything();
    const auto scale = (float) painted.getWidth() / 300.0f;
    const auto y = juce::roundToInt (90 * scale);
    CHECK (spreadAlong (painted, y, juce::roundToInt (10 * scale), juce::roundToInt (50 * scale)) == 255);
    CHECK (spreadAlong (painted, y, juce::roundToInt (80 * scale), juce::roundToInt (200 * scale)) < 24);
    window.setLookAndFeel (nullptr);
}

TEST_CASE ("Popups are frosted at VS Code's tint, but solid in the high contrast theme and where the system reduces transparency")
{
    LookAndFeel lookAndFeel;
    CHECK (lookAndFeel.isFrosted());
    CHECK (lookAndFeel.getPopupAlpha() == doctest::Approx (Popup::frostTint));
    CHECK (lookAndFeel.findColour (juce::PopupMenu::backgroundColourId).getFloatAlpha()
           == doctest::Approx (Popup::frostTint).epsilon (0.01));

    lookAndFeel.setTransparencyReduced (true);
    CHECK_FALSE (lookAndFeel.isFrosted());
    CHECK (lookAndFeel.findColour (juce::PopupMenu::backgroundColourId).isOpaque());
    lookAndFeel.setTransparencyReduced (false);

    lookAndFeel.setTheme (ThemeName::highContrast);
    CHECK_FALSE (lookAndFeel.isFrosted());
    CHECK (lookAndFeel.getPopupAlpha() == doctest::Approx (1.0f));
    CHECK (lookAndFeel.findColour (juce::PopupMenu::backgroundColourId).isOpaque());
}

TEST_CASE ("A menu's background: its colour as a tint over the glass inside a window, solid on the desktop")
{
    LookAndFeel lookAndFeel;
    juce::Component parent;
    const auto centreAlpha = [&lookAndFeel] (const juce::PopupMenu::Options& options)
    {
        juce::Image image (juce::Image::ARGB, 100, 60, true, juce::SoftwareImageType());
        juce::Graphics g (image);
        lookAndFeel.drawPopupMenuBackgroundWithOptions (g, 100, 60, options);
        return image.getPixelAt (50, 30).getFloatAlpha();
    };

    CHECK (centreAlpha (juce::PopupMenu::Options().withParentComponent (&parent)) == doctest::Approx (Popup::frostTint).epsilon (0.02));
    CHECK (centreAlpha (juce::PopupMenu::Options()) == doctest::Approx (1.0f));
}

TEST_CASE ("Menus open inside the window that their popups open in, on frosted glass, and keep their keys")
{
    LookAndFeel lookAndFeel;
    Fill content (juce::Colours::white, true);
    phazite::test::TestWindow window (content, 600, 400);
    window.setLookAndFeel (&lookAndFeel);
    Fill target (juce::Colours::grey);
    target.setBounds (40, 40, 80, 24);
    content.addAndMakeVisible (target);

    // Where a menu opens: inside the window once it is marked; a submenu inside its menu's.
    const auto from = juce::PopupMenu::Options().withTargetComponent (&target);
    CHECK (lookAndFeel.getParentComponentForMenuOptions (from) == nullptr);
    setPopupsInside (window);
    CHECK (findWindowForPopups (target) == &window);
    CHECK (lookAndFeel.getParentComponentForMenuOptions (from) == &window);
    CHECK (lookAndFeel.getParentComponentForMenuOptions (from.withParentComponent (&content)) == &content);

    // Shown: the menu in the window over its glass. JUCE places a menu on its display, which the tests' machine may not
    // have (Linux CI's): one for the while.
    const phazite::test::ScopedDisplay display;
    juce::PopupMenu menu;
    menu.setLookAndFeel (&lookAndFeel);
    menu.addItem (1, "One");
    menu.addItem (2, "Two");
    menu.showMenuAsync (from);

    auto* menuWindow = findMenuWindow (window);
    REQUIRE (menuWindow != nullptr);
    CHECK (menuWindow->isVisible());
    const auto* glass = FrostedGlass::findBehind (*menuWindow);
    REQUIRE (glass != nullptr);
    CHECK (glass->isVisible());
    CHECK (glass->getBounds() == menuWindow->getBounds());

    // A key the menu doesn't use doesn't reach the window's shortcuts; Escape closes the menu, and its glass goes with it.
    struct Heard final : juce::KeyListener
    {
        int keys = 0;
        bool keyPressed (const juce::KeyPress&, juce::Component*) override { return ++keys > 0; }
    } heard;

    window.addKeyListener (&heard);
    window.pressKey (juce::KeyPress ('m'));
    window.releaseKeys();
    CHECK (heard.keys == 0);

    window.pressKey (juce::KeyPress (juce::KeyPress::escapeKey));
    window.releaseKeys();
    phazite::test::dispatchPendingMessages();
    CHECK (findMenuWindow (window) == nullptr);
    CHECK (countChildren<FrostedGlass> (window) == 0);

    // Without the menu, the window's shortcuts hear the key.
    window.pressKey (juce::KeyPress ('m'));
    window.releaseKeys();
    CHECK (heard.keys == 1);
    window.removeKeyListener (&heard);
    window.setLookAndFeel (nullptr);
}

TEST_CASE ("Tooltips: inside the main window on frosted glass; on the desktop, the main window's tips left to those")
{
    LookAndFeel lookAndFeel;
    Fill content (juce::Colours::white);
    phazite::test::TestWindow window (content, 300, 200);
    window.setLookAndFeel (&lookAndFeel);

    struct Tipped final : public juce::Component,
                          public juce::SettableTooltipClient
    {
    } inWindow, elsewhere;

    inWindow.setTooltip ("In the main window");
    elsewhere.setTooltip ("Somewhere else");
    content.addAndMakeVisible (inWindow);

    Tooltips inside (window, true), outside (window, false);
    CHECK (inside.getParentComponent() == &window);
    CHECK (FrostedGlass::findBehind (inside) != nullptr);
    CHECK (outside.getParentComponent() == nullptr);

    CHECK (inside.getTipFor (inWindow) == "In the main window");
    CHECK (outside.getTipFor (inWindow).isEmpty());
    CHECK (outside.getTipFor (elsewhere) == "Somewhere else");
    window.setLookAndFeel (nullptr);
}
