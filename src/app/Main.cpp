// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite: application entry point.
// Written against tracktion_engine develop @bb386171 (module 3.5.0) and the JUCE it pins (37c894f8).

#include "AppCommands.h"
#include "AppServices.h"
#include "AppUIBehaviour.h"
#include "MainWindow.h"
#include "ProjectSession.h"
#include "RunMarker.h"
#include "model/commands/CommandRegistry.h"
#include "model/engine/internal/EngineSetup.h"
#include "model/project/internal/StarterProject.h"
#include "model/project/internal/UnsavedProjects.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/KeymapStore.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite
{
class Application final : public juce::JUCEApplication
{
public:
    Application() = default;

    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    // One Phazite at a time: a second start brings the first forward instead (two would autosave the same project, and the
    // first to close would delete its autosave: docs/performance-audit.md, R6). The out-of-process scanner runs this exe
    // again with "--PluginScan:<pipe>", which must start.
    bool moreThanOneInstanceAllowed() override { return getCommandLineParameters().contains ("--PluginScan"); }

    void anotherInstanceStarted (const juce::String&) override
    {
        if (mainWindow != nullptr)
        {
            mainWindow->setMinimised (false);
            mainWindow->toFront (true);
        }
    }

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

        // A log of what happened, for a report of what went wrong (%APPDATA%\Phazite\Phazite.log), kept short: JUCE trims
        // the start of an old one (docs/performance-audit.md, R7).
        logger.reset (juce::FileLogger::createDefaultAppLogger ("Phazite", "Phazite.log", "Phazite " + getApplicationVersion()));
        juce::Logger::setCurrentLogger (logger.get());

        // The Engine opens the default audio device and loads the saved plug-in list (settings live in
        // %APPDATA%\Phazite\Settings.xml). Plug-ins are scanned in a child copy of this exe (see above).
        engine = createEngine ({ .uiBehaviour = std::make_unique<AppUIBehaviour>() });
        runMarker = std::make_unique<RunMarker> (engine->getPropertyStorage().getAppPrefsFolder().getChildFile ("Running"));
        logAudioOutput();
        services = std::make_unique<AppServices> (*engine);

        lookAndFeel = std::make_unique<ui::LookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get()); // dialogs and menus use it too

        commands = std::make_unique<CommandRegistry>();
        commandBridge = std::make_unique<ui::CommandBridge> (*commands); // the commands' keys and menus, from here on
        session = std::make_unique<ProjectSession> (*engine, getUIBehaviour (*engine), services->recentProjects, getDefaultProjectsFolder(),
                                                    getUnsavedProjectsFolder());
        mainWindow = std::make_unique<MainWindow> (*engine, *commandBridge, *session, *lookAndFeel, *services);
        addSongCommands (*commands, session->getContext(), *mainWindow);
        session->addCommands (*commands);
        mainWindow->addCommands();

        // The keymap the user chose, with their own changes to it.
        ui::loadKeySettings (*commandBridge, services->settings);
        mainWindow->getShell().refreshShortcutTexts();

        // The window shows, painted ("Opening..."), before the project loads, which can take a while.
        if (auto* peer = mainWindow->getPeer())
            peer->performAnyPendingRepaintsNow();

        session->openAtStartup (runMarker->didLastRunEndBadly());
        mainWindow->showWelcomeOnFirstRun();
    }

    void shutdown() override
    {
        if (services != nullptr && commandBridge != nullptr)
            ui::saveKeySettings (*commandBridge, services->settings);

        // Reverse order of creation: the window uses the session and the commands, the session holds
        // the Edit, and every Edit must go before the Engine. The services keep their settings in the
        // Engine's settings file.
        mainWindow = nullptr;
        session = nullptr;
        commandBridge = nullptr;
        commands = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel = nullptr;
        services = nullptr;
        engine = nullptr;
        runMarker = nullptr; // closed properly

        juce::Logger::writeToLog ("Phazite closed.");
        juce::Logger::setCurrentLogger (nullptr);
        logger = nullptr;
    }

    // What plays the sound, in the log.
    void logAudioOutput()
    {
        if (auto* device = engine->getDeviceManager().deviceManager.getCurrentAudioDevice())
            juce::Logger::writeToLog ("Audio output: " + device->getTypeName() + ", " + device->getName() + ", "
                                      + juce::String (device->getCurrentSampleRate()) + " Hz, "
                                      + juce::String (device->getCurrentBufferSizeSamples()) + " samples");
        else
            juce::Logger::writeToLog ("No audio output is open.");
    }

    void systemRequestedQuit() override
    {
        // Let the user finish an open dialog first (such as "Save changes?").
        if (auto* modal = juce::Component::getCurrentlyModalComponent())
        {
            modal->toFront (true);
            return;
        }

        if (session == nullptr)
        {
            quit();
            return;
        }

        session->closeThen (
            [] (bool closed)
            {
                if (closed)
                    quit();
            });
    }

private:
    // Declaration order matters: members are destroyed in reverse order, so even if shutdown() is
    // never called, the window goes first and the Engine last.
    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<te::Engine> engine;
    std::unique_ptr<RunMarker> runMarker;
    std::unique_ptr<AppServices> services;
    std::unique_ptr<ui::LookAndFeel> lookAndFeel;
    std::unique_ptr<CommandRegistry> commands;
    std::unique_ptr<ui::CommandBridge> commandBridge; // uses the commands
    std::unique_ptr<ProjectSession> session;
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace phazite

//==============================================================================
START_JUCE_APPLICATION (phazite::Application)
