// SPDX-License-Identifier: AGPL-3.0-or-later
// Paint code runs: every view of the window painted, by JUCE's software renderer (PaintCheck.h), in each theme, over a song
// with something of everything (RepresentativeSong.h) and a track for each of Phazite's own devices, whose editors the
// Devices view shows in turn. What is checked is that painting finishes, raises no JUCE assertion (TestMain.cpp) and draws
// something; what each view should look like is its own tests' business. The main window's dialogs are painted, in each
// theme, where they open (MainWindowTests.cpp).
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "PaintCheck.h"
#include "RepresentativeSong.h"
#include "SandboxedPlugins.h"
#include "TestPlugin.h"
#include "model/arrangement/PlaylistModel.h"
#include "ui/audioclip/AudioClipEditor.h"
#include "ui/browser/Browser.h"
#include "ui/mixer/Inspector.h"
#include "ui/mixer/MixerView.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/playlist/Playlist.h"
#include "ui/project/ProjectView.h"
#include "ui/theme/LookAndFeel.h"
#include "ui/versions/VersionsView.h"

#include <optional>

TEST_CASE ("Every view of the window paints, in each theme, over a song with something of everything")
{
    phazite::ui::LookAndFeel lookAndFeel;
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };
    const phazite::test::TestPlugin testPlugin;
    const phazite::test::SandboxedPlugins sandboxedPlugins;
    phazite::test::AppScreen screen;

    // Before a project opens: Opening...
    phazite::test::checkPaints (screen.shell, "the window, opening");

    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& context = screen.session.getContext();
    phazite::test::checkPaints (screen.shell, "the window, an empty project");

    phazite::test::addRepresentativeSong (*screen.session.getProject(), context, screen.temp.folder.getChildFile ("Sounds"));
    const auto devices = screen.addEveryOwnDevice();
    phazite::test::dispatchPendingMessages();

    std::optional<phazite::ClipID> notes, audio;

    for (const auto track : phazite::getAllPlaylistTracks (context))
        for (const auto& clip : phazite::getClips (context, track))
            (clip.kind == phazite::ClipType::audio ? audio : notes) = clip.id;

    REQUIRE (notes.has_value());
    REQUIRE (audio.has_value());

    for (const auto theme : { phazite::ui::ThemeName::dark, phazite::ui::ThemeName::light, phazite::ui::ThemeName::highContrast })
    {
        const auto themeName = juce::String (theme == phazite::ui::ThemeName::dark    ? "dark"
                                             : theme == phazite::ui::ThemeName::light ? "light"
                                                                                      : "high contrast");
        CAPTURE (themeName.toStdString());
        lookAndFeel.setTheme (theme);
        phazite::ui::refreshTheme (screen.window);
        phazite::test::dispatchPendingMessages();
        const auto paint = [&themeName] (juce::Component& view, const juce::String& what)
        { phazite::test::checkPaints (view, what + ", " + themeName + " theme"); };

        // The whole window, in each layout and with each panel filling it.
        for (const auto* layout : { "view.layoutBeat", "view.layoutMix", "view.layoutFull", "view.maximizeLeftPanel",
                                    "view.maximizeLeftPanel", "view.maximizeEditor", "view.maximizePanel", "view.layoutFull" })
        {
            REQUIRE (screen.registry.invoke (layout));
            phazite::test::dispatchPendingMessages();
            paint (screen.shell, juce::String ("the window after ") + layout);
        }

        // Each view, the song's.
        paint (screen.showView<phazite::ui::Playlist> ("view.playlist"), "the playlist");
        paint (screen.showView<phazite::ui::MixerView> ("view.mixer"), "the mixer");
        context.getSongSelection().selectOnly (*notes);
        paint (screen.showView<phazite::ui::PianoRoll> ("view.pianoRoll"), "the piano roll");

        // Its controller lane, on each kind it draws apart: steps, the bend about its middle, program changes' marks.
        for (const auto& lane : { phazite::ControlLane { phazite::ControlKind::controller, 64, 1 },
                                  phazite::ControlLane { phazite::ControlKind::pitchWheel, 0, 1 },
                                  phazite::ControlLane { phazite::ControlKind::programChange, 0, 1 },
                                  phazite::ControlLane { phazite::ControlKind::aftertouch, 0, 1 } })
        {
            auto& pianoRoll = screen.showView<phazite::ui::PianoRoll> ("view.pianoRoll");
            pianoRoll.showLane (lane);
            paint (pianoRoll, "the piano roll's " + lane.getName() + " lane");
            pianoRoll.showLane (std::nullopt);
        }

        paint (screen.showView<phazite::ui::Inspector> ("view.toggleInspector"), "the inspector, a MIDI clip");
        context.getSongSelection().selectOnly (*audio);
        paint (screen.showView<phazite::ui::AudioClipEditor> ("view.pianoRoll"), "the audio clip editor");
        paint (screen.showView<phazite::ui::Inspector> ("view.toggleInspector"), "the inspector, an audio clip");
        paint (screen.showView<phazite::ui::Browser> ("view.browser"), "the browser");
        paint (screen.showView<phazite::ui::ProjectView> ("view.project"), "the project");
        paint (screen.showView<phazite::ui::VersionsView> ("view.versions"), "source control");

        // Each of Phazite's own devices in the Devices view, its editor beside the column.
        for (const auto device : devices)
            paint (screen.showInDevicesView (device), "the Devices view, " + phazite::getDeviceName (*phazite::findSong (context), device));

        // The whole window, at the smallest and the largest interface sizes (View > Interface Size).
        for (const auto scale : { 0.75f, 2.0f })
            phazite::test::checkPaints (screen.shell, "the window, " + themeName + " theme", scale);
    }
}
