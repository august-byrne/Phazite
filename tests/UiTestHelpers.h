// SPDX-License-Identifier: AGPL-3.0-or-later
// Helpers for tests that drive Phazite's components (phazite_ui) as the app does: a window that isn't on any
// screen, which JUCE sends the tests' mouse and key events through as it would through a real one (to the
// component under the pointer or with the keyboard focus, mouse and key listeners included), and the
// messages the app's message loop delivers. Nothing appears on the screen, so the tests also run where there
// is none (the Linux builds in CI).
#pragma once

#include "TestHelpers.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <thread>
#include <vector>

namespace phazite::test
{
// Delivers the messages that are waiting (AsyncUpdater callbacks, change messages, posted calls) and what they post in
// turn, and runs the timers that are due, as the app's message loop would between two events. Every message posted
// before the call is delivered, even while another thread's wake-up for JUCE's queue is late (it waits for that, and
// fails the test after 5 seconds). A message that another thread posts may come later, even after that thread's work
// looks done: wait for its effect with dispatchMessagesUntil().
void dispatchPendingMessages();

// Delivers messages and runs the timers that are due until the condition holds, for a message another thread posts
// (SoundPack's thread posts its change message after it sets the state that waitForSoundPack() sees). False if the
// condition still doesn't hold after timeoutMilliseconds.
bool dispatchMessagesUntil (const std::function<bool()>& condition, int timeoutMilliseconds = 5000);

// Delivers what waits on the system queue now, and what that posts in turn, without waiting for more: for a loop that waits
// as the app's does (host/ThreadCpu.h: waitForMessages()). How many it delivered.
int dispatchWaitingMessages();

#if JUCE_WINDOWS
// Leaves JUCE's message queue as another thread leaves it halfway through posting (JUCE's timer thread, say): a message
// queued (one that does nothing), and the window message that wakes the message thread for it (WM_USER + 123,
// juce_Messaging_windows.cpp) not posted yet. Until it is, a message posted here gets no wake-up of its own: JUCE posts
// one only into an empty queue. A thread of its own posts the wake-up after delayMilliseconds; the std::jthread
// returned waits for that when it goes.
std::jthread queueWithoutWakeUp (int delayMilliseconds);
#endif

// The first component in parent's tree (parent included) with this title, or nullptr.
juce::Component* findComponentWithTitle (juce::Component& parent, const juce::String& title);

// A menu's item, found by the texts of the submenus that lead to it and its own, or nullptr.
const juce::PopupMenu::Item* findMenuItem (const juce::PopupMenu&, const std::vector<juce::String>& path);

// Runs a menu's item, found as findMenuItem() finds it, if it is enabled and has an action. Returns whether it did.
bool runMenuItem (const juce::PopupMenu&, const std::vector<juce::String>& path);

//==============================================================================
// The window, holding the component under test, which fills it.
class TestWindow : public juce::Component
{
public:
    TestWindow (juce::Component& content, int width, int height);
    ~TestWindow() override;

    // A key going down (again for each repeat while it is held), and the keys being let go. The window's key
    // listeners hear about them, as the app's main window's do. (JUCE asks the computer which keys are down,
    // and none is while the tests run.) A key typed goes down before it is pressed, as JUCE hears it on Windows: that
    // the key state changed (keyGoesDown()), then the key (pressKey()).
    void pressKey (const juce::KeyPress&);
    void keyGoesDown();
    void releaseKeys();

    // Paints the window into an image, as the screen would: by the platform's renderer (Direct2D on Windows), or by another,
    // such as JUCE's software renderer, which Linux's windows paint with.
    juce::Image paintEverything (const juce::ImageType& renderer = juce::NativeImageType());

    void paint (juce::Graphics&) override; // a background where the content paints nothing

    // What the window was asked to repaint since the last call (or since it opened), in its own coordinates: where
    // components called repaint(), or moved.
    juce::RectangleList<int> takeRepaintedArea();

    void resized() override; // the content fills the window
    juce::ComponentPeer* createNewPeer (const juce::DesktopWindowOptions&) override;
};

// The components of a type in a tree (the root included).
template <typename ComponentType>
juce::Array<ComponentType*> findAll (juce::Component& root)
{
    juce::Array<ComponentType*> found;

    if (auto* c = dynamic_cast<ComponentType*> (&root))
        found.add (c);

    for (auto* child : root.getChildren())
        found.addArray (findAll<ComponentType> (*child));

    return found;
}

// Checks that a component's visible children are inside it and don't overlap.
void checkChildrenApart (juce::Component&);

// While it lives, JUCE has a display, as a screen gives, where the machine has none (Linux CI's), for what JUCE places
// or scales by its display: a drag's image let go, which a test mouse's release fades out through a copy that JUCE's
// animator scales (with no display it read nothing and crashed, 2026-11-01), or a menu. Only then: otherwise the machine
// is one without a screen, as it is (Desktop::isHeadless()), to what asks, such as code that turns the screen saver off,
// which without a screen crashes. JUCE workaround T3 (docs/juce-workarounds.md).
class ScopedDisplay
{
public:
    ScopedDisplay();
    ~ScopedDisplay();

private:
    juce::Displays& displays = const_cast<juce::Displays&> (juce::Desktop::getInstance().getDisplays());
    bool added = false;

    JUCE_DECLARE_NON_COPYABLE (ScopedDisplay)
};

// The mouse, in a test window. Positions are in the coordinates of a component in the window.
class Mouse
{
public:
    explicit Mouse (juce::Component& relativeTo);

    // With the left button, unless the modifiers name another one. Modifier keys stay held until the release.
    void press (juce::Point<int>, juce::ModifierKeys = {});
    void dragTo (juce::Point<int>);
    void release();

    void click (juce::Point<int>, juce::ModifierKeys = {});
    void doubleClick (juce::Point<int>, juce::ModifierKeys = {});
    void moveTo (juce::Point<int>);
    void wheel (juce::Point<int>, float deltaY, juce::ModifierKeys = {}); // deltaY: as JUCE reports it

    // Lets time pass, long enough that the next click isn't part of a double-click with the last press (clicks and
    // double-clicks pause after themselves; a drag doesn't).
    void pause();

    // How far apart in time its events are (juce::MouseEvent::eventTime), in milliseconds: 20 unless set, as a mouse
    // reporting 50 times a second; a moving mouse reports up to a thousand.
    void setEventInterval (int milliseconds) noexcept { interval = milliseconds; }

private:
    juce::Component& relativeTo;
    juce::ModifierKeys keys, button; // the modifier keys held, and the mouse button
    juce::Point<int> position;
    juce::int64 time = juce::Time::currentTimeMillis();
    int interval = 20;

    void send (juce::Point<int>, juce::ModifierKeys);
};
} // namespace phazite::test
