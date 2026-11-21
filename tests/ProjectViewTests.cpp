// SPDX-License-Identifier: AGPL-3.0-or-later
// The Project view in the left panel (src/ui/project/ProjectView.h), in the whole window as the app builds it: its icon between
// the browser's and source control's, the song's notes typed into it, and the song's audio files and plug-ins listed in it.
#include "AppScreen.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContents.h"
#include "model/project/UnusedAudio.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"
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
    phazite::test::HeldClock clock; // which a burst's pause is measured by, and which moves only as the test says

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

    // A pause of a second and a half ends a burst too.
    screen.click (notes);
    notes.insertTextAtCaret ("Verse");
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::Gesture::isInProgress (screen.getSong()));
    clock.advance (1499.0);
    view.tickNow();
    CHECK (phazite::Gesture::isInProgress (screen.getSong()));
    clock.advance (2.0);
    view.tickNow();
    CHECK_FALSE (phazite::Gesture::isInProgress (screen.getSong()));
    CHECK (screen.getSong().getUndoManager().getUndoDescription() == juce::String ("Change Notes"));
    CHECK (phazite::getProjectNotes (context) == juce::String ("Verse"));
}

TEST_CASE ("The Project view lists the song's audio files, a missing one with its Locate, and the files nothing plays")
{
    Screen screen;
    REQUIRE (screen.registry.invoke ("project.save")); // a folder of its own, which the audio files are copied into
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.session.isUntitled(); }));
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
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return view.describeAudioFile (0) == "Kick.wav, missing: " + trackName; }));

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
    phazite::test::waitForAudioWork(); // the file found copied into the project in the background (known bug 123)
    view.refreshNow();
    REQUIRE (view.getNumAudioFiles() == 1);
    CHECK (view.describeAudioFile (0) == "Kick found.wav: " + trackName);
    CHECK (song.getUndoManager().getUndoDescription() == juce::String ("Locate Audio File"));

    // A file in the project's folder that nothing plays: how many, with Clean Up (once the versions are read, which play
    // files too).
    phazite::test::writeSound (song.getFile().getParentDirectory().getChildFile ("Audio/Old take.wav"), 0.5);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return phazite::canCleanUpAudio (screen.session.getContext()); }));
    view.refreshNow();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getCleanUpButton().isShowing(); })); // found in the background
    CHECK (view.getUnusedText().startsWith ("1 file"));
    CHECK (view.getCleanUpButton().isEnabled());

    // While a snapshot of Local History is written, Clean Up can't look: the button stays, greyed, saying why.
    phazite::VersionHistory::holdWritesForTests = true;
    phazite::getVersionHistory (screen.session.getContext())->takeSnapshot ("auto");
    view.refreshNow();
    CHECK (view.getCleanUpButton().isShowing());
    CHECK_FALSE (view.getCleanUpButton().isEnabled());
    CHECK (view.getCleanUpButton().getTooltip().contains ("being written"));
    phazite::VersionHistory::holdWritesForTests = false;
    REQUIRE (phazite::test::waitForVersionHistory (screen.session.getContext()));
    view.refreshNow();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getCleanUpButton().isEnabled(); }));
}

TEST_CASE ("The Project view asks the disk nothing on the message thread, and looks again only once something could have changed")
{
    // The audit of 2026-11-20: every five seconds while it showed, and after each edit, the view asked the disk on the message
    // thread whether each of the song's audio files was there, and started a fresh look for the unused files: on a network
    // share or a synced folder with hundreds of files, the window stalled every few seconds.
    Screen screen;
    REQUIRE (screen.registry.invoke ("project.save")); // a folder of its own, which the audio files are copied into
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.session.isUntitled(); }));
    auto& song = screen.getSong();
    const auto track = screen.addTrack();
    const auto sounds = screen.temp.folder.getChildFile ("Sounds");

    for (int i = 0; i < 12; ++i)
    {
        const phazite::UndoStep step (song, "Add File");
        const auto sound = phazite::test::writeSound (sounds.getChildFile ("Hit " + juce::String (i + 1) + ".wav"), 0.1);
        REQUIRE (phazite::importFile (song, track, sound, phazite::BeatPosition::fromBeats (4.0 * i)).isValid());
    }

    REQUIRE (phazite::test::waitForVersionHistory (screen.session.getContext()));
    auto& view = screen.showView();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return ! view.isLooking(); }));
    REQUIRE (view.getNumAudioFiles() == 12);
    const auto audio = song.getFile().getParentDirectory().getChildFile ("Audio");
    const auto queriesBefore = phazite::getProjectDiskQueriesOnMessageThread();

    // Phazite in front or not as the test says: the system's answer would change as other windows take the front.
    auto inFront = false;
    view.isInFront = [&inFront] { return inFront; };
    view.tickNow();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return ! view.isLooking(); }));

    // Nothing changes: the ticks read the model again, and ask nothing of the disk.
    for (int i = 0; i < 25; ++i)
    {
        view.tickNow();
        CHECK_FALSE (view.isLooking());
    }

    // A file gone: the next look, as the song changes, finds it missing; held there, as a drive that doesn't answer, the view
    // goes on with the song meanwhile, showing what it knew.
    REQUIRE (audio.getChildFile ("Hit 3.wav").deleteFile());
    const auto going = std::make_shared<std::atomic<bool>> (false);
    const auto held = std::make_shared<std::atomic<int>> (0);
    const juce::ScopeGuard letGo { [going] { *going = true; } };
    view.getMissingCheck().setBeforeChecking (
        [going, held] (const juce::File&)
        {
            ++*held;
            const auto since = juce::Time::getMillisecondCounterHiRes();

            while (! going->load() && juce::Time::getMillisecondCounterHiRes() - since < phazite::test::hangTimeout)
                juce::Thread::sleep (1);
        });

    phazite::setTrackName (song, track, "Hits");
    phazite::test::dispatchPendingMessages();
    view.tickNow();
    REQUIRE (phazite::test::dispatchMessagesUntil ([held] { return held->load() > 0; }));
    CHECK (view.isLooking());
    CHECK (view.describeAudioFile (2) == "Hit 3.wav: Hits"); // the name changed; missing not known yet
    *going = true;
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return ! view.isLooking(); }));
    CHECK (view.describeAudioFile (2) == "Hit 3.wav, missing: Hits");

    // The next look, held again: still shown missing meanwhile, and after.
    *going = false;
    *held = 0;
    view.refreshNow();
    REQUIRE (phazite::test::dispatchMessagesUntil ([held] { return held->load() > 0; }));
    CHECK (view.describeAudioFile (2) == "Hit 3.wav, missing: Hits");
    *going = true;
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return ! view.isLooking(); }));
    CHECK (view.describeAudioFile (2) == "Hit 3.wav, missing: Hits");
    CHECK (view.describeAudioFile (3) == "Hit 4.wav: Hits");
    view.getMissingCheck().setBeforeChecking (nullptr);

    // Phazite coming to the front again (the user back from Explorer): a look.
    inFront = true;
    view.tickNow();
    CHECK (view.isLooking());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return ! view.isLooking(); }));
    view.isInFront = nullptr;

    // getProjectAudioFiles(), read at every tick, asked the disk nothing.
    CHECK (phazite::getProjectDiskQueriesOnMessageThread() == queriesBefore);
}

TEST_CASE ("A project opened while its versions are read shows its own count of unused files once they are read, with no edit")
{
    // The audit's review, 2026-11-20: Clean Up can't look while the versions are read, so the view kept the project before's
    // count, and nothing looked again until an edit or a trip to another program.
    Screen screen;
    auto& context = screen.session.getContext();
    REQUIRE (screen.registry.invoke ("project.save"));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.session.isUntitled(); }));
    REQUIRE (phazite::test::waitForVersionHistory (context));
    phazite::test::writeSound (screen.getSong().getFile().getParentDirectory().getChildFile ("Audio/Old take.wav"), 0.5);
    auto& view = screen.showView();
    view.isInFront = [] { return false; };
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getUnusedText().startsWith ("1 file"); }));

    // Another project, with two files nothing plays, opened while its versions' first read is held.
    juce::File other;

    {
        juce::String error;
        const auto made = phazite::Project::create (screen.temp.folder.getChildFile ("Others"), "Other", error);
        REQUIRE (made != nullptr);
        other = made->getFile();
        phazite::test::writeSound (made->getAudioFolder().getChildFile ("Take 1.wav"), 0.2);
        phazite::test::writeSound (made->getAudioFolder().getChildFile ("Take 2.wav"), 0.2);
    }

    struct FirstReadHeld
    {
        FirstReadHeld() { phazite::VersionHistory::holdFirstReadsForTests = true; }
        ~FirstReadHeld() { letGo(); }
        void letGo() { phazite::VersionHistory::holdFirstReadsForTests = false; }
    } held;

    screen.session.openProject (other);
    REQUIRE (phazite::test::dispatchMessagesUntil ([] { return phazite::VersionHistory::firstReadsHeldForTests.load() >= 1; }));
    view.tickNow();
    CHECK_FALSE (phazite::canCleanUpAudio (context));
    CHECK_FALSE (view.getCleanUpButton().isVisible()); // no count: not the project before's

    // Read: the view's ticks find its own, with no edit and no trip elsewhere.
    held.letGo();
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&view]
        {
            view.tickNow();
            return view.getUnusedText().startsWith ("2 files");
        }));
    view.isInFront = nullptr;
}

TEST_CASE ("The Project view finds the unused audio in the background, reading each branch's song once until the versions change")
{
    // Known bug 129: every branch's song was read again on the message thread every few seconds and after each edit.
    Screen screen;
    auto& context = screen.session.getContext();
    auto& song = screen.getSong();
    REQUIRE (phazite::test::waitForVersionHistory (context));
    const auto main = phazite::getCurrentBranch (context)->id;
    REQUIRE (phazite::test::finished (context, [&]
                                      { return phazite::newBranch (context, {}, [] (std::function<void (bool)> then) { then (true); }); }));
    const auto track = screen.addTrack();
    REQUIRE (phazite::test::finished (
        context,
        [&] { return phazite::switchBranch (context, main, [] (std::function<void (bool)> then) { then (true); }); })); // a song left there
    phazite::test::writeSound (song.getFile().getParentDirectory().getChildFile ("Audio/Old take.wav"), 0.5);
    REQUIRE (phazite::test::waitForVersionHistory (context));

    const auto before = phazite::VersionStore::countSongsRead();
    auto& view = screen.showView();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getUnusedText().startsWith ("1 file"); }));
    const auto first = phazite::VersionStore::countSongsRead();
    CHECK (first.onMessageThread == before.onMessageThread);
    CHECK (first.all > before.all); // the song left on the other branch, read in the background

    // Read again, after edits and while time goes by: the branch's song isn't read again.
    for (int i = 0; i < 3; ++i)
    {
        phazite::setTrackName (song, track, "Track " + juce::String (i));
        phazite::test::dispatchPendingMessages();
        view.refreshNow();
    }

    phazite::test::writeSound (song.getFile().getParentDirectory().getChildFile ("Audio/Older take.wav"), 0.5);
    view.refreshNow();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&view] { return view.getUnusedText().startsWith ("2 files"); }));
    const auto then = phazite::VersionStore::countSongsRead();
    CHECK (then.onMessageThread == before.onMessageThread);
    CHECK (then.all == first.all);
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
