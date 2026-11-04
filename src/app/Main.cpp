// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite: application entry point.

#include "AppCommands.h"
#include "AppServices.h"
#include "MainWindow.h"
#include "ProjectSession.h"
#include "RunMarker.h"
#include "model/commands/CommandRegistry.h"
#include "model/engine/EngineContext.h"
#include "model/engine/internal/EngineContext.h"
#include "model/project/internal/Project.h"
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
    // first to close would delete its autosave: docs/performance-audit.md, R6). Plug-ins are hosted and scanned in its
    // helper program, PhaziteHost, never in another copy of this one.
    bool moreThanOneInstanceAllowed() override { return false; }

    void anotherInstanceStarted (const juce::String&) override
    {
        if (mainWindow != nullptr)
        {
            mainWindow->setMinimised (false);
            mainWindow->toFront (true);
        }
    }

    void initialise (const juce::String&) override
    {
        // A log of what happened, for a report of what went wrong (%APPDATA%\Phazite\Phazite.log), kept short: JUCE trims
        // the start of an old one (docs/performance-audit.md, R7).
        logger.reset (juce::FileLogger::createDefaultAppLogger ("Phazite", "Phazite.log", "Phazite " + getApplicationVersion()));
        juce::Logger::setCurrentLogger (logger.get());

        // The one engine context: the audio device, Phazite's engine on it and the plug-ins' helpers. The services load the
        // plug-in list Phazite keeps and open the audio device and the MIDI inputs as its settings saved them (settings
        // live in %APPDATA%\Phazite\Phazite.settings). Plug-ins are scanned and hosted in its helper program.
        engineContext = createEngineContext ({});
        runMarker = std::make_unique<RunMarker> (Settings::getFolder().getChildFile ("Running"));
        services = std::make_unique<AppServices> (*engineContext);
        services->openAudioDevices();
        logAudioOutput();

        lookAndFeel = std::make_unique<ui::LookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get()); // dialogs and menus use it too

        commands = std::make_unique<CommandRegistry>();
        commandBridge = std::make_unique<ui::CommandBridge> (*commands); // the commands' keys and menus, from here on
        session = std::make_unique<ProjectSession> (*engineContext, services->recentProjects, getDefaultProjectsFolder(),
                                                    getUnsavedProjectsFolder());
        mainWindow = std::make_unique<MainWindow> (*commandBridge, *session, *lookAndFeel, *services);
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

        // Reverse order of creation: the window uses the session and the commands, the session holds the open project,
        // and the project, the services and the previews play on the engine context, which goes last.
        mainWindow = nullptr;
        session = nullptr;
        commandBridge = nullptr;
        commands = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel = nullptr;
        services = nullptr;
        engineContext = nullptr;
        runMarker = nullptr; // closed properly

        juce::Logger::writeToLog ("Phazite closed.");
        juce::Logger::setCurrentLogger (nullptr);
        logger = nullptr;
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
    // never called, the window goes first and the engine context last.
    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<EngineContext> engineContext;
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
