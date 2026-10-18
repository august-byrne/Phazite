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
    // and none is while the tests run.)
    void pressKey (const juce::KeyPress&);
    void releaseKeys();

    // Paints the window into an image, as the screen would.
    juce::Image paintEverything();

    void resized() override; // the content fills the window
    juce::ComponentPeer* createNewPeer (int styleFlags, void* nativeWindowToAttachTo) override;
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

private:
    juce::Component& relativeTo;
    juce::ModifierKeys keys, button; // the modifier keys held, and the mouse button
    juce::Point<int> position;
    juce::int64 time = juce::Time::currentTimeMillis();

    void send (juce::Point<int>, juce::ModifierKeys);
};
} // namespace phazite::test
