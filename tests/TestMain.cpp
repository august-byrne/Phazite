// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's test runner: doctest (MIT, bundled with Tracktion Engine) inside a JUCE environment.
// Run all tests with `ctest --preset <preset>`, or run the executable directly for doctest's options,
// e.g. `phazite_tests --test-case="*project*"` or `phazite_tests --help`.

#define DOCTEST_CONFIG_IMPLEMENT
#include "TestHelpers.h"
#include "model/engine/EngineContext.h"
#include "model/engine/internal/EngineContext.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/plugins/internal/PluginHost.h"

#include <cstdlib>

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
te::Engine* testEngine = nullptr;
phazite::EngineContext* testEngineContext = nullptr;
juce::File systemTemporaryFolder;

// Makes a folder of this program's own in the system's temporary folder and makes it the temporary folder for the rest
// of the run. JUCE asks the environment for it each time: Windows' GetTempPath() reads TMP, then TEMP; elsewhere, TMPDIR.
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

// Phazite's settings storage, kept in a temporary folder instead of the user's settings folder.
class TestPropertyStorage final : public phazite::PropertyStorage
{
public:
    explicit TestPropertyStorage (juce::File folder)
        : phazite::PropertyStorage ("PhaziteTests"),
          root (std::move (folder))
    {
    }

    juce::File getAppCacheFolder() override { return getFolder ("Cache"); }
    juce::File getAppPrefsFolder() override { return getFolder ("Settings"); }

private:
    juce::File root;

    juce::File getFolder (const char* name) const
    {
        auto folder = root.getChildFile (name);
        folder.createDirectory();
        return folder;
    }
};
} // namespace

te::Engine& phazite::test::getEngine()
{
    jassert (testEngine != nullptr);
    return *testEngine;
}

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

    // Before anything asks for the temporary folder (getSystemTemporaryFolder() says why).
    const auto temporaryFolder = useTemporaryFolderOfOwn();

    // Tracktion's Engine and Edits expect JUCE's message manager, as in the app. The tests run on this
    // thread, which becomes the message thread.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    const auto settingsFolder = temporaryFolder.getChildFile ("Settings");
    int result = 0;

    {
        // Audio is processed on one thread only, the one that calls the hosted device: the real-time
        // audit attributes everything that happens during processing to that thread.
        auto engine =
            phazite::createEngine ({ .applicationName = "PhaziteTests",
                                     .behaviour = { .openAudioDevice = false, .scanPluginsOutOfProcess = false, .numAudioThreads = 1 },
                                     .propertyStorage = std::make_unique<TestPropertyStorage> (settingsFolder) });
        testEngine = engine.get();
        const auto engineContext = phazite::createEngineContext (*engine);
        testEngineContext = engineContext.get();

        // Plug-ins in helpers start the tests' helper, which hosts the tests' stand-ins too (host/TestPlugins.h).
        phazite::getPluginHost (*engineContext).setHelperProgram (juce::File (PHAZITE_TEST_HOST));

        doctest::Context context (argc, argv);
        result = context.run();

        testEngineContext = nullptr;
        testEngine = nullptr;
    } // the Engine is destroyed here, before JUCE shuts down

    temporaryFolder.deleteRecursively();
    return result;
}
