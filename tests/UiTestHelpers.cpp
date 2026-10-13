// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UiTestHelpers.h"

// juce_events' own function, declared the same way in juce_MessageManager.cpp, whose dispatch loops call it
// (on Windows and Linux; macOS will need another way): delivers the next message on the system queue, or
// returns false if there is none and returnIfNoPendingMessages is set.
namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
} // namespace juce::detail

namespace phazite::test
{
void dispatchPendingMessages()
{
    // With a limit, in case something posts a new message each time it gets one.
    for (int i = 0; i < 10000 && juce::detail::dispatchNextMessageOnSystemQueue (true); ++i)
    {
    }

    juce::Timer::callPendingTimersSynchronously();
}

juce::Component* findComponentWithTitle (juce::Component& parent, const juce::String& title)
{
    if (parent.getTitle() == title)
        return &parent;

    for (auto* child : parent.getChildren())
        if (auto* found = findComponentWithTitle (*child, title))
            return found;

    return nullptr;
}

//==============================================================================
namespace
{
    // A window without a native one behind it: it is always showing and focused, and paints nothing itself.
    class HeadlessPeer final : public juce::ComponentPeer
    {
    public:
        explicit HeadlessPeer (juce::Component& c)
            : juce::ComponentPeer (c, 0)
        {
        }

        void* getNativeHandle() const override { return nullptr; }
        void setVisible (bool) override {}
        void setTitle (const juce::String&) override {}
        void setBounds (const juce::Rectangle<int>& newBounds, bool) override { bounds = newBounds; }
        juce::Rectangle<int> getBounds() const override { return bounds; }
        juce::Point<float> localToGlobal (juce::Point<float> p) override { return p + bounds.getPosition().toFloat(); }
        juce::Point<float> globalToLocal (juce::Point<float> p) override { return p - bounds.getPosition().toFloat(); }
        void setMinimised (bool) override {}
        bool isMinimised() const override { return false; }
        bool isShowing() const override { return true; }
        void setFullScreen (bool) override {}
        bool isFullScreen() const override { return false; }
        void setIcon (const juce::Image&) override {}
        bool contains (juce::Point<int> p, bool) const override { return bounds.withZeroOrigin().contains (p); }
        OptionalBorderSize getFrameSizeIfPresent() const override { return OptionalBorderSize (juce::BorderSize<int>()); }

        // Deprecated on Linux in favour of the one above, but still to be implemented.
        JUCE_BEGIN_IGNORE_DEPRECATION_WARNINGS
        juce::BorderSize<int> getFrameSize() const override { return {}; }
        JUCE_END_IGNORE_DEPRECATION_WARNINGS

        bool setAlwaysOnTop (bool) override { return true; }
        void toFront (bool) override {}
        void toBehind (juce::ComponentPeer*) override {}
        bool isFocused() const override { return true; }
        void grabFocus() override {}
        void repaint (const juce::Rectangle<int>&) override {}
        void performAnyPendingRepaintsNow() override {}
        void setAlpha (float) override {}
        juce::StringArray getAvailableRenderingEngines() override { return { "Software Renderer" }; }
        void textInputRequired (juce::Point<int>, juce::TextInputTarget&) override {}

    private:
        juce::Rectangle<int> bounds;
    };
} // namespace

TestWindow::TestWindow (juce::Component& content, int width, int height)
{
    setTitle ("Test window");
    setOpaque (true);
    setBounds (0, 0, width, height);
    addAndMakeVisible (content);
    content.setBounds (getLocalBounds());
    setVisible (true);
    addToDesktop (0);
}

TestWindow::~TestWindow()
{
    removeFromDesktop();
}

juce::ComponentPeer* TestWindow::createNewPeer (int, void*)
{
    return new HeadlessPeer (*this);
}

void TestWindow::pressKey (const juce::KeyPress& key)
{
    if (auto* peer = getPeer())
        peer->handleKeyPress (key);
}

void TestWindow::releaseKeys()
{
    if (auto* peer = getPeer())
        peer->handleKeyUpOrDown (false);
}

juce::Image TestWindow::paintEverything()
{
    return createComponentSnapshot (getLocalBounds());
}

void TestWindow::resized()
{
    if (auto* content = getChildComponent (0))
        content->setBounds (getLocalBounds());
}

void checkChildrenApart (juce::Component& parent)
{
    CAPTURE (parent.getTitle().toStdString());
    juce::Array<juce::Component*> shown;

    for (auto* child : parent.getChildren())
        if (child->isVisible() && ! child->getBounds().isEmpty())
            shown.add (child);

    for (int i = 0; i < shown.size(); ++i)
    {
        CAPTURE (shown[i]->getTitle().toStdString());
        CHECK (parent.getLocalBounds().contains (shown[i]->getBounds()));

        for (int j = i + 1; j < shown.size(); ++j)
        {
            CAPTURE (shown[j]->getTitle().toStdString());
            CHECK_FALSE (shown[i]->getBounds().intersects (shown[j]->getBounds()));
        }
    }
}

//==============================================================================
Mouse::Mouse (juce::Component& c)
    : relativeTo (c)
{
}

void Mouse::send (juce::Point<int> where, juce::ModifierKeys mods)
{
    auto* peer = relativeTo.getPeer();
    jassert (peer != nullptr); // the component isn't in a TestWindow

    if (peer == nullptr)
        return;

    position = where;
    time += 20; // milliseconds after the event before

    // As the platform code does before it passes an event on.
    juce::ModifierKeys::currentModifiers = mods;
    peer->handleMouseEvent (juce::MouseInputSource::InputSourceType::mouse,
                            peer->getComponent().getLocalPoint (&relativeTo, where).toFloat(), mods,
                            juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, time);
}

void Mouse::press (juce::Point<int> where, juce::ModifierKeys mods)
{
    // A press outside the window would reach nothing (drags may leave it).
    if (auto* peer = relativeTo.getPeer())
        CHECK_MESSAGE (peer->getComponent().getLocalBounds().contains (peer->getComponent().getLocalPoint (&relativeTo, where)),
                       "the mouse is pressed outside the test window");

    // The pointer gets there first, as a real one does.
    keys = mods.withoutMouseButtons();
    send (where, keys);
    button = mods.isAnyMouseButtonDown() ? mods.withOnlyMouseButtons() : juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
    send (where, keys.withFlags (button.getRawFlags()));
}

void Mouse::dragTo (juce::Point<int> where)
{
    send (where, keys.withFlags (button.getRawFlags()));
}

void Mouse::release()
{
    button = {};
    send (position, keys);
    keys = {};
    juce::ModifierKeys::currentModifiers = {};
}

void Mouse::click (juce::Point<int> where, juce::ModifierKeys mods)
{
    press (where, mods);
    release();
    pause();
}

void Mouse::doubleClick (juce::Point<int> where, juce::ModifierKeys mods)
{
    press (where, mods);
    release();
    press (where, mods);
    release();
    pause();
}

void Mouse::moveTo (juce::Point<int> where)
{
    send (where, keys);
}

void Mouse::wheel (juce::Point<int> where, float deltaY, juce::ModifierKeys mods)
{
    auto* peer = relativeTo.getPeer();
    jassert (peer != nullptr);

    if (peer == nullptr)
        return;

    position = where;
    time += 20;
    juce::ModifierKeys::currentModifiers = mods.withoutMouseButtons();

    juce::MouseWheelDetails details {};
    details.deltaY = deltaY;
    peer->handleMouseWheel (juce::MouseInputSource::InputSourceType::mouse,
                            peer->getComponent().getLocalPoint (&relativeTo, where).toFloat(), time, details);
    juce::ModifierKeys::currentModifiers = {};
}

void Mouse::pause()
{
    time += juce::MouseEvent::getDoubleClickTimeout() + 100;
}
} // namespace phazite::test
