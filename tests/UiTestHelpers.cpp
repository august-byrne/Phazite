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
namespace
{
    // Dispatches what is waiting on the system queue (on Windows, window messages, JUCE's wake-ups among them) and what
    // that posts in turn, and returns how many it dispatched. With a limit, in case something posts a new message each
    // time it gets one.
    int dispatchSystemQueue()
    {
        int count = 0;

        while (count < 10000 && juce::detail::dispatchNextMessageOnSystemQueue (true))
            ++count;

        return count;
    }
} // namespace

void dispatchPendingMessages()
{
    // On Windows, JUCE wakes the message thread for its queue with one window message, which the thread that finds the
    // queue empty posts just after adding its message (juce_Messaging_windows.cpp). A message posted here while another
    // thread (JUCE's timer thread, say) is between those two steps gets no wake-up of its own, so "no window message
    // waiting" doesn't mean "nothing to deliver": CI once saw a click in the playlist miss the selection's change
    // message that way. So each pass also queues a marker behind everything else, and dispatches until the marker has
    // run, waiting for a wake-up that is on its way. The passes go on until one in which the marker came alone: nothing
    // was waiting before it, its own wake-up delivered it, and nothing came after. (Linux's queue has no such gap; there
    // the passes cost little.)
    for (int pass = 0; pass < 100; ++pass)
    {
        const auto before = dispatchSystemQueue();
        auto markerRan = std::make_shared<bool> (false);

        if (! juce::MessageManager::callAsync ([markerRan] { *markerRan = true; }))
            break; // the message loop is shutting down

        const auto start = juce::Time::getMillisecondCounter();
        int tries = 0;

        while (! *markerRan)
        {
            ++tries;

            if (juce::detail::dispatchNextMessageOnSystemQueue (true))
                continue;

            if (juce::Time::getMillisecondCounter() - start > 5000u)
            {
                FAIL_CHECK ("A message waiting for the message thread wasn't delivered within 5 seconds");
                return;
            }

            juce::Thread::sleep (1); // its wake-up is on its way from another thread
        }

        if (const auto after = dispatchSystemQueue(); before == 0 && tries == 1 && after == 0)
            break;
    }

    juce::Timer::callPendingTimersSynchronously();
}

bool dispatchMessagesUntil (const std::function<bool()>& condition, int timeoutMilliseconds)
{
    const auto start = juce::Time::getMillisecondCounter();

    for (;;)
    {
        dispatchPendingMessages();

        if (condition())
            return true;

        if (juce::Time::getMillisecondCounter() - start > (juce::uint32) timeoutMilliseconds)
            return false;

        juce::Thread::sleep (5);
    }
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

const juce::PopupMenu::Item* findMenuItem (const juce::PopupMenu& menu, const std::vector<juce::String>& path)
{
    const auto* current = &menu;

    for (size_t level = 0; level < path.size(); ++level)
    {
        const juce::PopupMenu::Item* found = nullptr;

        for (juce::PopupMenu::MenuItemIterator it (*current); it.next() && found == nullptr;)
            if (it.getItem().text == path[level])
                found = &it.getItem();

        if (found == nullptr || level + 1 == path.size())
            return found;

        current = found->subMenu.get();

        if (current == nullptr)
            return nullptr;
    }

    return nullptr;
}

bool runMenuItem (const juce::PopupMenu& menu, const std::vector<juce::String>& path)
{
    const auto* item = findMenuItem (menu, path);

    if (item == nullptr || ! item->isEnabled || item->action == nullptr)
        return false;

    item->action();
    return true;
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

        // Keeps the base class's other overloads visible, as JUCE's own peers do (GCC's -Woverloaded-virtual).
        using juce::ComponentPeer::globalToLocal;
        using juce::ComponentPeer::localToGlobal;
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

    // A drag's auto-repeat (juce::Desktop::beginDragAutoRepeat(), which the editors start so that they go on scrolling while
    // the mouse rests past an edge) repeats it at the real pointer's position, wherever that is on the screen, while a test
    // waits with the button down. Tests move the mouse themselves.
    juce::Desktop::getInstance().beginDragAutoRepeat (0);
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

//==============================================================================
#if JUCE_WINDOWS
// Windows' header comes last, so that its macros reach nothing above. It is included as JUCE includes it
// (juce_BasicNativeHeaders.h): without min and max macros, and without warnings of its own.
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
JUCE_BEGIN_IGNORE_WARNINGS_LEVEL_MSVC (0, 4201)
#include <windows.h>
JUCE_END_IGNORE_WARNINGS_MSVC

namespace phazite::test
{
namespace
{
    constexpr UINT jucesWakeUp = WM_USER + 123; // InternalMessageQueue::customMessageID, juce_Messaging_windows.cpp
} // namespace

std::jthread queueWithoutWakeUp (int delayMilliseconds)
{
    // Into the empty queue, this message's post sends a wake-up (if another thread's message got in first, that
    // thread's comes instead, a moment later)...
    dispatchPendingMessages();
    juce::MessageManager::callAsync ([] {});

    // ...which is taken back...
    MSG wakeUp {};
    const auto start = juce::Time::getMillisecondCounter();

    while (! PeekMessageW (&wakeUp, nullptr, jucesWakeUp, jucesWakeUp, PM_REMOVE))
    {
        REQUIRE_MESSAGE (juce::Time::getMillisecondCounter() - start < 5000u,
                         "no WM_USER + 123 for JUCE's queue: if juce_Messaging_windows.cpp changed it, change this");
        juce::Thread::sleep (1);
    }

    // ...and posted again later, from a thread of its own. Until then no post sends one: the queue isn't empty.
    return std::jthread (
        [window = wakeUp.hwnd, delayMilliseconds]
        {
            juce::Thread::sleep (delayMilliseconds);
            PostMessageW (window, jucesWakeUp, 0, 0);
        });
}
} // namespace phazite::test
#endif
