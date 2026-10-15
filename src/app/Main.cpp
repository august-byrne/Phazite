// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite: application entry point.
// Written against tracktion_engine develop @bb386171 (module 3.5.0) and the JUCE it pins (37c894f8).

#include "AppCommands.h"
#include "AppServices.h"
#include "MainWindow.h"
#include "ProjectSession.h"
#include "model/arrangement/Selection.h"
#include "model/commands/CommandRegistry.h"
#include "model/engine/EngineSetup.h"
#include "model/project/StarterProject.h"
#include "ui/devices/PluginWindow.h"
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

    // Must stay true: the out-of-process scanner relaunches this exe with "--PluginScan:<pipe>".
    bool moreThanOneInstanceAllowed() override { return true; }

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

        // The Engine opens the default audio device and loads the saved plug-in list (settings live in
        // %APPDATA%\Phazite\Settings.xml). Plug-ins are scanned in a child copy of this exe (see above).
        engine = createEngine ({});
        services = std::make_unique<AppServices> (*engine);

        lookAndFeel = std::make_unique<ui::LookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get()); // dialogs and menus use it too

        auto& uiBehaviour = getUIBehaviour (*engine);
        uiBehaviour.createPluginWindowFunction = ui::createPluginWindow;
        uiBehaviour.recreatePluginWindowFunction = ui::recreatePluginWindowContent;

        commands = std::make_unique<CommandRegistry>();
        commandBridge = std::make_unique<ui::CommandBridge> (*commands); // the commands' keys and menus, from here on
        session = std::make_unique<ProjectSession> (*engine, uiBehaviour, services->recentProjects, getDefaultProjectsFolder());
        mainWindow = std::make_unique<MainWindow> (*engine, *commandBridge, *session, *lookAndFeel, *services);

        addSongCommands (*commands, session->getSelection(), session->getRangeSelection());
        session->addCommands (*commands);
        mainWindow->addCommands();

        // The keymap the user chose, with their own changes to it.
        ui::loadKeySettings (*commandBridge, services->settings);
        mainWindow->getShell().refreshShortcutTexts();

        session->openAtStartup();
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
    std::unique_ptr<te::Engine> engine;
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
