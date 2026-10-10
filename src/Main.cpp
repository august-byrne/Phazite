// Phazite smoke test: application entry point.
// Written against tracktion_engine develop @bb386171 (module 3.5.0) and the JUCE it pins (37c894f8).

#include "MainComponent.h"

// ThreadSanitizer builds (linux-tsan preset): GCC defines __SANITIZE_THREAD__, Clang has a feature check.
#if defined (__SANITIZE_THREAD__)
 #define PHAZITE_TSAN 1
#elif defined (__has_feature)
 #if __has_feature (thread_sanitizer)
  #define PHAZITE_TSAN 1
 #endif
#endif
#ifndef PHAZITE_TSAN
 #define PHAZITE_TSAN 0
#endif

namespace
{
    class PhaziteEngineBehaviour final : public te::EngineBehaviour
    {
    public:
        // Scan plug-ins in a child copy of this exe, so a crashing plug-in can't take Phazite down.
        // Needs the startChildProcessPluginScan() check at the top of initialise(). If it ever misbehaves,
        // return false here: scanning then runs in-process on PluginListComponent's worker thread.
        // Off under ThreadSanitizer: Tracktion's scan coordinator shares some state between threads
        // without locks, which would bury real reports.
        bool canScanPluginsOutOfProcess() override
        {
           #if PHAZITE_TSAN
            return false;
           #else
            return true;
           #endif
        }

        // Playback-only smoke test: don't open the microphone by default
        // (inputs can still be enabled in the Audio Settings dialog).
        bool shouldOpenAudioInputByDefault() override   { return false; }
    };
}

//==============================================================================
class PhaziteApplication final : public juce::JUCEApplication
{
public:
    PhaziteApplication() = default;

    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }

    // Must stay true: the out-of-process scanner relaunches this exe with "--PluginScan:<pipe>".
    bool moreThanOneInstanceAllowed() override             { return true; }

    void initialise (const juce::String& commandLine) override
    {
        // Running as the plug-in-scan child: serve scan requests only, no Engine and no window.
        if (te::PluginManager::startChildProcessPluginScan (commandLine))
            return;

        // Started as a scan child, but the parent had already given up on it (e.g. the machine was busy):
        // quit rather than open a second Phazite with its own window and audio device.
        if (commandLine.trim().startsWith ("--PluginScan:"))
        {
            quit();
            return;
        }

        // The Engine opens the default audio device and loads the saved plug-in list
        // (settings live in %APPDATA%\Phazite\Settings.xml). nullptr = default UIBehaviour.
        engine = std::make_unique<te::Engine> (getApplicationName(), nullptr,
                                               std::make_unique<PhaziteEngineBehaviour>());

        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine);
    }

    void shutdown() override
    {
        mainWindow = nullptr;   // the window owns MainComponent, which owns the Edit: destroy it first
        engine = nullptr;       // then the Engine
    }

    void systemRequestedQuit() override                    { quit(); }

private:
    //==============================================================================
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, te::Engine& e)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (e), true);
            setResizable (true, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    // Declaration order matters: members are destroyed in reverse order, so the window (and the Edit
    // inside it) always goes before the Engine, even if shutdown() is never called.
    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<MainWindow> mainWindow;
};

//==============================================================================
START_JUCE_APPLICATION (PhaziteApplication)
