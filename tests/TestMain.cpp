// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's test runner: doctest (MIT, external/doctest) inside a JUCE environment.
// Run all tests with `ctest --preset <preset>`, or run the executable directly for doctest's options,
// e.g. `phazite_tests --test-case="*project*"` or `phazite_tests --help`.

#define DOCTEST_CONFIG_IMPLEMENT
#include "AudioThreadMonitor.h"
#include "StallReport.h"
#include "TestHelpers.h"
#include "model/base/internal/SongIndex.h"
#include "model/engine/EngineContext.h"
#include "model/engine/internal/EngineContext.h"
#include "model/plugins/internal/PluginHost.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Built with a sanitizer (linux-asan, linux-tsan): GCC says so by these, Clang by __has_feature.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
 #define PHAZITE_SANITIZED 1
#elif defined(__has_feature)
 #if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
  #define PHAZITE_SANITIZED 1
 #endif
#endif

#ifdef PHAZITE_SANITIZED
 #include <sanitizer/common_interface_defs.h>
#endif

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace
{
phazite::EngineContext* testEngineContext = nullptr;

#if JUCE_WINDOWS
// Ends the program at once with a result, its output flushed, without the C runtime's teardown (below).
[[noreturn]] void endProcessNow (int code) noexcept;
#endif
juce::File systemTemporaryFolder;

// Makes a folder of this program's own in the system's temporary folder and makes it the temporary folder for the rest
// of the run. JUCE asks the environment for it each time: Windows' GetTempPath() reads TMP, then TEMP; elsewhere, TMPDIR.
// JUCE workaround T5 (docs/juce-workarounds.md).
juce::File useTemporaryFolderOfOwn()
{
    systemTemporaryFolder = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const auto folder = systemTemporaryFolder.getChildFile ("PhaziteTests " + juce::Uuid().toString());
    folder.createDirectory();

#if JUCE_WINDOWS
    _wputenv_s (L"TMP", folder.getFullPathName().toWideCharPointer());
    _wputenv_s (L"TEMP", folder.getFullPathName().toWideCharPointer());
#else
    setenv ("TMPDIR", folder.getFullPathName().toRawUTF8(), 1);
#endif

    jassert (juce::File::getSpecialLocation (juce::File::tempDirectory) == folder);
    return folder;
}

// The tests that play in real time sleep for a block's length (5 ms) after each block, and JUCE asks Windows for a 1 ms
// timer, which sleeps keep to. Windows 11 may ignore that while none of a program's windows can be seen, as the tests'
// never can, and round each sleep up to its own 15.6 ms timer: playing then takes three times as long. This program's
// asks are kept.
void keepTimerResolution()
{
#if JUCE_WINDOWS
    PROCESS_POWER_THROTTLING_STATE state {};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    SetProcessInformation (GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof (state));
#endif
}

//==============================================================================
// JUCE's assertions as failures. A jassert (JUCE's, or Phazite's own) that fires while a test case runs fails that case,
// with the assertion's file and line; without this, an assertion in a run without a debugger only writes a line that
// ctest never shows. In Debug builds the build sets JUCE_LOG_ASSERTIONS (CMakeLists.txt), so that JUCE hands each
// assertion to the current juce::Logger as "JUCE Assertion failure in <file>:<line>" (juce_core.h,
// juce_Logger.cpp: logAssertion()); the tests' logger is this one, which passes every other line on to where JUCE writes
// it without a logger. Release and RelWithDebInfo builds compile no assertions, so it has nothing to hear there.
//
// An assertion may fire on any thread. One that fires while a test case runs fails it (doctest takes failures from the
// test's other threads); one that fires between test cases, or as the program ends, fails the run, with the last test
// that ran named; one that fires after main() has returned (JUCE's leak detector, as static objects go) ends the program
// at once with a failure. On an audio thread (AudioThreadMonitor::isAudioThread(): a block of the tests' audio device,
// the engine's workers) the net neither locks nor allocates: it copies the assertion's place into a slot made before, and
// the tests' thread takes it at the test case's next check, the end of a run through its subcases or an
// ExpectedAssertion's call, and fails the test case then, naming the assertion and the audio thread; one still waiting as
// the test case ends fails the run, naming the test case. What JUCE allocates for the message before the net hears of it
// (logAssertion()) is all the audio thread allocates for it, and the real-time audit leaves that out (known bug 153).
#if JUCE_DEBUG && ! JUCE_LOG_ASSERTIONS
 #error "Debug builds of the tests need JUCE_LOG_ASSERTIONS, which CMakeLists.txt sets for phazite_core and every target that uses it"
#endif

// The assertions that may fire, each because Phazite does on purpose what JUCE asserts against, or because the machine
// the tests run on lacks what JUCE asserts it has: the file and line JUCE names (isPlace()), and why. A line here names
// JUCE's line, so that it is looked at again when JUCE changes, but for a file all of whose assertions say one thing;
// each also has its entry in docs/juce-workarounds.md.
struct AllowedAssertion
{
    const char* place; // "juce_File.cpp:219", as logAssertion() names it; or a file, for every line of it
    const char* why;
};

constexpr AllowedAssertion allowedAssertions[] = {
    { "juce_Midi_linux.cpp:539",
      "Linux without ALSA's sequencer (CI's machines have no /dev/snd/seq): JUCE asserts as it finds it can't list MIDI "
      "devices, and goes on with none (T7)" },
    { "juce_XWindowSystem_linux.cpp",
      "Linux without a display (CI's machines): the tests' windows have no X11 window behind them, and JUCE asserts that "
      "each call it makes for a window has one: a pointer's shape over a control, a menu or a window of its own (T7)" },
};

// Whether an assertion's place, as JUCE's log gives it, is the one named. The file by its name alone: the log gives it as
// the compiler does, with the folder JUCE includes it from with MSVC ("values/juce_ValueTree.cpp") and without with GCC
// and Clang. The line as named: one line, or "first-last" for an assertion written over several lines (MSVC gives its
// last line, GCC and Clang its first), or any line where none is named.
bool isPlace (const juce::String& place, const juce::String& named)
{
    const auto fileOf = [] (const juce::String& text)
    {
        const auto path = text.containsChar (':') ? text.upToLastOccurrenceOf (":", false, false) : text;
        return path.fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false);
    };

    if (named.isEmpty() || fileOf (place) != fileOf (named))
        return false;

    if (! named.containsChar (':'))
        return true;

    const auto line = place.fromLastOccurrenceOf (":", false, false).getIntValue();
    const auto lines = named.fromLastOccurrenceOf (":", false, false);
    const auto first = lines.upToFirstOccurrenceOf ("-", false, false).getIntValue();
    const auto last = lines.containsChar ('-') ? lines.fromFirstOccurrenceOf ("-", false, false).getIntValue() : first;
    return line >= first && line <= last;
}

// The same, for several places named at once, with | between them.
bool isAnyPlace (const juce::String& place, const juce::String& named)
{
    for (const auto& each : juce::StringArray::fromTokens (named, "|", {}))
        if (isPlace (place, each))
            return true;

    return false;
}

class AssertionNet final : public juce::Logger
{
public:
    static AssertionNet& get()
    {
        // Never deleted: it must outlive every static object, whose destructors may still assert (JUCE's leak detector).
        static auto* net = new AssertionNet();
        return *net;
    }

    // While installed (from main() on), the current logger. A test that sets a logger of its own and then clears it
    // leaves none: the next test case puts this one back.
    void install()
    {
        if (juce::Logger::getCurrentLogger() != this)
            juce::Logger::setCurrentLogger (this);
    }

    // The test case running, or the last that ran (any thread, even as a sanitizer ends the program).
    static const char* getRunningTest() noexcept { return runningTest.load(); }

    void testCaseStarted (const char* name)
    {
        install();
        takeFromAudioThreads(); // what fired before, as the one before ended
        runningTest = name;
        const std::lock_guard held (lock);
        currentTest = name;
        lastTest = name;
        reportedInThisTest.clear();
    }

    void testCaseEnded()
    {
        {
            const std::lock_guard held (lock);
            currentTest = nullptr;
        }

        // What an audio thread noted that the test's thread hasn't taken yet: the test case has made its last check, so
        // it fails the run, with the test case named.
        takeFromAudioThreads();
    }

    // The assertions that fired outside any test case, so far; true if there were any (each written to stderr).
    bool reportOutsideTestCases()
    {
        takeFromAudioThreads();
        const std::lock_guard held (lock);

        for (const auto& line : outside)
            std::fprintf (stderr, "%s\n", line.c_str());

        std::fflush (stderr);
        return ! outside.empty();
    }

    // The places a test expects an assertion at, while its ExpectedAssertion lives (its level among those living); and
    // how often one fired there.
    size_t expect (const char* place)
    {
        takeFromAudioThreads(); // what fired before it, which it doesn't expect
        const std::lock_guard held (lock);
        expectations.push_back ({ place != nullptr ? place : "", 0 });
        ++generation;
        return expectations.size() - 1;
    }

    void stopExpecting (size_t level)
    {
        takeFromAudioThreads(); // what fired while it lived
        const std::lock_guard held (lock);
        expectations.resize (std::min (level, expectations.size())); // those made after it went before it
        ++generation;
    }

    int getNumExpected (size_t level)
    {
        takeFromAudioThreads();
        const std::lock_guard held (lock);
        return level < expectations.size() ? expectations[level].count : 0;
    }

    // From here on, as static objects go, an assertion ends the program with a failure.
    void runEnded()
    {
        takeFromAudioThreads();
        const std::lock_guard held (lock);
        ended = true;
    }

    // The assertions noted on audio threads so far (getNumAudioThreadAssertions(), TestHelpers.h).
    int getNumAudioThreadAssertions() const noexcept { return numClaimed.load() + numLost.load(); }

    void logMessage (const juce::String& message) override
    {
        if (! message.startsWith (prefix))
        {
            juce::Logger::outputDebugString (message); // where writeToLog() writes without a logger
            return;
        }

        if (phazite::test::AudioThreadMonitor::isAudioThread())
        {
            noteOnAudioThread (message);
            return;
        }

        takeFromAudioThreads(); // what audio threads noted first
        const std::lock_guard held (lock);
        take (message.fromFirstOccurrenceOf (prefix, false, false).trim(), {});
    }

    // On a thread that isn't an audio thread, the test's own above all: what audio threads noted, each taken as an assertion
    // that fired on this thread is (take()). The net calls it each time it hears from the test's thread (a check, a
    // subcase's end, an ExpectedAssertion), so that an assertion on an audio thread fails the test case it fired in.
    void takeFromAudioThreads()
    {
        if (taking || (numTaken.load() == numClaimed.load() && numLostTaken.load() == numLost.load()))
            return;

        taking = true; // a failure added here is a check, which the listener hears, on this thread
        const std::lock_guard held (lock);

        for (auto taken = numTaken.load(); taken < numClaimed.load(); ++taken)
        {
            auto& slot = audioThreadAssertions[(size_t) (taken % maxAudioThreadAssertions)];

            if (! slot.written.load (std::memory_order_acquire))
                break; // still being written: the next call takes it

            const juce::String place (slot.place);
            const AudioThreadAssertion fired { slot.test, slot.generation == generation };
            slot.written.store (false, std::memory_order_relaxed);
            numTaken = taken + 1;
            take (place, fired);
        }

        if (const auto lost = numLost.load(); lost > numLostTaken.load())
        {
            numLostTaken = lost;
            outside.insert ("JUCE assertions fired on audio threads faster than the tests' thread took them: " + std::to_string (lost)
                            + " in all could not be kept (tests/TestMain.cpp)");
        }

        taking = false;
    }

private:
    AssertionNet() = default;

    static constexpr auto prefix = "JUCE Assertion failure in ";

    // Where one an audio thread noted came from: the test case running as it fired, and whether the ExpectedAssertions
    // living then still are (none made or gone since).
    struct AudioThreadAssertion
    {
        const char* test = nullptr;
        bool asExpected = false;
    };

    // An assertion that fired (with the lock held): one that is allowed, or that a test expects, passes; one outside a test
    // case fails the run; one in a test case fails it, once for each place. fromAudioThread says the test case it fired in
    // and what it can be expected by, for one an audio thread noted; none, for one that fired on this thread now.
    void take (const juce::String& place, std::optional<AudioThreadAssertion> fromAudioThread)
    {
        for (const auto& allowed : allowedAssertions)
            if (isPlace (place, allowed.place))
                return;

        // One a test makes on purpose (ExpectedAssertion, TestHelpers.h): the one made last that names its place.
        if (! fromAudioThread.has_value() || fromAudioThread->asExpected)
            for (auto expectation = expectations.rbegin(); expectation != expectations.rend(); ++expectation)
                if (isAnyPlace (place, juce::String (expectation->place)))
                {
                    ++expectation->count;
                    return;
                }

        const std::string onThread = fromAudioThread.has_value() ? " on an audio thread" : "";

        if (ended)
        {
            std::fprintf (stderr,
                          "JUCE assertion failed at %s%s after the tests had ended, as static objects went (at "
                          "juce_LeakedObjectDetector.h, objects of a class with JUCE_LEAK_DETECTOR were still alive: the stack "
                          "names the class, as LeakedObjectDetector<class>)\n%s\n",
                          place.toRawUTF8(), onThread.c_str(), juce::SystemStats::getStackBacktrace().toRawUTF8());
            std::fflush (stderr);
#if JUCE_WINDOWS
            endProcessNow (EXIT_FAILURE);
#else
            std::_Exit (EXIT_FAILURE);
#endif
        }

        if (fromAudioThread.has_value() && (currentTest == nullptr || fromAudioThread->test != currentTest))
        {
            const auto* test = fromAudioThread->test;
            outside.insert ("JUCE assertion failed at " + place.toStdString() + onThread
                            + (test != nullptr ? ", in or after \"" + std::string (test)
                                                     + "\", which had ended before the "
                                                       "tests' thread heard of it"
                                               : std::string (" before any test case")));
            return;
        }

        if (currentTest == nullptr)
        {
            outside.insert ("JUCE assertion failed at " + place.toStdString() + " outside any test case"
                            + (lastTest != nullptr ? ", after \"" + std::string (lastTest) + "\"" : std::string()));
            return;
        }

        // Once for each place in a test case: one that fires at every block or repaint would fill the output.
        if (! reportedInThisTest.insert (place.toStdString()).second)
            return;

        const auto file = places.emplace (place.upToLastOccurrenceOf (":", false, false).toStdString()).first;
        const auto line = place.fromLastOccurrenceOf (":", false, false).getIntValue();

        // Where it was called from: the stack's first frames past this logger and JUCE's assertion. None from an audio
        // thread, where taking it would lock and allocate: the real-time audit lists the stacks of what JUCE allocated for
        // the assertion's message there, and a debugger stops at it.
        std::string where;

        if (! fromAudioThread.has_value())
        {
            auto stack = juce::StringArray::fromLines (juce::SystemStats::getStackBacktrace());
            stack.removeEmptyStrings();
            stack.removeRange (0, 4);
            stack.removeRange (16, stack.size());
            where = "\n    " + stack.joinIntoString ("\n    ").toStdString();
        }

        ADD_FAIL_CHECK_AT (file->c_str(), line, // a check's failure, which goes on: an assertion goes on in JUCE too
                           "JUCE assertion failed at " << place.toStdString() << onThread
                                                       << ": fix its cause, or allow it with why in tests/TestMain.cpp (allowedAssertions)"
                                                       << where);
    }

    // On an audio thread (AudioThreadMonitor::isAudioThread()): the assertion's place copied into a slot made before,
    // without a lock or an allocation, for the tests' thread to take (takeFromAudioThreads()). JUCE has made its message,
    // a juce::String, before the net hears of it (juce_Logger.cpp: logAssertion()), which the real-time audit leaves out.
    void noteOnAudioThread (const juce::String& message) noexcept
    {
        auto claimed = numClaimed.load();

        do
        {
            if (claimed - numTaken.load() >= maxAudioThreadAssertions)
            {
                ++numLost;
                return;
            }
        } while (! numClaimed.compare_exchange_weak (claimed, claimed + 1));

        auto& slot = audioThreadAssertions[(size_t) (claimed % maxAudioThreadAssertions)];
        auto text = message.getCharPointer();

        for (auto skip = std::char_traits<char>::length (prefix); skip > 0; --skip)
            text.getAndAdvance();

        size_t length = 0;

        for (auto c = text.getAndAdvance(); c != 0 && length + 1 < sizeof (slot.place); c = text.getAndAdvance())
            slot.place[length++] = c >= 32 && c < 127 ? (char) c : '?'; // a file's name and a line

        slot.place[length] = 0;
        slot.test = runningTest.load();
        slot.generation = generation.load();
        slot.written.store (true, std::memory_order_release);
    }

    static inline std::atomic<const char*> runningTest { nullptr };
    std::mutex lock;
    const char* currentTest = nullptr;
    const char* lastTest = nullptr;

    struct Expectation
    {
        std::string place; // an ExpectedAssertion's places
        int count = 0;
    };

    // Standard containers: this object is never deleted, and JUCE's leak detector would count JUCE's at exit.
    std::set<std::string> reportedInThisTest, outside;
    std::set<std::string> places;          // the files' names, which doctest's report points to
    std::vector<Expectation> expectations; // the ExpectedAssertions living, the one made last at the end
    bool ended = false;

    // What audio threads noted, in a ring of slots that one of them claims at a time and the tests' thread takes from in
    // order; the count of those lost to a full ring; and the expectations' generation, one more each time an
    // ExpectedAssertion is made or goes (from the tests' thread, with the lock held).
    struct NotedAssertion
    {
        std::atomic<bool> written { false };
        char place[256] {};
        const char* test = nullptr;
        int generation = 0;
    };

    static constexpr int maxAudioThreadAssertions = 64;
    std::array<NotedAssertion, maxAudioThreadAssertions> audioThreadAssertions;
    std::atomic<int> numClaimed { 0 }, numTaken { 0 }, numLost { 0 }, numLostTaken { 0 }, generation { 0 };
    static inline thread_local bool taking = false;
};

//==============================================================================
// The program's end, on Windows (known bug 182). Every test passed, it crashed now and then as it exited, after doctest's
// "Status: SUCCESS!": the main thread was in ExitProcess, past every static object's destructor and the C runtime's
// teardown, when a thread that the graphics driver started (NVIDIA's, under the Direct2D windows) ran the program's
// thread-local initialisers, as MSVC's runtime runs them on every new thread, and doctest's (g_oss) allocated without the
// heap's lock, which was gone. So once main() has returned and every static object has gone (JUCE's leak detector checks
// each class's count as its counter goes, and fails the run: AssertionNet), the program ends there with main()'s result,
// its output flushed, without the runtime's teardown, which nothing of the tests needs (it would only write the debug
// heap's leaks where a debugger reads them): no thread that starts then runs anything of the program's. Elsewhere the
// program ends as ever, LeakSanitizer's check at exit (linux-asan) with it.
#if JUCE_WINDOWS
std::atomic<bool> mainReturned { false };
std::atomic<int> mainResult { 0 };

[[noreturn]] void endProcessNow (int code) noexcept
{
    std::fflush (nullptr);
    TerminateProcess (GetCurrentProcess(), (UINT) code);
    std::_Exit (code); // not reached
}

// Run by exit() after every static object's destructor (registered before any of them, below), but only as main() returns:
// an exit() from elsewhere ends the program as it always did.
void endAfterStaticObjects()
{
    if (mainReturned.load())
        endProcessNow (mainResult.load());
}

void registerEndAfterStaticObjects()
{
    std::atexit (&endAfterStaticObjects);
}

// For TestProgramTests, which starts this program with PHAZITE_TEST_EXIT_MARKER set: a line written if exit() goes on past
// endAfterStaticObjects(), registered before it (so run after it), to show that the program did end there.
// PHAZITE_TEST_EXIT_AS_EVER ends it as the runtime would, to show that the line would come.
bool markingExit = false;

void markRuntimeWentOn()
{
    if (markingExit)
    {
        std::fputs ("PHAZITE: exit() went on past the end of the tests\n", stdout);
        std::fflush (stdout);
    }
}

void registerMarkRuntimeWentOn()
{
    markingExit = GetEnvironmentVariableA ("PHAZITE_TEST_EXIT_MARKER", nullptr, 0) > 0;
    std::atexit (&markRuntimeWentOn);
}
} // namespace

// The C runtime runs the functions in the sections .CRT$XCA to .CRT$XCZ, in their order, as the program starts: its own
// first, the program's static objects' constructors in .CRT$XCU. One in .CRT$XCB registers its function with atexit()
// before any static object registers its destructor, so that exit(), which runs them in reverse, runs it after them all.
// The linker keeps it, though nothing names it. The exit marker's comes before it (.CRT$XCAA sorts before .CRT$XCB).
#pragma section(".CRT$XCAA", read)
extern "C" __declspec (allocate (".CRT$XCAA")) void (*const phaziteRegisterMarkRuntimeWentOn)() = &registerMarkRuntimeWentOn;
#pragma comment(linker, "/include:phaziteRegisterMarkRuntimeWentOn")
#pragma section(".CRT$XCB", read)
extern "C" __declspec (allocate (".CRT$XCB")) void (*const phaziteRegisterEndAfterStaticObjects)() = &registerEndAfterStaticObjects;
#pragma comment(linker, "/include:phaziteRegisterEndAfterStaticObjects")

namespace
{
#endif

// Says where a test case that takes too long is, where the environment asks (StallReport.h): main()'s, while it runs.
phazite::test::StallReport* stallReport = nullptr;

// Tells the net, and the stall report, which test case runs, as doctest starts and ends each. And has each answer of the
// songs' indexes compared with a walk through the song (model/base/internal/SongIndex.h), which asserts where they
// differ, in every test case but those that time or count what they do (the benchmarks, the benchmark gates, the long run
// and the performance gates).
struct AssertionNetListener final : public doctest::IReporter
{
    explicit AssertionNetListener (const doctest::ContextOptions&) {}

    void test_case_start (const doctest::TestCaseData& data) override
    {
        AssertionNet::get().testCaseStarted (data.m_name);

        if (stallReport != nullptr)
            stallReport->testCaseStarted (data.m_name);

        // Nor in the performance gates, which count the work Phazite does (the checks' walks would count with it): each of
        // those compares its songs' whole indexes with a walk at its end instead (isExact()).
        const std::string suite = data.m_test_suite != nullptr ? data.m_test_suite : "";
        const auto isGate = std::string (data.m_name).rfind ("Performance gate:", 0) == 0;
        phazite::SongIndex::setChecking (! isGate && suite != "benchmark" && suite != "benchmark-gate" && suite != "long-run");
    }

    // What audio threads noted, taken while the test case can still fail by it: at each check it makes, and as each run
    // through its subcases ends (AssertionNet::takeFromAudioThreads()).
    static void takeFromAudioThreads()
    {
        if (! phazite::test::AudioThreadMonitor::isAudioThread())
            AssertionNet::get().takeFromAudioThreads();
    }

    void test_case_reenter (const doctest::TestCaseData&) override { takeFromAudioThreads(); }
    void test_case_end (const doctest::CurrentTestCaseStats&) override
    {
        AssertionNet::get().testCaseEnded();

        if (stallReport != nullptr)
            stallReport->testCaseEnded();
    }

    void report_query (const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end (const doctest::TestRunStats&) override {}
    void test_case_exception (const doctest::TestCaseException&) override {}
    void subcase_start (const doctest::SubcaseSignature&) override {}
    void subcase_end() override { takeFromAudioThreads(); }
    void log_assert (const doctest::AssertData&) override { takeFromAudioThreads(); }
    void log_message (const doctest::MessageData&) override {}
    void test_case_skipped (const doctest::TestCaseData&) override {}
};

DOCTEST_REGISTER_LISTENER ("phazite-assertions", 1, AssertionNetListener);

#ifdef PHAZITE_SANITIZED
// A sanitizer that ends the program (AddressSanitizer's error, UndefinedBehaviorSanitizer's with halt_on_error,
// CMakePresets.json) does so before doctest can say which test ran: this says it, after the report.
void sayWhichTestDied()
{
    if (const auto* name = AssertionNet::getRunningTest())
        std::fprintf (stderr, "The sanitizer ended the tests in the test case \"%s\" (or just after it).\n", name);
}
#endif
} // namespace

phazite::EngineContext& phazite::test::getEngineContext()
{
    jassert (testEngineContext != nullptr);
    return *testEngineContext;
}

juce::File phazite::test::getSystemTemporaryFolder()
{
    jassert (systemTemporaryFolder != juce::File());
    return systemTemporaryFolder;
}

phazite::test::ExpectedAssertion::ExpectedAssertion (const char* place)
    : level (AssertionNet::get().expect (place))
{
}

phazite::test::ExpectedAssertion::~ExpectedAssertion()
{
    AssertionNet::get().stopExpecting (level);
}

int phazite::test::ExpectedAssertion::getCount() const
{
    return AssertionNet::get().getNumExpected (level);
}

int phazite::test::getNumAudioThreadAssertions()
{
    return AssertionNet::get().getNumAudioThreadAssertions();
}

bool phazite::test::runTimersUntil (const std::function<bool()>& condition, int timeoutMilliseconds)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMilliseconds;

    while (! condition())
    {
        if (juce::Time::getMillisecondCounter() > end)
            return false;

        juce::Thread::sleep (5);
        juce::Timer::callPendingTimersSynchronously();
    }

    return true;
}

int main (int argc, char** argv)
{
    keepTimerResolution();

    // From the start, before any thread that could assert or log is running.
    auto& assertions = AssertionNet::get();
    assertions.install();

#ifdef PHAZITE_SANITIZED
    __sanitizer_set_death_callback (&sayWhichTestDied);
#endif

    // From here until main() returns, so that a wait as the engine context or JUCE goes is seen too.
    std::unique_ptr<phazite::test::StallReport> stalls;

    if (const auto seconds = phazite::test::StallReport::getSecondsAsked(); seconds > 0)
    {
        stalls = std::make_unique<phazite::test::StallReport> (seconds);
        stallReport = stalls.get();
    }

    // Before anything asks for the temporary folder (getSystemTemporaryFolder() says why).
    const auto temporaryFolder = useTemporaryFolderOfOwn();

    int result = 0;

    {
        // The model expects JUCE's message manager, as in the app. The tests run on this thread, which becomes the message
        // thread.
        juce::ScopedJuceInitialiser_GUI juceInitialiser;

        // Audio is processed on one thread only, the one that calls the hosted device: the real-time
        // audit attributes everything that happens during processing to that thread.
        const auto engineContext = phazite::createEngineContext ({ .numAudioThreads = 1 });
        testEngineContext = engineContext.get();

        // Plug-ins in helpers start the tests' helper, which hosts the tests' stand-ins too (host/TestPlugins.h).
        phazite::getPluginHost (*engineContext).setHelperProgram (juce::File (PHAZITE_TEST_HOST));

        // A test case outside a TEST_SUITE block takes the suite that doctest.h, as it is included, sets back to "" for the
        // file; that is a static initialiser, which MSVC ran after some of the file's own once an inline variable made at
        // startup split them (DrumSampler's noSound, in AudioWorkTests.cpp), depending on how the program was linked. A test
        // case registered first had no suite, which doctest reads as a string under any --test-suite filter (the golden
        // tests, the audit, the benchmarks): the program crashed (2026-11-16). The tests open suites as blocks alone, so a
        // case outside one is in "", as doctest's own "set back" says; its suite isn't part of how doctest tells cases apart.
        // This leans on doctest's insides (detail::getRegisteredTests(), the registered case changed through a const_cast,
        // TestCase::operator< leaving the suite out): recheck it when doctest is bumped.
        for (const auto& testCase : doctest::detail::getRegisteredTests())
            if (testCase.m_test_suite == nullptr)
                const_cast<doctest::detail::TestCase&> (testCase).m_test_suite = "";

        doctest::Context context (argc, argv);
        result = context.run();

        testEngineContext = nullptr;
    } // the engine context is destroyed here, before JUCE shuts down

    // What asserted between test cases, or as the engine context and JUCE went, fails the run.
    if (assertions.reportOutsideTestCases() && result == 0)
        result = EXIT_FAILURE;

    assertions.runEnded();
    stallReport = nullptr;
    temporaryFolder.deleteRecursively();

#if JUCE_WINDOWS
    mainResult = result;

    // The program ends once every static object has gone (endAfterStaticObjects()), unless TestProgramTests asks otherwise.
    mainReturned = juce::SystemStats::getEnvironmentVariable ("PHAZITE_TEST_EXIT_AS_EVER", {}).isEmpty();
#endif

    return result;
}
