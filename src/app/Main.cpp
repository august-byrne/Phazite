// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite: application entry point.

#include "AppCommands.h"
#include "AppServices.h"
#include "MainWindow.h"
#include "ProjectSession.h"
#include "RunMarker.h"
#include "model/commands/CommandRegistry.h"
#include "model/engine/EngineContext.h"
#include "model/engine/StartupSound.h"
#include "model/engine/internal/EngineContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/UnsavedProjects.h"
#include "model/versions/VersionsModel.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/KeymapStore.h"
#include "ui/theme/LookAndFeel.h"

#include "SessionEnd.h"

#include <atomic>
#include <cstdlib>
#include <iostream>

namespace phazite
{
namespace
{
    // `Phazite --smoke-test`: Phazite starts as it does for the user, its window shown and painted, with a new project
    // open, and quits by itself, its exit code saying whether that went well: 0 if it did, 1 if it didn't within a minute
    // or a JUCE assertion fired (in a Debug build), and 77 where there is no display to show a window on (Linux without
    // X11 or Wayland), which ctest counts as skipped. It runs beside a Phazite that is open, and leaves the user's
    // settings, recent projects, presets, sounds, log and audio device alone: its settings are in memory, its projects,
    // presets and sounds in a temporary folder of its own, deleted as it quits, and it opens no audio device, plays no
    // sound, shows no welcome and asks for no update. What it writes goes to stderr. The tests start it
    // (tests/ShippedProgramsTests.cpp).
    constexpr const char* smokeTestSwitch = "--smoke-test";
    constexpr int smokeTestSkipped = 77;
    constexpr int smokeTestMilliseconds = 60000;

    bool isSmokeTest (const juce::String& commandLine)
    {
        for (const auto& token : juce::StringArray::fromTokens (commandLine, true))
            if (token.trim().unquoted() == smokeTestSwitch)
                return true;

        return false;
    }

    // Whether there is a display to show a window on: always but on Linux, where a computer without X11 or Wayland (a
    // headless build machine) has none.
    bool hasDisplay()
    {
#if JUCE_LINUX || JUCE_BSD
        const auto* x11 = std::getenv ("DISPLAY");
        const auto* wayland = std::getenv ("WAYLAND_DISPLAY");
        return (x11 != nullptr && *x11 != 0) || (wayland != nullptr && *wayland != 0);
#else
        return true;
#endif
    }

    // The smoke test's log: every line to stderr, and the JUCE assertions counted (JUCE_LOG_ASSERTIONS, Debug builds).
    class SmokeTestLog final : public juce::Logger
    {
    public:
        int getAssertions() const noexcept { return assertions.load(); }

        void logMessage (const juce::String& message) override
        {
            if (message.startsWith ("JUCE Assertion failure"))
                ++assertions;

            const juce::ScopedLock held (lock);
            std::cerr << message << std::endl;
        }

    private:
        std::atomic<int> assertions { 0 };
        juce::CriticalSection lock;
    };
} // namespace

class Application final : public juce::JUCEApplication
{
public:
    Application() = default;

    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    // One Phazite at a time: a second start brings the first forward instead (two would autosave the same project, and the
    // first to close would delete its autosave: docs/archive/performance-audit.md, R6). Plug-ins are hosted and scanned in its
    // helper program, PhaziteHost, never in another copy of this one. A smoke test (--smoke-test) shares nothing with
    // another Phazite, and runs beside one.
    bool moreThanOneInstanceAllowed() override { return isSmokeTest (getCommandLineParameters()); }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (mainWindow != nullptr)
        {
            mainWindow->setMinimised (false);
            mainWindow->toFront (true);
        }

        // A project double-clicked while Phazite runs opens here, once the user has answered what an open dialog asks, which
        // comes forward (ProjectSession::openFile() waits for it).
        if (auto* modal = juce::Component::getCurrentlyModalComponent())
            modal->toFront (true);

        if (const auto file = ProjectSession::findFileToOpen (commandLine); file != juce::File() && session != nullptr)
            session->openFile (file);
    }

    void initialise (const juce::String& commandLine) override
    {
        const auto smoke = isSmokeTest (commandLine);

        if (smoke)
        {
            if (! hasDisplay())
            {
                std::cerr << "Phazite's smoke test is skipped: there is no display to show its window on." << std::endl;
                setApplicationReturnValue (smokeTestSkipped);
                quit();
                return;
            }

            // Its log on stderr, and its files in a folder of its own.
            smokeTestLog = std::make_unique<SmokeTestLog>();
            juce::Logger::setCurrentLogger (smokeTestLog.get());
            smokeTestFolder =
                juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("Phazite smoke test", {}, false);

            for (const auto* name : { "Projects", "Untitled", "Sounds", "Presets", "Updates" })
                smokeTestFolder.getChildFile (name).createDirectory();
        }
        else
        {
            // Held while Phazite runs, so that its installer waits for it to close, or asks the user to close it, before it
            // replaces its files (packaging/windows/Phazite.iss).
#if JUCE_WINDOWS
            juce::ignoreUnused (runningLock.enter (0)); // only this Phazite holds it: a second one passes its command line here
#endif

            // A log of what happened, for a report of what went wrong (%APPDATA%\Phazite\Phazite.log), kept short: JUCE
            // trims the start of an old one (docs/archive/performance-audit.md, R7).
            logger.reset (juce::FileLogger::createDefaultAppLogger ("Phazite", "Phazite.log", "Phazite " + getApplicationVersion()));
            juce::Logger::setCurrentLogger (logger.get());
        }

        // The one engine context: the audio device, Phazite's engine on it and the plug-ins' helpers. The services load the
        // plug-in list Phazite keeps and open the audio device and the MIDI inputs as its settings saved them (settings
        // live in %APPDATA%\Phazite\Phazite.settings). Plug-ins are scanned and hosted in its helper program.
        engineContext = createEngineContext ({});
        runMarker = std::make_unique<RunMarker> ((smoke ? smokeTestFolder : Settings::getFolder()).getChildFile ("Running"));
        services = std::make_unique<AppServices> (*engineContext, smoke ? getSmokeTestServices() : AppServicesOptions {});

        if (! smoke)
        {
            services->openAudioDevices();
            logAudioOutput();
        }

        lookAndFeel = std::make_unique<ui::LookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get()); // dialogs and menus use it too

        commands = std::make_unique<CommandRegistry>();
        commandBridge = std::make_unique<ui::CommandBridge> (*commands); // the commands' keys and menus, from here on
        session = std::make_unique<ProjectSession> (*engineContext, services->recentProjects,
                                                    smoke ? smokeTestFolder.getChildFile ("Projects") : getDefaultProjectsFolder(),
                                                    smoke ? smokeTestFolder.getChildFile ("Untitled") : getUnsavedProjectsFolder());
        mainWindow = std::make_unique<MainWindow> (*commandBridge, *session, *lookAndFeel, *services);
        addSongCommands (*commands, session->getContext(), *mainWindow);
        session->addCommands (*commands, *mainWindow);
        mainWindow->addCommands();
        sessionEnd = std::make_unique<SessionEndWatch> ([this] (bool ending) { sessionEndChanged (ending); });

        // The keymap the user chose, with their own changes to it.
        ui::loadKeySettings (*commandBridge, services->settings);
        mainWindow->getShell().refreshShortcutTexts();

        // The window shows, painted ("Opening..."), before the project loads, which can take a while.
        if (auto* peer = mainWindow->getPeer())
            peer->performAnyPendingRepaintsNow();

        if (smoke)
        {
            // Nothing asks: a question would wait for an answer nobody gives.
            session->askToSave = [] (const juce::File&, const juce::String&, std::function<void (ProjectSession::UnsavedChanges)> answer)
            { answer (ProjectSession::UnsavedChanges::discard); };
            session->askToRestore = [] (const juce::File&, bool, std::function<void (bool)> answer) { answer (false); };
            session->askWhichCopy = [] (const juce::String&, const juce::String&, const juce::String&, std::function<void (int)> answer)
            { answer (0); };
            session->askAfterCrash = [] (const juce::File&, std::function<void (ProjectSession::AfterCrash)> answer)
            { answer (ProjectSession::AfterCrash::open); };

            // A new project, as a first start makes one; then the window is checked and Phazite quits.
            session->openAtStartup();
            smokeTestCheck = std::make_unique<SmokeTestCheck> (*this);
            return;
        }

        // The project or DAWproject file Phazite was started with (a double-click in Explorer), or the last project.
        session->openAtStartup (runMarker->didLastRunEndBadly(), ProjectSession::findFileToOpen (commandLine));

        // Phazite's sound, now that it has opened (model/engine/StartupSound.h), unless the settings turn it off: after the
        // project, whose opening stops what the previews play, and only through an audio output that opened.
        if (services->settings.get (Settings::startupSound) && engineContext->getAudioDeviceManager().getCurrentAudioDevice() != nullptr)
            playStartupSound (*services->previewer);

        mainWindow->showWelcomeOnFirstRun();

        // An earlier update's installer goes, and a newer release is asked for if the user turned that on (services/Updates.h).
        services->updates.startUp();
    }

    void shutdown() override
    {
        smokeTestCheck = nullptr;
        sessionEnd = nullptr; // before the session it tells

        if (services != nullptr && commandBridge != nullptr)
            ui::saveKeySettings (*commandBridge, services->settings);

        // Reverse order of creation: the window uses the session and the commands, the session holds the open project,
        // and the project, the services and the previews play on the engine context, which goes last.
        mainWindow = nullptr;
        session = nullptr;
        waitForVersionsFinished(); // a commit given as the project closed, written before the program ends
        commandBridge = nullptr;
        commands = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel = nullptr;
        services = nullptr;
        engineContext = nullptr;
        runMarker = nullptr; // closed properly

        juce::Logger::writeToLog ("Phazite closed.");

        if (smokeTestLog != nullptr)
        {
            // An assertion anywhere, as it started, ran or quit, fails the smoke test.
            if (const auto assertions = smokeTestLog->getAssertions(); assertions > 0)
            {
                std::cerr << "Phazite's smoke test failed: " << assertions << " JUCE assertion(s) fired." << std::endl;
                setApplicationReturnValue (EXIT_FAILURE);
            }

            smokeTestFolder.deleteRecursively();
        }

        juce::Logger::setCurrentLogger (nullptr);
        logger = nullptr;
        smokeTestLog = nullptr;
    }

    // What plays the sound, in the log.
    void logAudioOutput()
    {
        if (auto* device = engineContext->getAudioDeviceManager().getCurrentAudioDevice())
            juce::Logger::writeToLog ("Audio output: " + device->getTypeName() + ", " + device->getName() + ", "
                                      + juce::String (device->getCurrentSampleRate()) + " Hz, "
                                      + juce::String (device->getCurrentBufferSizeSamples()) + " samples");
        else
            juce::Logger::writeToLog ("No audio output is open.");
    }

    // The computer's session ending, as Phazite's own window for it hears it (SessionEnd.h): nothing waits for a plug-in's
    // settings from now on, so that the save question comes before Windows ends Phazite (known bug 168); a close already
    // waiting for them asks it now, and otherwise one begins, as JUCE's window may be asked after this one or before it.
    // Its end cancelled: Phazite waits for its plug-ins again.
    void sessionEndChanged (bool ending)
    {
        if (session == nullptr)
            return;

        session->setSessionEnding (ending);

        // A song being read from the versions for a command is let go, the song and the branch as they were, as a quit lets
        // it go.
        if (ending)
            session->cancelWork();

        if (ending && ! session->isClosing() && ! isDialogShowing())
            systemRequestedQuit();
    }

    // A dialog the user is to finish first (such as "Save changes?"): not the window's hold on its input while a command
    // waits for the plug-ins' states, after which a quit asked meanwhile goes on.
    bool isDialogShowing() const
    {
        auto* modal = juce::Component::getCurrentlyModalComponent();
        return modal != nullptr && (session == nullptr || ! session->isHoldingInput (modal));
    }

    void systemRequestedQuit() override
    {
        // A song being read from the versions for a command is let go first, the song and the branch as they were (its
        // progress window is no dialog to finish), and the close goes on to its question.
        if (session != nullptr)
            session->cancelWork();

        if (isDialogShowing())
        {
            juce::Component::getCurrentlyModalComponent()->toFront (true);
            return;
        }

        if (session == nullptr)
        {
            quit();
            return;
        }

        // Once a command that waits for the plug-ins' states has run, or been dropped.
        if (session->isWaitingForPluginStates())
        {
            session->afterPluginStatesGate ([this] { systemRequestedQuit(); });
            return;
        }

        session->closeThen (
            [this] (bool closed)
            {
                // Closed, then what the versions still write finished behind a window that says so (ProjectSession.h).
                if (closed && session != nullptr)
                    session->closeAndFinishThen ([] { quit(); });
                else if (session != nullptr)
                    session->setSessionEnding (false); // the close cancelled: Phazite goes on, and waits for its plug-ins again
            });
    }

private:
    // The smoke test's services: settings in memory, and its sounds, presets and updates in its own folder.
    AppServicesOptions getSmokeTestServices() const
    {
        AppServicesOptions options;
        options.settingsInMemory = true;
        options.packsFolder = smokeTestFolder.getChildFile ("Sounds");
        options.presetsFolder = smokeTestFolder.getChildFile ("Presets");
        options.updatesFolder = smokeTestFolder.getChildFile ("Updates");
        return options;
    }

    // Waits, on the message thread, for the window to show with the project open, paints it and quits as the user's quit
    // does (systemRequestedQuit()); or, if that doesn't come within the time, fails and quits.
    class SmokeTestCheck final : private juce::Timer
    {
    public:
        explicit SmokeTestCheck (Application& a)
            : app (a)
        {
            startTimer (50);
        }

    private:
        Application& app;
        const juce::uint32 deadline = juce::Time::getMillisecondCounter() + (juce::uint32) smokeTestMilliseconds;

        void timerCallback() override
        {
            if (app.session->getProject() != nullptr && app.mainWindow->isShowing())
            {
                stopTimer();

                if (auto* peer = app.mainWindow->getPeer())
                    peer->performAnyPendingRepaintsNow();

                juce::Logger::writeToLog ("Phazite's smoke test: the window shows, with "
                                          + app.session->getProject()->getFile().getFileName() + " open.");
                app.systemRequestedQuit();
                return;
            }

            if (juce::Time::getMillisecondCounter() > deadline)
            {
                stopTimer();
                juce::Logger::writeToLog ("Phazite's smoke test failed: no project was open in a window that shows within a minute.");
                app.setApplicationReturnValue (EXIT_FAILURE);
                juce::JUCEApplicationBase::quit();
            }
        }
    };

    // Declaration order matters: members are destroyed in reverse order, so even if shutdown() is
    // never called, the window goes first and the engine context last.
#if JUCE_WINDOWS
    // The named mutex the installer looks for, held from initialise() on. JUCE workaround W10 (docs/juce-workarounds.md).
    juce::InterProcessLock runningLock { "PhaziteRunning" };
#endif
    std::unique_ptr<SmokeTestLog> smokeTestLog; // --smoke-test's
    juce::File smokeTestFolder;
    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<EngineContext> engineContext;
    std::unique_ptr<RunMarker> runMarker;
    std::unique_ptr<AppServices> services;
    std::unique_ptr<ui::LookAndFeel> lookAndFeel;
    std::unique_ptr<CommandRegistry> commands;
    std::unique_ptr<ui::CommandBridge> commandBridge; // uses the commands
    std::unique_ptr<ProjectSession> session;
    std::unique_ptr<SessionEndWatch> sessionEnd; // tells the session
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<SmokeTestCheck> smokeTestCheck; // uses the window and the session
};
} // namespace phazite

//==============================================================================
START_JUCE_APPLICATION (phazite::Application)
