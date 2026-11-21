// SPDX-License-Identifier: AGPL-3.0-or-later
// The Project view in the left panel (src/ui/project/ProjectView.h), in the whole window as the app builds it: its icon between
// the browser's and source control's, the song's notes typed into it, and the song's audio files and plug-ins listed in it.
#include "AppScreen.h"
#include "TestPlugin.h"
#include "TestSong.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Import.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/engine/internal/AudioIO.h"
#include "model/engine/internal/EngineContext.h"
#include "model/midi/MidiLinkModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/project/ProjectContents.h"
#include "model/project/UnusedAudio.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"
#include "model/versions/internal/VersionStore.h"
#include "ui/components/Controls.h"
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

TEST_CASE ("MIDI learn the whole way: a control's MIDI Learn, a knob moved, the notice, the control turned, and the MIDI Links pane")
{
    Screen screen;
    auto& context = screen.session.getContext();
    auto& song = screen.getSong();
    const auto track = screen.addTrack();
    auto& audio = phazite::getAudioIO (phazite::test::getEngineContext());
    const auto send = [&audio] (int value)
    {
        audio.takeMidi (juce::MidiMessage::controllerEvent (1, 7, value));
        phazite::test::dispatchPendingMessages();
    };

    // The fader as a slider shows it, with its menu.
    juce::Slider fader;
    phazite::ui::ParameterAttachment attachment (fader);
    attachment.setParameter (phazite::getVolume (context, track));
    auto& view = screen.showView();
    CHECK (view.getNumLinks() == 0);
    CHECK (view.getEmptyLinksText() == "No MIDI links. Right-click a control and choose MIDI Learn.");

    // MIDI Learn: the notice says so, with Cancel, until a knob moves.
    REQUIRE (phazite::test::runMenuItem (attachment.getMenu(), { "MIDI Learn" }));
    auto& notices = screen.shell.getNotices();
    REQUIRE (notices.getNumNotices() == 1);
    CHECK (notices.getText (0) == "Move a knob or fader on your MIDI controller to link it to Volume.");
    REQUIRE (notices.getActionButton (0) != nullptr);
    CHECK (notices.getActionButton (0)->getButtonText() == "Cancel");
    CHECK (phazite::test::findMenuItem (attachment.getMenu(), { "Cancel MIDI Learn" }) != nullptr);

    send (100);
    REQUIRE (notices.getNumNotices() == 1);
    CHECK (notices.getText (0) == "CC 7 on channel 1 now turns Volume.");
    CHECK (phazite::test::findMenuItem (attachment.getMenu(), { "Remove MIDI Link (CC 7, Channel 1)" }) != nullptr);
    CHECK (phazite::test::findMenuItem (attachment.getMenu(), { "MIDI Learn" }) != nullptr);

    // The knob turns the fader, which the slider shows.
    send (0);
    CHECK (phazite::getVolume (context, track).getValue() == doctest::Approx (0.0f));
    attachment.update(); // as the display ticker has the control's owner update it
    CHECK (fader.getValue() == doctest::Approx (0.0));

    // The pane lists it; a click on its row selects its track, and its cross removes it.
    view.refreshNow();
    REQUIRE (view.getNumLinks() == 1);
    CHECK (view.describeLink (0) == "CC 7, Channel 1: " + phazite::getTrackName (song, track) + ": Mixer: Volume");
    auto& list = view.getLinkList();
    phazite::test::Mouse mouse (list);
    mouse.click (list.getRowPosition (0, true).withTrimmedRight (list.getRowPosition (0, true).getWidth() / 2).getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (context.getSongSelection().getTracks() == std::vector { track });
    list.selectRow (0);
    const auto remove = view.getRemovePosition (0);
    REQUIRE_FALSE (remove.isEmpty());
    mouse.click (remove.getCentre());
    phazite::test::dispatchPendingMessages();
    view.tickNow(); // read again after the click
    CHECK (view.getNumLinks() == 0);
    CHECK (phazite::describeUndo (&song) == "Undo Remove MIDI Link");

    // The notice's Cancel ends learning, and the notice with it.
    REQUIRE (phazite::test::runMenuItem (attachment.getMenu(), { "MIDI Learn" }));
    auto* cancel = notices.getActionButton (notices.getNumNotices() - 1);
    REQUIRE (cancel != nullptr);
    screen.click (*cancel);
    CHECK_FALSE (phazite::isLearningMidi (context));
    CHECK (phazite::test::runTimersUntil (
        [&notices] { return notices.getNumNotices() == 0 || notices.getText (notices.getNumNotices() - 1).startsWith ("CC"); }));

    // A macro's knob and a device's have it too.
    const auto macro = phazite::addModulator (context, track, phazite::ModulatorType::macro);
    REQUIRE (macro.isValid());
    juce::Slider macroKnob;
    phazite::ui::ParameterAttachment macroAttachment (macroKnob);
    macroAttachment.setParameter (
        phazite::getModulatorParameter (context, macro, phazite::getModulatorParameters (context, macro).front().id));
    CHECK (phazite::test::findMenuItem (macroAttachment.getMenu(), { "MIDI Learn" }) != nullptr);

    phazite::DeviceID synth;

    {
        const phazite::UndoStep step (song, "Add Device");
        synth = phazite::setInstrument (song, track, phazite::makeOwnDeviceState (song, phazite::devices::Synth::typeID));
    }

    juce::Slider cutoffKnob;
    phazite::ui::ParameterAttachment cutoffAttachment (cutoffKnob);
    cutoffAttachment.setParameter (phazite::getDeviceHandle (context, synth).getParameter ("cutoff"));
    CHECK (phazite::test::findMenuItem (cutoffAttachment.getMenu(), { "MIDI Learn" }) != nullptr);
}
