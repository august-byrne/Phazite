// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content built as the app builds it (Main.cpp and MainWindow), without a screen: the services, the
// session, AppShell and the commands, in the app's order, in a TestWindow. For the tests and benchmarks that need the
// whole window (AppShellTests.cpp, Benchmarks.cpp). Nothing in it opens a dialog: the save dialog answers with what it
// suggests, and the question about restoring an autosave with no.
#pragma once

#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "app/AppCommands.h"
#include "app/AppServices.h"
#include "app/ProjectSession.h"
#include "model/engine/internal/EngineSetup.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/shell/AppShell.h"

namespace phazite::test
{
// Members in the order the app makes them, so that they go in the order the app destroys them.
struct AppScreen
{
    AppScreen()
    {
        // As Main.cpp and MainWindow do.
        addSongCommands (registry, session.getContext(), renders);
        session.addCommands (registry);
        shell.addCommands (registry);

        session.chooseSaveFile = [] (const juce::File& suggested, std::function<void (const juce::File&)> callback)
        { callback (suggested); };
        session.askToRestore = [] (const juce::File&, bool, std::function<void (bool)> callback) { callback (false); };
        dispatchPendingMessages();
    }

    ~AppScreen()
    {
        runTimersUntil ([this] { return ! services.library.isIndexing(); }, 10000);
    }

    te::Engine& engine = getEngine();
    TemporaryFolder temp;
    AppServices services { engine,
                           { .settingsInMemory = true,
                             .packsFolder = temp.folder.getChildFile ("Packs"),
                             .presetsFolder = temp.folder.getChildFile ("Presets") } };
    RenderHost renders; // the main window's part
    CommandRegistry registry;
    ui::CommandBridge commands { registry };
    ui::KeyDispatcher shortcutKeys { commands };
    ProjectSession session { engine, getUIBehaviour (engine), services.recentProjects, temp.folder.getChildFile ("Projects"),
                             temp.folder.getChildFile ("Unsaved") };
    ui::ComputerKeyboard keyboard { [] (const juce::MidiMessage&) {} };
    ui::AppShell shell { *services.engineContext, commands,           shortcutKeys,     keyboard,           session.getContext(),
                         services.settings,       services.soundPack, services.library, *services.previewer };
    TestWindow window { shell, 1280, 800 };
};
} // namespace phazite::test
