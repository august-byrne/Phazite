// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's content built as the app builds it (Main.cpp and MainWindow), without a screen: the services, the
// session, AppShell and the commands, in the app's order, in a TestWindow. For the tests and benchmarks that need the
// whole window (AppShellTests.cpp, Benchmarks.cpp). Nothing in it opens a dialog: the save dialog answers with what it
// suggests, the question about restoring an autosave with no, and an error (a save that failed) fails the test, as a
// message box can't show without a display.
#pragma once

#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "app/AppCommands.h"
#include "app/AppServices.h"
#include "app/ProjectSession.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/input/KeyDispatcher.h"
#include "ui/mixer/DevicesView.h"
#include "ui/shell/AppShell.h"

#include <vector>

namespace phazite::test
{
// Members in the order the app makes them, so that they go in the order the app destroys them.
struct AppScreen
{
    AppScreen()
    {
        // As Main.cpp and MainWindow do.
        addSongCommands (registry, session.getContext(), renders);
        session.addCommands (registry, renders);
        shell.addCommands (registry);

        session.chooseSaveFile = [] (const juce::File& suggested, std::function<void (const juce::File&)> callback)
        { callback (suggested); };
        session.askToRestore = [] (const juce::File&, bool, std::function<void (bool)> callback) { callback (false); };
        session.onError = [] (const juce::String& title, const juce::String& message)
        { FAIL_CHECK ((title + ": " + message).toStdString()); };
        dispatchPendingMessages();
    }

    ~AppScreen()
    {
        runTimersUntil ([this] { return ! services.library.isIndexing(); }, 60000);
    }

    TemporaryFolder temp;
    AppServices services { getEngineContext(),
                           { .settingsInMemory = true,
                             .packsFolder = temp.folder.getChildFile ("Packs"),
                             .presetsFolder = temp.folder.getChildFile ("Presets") } };
    RenderHost renders; // the main window's part
    CommandRegistry registry;
    ui::CommandBridge commands { registry };
    ui::KeyDispatcher shortcutKeys { commands };
    ProjectSession session { getEngineContext(), services.recentProjects, temp.folder.getChildFile ("Projects"),
                             temp.folder.getChildFile ("Unsaved") };
    ui::ComputerKeyboard keyboard { [] (const juce::MidiMessage&) {} };
    ui::AppShell shell { services.engineContext, commands,           shortcutKeys,     keyboard,           session.getContext(),
                         services.settings,      services.soundPack, services.library, *services.previewer };
    TestWindow window { shell, 1280, 800 };

    // Runs a command that shows a view until a view of a type shows (a view's command hides it where it showed already,
    // the piano roll's say: then it shows it again), and returns it.
    template <typename View>
    View& showView (const juce::String& commandID)
    {
        View* shown = nullptr;

        for (int tries = 0; tries < 2 && shown == nullptr; ++tries)
        {
            REQUIRE (registry.invoke (commandID));
            dispatchPendingMessages();

            for (auto* view : findAll<View> (shell))
                if (view->isShowing())
                    shown = view;
        }

        INFO ("no view shows after " << commandID);
        REQUIRE (shown != nullptr);
        return *shown;
    }

    // One track for each of Phazite's own devices (getOwnDevices()), named after it, with the device on it: an instrument as
    // its instrument, an effect after its effects. The devices, in the order the device list offers them.
    std::vector<DeviceID> addEveryOwnDevice()
    {
        auto& song = *findSong (session.getContext());
        std::vector<DeviceID> added;

        for (const auto& type : getDeviceTypes (session.getContext()))
        {
            if (! type.isBuiltIn)
                continue;

            const auto track = addTrackAtEnd (song, false, nullptr);
            REQUIRE (track.isValid());
            setTrackName (song, track, type.name);
            const auto device = addDeviceTo (song, track, type, nullptr);
            INFO (type.name);
            REQUIRE (device.isValid());
            added.push_back (device);
        }

        dispatchPendingMessages();
        return added;
    }

    // The Devices view, on the page of a device, showing it.
    ui::DevicesView& showInDevicesView (DeviceID device)
    {
        // The view is made as it first shows.
        auto* view = findAll<ui::DevicesView> (shell).getFirst();

        if (view == nullptr || ! view->isShowing())
            view = &showView<ui::DevicesView> ("view.effects");

        const auto onInstrumentPage = view->getPageOf (device) == ui::DevicesView::Page::instrument;
        auto& shown = showView<ui::DevicesView> (onInstrumentPage ? "view.instrument" : "view.effects");
        shown.showDevice (device);
        dispatchPendingMessages();
        CHECK (shown.getShownBody() != nullptr);
        return shown;
    }
};
} // namespace phazite::test
