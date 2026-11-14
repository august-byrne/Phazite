// SPDX-License-Identifier: AGPL-3.0-or-later
// The helper program that ships beside Phazite, PhaziteHost, started as Phazite starts it: the tests' plug-ins load in
// the tests' own helper (phazite_test_host), so this is the one place the shipped one runs. Phazite itself is started
// by a ctest entry of its own, PhaziteStarts (CMakeLists.txt), in its smoke test (`Phazite --smoke-test`, src/app/Main.cpp).
#include "TestHelpers.h"
#include "sandbox/client/Helper.h"

#include <atomic>

TEST_CASE ("PhaziteHost, the helper that ships beside Phazite, starts and answers in this version's protocol, and quits when asked")
{
    const juce::File program (PHAZITE_HOST_PROGRAM);
    REQUIRE (program.existsAsFile());

    {
        phazite::sandbox::Helper helper (program);
        std::atomic<bool> lost { false };
        helper.onLost = [&lost] { lost = true; };

        // Started, it answered the hello with this version's protocol (Helper::start()), and still runs.
        REQUIRE (helper.start (30000));
        CHECK (helper.isRunning());

        // It answers a request as well: the heartbeat, which its message thread answers.
        auto beat = helper.makeRequest (phazite::sandbox::MessageType::heartbeat);
        CHECK (helper.request (beat, 30000).has_value());
        CHECK_FALSE (lost.load());
    } // asked to quit, and ended if it doesn't soon
}
