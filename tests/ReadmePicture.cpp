// SPDX-License-Identifier: AGPL-3.0-or-later
// The README's picture of the window (docs/images/phazite.png): the window as the app builds it (AppScreen.h), at its
// default size and in its look, painted without a screen at twice its size, for sharp screens. Inter comes from the source
// tree, as the tests have no fonts next to them. It shows the House Groove example song: the whole song in the playlist from
// its first track, the drums' folder collapsed and every track a step shorter so that more of them show, the chords' synth
// in the dock, Phazite's instruments in the browser, and the audio output as an open device shows it (the tests open none).
// ctest and CI skip it; after a change to how the window looks, run it from a build in the repository with
//   phazite_tests.exe --test-suite=readme-picture --no-skip
// then tools\social-preview.ps1, which draws the picture GitHub shows for links to the repository from it.
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "app/MainWindow.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/TimelineModel.h"
#include "model/engine/EngineContext.h"
#include "ui/browser/Browser.h"
#include "ui/mixer/DevicesView.h"
#include "ui/mixer/Inspector.h"
#include "ui/playlist/Playlist.h"
#include "ui/theme/LookAndFeel.h"

namespace
{
// The main window's menus as its title bar shows them: their names only.
struct MenuNames : juce::MenuBarModel
{
    juce::StringArray getMenuBarNames() override { return phazite::getMainMenuNames(); }
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override { return {}; }
    void menuItemSelected (int, int) override {}
};

phazite::TrackID findTrack (const phazite::ProjectContext& context, const char* name)
{
    for (const auto track : phazite::getAllPlaylistTracks (context))
        if (phazite::getTrackRow (context, track)->name == juce::String (name))
            return track;

    FAIL ("House Groove has no track named " << name);
    return {};
}

template <typename ViewType>
bool isShown (juce::Component& shell)
{
    phazite::test::dispatchPendingMessages();
    auto* view = phazite::test::findAll<ViewType> (shell).getFirst();
    return view != nullptr && view->isShowing();
}
} // namespace

TEST_CASE ("The README's picture of the window" * doctest::test_suite ("readme-picture") * doctest::skip())
{
    const auto repository = phazite::test::getGoldenFolder().getParentDirectory().getParentDirectory();
    phazite::ui::LookAndFeel lookAndFeel (repository.getChildFile ("resources/fonts/Inter"));
    REQUIRE (lookAndFeel.hasInter());
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
    const juce::ScopeGuard noLook { [] { juce::LookAndFeel::setDefaultLookAndFeel (nullptr); } };

    // The title bar as the main window's on Windows: its menus and its own window buttons.
    MenuNames menus;
    phazite::test::AppScreen screen;
    auto& titleBar = screen.shell.getTitleBar();
    titleBar.setMenus (&menus);
    titleBar.setWindowActions (phazite::ui::TitleBar::WindowActions { [] {}, [] {}, [] {}, [] { return false; } });

    // House Groove, opened from Help as a new project, with the renders of its clips that follow the tempo made.
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.registry.invoke ("help.example.houseGroove"));
    phazite::test::dispatchPendingMessages();
    auto& context = screen.session.getContext();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&context] { return phazite::getClipRenderProgress (context).renders == 0; }, 60000));

    // More of the tracks in view: the drums' folder collapsed, and every track a step shorter.
    context.getSongSelection().selectOnly (findTrack (context, "Drums"));
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.registry.invoke ("track.collapse"));
    context.getSongSelection().clear();
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.registry.invoke ("track.shorter"));

    // The chords' clip selected and their synth in the dock, whose cutoff the lane under the chords automates; no inspector.
    context.getSongSelection().selectOnly (phazite::getClips (context, findTrack (context, "Chords")).front().id);

    if (isShown<phazite::ui::Inspector> (screen.shell))
        REQUIRE (screen.registry.invoke ("view.toggleInspector"));

    REQUIRE (screen.registry.invoke ("view.instrument"));

    if (! isShown<phazite::ui::DevicesView> (screen.shell)) // the command hides the view where it showed already
        REQUIRE (screen.registry.invoke ("view.instrument"));

    REQUIRE (isShown<phazite::ui::DevicesView> (screen.shell));

    // The browser's instruments open, and Phazite Sounds, which offers its download, closed.
    auto* browser = phazite::test::findAll<phazite::ui::Browser> (screen.shell).getFirst();
    REQUIRE (browser != nullptr);
    REQUIRE (browser->getTree().getRootItem() != nullptr);
    auto& places = *browser->getTree().getRootItem();

    for (int i = 0; i < places.getNumSubItems(); ++i)
    {
        auto& place = *places.getSubItem (i);

        if (place.getUniqueName().contains (":Instruments:"))
            place.setOpen (true);
        else if (place.getUniqueName().contains (":Phazite Sounds:"))
            place.setOpen (false);
    }

    // The playhead in the second drop, and the whole song in the playlist from its first track. The pointer then leaves the
    // window, so that the hint bar shows the guide's tip rather than what is under it.
    phazite::movePlayhead (context, phazite::BeatPosition::fromBeats (168.0));
    phazite::test::settle (300);
    auto* playlist = phazite::test::findAll<phazite::ui::Playlist> (screen.shell).getFirst();
    REQUIRE (playlist != nullptr);
    playlist->zoomToFit();
    phazite::test::Mouse mouse (*playlist);

    for (int notch = 0; notch < 10; ++notch)
        mouse.wheel ({ playlist->getWidth() / 2, playlist->getHeight() - 30 }, 1.0f);

    phazite::test::Mouse (screen.window).moveTo ({ -100, -100 });
    phazite::test::settle (300);

    // The audio output as the status bar shows an open one, without the alert the hint bar shows while none is.
    phazite::EngineContext::AudioOutput output;
    output.isOpen = true;
    output.type = "Windows Audio";
    output.name = "Speakers";
    output.sampleRate = 48000.0;
    output.bufferSamples = 480;
    output.bufferSeconds = output.latencySeconds = 0.01;
    screen.shell.getStatusBar().showAudioOutput (output);
    screen.shell.getHintBar().setAlert ({}, nullptr);
    phazite::test::settle (300);

    const auto picture = screen.window.createComponentSnapshot (screen.window.getLocalBounds(), true, 2.0f);
    const auto file = repository.getChildFile ("docs/images/phazite.png");
    REQUIRE (file.deleteFile());
    juce::FileOutputStream stream (file);
    REQUIRE (stream.openedOk());
    REQUIRE (juce::PNGImageFormat().writeImageToStream (picture, stream));
    const auto wrote =
        "Wrote " + file.getFullPathName() + " (" + juce::String (picture.getWidth()) + " x " + juce::String (picture.getHeight()) + ")";
    MESSAGE (wrote.toStdString());
}
