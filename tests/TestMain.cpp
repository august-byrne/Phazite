// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's test runner: doctest (MIT, bundled with Tracktion Engine) inside a JUCE environment.
// Run all tests with `ctest --preset <preset>`, or run the executable directly for doctest's options,
// e.g. `phazite_tests --test-case="*project*"` or `phazite_tests --help`.

#define DOCTEST_CONFIG_IMPLEMENT
#include "TestHelpers.h"
#include "model/engine/EngineSetup.h"

namespace
{
te::Engine* testEngine = nullptr;

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
    // Tracktion's Engine and Edits expect JUCE's message manager, as in the app. The tests run on this
    // thread, which becomes the message thread.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    const auto settingsFolder =
        juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("PhaziteTests", {}, false);
    int result = 0;

    {
        // Audio is processed on one thread only, the one that calls the hosted device: the real-time
        // audit attributes everything that happens during processing to that thread.
        auto engine =
            phazite::createEngine ({ .applicationName = "PhaziteTests",
                                     .behaviour = { .openAudioDevice = false, .scanPluginsOutOfProcess = false, .numAudioThreads = 1 },
                                     .propertyStorage = std::make_unique<TestPropertyStorage> (settingsFolder) });
        testEngine = engine.get();

        doctest::Context context (argc, argv);
        result = context.run();

        testEngine = nullptr;
    } // the Engine is destroyed here, before JUCE shuts down

    settingsFolder.deleteRecursively();
    return result;
}
