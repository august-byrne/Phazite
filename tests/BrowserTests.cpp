// SPDX-License-Identifier: AGPL-3.0-or-later
// The browser and the plug-in picker, driven as the user would: places and folders, previews, search, favorites, and
// putting items into the song with Enter, a drop or the picker (roadmap step 1.5).
#include "SoundPackTestHelpers.h"
#include "UiTestHelpers.h"
#include "model/engine/Preview.h"
#include "model/mixer/Mixer.h"
#include "model/transport/Playback.h"
#include "services/Library.h"
#include "ui/browser/Browser.h"
#include "ui/browser/PluginPicker.h"
#include "ui/playlist/Playlist.h"

namespace
{
const juce::KeyPress downKey (juce::KeyPress::downKey), returnKey (juce::KeyPress::returnKey), escapeKey (juce::KeyPress::escapeKey);

// Phazite Sounds, installed in a folder of packs: a few sounds, and the list that names the pack.
juce::File writeSoundPack (const juce::File& packs)
{
    const auto folder = packs.getChildFile ("Phazite Sounds");
    phazite::test::writeSound (folder.getChildFile ("Drums/Kick.wav"), 0.3);
    phazite::test::writeSound (folder.getChildFile ("Drums/Snare.wav"), 0.3);
    phazite::test::writeSound (folder.getChildFile ("Loops/Break 120 BPM.wav"), 4.0);
    folder.getChildFile ("sources.json").replaceWithText (R"({ "pack": "Phazite Sounds", "version": 1 })");
    return packs;
}

// The rows of a browser's tree, as screen readers read them.
juce::StringArray getRows (phazite::ui::Browser& browser)
{
    juce::StringArray rows;
    auto& tree = browser.getTree();

    for (int i = 0; i < tree.getNumRowsInTree(); ++i)
        rows.add (tree.getItemOnRow (i)->getAccessibilityName());

    return rows;
}

// Where the first row of that name is, in the tree.
juce::Rectangle<int> getRow (phazite::ui::Browser& browser, const juce::String& name)
{
    auto& tree = browser.getTree();

    for (int i = 0; i < tree.getNumRowsInTree(); ++i)
        if (auto* item = tree.getItemOnRow (i); item->getAccessibilityName().upToFirstOccurrenceOf (",", false, false) == name)
            return item->getItemPosition (true).withHeight (item->getItemHeight()); // without the rows under it

    FAIL_CHECK ("no row called " << name);
    return {};
}

// A browser in a window, over that library, with a project to put sounds into. Enter and double-clicks put them into
// placed.
struct BrowserScreen
{
    BrowserScreen()
    {
        browser.onPlace = [this] (const phazite::LibraryItem& item)
        {
            placed.push_back (item);
            return true;
        };

        REQUIRE (phazite::test::runTimersUntil ([this] { return ! library.isIndexing(); }, 10000));
        phazite::test::dispatchPendingMessages();
    }

    ~BrowserScreen() { previewer.stop(); }

    juce::StringArray getRows() { return ::getRows (browser); }
    juce::Rectangle<int> getRow (const juce::String& name) { return ::getRow (browser, name); }

    phazite::test::TemporaryFolder temp;
    phazite::SoundPack soundPack { writeSoundPack (temp.folder) };
    std::unique_ptr<te::Edit> edit = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    phazite::Settings settings;
    phazite::Library library { phazite::test::getEngine(), settings, soundPack.getFolder() };
    phazite::Previewer previewer { phazite::test::getEngine() };
    phazite::ui::Browser browser { library, previewer, soundPack, settings, [this] { return edit.get(); } };
    phazite::test::TestWindow window { browser, 320, 700 };
    std::vector<phazite::LibraryItem> placed;
};
} // namespace

TEST_CASE ("The browser shows its places, plays a sound when clicked, and Enter puts it into the song")
{
    BrowserScreen screen;

    // Phazite's sounds are open at first.
    CHECK (screen.getRows()
           == juce::StringArray { "Favorites", "Recent", "Sounds by Type", "Phazite Sounds", "Drums", "Loops", "Instruments", "Effects",
                                  "Add Folder..." });

    // Sounds by Type lists every kick, from every library.
    phazite::test::Mouse mouse (screen.browser.getTree());
    mouse.click (screen.getRow ("Sounds by Type").getCentre());
    phazite::test::dispatchPendingMessages();
    mouse.click (screen.getRow ("Kicks").getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getRows().contains ("Kick"));
    mouse.click (screen.getRow ("Sounds by Type").getCentre()); // closes it again
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (screen.getRows().contains ("Kicks"));

    // One click opens a folder; a click on a sound plays it.
    mouse.click (screen.getRow ("Drums").getCentre());
    phazite::test::dispatchPendingMessages(); // the tree shows the rows of a folder it opens
    CHECK (screen.getRows().contains ("Kick"));

    mouse.click (screen.getRow ("Kick").getCentre());
    CHECK (screen.previewer.getFile().getFileName() == juce::String ("Kick.wav"));
    REQUIRE (screen.browser.getSelectedItem().has_value());
    CHECK (screen.browser.getSelectedItem()->name == juce::String ("Kick"));

    // The arrow keys move through the sounds, and play each.
    screen.window.pressKey (downKey);
    CHECK (screen.previewer.getFile().getFileName() == juce::String ("Snare.wav"));

    // Enter puts the sound into the song, and Recent lists it.
    screen.window.pressKey (returnKey);
    REQUIRE (screen.placed.size() == 1);
    CHECK (screen.placed[0].name == juce::String ("Snare"));
    REQUIRE (! screen.library.getRecent().empty());
    CHECK (screen.library.getRecent()[0].name == juce::String ("Snare"));

    // Esc stops the sound.
    screen.window.pressKey (escapeKey);
    CHECK_FALSE (screen.previewer.isPlaying());

    // A folder's sound dragged into the song and dropped there is noted too.
    auto kick = screen.library.findItem (screen.soundPack.getFolder().getChildFile ("Drums/Kick.wav").getFullPathName());
    REQUIRE (kick.has_value());
    const auto description = phazite::toDragDescription (*kick);
    screen.browser.dragOperationEnded ({ description, &screen.browser, {} }); // dropped where nothing took it
    CHECK (screen.library.getRecent()[0].name == juce::String ("Snare"));
    phazite::markPlaced (description);
    screen.browser.dragOperationEnded ({ description, &screen.browser, {} });
    CHECK (screen.library.getRecent()[0].name == juce::String ("Kick"));
}

TEST_CASE ("The browser's search finds sounds and devices, and the star keeps favorites")
{
    BrowserScreen screen;
    auto& search = screen.browser.getSearchField();

    search.setText ("break", true);
    phazite::test::dispatchPendingMessages(); // a text field tells of changes asynchronously
    CHECK (screen.getRows() == juce::StringArray { "Break 120 BPM, 120 BPM" });

    search.setText ("reverb", true);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getRows() == juce::StringArray { "Reverb, Effect" });

    search.setText ("nothing is called this", true);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getRows() == juce::StringArray { "Nothing matches \"nothing is called this\"" });

    // Without a search, the places are back as they were.
    search.setText ({}, true);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getRows().contains ("Drums"));

    // A click on the far right of a row selects it, but doesn't star it.
    phazite::test::Mouse mouse (screen.browser.getTree());
    mouse.click (screen.getRow ("Drums").getCentre());
    phazite::test::dispatchPendingMessages(); // the tree shows the rows of a folder it opens
    auto kick = screen.getRow ("Kick");
    mouse.click ({ kick.getRight() - 12, kick.getCentreY() });
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.browser.getSelectedItem().has_value());
    CHECK (screen.browser.getSelectedItem()->name == juce::String ("Kick"));
    CHECK_FALSE (screen.library.isFavorite (*screen.browser.getSelectedItem()));

    // Its star then makes it a favorite, which Favorites shows.
    mouse.click ({ kick.getRight() - 12, kick.getCentreY() });
    phazite::test::dispatchPendingMessages();
    CHECK (screen.library.isFavorite (*screen.browser.getSelectedItem()));

    mouse.click (screen.getRow ("Favorites").getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getRows()[1] == juce::String ("Kick, favorite"));

    // Its star there takes it out again.
    kick = screen.getRow ("Kick");
    mouse.click ({ kick.getRight() - 12, kick.getCentreY() });
    phazite::test::dispatchPendingMessages();
    CHECK (screen.library.getFavorites().empty());
    CHECK (screen.getRows()[1].startsWith ("Nothing yet"));
}

TEST_CASE ("The plug-in picker finds a device as its name is typed, and Enter adds it")
{
    phazite::Settings settings;
    phazite::Library library (phazite::test::getEngine(), settings, {});
    std::optional<phazite::LibraryItem> chosen;
    phazite::ui::PluginPicker picker (library, [&chosen] (const phazite::LibraryItem& item) { chosen = item; });
    phazite::test::TestWindow window (picker, 520, 400);

    // Instruments, then effects.
    const auto& all = picker.getResults();
    REQUIRE (all.size() >= 2);
    CHECK (all.front().kind == phazite::LibraryItem::Kind::instrument);
    CHECK (all.back().kind == phazite::LibraryItem::Kind::effect);

    auto& search = picker.getSearchField();
    search.grabKeyboardFocus();
    search.setText ("rev", true);
    phazite::test::dispatchPendingMessages(); // a text field tells of changes asynchronously
    REQUIRE (! picker.getResults().empty());
    CHECK (picker.getResults()[0].name == juce::String ("Reverb"));

    window.pressKey (returnKey);
    phazite::test::dispatchPendingMessages();
    REQUIRE (chosen.has_value());
    CHECK (chosen->name == juce::String ("Reverb"));

    // Nothing to pick, nothing picked.
    chosen.reset();
    search.setText ("nothing is called this", true);
    phazite::test::dispatchPendingMessages();
    CHECK (picker.getResults().empty());
    window.pressKey (returnKey);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (chosen.has_value());
}

TEST_CASE ("Items from the browser go onto the playlist: dropped on a track or below the tracks, or at the play start")
{
    auto& engine = phazite::test::getEngine();
    auto edit = te::Edit::createSingleTrackEdit (engine); // 120 BPM, 4/4
    te::SelectionManager selection (engine);
    selection.edit = edit.get();
    phazite::RangeSelection range;
    phazite::CommandRegistry registry;
    phazite::ui::Playlist playlist (registry, selection, range);
    phazite::test::TestWindow window (playlist, 1000, 600);
    playlist.setEdit (edit.get());
    phazite::test::dispatchPendingMessages();

    auto* lanes = phazite::test::findComponentWithTitle (playlist, "Tracks");
    REQUIRE (lanes != nullptr);
    auto* track = te::getAudioTracks (*edit)[0];
    juce::DragAndDropTarget& target = playlist;

    // A sound dropped on the first track at beat 4.
    phazite::test::TemporaryFolder temp;
    phazite::LibraryItem kick;
    kick.name = "Kick";
    kick.file = phazite::test::writeSound (temp.folder.getChildFile ("Kick.wav"), 0.3);
    const auto sound = phazite::toDragDescription (kick);
    const auto beat4 = juce::roundToInt (playlist.getView().beatToX (te::BeatPosition::fromBeats (4.0)));
    const juce::DragAndDropTarget::SourceDetails onTrack (sound, &playlist, playlist.getLocalPoint (lanes, juce::Point<int> (beat4, 10)));

    REQUIRE (target.isInterestedInDragSource (onTrack));
    target.itemDropped (onTrack);
    REQUIRE (track->getClips().size() == 1);
    CHECK (track->getClips()[0]->getStartBeat().inBeats() == doctest::Approx (4.0));
    CHECK (phazite::wasPlaced (sound));

    // An instrument dropped below the tracks gets a track of its own.
    phazite::LibraryItem synth;
    synth.kind = phazite::LibraryItem::Kind::instrument;
    synth.device = phazite::getDeviceTypes (engine).at (0);
    synth.name = synth.device->name;
    REQUIRE (synth.device->isInstrument);
    const auto device = phazite::toDragDescription (synth);
    const juce::DragAndDropTarget::SourceDetails below (device, &playlist, playlist.getLocalPoint (lanes, juce::Point<int> (beat4, 500)));

    REQUIRE (target.isInterestedInDragSource (below));
    target.itemDropped (below);
    REQUIRE (te::getAudioTracks (*edit).size() == 2);
    CHECK (phazite::getInstrument (*te::getAudioTracks (*edit)[1]) != nullptr);
    CHECK (phazite::wasPlaced (device));

    // A loop that knows its tempo follows the song's, as it played in the preview: two bars at 120 BPM span two bars
    // at 100.
    edit->tempoSequence.getTempos()[0]->setBpm (100.0);
    phazite::LibraryItem breakLoop;
    breakLoop.name = "Break 120 BPM";
    breakLoop.file = phazite::test::writeSound (temp.folder.getChildFile ("Break 120 BPM.wav"), 4.0);
    const auto beat16 = juce::roundToInt (playlist.getView().beatToX (te::BeatPosition::fromBeats (16.0)));
    target.itemDropped (
        { phazite::toDragDescription (breakLoop), &playlist, playlist.getLocalPoint (lanes, juce::Point<int> (beat16, 10)) });
    REQUIRE (track->getClips().size() == 2);
    auto* loopClip = dynamic_cast<te::WaveAudioClip*> (track->getClips().getLast());
    REQUIRE (loopClip != nullptr);
    CHECK (loopClip->getAutoTempo());
    CHECK (loopClip->getStartBeat().inBeats() == doctest::Approx (16.0));
    CHECK (loopClip->getLengthInBeats().inBeats() == doctest::Approx (8.0));

    // Enter in the browser puts a sound on the selected track, where Space plays from.
    phazite::movePlayhead (*edit, edit->tempoSequence.toTime (te::BeatPosition::fromBeats (8.0)));
    selection.selectOnly (track);
    REQUIRE (playlist.placeFile (kick.file));
    REQUIRE (track->getClips().size() == 3);
    CHECK (std::any_of (track->getClips().begin(), track->getClips().end(),
                        [] (te::Clip* clip) { return std::abs (clip->getStartBeat().inBeats() - 8.0) < 0.001; }));

    playlist.setEdit (nullptr);
    selection.deselectAll();
}

TEST_CASE ("Until Phazite Sounds are installed, the browser offers them, and lists them once a click has downloaded them")
{
    phazite::test::PackRelease source;
    phazite::test::TemporaryFolder packs;
    phazite::SoundPack soundPack (packs.folder, source.release);
    phazite::Settings settings;
    phazite::Library library (phazite::test::getEngine(), settings, soundPack.getFolder());
    phazite::Previewer previewer (phazite::test::getEngine());
    phazite::ui::Browser browser (library, previewer, soundPack, settings, []() -> te::Edit* { return nullptr; });
    phazite::test::TestWindow window (browser, 320, 700);
    bool pageShown = false;
    browser.onShowSoundPack = [&pageShown] { pageShown = true; };
    phazite::test::dispatchPendingMessages();

    // Their place has a row that downloads them, which says how large they are.
    const auto rows = getRows (browser);
    REQUIRE (rows.size() > 5);
    CHECK (rows[3] == juce::String ("Phazite Sounds"));
    CHECK (rows[4].startsWith (phazite::SoundPack::canDownload() ? "Download (" : "Get Them ("));
    CHECK (rows[5] == juce::String ("Free drums, loops and effects to start with"));

    // A click downloads them. Where Phazite can't download, it shows the page instead, and the user installs the zip
    // downloaded in a browser.
    phazite::test::Mouse mouse (browser.getTree());
    mouse.click (getRow (browser, rows[4]).getCentre());
    CHECK (pageShown != phazite::SoundPack::canDownload());

    if (! phazite::SoundPack::canDownload())
        soundPack.installFromFile (source.zip);

    REQUIRE (phazite::test::waitForSoundPack (soundPack));
    REQUIRE (soundPack.isInstalled());

    // The place lists them now (after the pack's change message, which its thread posts after it sets the state), and
    // search finds them once they are indexed.
    CHECK (phazite::test::dispatchMessagesUntil ([&browser] { return getRows (browser).contains ("Drums"); }));
    REQUIRE (phazite::test::runTimersUntil ([&library] { return ! library.isIndexing(); }, 10000));
    CHECK_FALSE (library.search ("kick").empty());
}
