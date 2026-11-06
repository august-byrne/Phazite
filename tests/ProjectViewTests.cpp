// SPDX-License-Identifier: AGPL-3.0-or-later
// The Project view in the left panel (src/ui/project/ProjectView.h), in the whole window as the app builds it: its icon between
// the browser's and source control's, the song's notes typed into it, and the song's audio files and plug-ins listed in it.
#include "AppScreen.h"
#include "TestPlugin.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContents.h"
#include "model/project/UnusedAudio.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/project/ProjectView.h"
#include "ui/shell/ActivityBar.h"

namespace
{
using phazite::ui::LeftView;
using phazite::ui::Panel;

struct Screen : public phazite::test::AppScreen
{
    Screen()
    {
        session.openAtStartup();
        phazite::test::dispatchPendingMessages();
    }

    phazite::ui::ActivityBar& getBar()
    {
        const auto bars = phazite::test::findAll<phazite::ui::ActivityBar> (shell);
        REQUIRE (bars.size() == 1);
        return *bars[0];
    }

    // A track, as the app's command adds one (a new project starts without any).
    phazite::TrackID addTrack()
    {
        REQUIRE (registry.invoke ("track.add"));
        const auto tracks = phazite::getTrackIDs (*findSong (session.getContext()));
        REQUIRE_FALSE (tracks.empty());
        return tracks.back();
    }

    phazite::ui::ProjectView& getView()
    {
        const auto views = phazite::test::findAll<phazite::ui::ProjectView> (shell);
        REQUIRE (views.size() == 1);
        return *views[0];
    }

    // The Project view in the left panel, with what the song holds now.
    phazite::ui::ProjectView& showView()
    {
        if (! (shell.getPanel (Panel::left).isShowing() && shell.getPanelLayout().leftView == LeftView::project))
            REQUIRE (registry.invoke ("view.project"));

        phazite::test::dispatchPendingMessages();
        auto& view = getView();
        REQUIRE (view.isShowing());
        view.refreshNow();
        return view;
    }

    void click (juce::Component& component)
    {
        phazite::test::Mouse mouse (component);
        mouse.click (component.getLocalBounds().getCentre());
        phazite::test::dispatchPendingMessages();
    }

    phazite::Song& getSong() { return *findSong (session.getContext()); }
};
} // namespace

TEST_CASE ("The Project view's icon sits between the browser's and source control's, and shows or hides the view")
{
    Screen screen;
    auto& bar = screen.getBar();
    CHECK (bar.getButton (LeftView::browser).getY() < bar.getButton (LeftView::project).getY());
    CHECK (bar.getButton (LeftView::project).getY() < bar.getButton (LeftView::versions).getY());
    CHECK (bar.getButton (LeftView::project).getTitle() == juce::String ("Project"));

    // Its icon shows it, marked, with its title along the top; again, it hides the panel.
    screen.click (bar.getButton (LeftView::project));
    CHECK (screen.getView().isShowing());
    CHECK (bar.isMarked (LeftView::project));
    CHECK_FALSE (bar.isMarked (LeftView::browser));
    const auto titles = phazite::test::findAll<phazite::ui::ViewTitle> (screen.getView());
    REQUIRE (titles.size() == 1);
    CHECK (titles[0]->getName() == juce::String ("Project"));
    CHECK (screen.services.settings.get (phazite::Settings::panelLayout).contains ("leftView=project"));

    screen.click (bar.getButton (LeftView::project));
    CHECK_FALSE (screen.shell.getPanel (Panel::left).isShowing());
    CHECK_FALSE (bar.isMarked (LeftView::project));
}

TEST_CASE ("Notes typed in the Project view go into the song, a burst of typing one undo step, and show what an undo leaves")
{
    Screen screen;
    auto& view = screen.showView();
    auto& notes = view.getNotesField();
    auto& context = screen.session.getContext();

    // Typed, in the song at once (a save would have them).
    screen.click (notes);
    REQUIRE (notes.hasKeyboardFocus (true));
    notes.insertTextAtCaret ("Bridge: try a key change");
    phazite::test::dispatchPendingMessages(); // a text field tells of its change after it
    CHECK (phazite::getProjectNotes (context) == juce::String ("Bridge: try a key change"));

    // More typed in the same burst, which a click elsewhere ends: one step.
    notes.insertTextAtCaret (" up");
    phazite::test::dispatchPendingMessages();
    screen.click (screen.shell.getPanel (Panel::song));
    CHECK (screen.getSong().getUndoManager().getUndoDescription() == juce::String ("Change Notes"));
    phazite::undo (screen.getSong());
    CHECK (phazite::getProjectNotes (context).isEmpty());

    // What the undo left shows in the field.
    phazite::test::dispatchPendingMessages();
    CHECK (notes.getText().isEmpty());
}

TEST_CASE ("The Project view lists the song's audio files, a missing one with its Locate, and the files nothing plays")
{
    Screen screen;
    REQUIRE (screen.registry.invoke ("project.save")); // a folder of its own, which the audio files are copied into
    auto& song = screen.getSong();
    const auto track = screen.addTrack();
    const auto sounds = screen.temp.folder.getChildFile ("Sounds");
    const auto kick = phazite::test::writeSound (sounds.getChildFile ("Kick.wav"), 0.5);

    {
        const phazite::UndoStep step (song, "Add File");
        REQUIRE (phazite::importFile (song, track, kick, phazite::BeatPosition()).isValid());
    }

    auto& view = screen.showView();
    REQUIRE (view.getNumAudioFiles() == 1);
    const auto trackName = phazite::getTrackName (song, track);
    CHECK (view.describeAudioFile (0) == "Kick.wav: " + trackName);
    CHECK (view.getLocatePosition (0).isEmpty()); // a file that is there has none

    // Its copy gone from the project's folder: missing, its Locate under the pointer; what is chosen takes its place.
    const auto copy = song.getFile().getParentDirectory().getChildFile ("Audio/Kick.wav");
    REQUIRE (copy.deleteFile());
    view.refreshNow();
    CHECK (view.describeAudioFile (0) == "Kick.wav, missing: " + trackName);

    auto& list = view.getAudioList();
    phazite::test::Mouse mouse (list);
    mouse.moveTo (list.getRowPosition (0, true).getCentre());
    const auto locate = view.getLocatePosition (0);
    REQUIRE_FALSE (locate.isEmpty());

    const auto found = phazite::test::writeSound (sounds.getChildFile ("Kick found.wav"), 0.5);
    view.chooseReplacement = [&found] (const phazite::ProjectAudioFile& missing, std::function<void (const juce::File&)> put)
    {
        CHECK (missing.path == juce::String ("Audio/Kick.wav"));
        put (found);
    };

    mouse.click (locate.getCentre());
    REQUIRE (view.getNumAudioFiles() == 1);
    CHECK (view.describeAudioFile (0) == "Kick found.wav: " + trackName);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Locate Audio File"));

    // A file in the project's folder that nothing plays: how many, with Clean Up (once the versions are read, which play
    // files too).
    phazite::test::writeSound (song.getFile().getParentDirectory().getChildFile ("Audio/Old take.wav"), 0.5);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return phazite::canCleanUpAudio (screen.session.getContext()); }));
    view.refreshNow();
    CHECK (view.getCleanUpButton().isShowing());
    CHECK (view.getUnusedText().startsWith ("1 file"));
}

TEST_CASE ("The Project view lists the song's plug-ins on their tracks, and a click on one selects it and shows it")
{
    phazite::test::TestPlugin plugin; // the host knows it, before the songs that hold it

    {
        Screen screen;
        auto& song = screen.getSong();
        const auto track = screen.addTrack();
        phazite::DeviceID device;

        {
            const phazite::UndoStep step (song, "Add Device");
            device = phazite::test::TestPlugin::add (song, track);
        }

        auto& view = screen.showView();
        REQUIRE (view.getNumPlugins() == 1);
        CHECK (view.describePlugin (0) == "Test Effect: " + phazite::getTrackName (song, track));
        CHECK (view.getReloadPosition (0).isEmpty()); // it plays

        auto& list = view.getPluginList();
        phazite::test::Mouse mouse (list);
        mouse.click (list.getRowPosition (0, true).getCentre());
        phazite::test::dispatchPendingMessages();
        const auto selected = screen.session.getContext().getSongSelection().getDevices();
        REQUIRE (selected.size() == 1);
        CHECK (selected.front() == device);
    }
}
