// SPDX-License-Identifier: AGPL-3.0-or-later
// Where the tests are when a test case takes too long. A test file that reaches ctest's limit is ended with nothing said
// of where it was, and doctest names a test case only once it has ended; so where the environment asks
// (PHAZITE_TEST_STALL_SECONDS, as CI's and the nightly workflow's jobs do, whose tests no PC of a contributor's need be able
// to run), a thread of this watches how long the test case has run, and after so many seconds, and again after each so many
// more, writes its name and, on Windows and Linux, the stack of the thread that runs the tests: the same stack each time
// is a hang, and stacks that move on are slow work.
#pragma once

#include <memory>

namespace phazite::test
{
class StallReport
{
public:
    // The seconds the environment asks for, or none: nothing is watched then.
    static int getSecondsAsked();

    // Made on the thread that runs the tests, which is the one it writes the stack of.
    explicit StallReport (int seconds);
    ~StallReport();

    // As doctest starts and ends each test case; the name is doctest's, kept as long as the run. The time between two
    // test cases, and after the last, is watched too.
    void testCaseStarted (const char* name);
    void testCaseEnded();

private:
    struct Watch;
    const std::unique_ptr<Watch> watch;
};
} // namespace phazite::test
