// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's test runner: doctest (MIT, external/doctest) inside a JUCE environment.
// Run all tests with `ctest --preset <preset>`, or run the executable directly for doctest's options,
// e.g. `phazite_tests --test-case="*project*"` or `phazite_tests --help`.

#define DOCTEST_CONFIG_IMPLEMENT
#include "TestHelpers.h"
#include "model/engine/EngineContext.h"
#include "model/engine/internal/EngineContext.h"
#include "model/plugins/internal/PluginHost.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

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
// at once with a failure.
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
        runningTest = name;
        const std::lock_guard held (lock);
        currentTest = name;
        lastTest = name;
        reportedInThisTest.clear();
    }

    void testCaseEnded()
    {
        const std::lock_guard held (lock);
        currentTest = nullptr;
    }

    // The assertions that fired outside any test case, so far; true if there were any (each written to stderr).
    bool reportOutsideTestCases()
    {
        const std::lock_guard held (lock);

        for (const auto& line : outside)
            std::fprintf (stderr, "%s\n", line.c_str());

        std::fflush (stderr);
        return ! outside.empty();
    }

    // The one place a test expects an assertion at, while its ExpectedAssertion lives; and how often it fired there.
    void expect (const char* place)
    {
        const std::lock_guard held (lock);
        expected = place != nullptr ? place : "";
        numExpected = 0;
    }

    int getNumExpected()
    {
        const std::lock_guard held (lock);
        return numExpected;
    }

    // From here on, as static objects go, an assertion ends the program with a failure.
    void runEnded()
    {
        const std::lock_guard held (lock);
        ended = true;
    }

    void logMessage (const juce::String& message) override
    {
        static constexpr auto prefix = "JUCE Assertion failure in ";

        if (! message.startsWith (prefix))
        {
            juce::Logger::outputDebugString (message); // where writeToLog() writes without a logger
            return;
        }

        const auto place = message.fromFirstOccurrenceOf (prefix, false, false).trim();

        for (const auto& allowed : allowedAssertions)
            if (isPlace (place, allowed.place))
                return;

        const std::lock_guard held (lock);

        // One a test makes on purpose (ExpectedAssertion, TestHelpers.h).
        if (! expected.empty() && isPlace (place, juce::String (expected)))
        {
            ++numExpected;
            return;
        }

        if (ended)
        {
            std::fprintf (stderr,
                          "JUCE assertion failed at %s after the tests had ended, as static objects went (at "
                          "juce_LeakedObjectDetector.h, objects of a class with JUCE_LEAK_DETECTOR were still alive: the stack "
                          "names the class, as LeakedObjectDetector<class>)\n%s\n",
                          place.toRawUTF8(), juce::SystemStats::getStackBacktrace().toRawUTF8());
            std::fflush (stderr);
            std::_Exit (EXIT_FAILURE);
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

        // Where it was called from: the stack's first frames past this logger and JUCE's assertion.
        auto stack = juce::StringArray::fromLines (juce::SystemStats::getStackBacktrace());
        stack.removeEmptyStrings();
        stack.removeRange (0, 4);
        stack.removeRange (16, stack.size());
        const auto where = stack.joinIntoString ("\n    ").toStdString();
        ADD_FAIL_CHECK_AT (file->c_str(), line, // a check's failure, which goes on: an assertion goes on in JUCE too
                           "JUCE assertion failed at "
                               << place.toStdString()
                               << ": fix its cause, or allow it with why in tests/TestMain.cpp (allowedAssertions)\n    " << where);
    }

private:
    AssertionNet() = default;

    static inline std::atomic<const char*> runningTest { nullptr };
    std::mutex lock;
    const char* currentTest = nullptr;
    const char* lastTest = nullptr;
    // Standard containers: this object is never deleted, and JUCE's leak detector would count JUCE's at exit.
    std::set<std::string> reportedInThisTest, outside;
    std::set<std::string> places; // the files' names, which doctest's report points to
    std::string expected;         // the place of an ExpectedAssertion, while one lives
    int numExpected = 0;
    bool ended = false;
};

// Tells the net which test case runs, as doctest starts and ends each.
struct AssertionNetListener final : public doctest::IReporter
{
    explicit AssertionNetListener (const doctest::ContextOptions&) {}

    void test_case_start (const doctest::TestCaseData& data) override { AssertionNet::get().testCaseStarted (data.m_name); }
    void test_case_reenter (const doctest::TestCaseData&) override {}
    void test_case_end (const doctest::CurrentTestCaseStats&) override { AssertionNet::get().testCaseEnded(); }

    void report_query (const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end (const doctest::TestRunStats&) override {}
    void test_case_exception (const doctest::TestCaseException&) override {}
    void subcase_start (const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert (const doctest::AssertData&) override {}
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
{
    AssertionNet::get().expect (place);
}

phazite::test::ExpectedAssertion::~ExpectedAssertion()
{
    AssertionNet::get().expect (nullptr);
}

int phazite::test::ExpectedAssertion::getCount() const
{
    return AssertionNet::get().getNumExpected();
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

        doctest::Context context (argc, argv);
        result = context.run();

        testEngineContext = nullptr;
    } // the engine context is destroyed here, before JUCE shuts down

    // What asserted between test cases, or as the engine context and JUCE went, fails the run.
    if (assertions.reportOutsideTestCases() && result == 0)
        result = EXIT_FAILURE;

    assertions.runEnded();
    temporaryFolder.deleteRecursively();
    return result;
}
