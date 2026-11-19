// SPDX-License-Identifier: AGPL-3.0-or-later
// The website's pictures of Phazite, for the home page of the phazite-site repository, which keeps them (its
// src/pictures/: docs/design/decisions.md says why they aren't kept here): the window as the app builds it
// (AppScreen.h), in its look, painted without a screen at three times its size, for the page to zoom into, each with an
// example song: the whole window as the README shows it (ReadmePicture.cpp), the playlist, the piano roll with chords and
// with drums, the mixer while the song plays, and each of Phazite's own devices as an example song sets it (or with a
// preset of its own, for those the songs don't use). Each is painted in the dark theme and in the light one ("-light"),
// as the site has both. ctest and CI skip them; after a change to how the window looks, run them from a build in the
// repository with
//   phazite_tests.exe --test-suite=site-pictures --no-skip
// with the environment variable PHAZITE_SITE_PICTURES naming the folder for them, the site's src/pictures. The home
// page's captions point at places in these pictures (phazite-site: src/pages/index.astro), so look at the page after
// painting them anew, and commit both there together.
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "RealTimePlayback.h"
#include "app/MainWindow.h"
#include "devices/core/Clock.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/TimelineModel.h"
#include "model/engine/EngineContext.h"
#include "model/mixer/MixerModel.h"
#include "model/presets/PresetModel.h"
#include "services/Library.h"
#include "ui/browser/Browser.h"
#include "ui/mixer/DevicesView.h"
#include "ui/mixer/Inspector.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/playlist/Playlist.h"
#include "ui/theme/LookAndFeel.h"

namespace
{
using namespace phazite::test;

// The main window's menus as its title bar shows them: their names only.
struct MenuNames : juce::MenuBarModel
{
    juce::StringArray getMenuBarNames() override { return phazite::getMainMenuNames(); }
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override { return {}; }
    void menuItemSelected (int, int) override {}
};

juce::File getRepository()
{
    return getGoldenFolder().getParentDirectory().getParentDirectory();
}

// A theme the website shows, and what its pictures' names end with.
struct Theme
{
    phazite::ui::ThemeName name;
    const char* ending;
};

const Theme themes[] = { { phazite::ui::ThemeName::dark, "" }, { phazite::ui::ThemeName::light, "-light" } };

// What the app has next to it and the tests have not, for as long as it lives: the look with Inter from the source tree,
// and a copy of Phazite's presets next to the test program, where the app looks for them as it makes an example song.
struct Look
{
    Look()
    {
        REQUIRE (lookAndFeel.hasInter());
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        presets.deleteRecursively();
        REQUIRE (getFactoryPresetsFolder().copyDirectoryTo (presets));
    }

    ~Look()
    {
        presets.deleteRecursively();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    // The theme of the windows made from now on, and of their pictures' names.
    void show (const Theme& theme)
    {
        lookAndFeel.setTheme (theme.name);
        ending = theme.ending;
    }

    const juce::File presets = phazite::Library::getDefaultFactoryPresetsFolder();
    phazite::ui::LookAndFeel lookAndFeel { getRepository().getChildFile ("resources/fonts/Inter") };
    juce::String ending;
};

// The window with an example song, as the main window shows it on Windows with an audio output open.
struct Stage
{
    Stage (const Look& lookToUse, const char* song, int height = 800)
        : look (lookToUse)
    {
        screen.window.setSize (1280, height);
        auto& titleBar = screen.shell.getTitleBar();
        titleBar.setMenus (&menus);
        titleBar.setWindowActions (phazite::ui::TitleBar::WindowActions { [] {}, [] {}, [] {}, [] { return false; } });

        // The song, opened from Help as a new project, with the renders of its clips that follow the tempo made.
        screen.session.openAtStartup();
        dispatchPendingMessages();
        REQUIRE (screen.registry.invoke ("help.example." + juce::String (song)));
        dispatchPendingMessages();
        REQUIRE (dispatchMessagesUntil ([this] { return phazite::getClipRenderProgress (context()).renders == 0; }, 60000));
    }

    phazite::ProjectContext& context() { return screen.session.getContext(); }

    phazite::TrackID track (const char* name)
    {
        for (const auto id : phazite::getAllPlaylistTracks (context()))
            if (phazite::getTrackRow (context(), id)->name == juce::String (name))
                return id;

        FAIL ("the song has no track named " << name);
        return {};
    }

    void run (const char* command)
    {
        INFO (command);
        REQUIRE (screen.registry.invoke (command));
        dispatchPendingMessages();
    }

    template <typename View>
    bool shows()
    {
        dispatchPendingMessages();
        auto* view = findAll<View> (screen.shell).getFirst();
        return view != nullptr && view->isShowing();
    }

    phazite::ui::Playlist& playlist()
    {
        auto* found = findAll<phazite::ui::Playlist> (screen.shell).getFirst();
        REQUIRE (found != nullptr);
        return *found;
    }

    // A part of the window, or all of it, into <name>.png (<name>-light.png in the light theme) in the pictures' folder, at
    // so many times its size: with the pointer out of the window, the status bar as with an open output, and what the
    // display's frames move where it is now.
    void write (const juce::String& name, juce::Component* part = nullptr, float scale = 3.0f)
    {
        Mouse (screen.window).moveTo ({ -100, -100 });
        phazite::EngineContext::AudioOutput output;
        output.isOpen = true;
        output.type = "Windows Audio";
        output.name = "Speakers";
        output.sampleRate = 48000.0;
        output.bufferSamples = 480;
        output.bufferSeconds = output.latencySeconds = 0.01;
        screen.shell.getStatusBar().showAudioOutput (output);
        screen.shell.getHintBar().setAlert ({}, nullptr);
        settle (300);
        screen.window.showFrame (phazite::devices::Clock::getMilliseconds() / 1000.0);

        auto& painted = part != nullptr ? *part : static_cast<juce::Component&> (screen.window);
        const auto picture = painted.createComponentSnapshot (painted.getLocalBounds(), true, scale);
        const juce::File folder (juce::SystemStats::getEnvironmentVariable ("PHAZITE_SITE_PICTURES", {}));
        REQUIRE_MESSAGE (folder.isDirectory(), "set PHAZITE_SITE_PICTURES to the folder for the pictures, phazite-site's src/pictures");
        const auto file = folder.getChildFile (name + look.ending + ".png");
        file.deleteFile();
        juce::FileOutputStream stream (file);
        REQUIRE (stream.openedOk());
        REQUIRE (juce::PNGImageFormat().writeImageToStream (picture, stream));
        const auto wrote =
            "Wrote " + file.getFullPathName() + " (" + juce::String (picture.getWidth()) + " x " + juce::String (picture.getHeight()) + ")";
        MESSAGE (wrote.toStdString());
    }

    // A device's editor, as the Devices view shows it in the lower panel.
    void writeDevice (const juce::String& typeName, phazite::DeviceID device)
    {
        auto& view = screen.showInDevicesView (device);
        write ("device-" + typeName.toLowerCase().replaceCharacter (' ', '-'), view.getShownBody(), 2.0f);
    }

    const Look& look;
    MenuNames menus;
    AppScreen screen;
};
} // namespace

// As the README's picture (ReadmePicture.cpp), which the app's own presets make a little different: the devices as the
// song sets them.
TEST_CASE ("The website's picture of the window" * doctest::test_suite ("site-pictures") * doctest::skip())
{
    Look look;

    for (const auto& theme : themes)
    {
        look.show (theme);
        Stage stage (look, "houseGroove");
        auto& context = stage.context();

        // More of the tracks in view: the drums' folder collapsed, and every track a step shorter.
        context.getSongSelection().selectOnly (stage.track ("Drums"));
        dispatchPendingMessages();
        stage.run ("track.collapse");
        context.getSongSelection().clear();
        dispatchPendingMessages();
        stage.run ("track.shorter");

        // The chords' clip selected and their synth in the lower panel, whose cutoff the lane under them automates.
        context.getSongSelection().selectOnly (phazite::getClips (context, stage.track ("Chords")).front().id);

        if (stage.shows<phazite::ui::Inspector>())
            stage.run ("view.toggleInspector");

        stage.screen.showView<phazite::ui::DevicesView> ("view.instrument");

        // The browser's instruments open, and Sounds, which offers Phazite Sounds' download, closed.
        auto* browser = findAll<phazite::ui::Browser> (stage.screen.shell).getFirst();
        REQUIRE (browser != nullptr);
        REQUIRE (browser->getTree().getRootItem() != nullptr);
        auto& places = *browser->getTree().getRootItem();

        for (int i = 0; i < places.getNumSubItems(); ++i)
        {
            auto& place = *places.getSubItem (i);

            if (place.getUniqueName().contains (":Instruments:"))
                place.setOpen (true);
            else if (place.getUniqueName().contains (":Sounds:"))
                place.setOpen (false);
        }

        // The playhead in the second drop, and the whole song in the playlist from its first track.
        phazite::movePlayhead (context, phazite::BeatPosition::fromBeats (168.0));
        settle (300);
        auto& playlist = stage.playlist();
        playlist.zoomToFit();
        Mouse mouse (playlist);

        for (int notch = 0; notch < 10; ++notch)
            mouse.wheel ({ playlist.getWidth() / 2, playlist.getHeight() - 30 }, 1.0f);

        stage.write ("window", nullptr, 2.0f);
    }
}

TEST_CASE ("The website's picture of the playlist" * doctest::test_suite ("site-pictures") * doctest::skip())
{
    Look look;

    for (const auto& theme : themes)
    {
        look.show (theme);
        Stage stage (look, "houseGroove");
        stage.run ("view.maximizeSong");

        // The playhead past the song's end, out of the picture: the page draws one of its own, which its scroll moves.
        phazite::movePlayhead (stage.context(), phazite::BeatPosition::fromBeats (400.0));
        dispatchPendingMessages();
        stage.playlist().zoomToFit();
        stage.write ("playlist");
    }
}

TEST_CASE ("The website's pictures of the piano roll" * doctest::test_suite ("site-pictures") * doctest::skip())
{
    Look look;

    for (const auto& theme : themes)
    {
        look.show (theme);

        {
            Stage stage (look, "loFiBeat");
            stage.context().getSongSelection().selectOnly (phazite::getClips (stage.context(), stage.track ("Keys")).front().id);
            stage.screen.showView<phazite::ui::PianoRoll> ("view.pianoRoll");
            stage.run ("view.maximizeEditor");
            stage.write ("piano-roll");
        }

        // Four bars of the drop's drums, a clip each.
        {
            Stage stage (look, "houseGroove");
            auto& selection = stage.context().getSongSelection();
            selection.clear();

            for (const auto& clip : phazite::getClips (stage.context(), stage.track ("Beat")))
                if (clip.range.getStart().inBeats() >= 64.0 && clip.range.getStart().inBeats() < 80.0)
                    selection.add (clip.id);

            stage.screen.showView<phazite::ui::PianoRoll> ("view.pianoRoll");
            stage.run ("view.maximizeEditor");
            stage.write ("piano-roll-drums");
        }
    }
}

TEST_CASE ("The website's picture of the mixer, while the song plays" * doctest::test_suite ("site-pictures") * doctest::skip())
{
    Look look;

    for (const auto& theme : themes)
    {
        look.show (theme);
        Stage stage (look, "houseGroove", 700);
        stage.context().getSongSelection().selectOnly (stage.track ("Chords"));
        stage.run ("view.mixer");
        stage.run ("view.maximizeSong");
        phazite::movePlayhead (stage.context(), phazite::BeatPosition::fromBeats (64.0)); // the drop

        RealTimePlayback playback;
        stage.run ("transport.playStop");
        REQUIRE (playback.waitForBlocks (700)); // long enough for the loudness meter's short-term reading
        stage.write ("mixer");
        stage.run ("transport.playStop");
    }
}

TEST_CASE ("The website's pictures of Phazite's devices" * doctest::test_suite ("site-pictures") * doctest::skip())
{
    Look look;

    for (const auto& theme : themes)
    {
        look.show (theme);
        juce::StringArray painted;

        // Each as the first example song that has it sets it.
        for (const auto* song : { "houseGroove", "loFiBeat", "exodus" })
        {
            Stage stage (look, song);
            juce::StringArray own;

            for (const auto& type : phazite::getDeviceTypes (stage.context()))
                if (type.isBuiltIn)
                    own.add (type.name);

            const auto channels = phazite::getMixerChannels (stage.context());
            auto tracks = channels.tracks;
            tracks.insert (tracks.end(), channels.returns.begin(), channels.returns.end());
            tracks.push_back ({}); // the master

            for (const auto track : tracks)
            {
                auto devices = phazite::getEffects (stage.context(), track);

                if (const auto instrument = phazite::getInstrument (stage.context(), track))
                    devices.push_back (*instrument);

                for (const auto& device : devices)
                {
                    if (device.isContainer || ! own.contains (device.name) || painted.contains (device.name))
                        continue;

                    stage.context().getSongSelection().selectOnly (track);
                    stage.writeDevice (device.name, device.id);
                    painted.add (device.name);
                }
            }
        }

        // The others on a track of their own, each with the first of its presets.
        Stage stage (look, "houseGroove");
        const auto types = phazite::getDeviceTypes (stage.context());
        const auto devices = stage.screen.addEveryOwnDevice();
        size_t next = 0;

        for (const auto& type : types)
        {
            if (! type.isBuiltIn)
                continue;

            const auto device = devices[next++];

            if (painted.contains (type.name))
                continue;

            auto presets = getFactoryPresetsFolder().getChildFile (type.name).findChildFiles (
                juce::File::findFiles, true, juce::String ("*") + phazite::presetExtension);
            presets.sort();
            INFO (type.name);
            REQUIRE (! presets.isEmpty());
            REQUIRE (phazite::loadPresetInto (stage.context(), device, presets.getFirst()));
            dispatchPendingMessages();
            stage.writeDevice (type.name, device);
            painted.add (type.name);
        }

        MESSAGE (("Painted " + painted.joinIntoString (", ")).toStdString());
    }
}
