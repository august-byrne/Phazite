// SPDX-License-Identifier: AGPL-3.0-or-later
// The tests' message loop (UiTestHelpers.h): dispatchPendingMessages() delivers every message posted before it is
// called, and what those post in turn, also while JUCE's wake-up for them is late (on Windows such a late wake-up once
// made a playlist test click before the selection's change message came).
#include "UiTestHelpers.h"

TEST_CASE ("The tests' message loop delivers the messages waiting, and what they post in turn")
{
    auto delivered = std::make_shared<int> (0);

    juce::MessageManager::callAsync (
        [delivered]
        {
            ++*delivered;
            juce::MessageManager::callAsync ([count = delivered] { ++*count; });
        });

    phazite::test::dispatchPendingMessages();
    CHECK (*delivered == 2);
}

#if JUCE_WINDOWS
TEST_CASE ("A message posted while another thread's wake-up is on its way is delivered")
{
    // Another thread has queued a message and not yet posted its wake-up, so a message posted now gets none.
    auto lateWakeUp = phazite::test::queueWithoutWakeUp (100);
    auto delivered = std::make_shared<bool> (false);
    juce::MessageManager::callAsync ([delivered] { *delivered = true; });

    phazite::test::dispatchPendingMessages();
    CHECK (*delivered);
}

TEST_CASE ("What a delivered message posts is delivered too, while another wake-up is late")
{
    // The first message waits for a late wake-up; when it runs, it posts a second one behind another late one.
    auto lateForFirst = phazite::test::queueWithoutWakeUp (100);
    auto lateForSecond = std::make_shared<std::jthread>();
    auto secondDelivered = std::make_shared<bool> (false);

    juce::MessageManager::callAsync (
        [lateForSecond, secondDelivered]
        {
            *lateForSecond = phazite::test::queueWithoutWakeUp (100);
            juce::MessageManager::callAsync ([flag = secondDelivered] { *flag = true; });
        });

    phazite::test::dispatchPendingMessages();
    CHECK (*secondDelivered);
}
#endif
