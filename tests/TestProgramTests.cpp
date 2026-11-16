// SPDX-License-Identifier: AGPL-3.0-or-later
// The test program itself (TestMain.cpp), run again as a child of the tests: how it ends. ctest reads its exit code and its
// output, and the run fails on a JUCE object leaked as static objects go; on Windows it ends without the C runtime's
// teardown (known bug 182), which must keep all of that.
#include "TestHelpers.h"

#include <cstdlib>

namespace
{
constexpr const char* childVariable = "PHAZITE_TEST_PROGRAM_CHILD";

// A variable of the tests' environment, which the programs they start inherit, set while it lasts.
class EnvironmentVariable
{
public:
    EnvironmentVariable (const char* variableName, const char* value)
        : name (variableName)
    {
#if JUCE_WINDOWS
        _putenv_s (name, value);
#else
        setenv (name, value, 1);
#endif
    }

    ~EnvironmentVariable()
    {
#if JUCE_WINDOWS
        _putenv_s (name, "");
#else
        unsetenv (name);
#endif
    }

    EnvironmentVariable (const EnvironmentVariable&) = delete;
    EnvironmentVariable& operator= (const EnvironmentVariable&) = delete;

private:
    const char* name;
};

bool isChild (const char* what)
{
    return juce::SystemStats::getEnvironmentVariable (childVariable, {}) == what;
}

// The test program run with one test case of its own, as a child told what to do: its exit code and all it wrote.
struct ChildRun
{
    int exitCode = -1;
    juce::String output;
};

// With the exit marker on (TestMain.cpp: a line written if exit() goes on past where the program ends on Windows), and with
// the program ended as the runtime would end it, if asked.
ChildRun runChild (const char* what, const char* testCase, bool endAsEver = false)
{
    const EnvironmentVariable variable (childVariable, what);
    const EnvironmentVariable marker ("PHAZITE_TEST_EXIT_MARKER", "1");
    const EnvironmentVariable asEver ("PHAZITE_TEST_EXIT_AS_EVER", endAsEver ? "1" : "");
    const auto program = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName();
    juce::ChildProcess child;
    REQUIRE (child.start (juce::StringArray { program, juce::String ("--test-case=") + testCase },
                          juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr));
    ChildRun run;
    run.output = child.readAllProcessOutput(); // until it has ended
    REQUIRE (child.waitForProcessToFinish (phazite::test::hangTimeout));
    run.exitCode = (int) child.getExitCode();
    return run;
}

// The tests' own temporary folders in a folder: the child's goes in this program's.
int countTestFolders()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getNumberOfChildFiles (juce::File::findDirectories, "PhaziteTests *");
}
} // namespace

// The children's cases, which do nothing in any other run.
TEST_CASE ("The test program as a child: a case that passes (for the case after it)")
{
    if (isChild ("passes"))
        CHECK (true);
}

TEST_CASE ("The test program as a child: a case that fails (for the case after it)")
{
    if (isChild ("fails"))
        CHECK (false);
}

TEST_CASE ("The test program as a child: a case that leaks a JUCE object (for the case after it)")
{
    if (isChild ("leaks"))
        juce::ignoreUnused (new juce::XmlElement ("leaked")); // which JUCE's leak detector finds as static objects go
}

TEST_CASE ("The test program ends with its result and all its output, its temporary folder gone, and a leak fails it")
{
    const auto foldersBefore = countTestFolders();

    // All passed: 0, and doctest's last line written.
    const auto passed = runChild ("passes", "The test program as a child: a case that passes*");
    CHECK (passed.exitCode == 0);
    CHECK (passed.output.contains ("Status: SUCCESS!"));
    CHECK (countTestFolders() == foldersBefore);

#if JUCE_WINDOWS
    // On Windows it ends as its last static object goes, before the C runtime's teardown (known bug 182), which exit()
    // would go on to: the marker line, written when exit() goes on past that point, never comes; and it does come when the
    // program is told to end as the runtime would.
    constexpr auto wentOn = "PHAZITE: exit() went on past the end of the tests";
    CHECK_FALSE (passed.output.contains (wentOn));
    const auto asEver = runChild ("passes", "The test program as a child: a case that passes*", true);
    CHECK (asEver.exitCode == 0);
    CHECK (asEver.output.contains (wentOn));
#endif

    // One failed: not 0, and its failure written.
    const auto failed = runChild ("fails", "The test program as a child: a case that fails*");
    CHECK (failed.exitCode != 0);
    CHECK (failed.output.contains ("Status: FAILURE!"));
    CHECK (countTestFolders() == foldersBefore);

#if JUCE_CHECK_MEMORY_LEAKS
    // An object of a class with JUCE_LEAK_DETECTOR left alive: the run fails, as static objects go, and says which.
    const auto leaked = runChild ("leaks", "The test program as a child: a case that leaks*");
    CHECK (leaked.exitCode != 0);
    CHECK (leaked.output.contains ("Status: SUCCESS!")); // its tests passed
    CHECK (leaked.output.contains ("after the tests had ended"));
    CHECK (countTestFolders() == foldersBefore);
#endif
}
