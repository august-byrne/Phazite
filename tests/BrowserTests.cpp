// SPDX-License-Identifier: AGPL-3.0-or-later
// The browser and the plug-in picker, driven as the user would: places and folders, previews, search, favorites, and
// putting items into the song with Enter, a drop or the picker (roadmap step 1.5).
#include "SoundPackTestHelpers.h"
#include "UiTestHelpers.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/engine/Preview.h"
#include "model/engine/internal/Preview.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"
#include "services/Library.h"
#include "ui/browser/Browser.h"
#include "ui/browser/PluginPicker.h"
#include "ui/input/LibraryDrag.h"
#include "ui/playlist/Playlist.h"
#include "ui/theme/LookAndFeel.h"

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
    // With folders of the user's too, as the settings give them as Phazite starts, if any.
    explicit BrowserScreen (juce::StringArray userFolders = {})
        : foldersGiven ((settings.set (phazite::Settings::browserFolders, userFolders), true))
    {
        setSong (context, song.get());
        browser.onPlace = [this] (const phazite::LibraryItem& item)
        {
            placed.push_back (item);
            return true;
        };

        REQUIRE (phazite::test::runTimersUntil ([this] { return ! library.isIndexing(); }));
        phazite::test::dispatchPendingMessages();
    }

    ~BrowserScreen() { previewer.stop(); }

    juce::StringArray getRows() { return ::getRows (browser); }
    juce::Rectangle<int> getRow (const juce::String& name) { return ::getRow (browser, name); }

    phazite::test::TemporaryFolder temp;
    phazite::SoundPack soundPack { writeSoundPack (temp.folder) };
    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() }; // with the song open
    phazite::Settings settings;
    const bool foldersGiven;
    phazite::Library library { phazite::test::getEngineContext(), settings, soundPack.getFolder() };
    std::unique_ptr<phazite::Previewer> previewerOwner = phazite::createPreviewer (phazite::test::getEngineContext());
    phazite::Previewer& previewer = *previewerOwner;
    phazite::ui::Browser browser { library, previewer, soundPack, settings, context };
    phazite::test::TestWindow window { browser, 320, 700 };
    std::vector<phazite::LibraryItem> placed;
};
} // namespace

TEST_CASE ("A loop playing stops at a click on it, a one-shot plays again, and starting or stopping the song stops either")
{
    BrowserScreen screen;
    phazite::test::Mouse mouse (screen.browser.getTree());
    const auto click = [&] (const char* row)
    {
        mouse.click (screen.getRow (row).getCentre());
        phazite::test::dispatchPendingMessages();
    };

    // A loop, in time with the song, plays until it is clicked again; then it plays again.
    click ("Loops");
    click ("Break 120 BPM");
    REQUIRE (screen.previewer.isPlaying());
    CHECK (screen.previewer.isInTime());
    click ("Break 120 BPM");
    CHECK_FALSE (screen.previewer.isPlaying());
    click ("Break 120 BPM");
    CHECK (screen.previewer.isPlaying());

    // The song started: it stops.
    phazite::playOrStop (*screen.song);
    CHECK (phazite::test::runTimersUntil ([&screen] { return ! screen.previewer.isPlaying(); }));

    // One started while the song plays plays on, until the song stops.
    click ("Break 120 BPM");
    phazite::test::runTimersUntil ([] { return false; }, 300); // an absence: the timers run, and a stop would come at their first tick
    CHECK (screen.previewer.isPlaying());
    phazite::playOrStop (*screen.song);
    CHECK (phazite::test::runTimersUntil ([&screen] { return ! screen.previewer.isPlaying(); }));

    // A one-shot clicked again plays again from its start.
    click ("Drums");
    click ("Kick");
    REQUIRE (screen.previewer.isPlaying());
    click ("Kick");
    CHECK (screen.previewer.isPlaying());
    CHECK (screen.previewer.getFile().getFileName() == juce::String ("Kick.wav"));
}

TEST_CASE ("A loop playing stops when what holds the browser hides it, though the browser itself stays visible")
{
    // Known bug 109: hiding the left panel, which holds the browser, left a looping preview playing with nothing in view to
    // stop it.
    BrowserScreen screen;
    phazite::test::Mouse mouse (screen.browser.getTree());

    for (const auto* row : { "Loops", "Break 120 BPM" })
    {
        mouse.click (screen.getRow (row).getCentre());
        phazite::test::dispatchPendingMessages();
    }

    REQUIRE (screen.previewer.isPlaying());

    // Its window hidden, as the left panel is: the browser's own flag doesn't change.
    screen.window.setVisible (false);
    CHECK (screen.browser.isVisible());
    CHECK (phazite::test::runTimersUntil ([&screen] { return ! screen.previewer.isPlaying(); }));
}

TEST_CASE ("The sound selected shows under the tree: its waveform, whether it is a loop, how long it lasts and its format")
{
    BrowserScreen screen;
    phazite::test::Mouse mouse (screen.browser.getTree());
    const auto click = [&] (const char* row)
    {
        mouse.click (screen.getRow (row).getCentre());
        phazite::test::dispatchPendingMessages();
    };

    // A folder: nothing.
    click ("Loops");
    CHECK_FALSE (screen.browser.getSoundView().isVisible());

    // A loop of 4 seconds at 120 BPM: two bars; the tree makes room for it.
    click ("Break 120 BPM");
    REQUIRE (screen.browser.getSoundView().isVisible());
    auto details = screen.browser.getSoundView().getDetails();
    CHECK (details.startsWith ("Loop"));
    CHECK (details.contains ("120 BPM"));
    CHECK (details.contains ("2 bars"));
    CHECK (details.contains ("4.00 s"));
    CHECK (details.contains ("kHz"));
    CHECK (screen.browser.getTree().getBottom() <= screen.browser.getSoundView().getY());

    // A kick: a one-shot, to the hundredth of a second.
    click ("Drums");
    click ("Kick");
    details = screen.browser.getSoundView().getDetails();
    CHECK (details.startsWith ("One-shot"));
    CHECK (details.contains ("0.30 s"));
}

TEST_CASE ("The browser shows its places, plays a sound when clicked, and Enter puts it into the song")
{
    BrowserScreen screen;

    // Phazite's sounds are open at first.
    CHECK (screen.getRows()
           == juce::StringArray { "Favorites", "Recent", "Collections", "Phazite Sounds", "Drums", "Loops", "Instruments", "Effects",
                                  "Phazite Presets", "Presets", "Add Folder..." });

    // One click opens a folder; a click on a sound plays it.
    phazite::test::Mouse mouse (screen.browser.getTree());
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

    // Enter puts the sound into the song, and Recent lists it once its import has put it there (the playlist says so).
    screen.window.pressKey (returnKey);
    REQUIRE (screen.placed.size() == 1);
    CHECK (screen.placed[0].name == juce::String ("Snare"));
    CHECK (screen.library.getRecent().empty());
    screen.browser.notePlaced (screen.placed[0].file);
    REQUIRE (! screen.library.getRecent().empty());
    CHECK (screen.library.getRecent()[0].name == juce::String ("Snare"));

    // Esc stops the sound.
    screen.window.pressKey (escapeKey);
    CHECK_FALSE (screen.previewer.isPlaying());

    // A folder's sound dragged into the song and dropped there is noted too.
    auto kick = screen.library.findItem (screen.soundPack.getFolder().getChildFile ("Drums/Kick.wav").getFullPathName());
    REQUIRE (kick.has_value());
    const auto description = phazite::ui::toDragDescription (*kick);
    screen.browser.dragOperationEnded ({ description, &screen.browser, {} }); // dropped where nothing took it
    CHECK (screen.library.getRecent()[0].name == juce::String ("Snare"));
    phazite::ui::markPlaced (description);
    screen.browser.dragOperationEnded ({ description, &screen.browser, {} });
    CHECK (screen.library.getRecent()[0].name == juce::String ("Kick"));
}

TEST_CASE ("The browser's search finds sounds and devices, and the star keeps favorites")
{
    BrowserScreen screen;
    auto& search = screen.browser.getSearchField();

    // The rows once a search has run: a text field tells of changes asynchronously, and the search runs in the background
    // (known bug 128), its results posted to the message thread.
    const auto rowsFor = [&screen, &search] (const juce::String& query, const juce::StringArray& expected)
    {
        search.setText (query, true);
        phazite::test::dispatchMessagesUntil ([&screen, &expected] { return screen.getRows() == expected; });
        return screen.getRows();
    };

    CHECK (rowsFor ("break", { "Break 120 BPM, 120 BPM" }) == juce::StringArray { "Break 120 BPM, 120 BPM" });
    CHECK (rowsFor ("reverb", { "Reverb, Effect" }) == juce::StringArray { "Reverb, Effect" });
    CHECK (rowsFor ("nothing is called this", { "Nothing matches \"nothing is called this\"" })
           == juce::StringArray { "Nothing matches \"nothing is called this\"" });

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

TEST_CASE ("Performance gate: a key typed in the browser's search looks through none of the library on the message thread")
{
    BrowserScreen screen;
    auto& search = screen.browser.getSearchField();
    const auto before = phazite::Library::getItemsSearchedOnMessageThread();

    // Each key's search runs in the background, and its rows come once it is done (known bug 128).
    for (const auto* typed : { "b", "br", "bre", "brea", "break" })
        search.setText (typed, true);

    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return screen.getRows().contains ("Break 120 BPM, 120 BPM"); }));
    CHECK (phazite::Library::getItemsSearchedOnMessageThread() == before);
}

namespace
{
// How many rows are called so.
int countOf (const juce::StringArray& rows, const juce::String& name)
{
    return (int) std::count (rows.begin(), rows.end(), name);
}

// How many folders say they are being read: those whose drive hasn't answered for a few seconds say so instead.
int countReading (const juce::StringArray& rows)
{
    return countOf (rows, "Reading this folder...") + countOf (rows, "This drive isn't answering...");
}

// Until the folders being read for the browser are read, and what came of them is shown.
void waitForFolders (BrowserScreen& screen)
{
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&screen]
        { return ! screen.library.isReadingFolders() && ! screen.browser.isReadingFolders() && ! screen.browser.isWaitingForRows(); }));
    phazite::test::dispatchPendingMessages(); // what a read posted as it ended
}

// A hold on the reads of some folders (all of them, with none named), as a drive asleep holds them: let go of as it goes, and
// waited for until no read of the library's is under way, so that nothing reads its flag after it (a shared flag).
struct HeldReads
{
    HeldReads (const phazite::Library& l, juce::Array<juce::File> heldFolders = {})
        : library (l)
    {
        library.setBeforeListing (
            [going = goOn, folders = std::move (heldFolders), inHold = holding, heldReads = held] (const juce::File& folder)
            {
                if (! folders.isEmpty() && ! folders.contains (folder))
                    return;

                ++*inHold;
                ++*heldReads;

                while (! going->load())
                    juce::Thread::sleep (1);

                --*inHold;
            });
    }

    ~HeldReads()
    {
        letGo();
        phazite::test::dispatchMessagesUntil ([this] { return ! library.isReadingFolders(); });
        library.setBeforeListing (nullptr);
    }

    void letGo() { *goOn = true; }
    int countHolding() const { return holding->load(); } // reads inside the hold now: in their system call, as it were
    int countHeldReads() const { return held->load(); }  // reads of the folders held, ever

    const phazite::Library& library;
    const std::shared_ptr<std::atomic<bool>> goOn = std::make_shared<std::atomic<bool>> (false);
    const std::shared_ptr<std::atomic<int>> holding = std::make_shared<std::atomic<int>> (0);
    const std::shared_ptr<std::atomic<int>> held = std::make_shared<std::atomic<int>> (0);
};
} // namespace

TEST_CASE ("Performance gate: a folder opened in the browser reads no folder from the disk on the message thread")
{
    // Opening a place or a folder read it from the disk on the message thread (Library::listFolder()): a folder of tens of
    // thousands of files, or one on a network drive slow to wake, held the window. A folder of a place the index walked
    // whole comes from the index; any other is read on a listing thread.
    BrowserScreen screen;
    const auto before = phazite::Library::getFileQueriesOnMessageThread();

    for (const auto fromIndex : { true, false })
    {
        CAPTURE (fromIndex);
        screen.library.setListsFromIndex (fromIndex);
        auto& tree = screen.browser.getTree();
        phazite::test::Mouse mouse (tree);

        // Phazite Sounds, open at first: closed and opened again; then its Drums folder.
        mouse.click (screen.getRow ("Phazite Sounds").getCentre());
        phazite::test::dispatchPendingMessages();
        mouse.click (screen.getRow ("Phazite Sounds").getCentre());
        waitForFolders (screen);
        REQUIRE (screen.getRows().contains ("Drums"));
        mouse.click (screen.getRow ("Drums").getCentre());
        waitForFolders (screen);
        CHECK (screen.getRows().contains ("Kick"));
        CHECK (screen.getRows().contains ("Snare"));

        mouse.click (screen.getRow ("Drums").getCentre()); // closed again, for the next round
        phazite::test::dispatchPendingMessages();
    }

    CHECK (phazite::Library::getFileQueriesOnMessageThread() == before); // the folders opened: none

    // Favorites, Recent and a collection: each member asked of the disk once, whether it is there (the user's own lists,
    // bounded by them); what it is, from the index.
    const auto kick = screen.library.findItem (screen.soundPack.getFolder().getChildFile ("Drums/Kick.wav").getFullPathName());
    REQUIRE (kick.has_value());
    screen.library.setFavorite (*kick, true);
    screen.library.noteUsed (*kick);
    screen.library.setInCollection (*kick, screen.library.addCollection ("Mine"), true);
    const phazite::BrowserModel model { screen.library, screen.soundPack };
    const auto queries = phazite::Library::getFileQueriesOnMessageThread();
    CHECK (model.getContents ({ phazite::BrowserPlace::Kind::favorites, "Favorites", {} }).items.size() == 1);
    CHECK (model.getContents ({ phazite::BrowserPlace::Kind::recent, "Recent", {} }).items.size() == 1);
    CHECK (model.getContents ({ phazite::BrowserPlace::Kind::collection, "Mine", {} }).items.size() == 1);
    CHECK (phazite::Library::getFileQueriesOnMessageThread() - queries == 3);
    screen.library.setListsFromIndex (true);
}

TEST_CASE (
    "A folder whose drive doesn't answer says it is being read, holds up neither the search nor its keys, and a key waits for its rows alone")
{
    // Its read held on its listing thread, as a network drive that is slow to wake holds it.
    BrowserScreen screen;
    screen.library.setListsFromIndex (false);
    const auto drums = screen.soundPack.getFolder().getChildFile ("Drums");
    const phazite::test::HeldClock clock;
    HeldReads held (screen.library, { drums });

    auto& tree = screen.browser.getTree();
    phazite::test::Mouse mouse (tree);
    mouse.click (screen.getRow ("Drums").getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getRows().contains ("Reading this folder..."));
    CHECK (screen.browser.isReadingFolders());
    CHECK_FALSE (screen.browser.isWaitingForRows()); // the search's and the filters' rows: none to come

    // Down from the search field acts at once, on the search's rows, as the folder's read is held.
    auto& search = screen.browser.getSearchField();
    search.grabKeyboardFocus();
    search.setText ("snare", true);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return screen.getRows().contains ("Snare"); }));
    screen.window.pressKey (downKey);
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.browser.getSelectedItem().has_value());
    CHECK (screen.browser.getSelectedItem()->name == juce::String ("Snare"));
    search.setText ({}, true);
    phazite::test::dispatchPendingMessages();

    // Enter on the folder being read waits for its rows; a click elsewhere first drops it: it never acts on what is
    // selected then.
    mouse.click (screen.getRow ("Drums").getCentre()); // closed
    mouse.click (screen.getRow ("Drums").getCentre()); // opened: read again, held
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.browser.isReadingFolders());
    tree.grabKeyboardFocus();
    screen.window.pressKey (returnKey);
    mouse.click (screen.getRow ("Recent").getCentre()); // something else first
    held.letGo();
    waitForFolders (screen);
    CHECK (screen.getRows().contains ("Kick"));                                  // Drums open
    CHECK (screen.getRows().contains ("What you put into the song shows here")); // Recent open still: no Enter closed it
}

TEST_CASE ("A key waits for the rows of a folder being read only a moment: later, it is dropped")
{
    BrowserScreen screen;
    screen.library.setListsFromIndex (false);
    const auto drums = screen.soundPack.getFolder().getChildFile ("Drums");
    phazite::test::HeldClock clock;
    HeldReads held (screen.library, { drums });

    auto& tree = screen.browser.getTree();
    phazite::test::Mouse mouse (tree);
    mouse.click (screen.getRow ("Drums").getCentre());
    phazite::test::dispatchPendingMessages();
    tree.grabKeyboardFocus();
    screen.window.pressKey (downKey); // on Drums, being read

    clock.advance (5000.0); // a drive that took five seconds to wake
    held.letGo();
    waitForFolders (screen);
    REQUIRE (screen.browser.getSelectedItem().has_value());
    CHECK (screen.browser.getSelectedItem()->name == juce::String ("Drums")); // not moved on by a key five seconds old
}

TEST_CASE (
    "A folder whose drive doesn't answer takes one listing thread: reopened and looked at again, it joins its read, and other folders open")
{
    // A dead drive's folder opened again and again, and looked at again, each joining the one read in flight; four user
    // folders, each a root of its own, open meanwhile.
    phazite::test::TemporaryFolder others;
    juce::StringArray folders;

    for (int i = 0; i < 4; ++i)
    {
        const auto folder = others.folder.getChildFile ("Other " + juce::String (i));
        phazite::test::writeSound (folder.getChildFile ("Hat " + juce::String (i) + ".wav"), 0.1);
        folders.add (folder.getFullPathName());
    }

    BrowserScreen screen (folders);
    screen.library.setListsFromIndex (false);
    const auto drums = screen.soundPack.getFolder().getChildFile ("Drums");
    phazite::test::HeldClock clock;
    HeldReads held (screen.library, { drums });

    auto& tree = screen.browser.getTree();
    phazite::test::Mouse mouse (tree);
    mouse.click (screen.getRow ("Drums").getCentre());
    phazite::test::dispatchPendingMessages();

    for (int i = 0; i < 10; ++i)
    {
        mouse.click (screen.getRow ("Drums").getCentre()); // closed
        mouse.click (screen.getRow ("Drums").getCentre()); // opened again
        clock.advance (6000.0);
        screen.browser.lookAgainAtOpenFolders (false);
        phazite::test::dispatchPendingMessages();
    }

    for (int i = 0; i < 4; ++i)
    {
        CAPTURE (i);
        mouse.click (screen.getRow ("Other " + juce::String (i)).getCentre());
        REQUIRE (phazite::test::dispatchMessagesUntil ([&screen, i] { return screen.getRows().contains ("Hat " + juce::String (i)); }));
    }

    CHECK (held.countHeldReads() == 1); // Drums once: the others joined it (the root's other folders read beside it, once it was stuck)
    CHECK (countReading (screen.getRows()) == 1);
}

TEST_CASE ("Folders open within a folder still being read open again once it is in: as the browser is made again, and through a filter")
{
    phazite::test::TemporaryFolder temp;
    phazite::SoundPack soundPack { writeSoundPack (temp.folder) };
    phazite::Settings settings;
    phazite::Library library (phazite::test::getEngineContext(), settings, soundPack.getFolder());
    REQUIRE (phazite::test::runTimersUntil ([&library] { return ! library.isIndexing(); }));
    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, song.get());
    auto previewer = phazite::createPreviewer (phazite::test::getEngineContext());
    const auto rowsOf = [] (phazite::ui::Browser& browser) { return ::getRows (browser); };

    // Drums opened within Phazite Sounds; the browser goes, keeping what is open.
    {
        phazite::ui::Browser browser { library, *previewer, soundPack, settings, context };
        phazite::test::TestWindow window { browser, 320, 700 };
        phazite::test::dispatchPendingMessages();
        phazite::test::Mouse mouse (browser.getTree());
        mouse.click (getRow (browser, "Drums").getCentre());
        phazite::test::dispatchPendingMessages();
        REQUIRE (rowsOf (browser).contains ("Kick"));
    }

    // Made again, every folder read held: Phazite Sounds says it is being read; once let go, Drums is open within it.
    library.setListsFromIndex (false);
    HeldReads held (library);
    phazite::ui::Browser browser { library, *previewer, soundPack, settings, context };
    phazite::test::TestWindow window { browser, 320, 700 };
    phazite::test::dispatchPendingMessages();
    CHECK (rowsOf (browser).contains ("Reading this folder..."));
    held.letGo();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! browser.isReadingFolders() && rowsOf (browser).contains ("Kick"); }));

    // A filter set and taken away, the reads held again: the folders open before it open again once read.
    std::optional<juce::PopupMenu> menu;
    browser.getFilterBar().askMenu (phazite::ui::FilterBar::Filter::loops, [&menu] (juce::PopupMenu made) { menu = std::move (made); });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&menu] { return menu.has_value(); }));
    REQUIRE (phazite::test::runMenuItem (*menu, { "Loops" }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! browser.isWaitingForRows(); }));

    HeldReads heldAgain (library);
    phazite::test::Mouse clear (browser.getFilterBar().getClearButton());
    clear.click (browser.getFilterBar().getClearButton().getLocalBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    heldAgain.letGo();
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return ! browser.isReadingFolders() && rowsOf (browser).contains ("Kick"); }));
    library.setListsFromIndex (true);
}

TEST_CASE ("A file added to an indexed folder from outside shows as the folder opens, and search and the filters find it; one removed goes")
{
    // An indexed folder opens from the index at once, and is read from the disk all the same: what changed outside Phazite
    // (Explorer, another program bouncing a sample into the folder) shows, and the index is patched for search and filters.
    BrowserScreen screen;
    const auto before = phazite::Library::getFileQueriesOnMessageThread();
    auto& tree = screen.browser.getTree();
    phazite::test::Mouse mouse (tree);
    const auto drums = screen.soundPack.getFolder().getChildFile ("Drums");

    // Nothing changed: the folder's rows, made once, and none made again as its read finds the same.
    auto made = phazite::ui::Browser::countRowsMade();
    mouse.click (screen.getRow ("Drums").getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getRows().contains ("Kick")); // from the index, at once
    waitForFolders (screen);
    CHECK (phazite::ui::Browser::countRowsMade() - made == 2); // Kick and Snare
    mouse.click (screen.getRow ("Drums").getCentre());         // closed
    phazite::test::dispatchPendingMessages();

    // A clap added, a snare removed, outside: opened again, the index's rows first, then what the folder holds now.
    phazite::test::writeSound (drums.getChildFile ("Clap.wav"), 0.2);
    REQUIRE (drums.getChildFile ("Snare.wav").deleteFile());
    mouse.click (screen.getRow ("Drums").getCentre());
    waitForFolders (screen);
    CHECK (screen.getRows().contains ("Clap"));
    CHECK_FALSE (screen.getRows().contains ("Snare"));
    CHECK (screen.getRows().contains ("Kick"));

    // Search finds it, and the Type filter counts it; the snare is gone from both.
    const phazite::BrowserModel model { screen.library, screen.soundPack };
    CHECK (model.countSoundsOfType ("Clap") == 1);
    CHECK (model.countSoundsOfType ("Snare") == 0);
    screen.browser.getSearchField().setText ("clap", true);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return screen.getRows().contains ("Clap"); }));
    screen.browser.getSearchField().setText ("snare", true);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.browser.isWaitingForRows(); }));
    CHECK_FALSE (screen.getRows().contains ("Snare"));
    screen.browser.getSearchField().setText ({}, true);
    phazite::test::dispatchPendingMessages();

    CHECK (phazite::Library::getFileQueriesOnMessageThread() == before);
}

TEST_CASE ("Two folders changed outside at once are both found by search: neither patch is lost to the other")
{
    BrowserScreen screen;
    phazite::test::HeldClock clock;
    auto& tree = screen.browser.getTree();
    phazite::test::Mouse mouse (tree);
    mouse.click (screen.getRow ("Drums").getCentre());
    mouse.click (screen.getRow ("Loops").getCentre());
    waitForFolders (screen);

    const auto pack = screen.soundPack.getFolder();
    phazite::test::writeSound (pack.getChildFile ("Drums/Clap.wav"), 0.2);
    phazite::test::writeSound (pack.getChildFile ("Loops/Groove 100 BPM.wav"), 0.5);
    clock.advance (6000.0);
    screen.browser.lookAgainAtOpenFolders (false);
    waitForFolders (screen);
    CHECK (screen.getRows().contains ("Clap"));
    CHECK (screen.getRows().contains ("Groove 100 BPM, 100 BPM"));

    for (const auto* word : { "clap", "groove" })
    {
        CAPTURE (word);
        screen.browser.getSearchField().setText (word, true);
        REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.browser.isWaitingForRows(); }));
        CHECK (screen.getRows().size() >= 1);
        CHECK_FALSE (screen.getRows()[0].startsWith ("Nothing matches"));
    }
}

TEST_CASE ("Open folders looked at again keep their selection and their open folders, show what came, and make no row where nothing did")
{
    // As the browser takes the keys again, or Phazite comes to the front: the open folders read again, what changed brought
    // up to date in place; at most once in a few seconds.
    BrowserScreen screen;
    phazite::test::HeldClock clock;
    auto& tree = screen.browser.getTree();
    phazite::test::Mouse mouse (tree);
    mouse.click (screen.getRow ("Drums").getCentre());
    waitForFolders (screen);
    mouse.click (screen.getRow ("Kick").getCentre());
    phazite::test::dispatchPendingMessages();
    REQUIRE (screen.browser.getSelectedItem().has_value());
    REQUIRE (screen.browser.getSelectedItem()->name == juce::String ("Kick"));

    // Nothing changed: nothing made, and nothing read again so soon.
    const auto made = phazite::ui::Browser::countRowsMade();
    screen.browser.lookAgainAtOpenFolders (false);
    CHECK_FALSE (screen.library.isReadingFolders());
    clock.advance (6000.0);
    screen.browser.lookAgainAtOpenFolders (false);
    waitForFolders (screen);
    CHECK (phazite::ui::Browser::countRowsMade() == made);

    // A sound added beside Drums, and one named as the kick in another format: their rows alone, each once, in place; Drums
    // open still, Kick selected still.
    phazite::test::writeSound (screen.soundPack.getFolder().getChildFile ("Zap.wav"), 0.2);
    REQUIRE (screen.soundPack.getFolder()
                 .getChildFile ("Drums/Kick.wav")
                 .copyFileTo (screen.soundPack.getFolder().getChildFile ("Drums/Kick.flac")));
    clock.advance (6000.0);
    screen.browser.lookAgainAtOpenFolders (false);
    waitForFolders (screen);
    CHECK (screen.getRows().contains ("Zap"));
    CHECK (phazite::ui::Browser::countRowsMade() - made == 2);
    CHECK (screen.getRows().contains ("Kick"));
    REQUIRE (screen.browser.getSelectedItem().has_value());
    CHECK (screen.browser.getSelectedItem()->name == juce::String ("Kick"));

    // Again, with nothing new: the two kicks stay as they are, none made twice.
    clock.advance (6000.0);
    screen.browser.lookAgainAtOpenFolders (false);
    waitForFolders (screen);
    CHECK (phazite::ui::Browser::countRowsMade() - made == 2);
    CHECK (countOf (screen.getRows(), "Kick") == 2);
}

TEST_CASE ("Performance gate: the open folders looked at again make no row and read no folder on the message thread")
{
    // Fifty open folders: a focus change reads at most mostLookedAgain of them, each on a listing thread, and makes nothing.
    phazite::test::TemporaryFolder temp;
    const auto many = temp.folder.getChildFile ("Many");

    for (int i = 0; i < 50; ++i)
        phazite::test::writeSound (many.getChildFile ("Folder " + juce::String (i) + "/Sound " + juce::String (i) + ".wav"), 0.05);

    BrowserScreen screen (juce::StringArray { many.getFullPathName() });
    phazite::test::HeldClock clock;
    auto& tree = screen.browser.getTree();
    phazite::test::Mouse mouse (tree);
    mouse.click (screen.getRow ("Many").getCentre());
    waitForFolders (screen);

    std::function<void (juce::TreeViewItem*)> openAll = [&openAll] (juce::TreeViewItem* item)
    {
        for (int i = 0; i < item->getNumSubItems(); ++i)
            if (auto* sub = item->getSubItem (i); sub->mightContainSubItems() && sub->getUniqueName().contains ("Folder "))
                sub->setOpen (true);
    };

    for (int i = 0; i < tree.getRootItem()->getNumSubItems(); ++i)
        if (tree.getRootItem()->getSubItem (i)->getUniqueName().contains ("Many"))
            openAll (tree.getRootItem()->getSubItem (i));

    waitForFolders (screen);
    REQUIRE (screen.getRows().contains ("Sound 49"));

    const HeldReads counted (screen.library);
    counted.goOn->store (true);
    const auto made = phazite::ui::Browser::countRowsMade();
    const auto queries = phazite::Library::getFileQueriesOnMessageThread();
    clock.advance (6000.0);
    const auto looked = screen.browser.lookAgainAtOpenFolders (false);
    CHECK (phazite::Library::getFileQueriesOnMessageThread() == queries);
    waitForFolders (screen);
    CHECK (phazite::ui::Browser::countRowsMade() == made);
    CHECK (looked > 0);
    CHECK (looked <= 16); // Browser::mostLookedAgain: the timer's own looks (a focus change under load) aren't counted here
}

TEST_CASE ("A folder that links back to one it is in opens, and is looked at again, without the patch following it")
{
    // A junction inside Drums to Phazite Sounds itself: the walk meets it inside itself and lists nothing under it; opened,
    // it is read from the disk, and the index isn't patched through it (which followed it to the path's limit).
    BrowserScreen screen;
    const auto pack = screen.soundPack.getFolder();

    if (! phazite::test::linkFolder (pack.getChildFile ("Drums/Back"), pack))
    {
        MESSAGE ("No link could be made here: nothing to follow.");
        return;
    }

    screen.library.refresh();
    REQUIRE (phazite::test::runTimersUntil ([&screen] { return ! screen.library.isIndexing(); }));
    phazite::test::dispatchPendingMessages();
    const auto filesBefore = screen.library.getWholeIndex()->files.size();

    phazite::test::HeldClock clock;
    auto& tree = screen.browser.getTree();
    phazite::test::Mouse mouse (tree);
    mouse.click (screen.getRow ("Drums").getCentre());
    waitForFolders (screen);
    mouse.click (screen.getRow ("Back").getCentre());
    waitForFolders (screen);
    CHECK (countOf (screen.getRows(), "Loops") == 2); // what it leads to, from the disk

    clock.advance (6000.0);
    screen.browser.lookAgainAtOpenFolders (false);
    waitForFolders (screen);
    REQUIRE (phazite::test::runTimersUntil ([&screen] { return ! screen.library.isIndexing(); }));
    CHECK (screen.library.getWholeIndex()->files.size() == filesBefore);
}

TEST_CASE ("A user's folder within another is indexed once: search finds each sound in it once")
{
    BrowserScreen screen;
    screen.library.addUserFolder (screen.soundPack.getFolder().getChildFile ("Drums")); // within Phazite Sounds
    REQUIRE (phazite::test::runTimersUntil ([&screen] { return ! screen.library.isIndexing(); }));
    phazite::test::dispatchPendingMessages();

    screen.browser.getSearchField().setText ("kick", true);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.browser.isWaitingForRows(); }));
    const auto rows = screen.getRows();
    CHECK (countOf (rows, "Kick") == 1);
}

TEST_CASE ("Performance gate: a folder's listing reads no sound's header for its rows; a patch of the index reads its new files' alone")
{
    // A reviewer, of the known bugs' round (2026-11-16): a folder's read opened every new WAV to read its loop data before its
    // rows came, and the library's going waited for it. Its rows come from what is known of its files (the index, the facts
    // read before, their names); a folder the index holds whole and that changed reads what is new in it, as it patches.
    BrowserScreen screen;
    waitForFolders (screen);
    phazite::test::TemporaryFolder others;
    const auto outside = others.folder.getChildFile ("Outside"); // no place's: the index holds nothing of it

    for (int i = 0; i < 20; ++i)
        phazite::test::writeSound (outside.getChildFile ("Hit " + juce::String (i) + ".wav"), 0.05);

    auto headers = phazite::Library::getHeadersReadByListings();
    std::optional<std::vector<phazite::LibraryItem>> listed;
    const auto listing = screen.library.checkListing (outside, [&listed] (std::optional<std::vector<phazite::LibraryItem>> items)
                                                      { listed = items.value_or (std::vector<phazite::LibraryItem>()); });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return listed.has_value() && ! screen.library.isReadingFolders(); }));
    CHECK (listed->size() == 20);
    CHECK (phazite::Library::getHeadersReadByListings() == headers);

    // A clap saved into Drums from outside: its header alone, as the index takes it in.
    const auto clap = screen.soundPack.getFolder().getChildFile ("Drums/Clap.wav");
    phazite::test::writeSound (clap, 0.2);
    headers = phazite::Library::getHeadersReadByListings();
    const auto patching = screen.library.checkListing (clap.getParentDirectory(), [] (std::optional<std::vector<phazite::LibraryItem>>) {});
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return ! screen.library.isReadingFolders() && screen.library.getWholeIndex()->find (clap.getFullPathName()).has_value(); }));
    CHECK (phazite::Library::getHeadersReadByListings() - headers == 1);
}

TEST_CASE ("The library goes at once while a patch of its index reads a new sound's header: the read is left behind")
{
    phazite::test::TemporaryFolder temp;
    phazite::SoundPack soundPack { writeSoundPack (temp.folder) };
    phazite::Settings settings;
    auto library = std::make_unique<phazite::Library> (phazite::test::getEngineContext(), settings, soundPack.getFolder());
    REQUIRE (phazite::test::runTimersUntil ([&library] { return ! library->isIndexing(); }));

    // The header's read held, as a drive that stops answering holds it; given up only by a library that waited for it, so
    // that the test fails rather than hangs.
    const auto entered = std::make_shared<std::atomic<bool>> (false), going = std::make_shared<std::atomic<bool>> (false),
               done = std::make_shared<std::atomic<bool>> (false);
    library->setBeforeHeader (
        [entered, going, done] (const juce::File&)
        {
            *entered = true;
            const auto until = juce::Time::getMillisecondCounterHiRes() + phazite::test::hangTimeout;

            while (! going->load() && juce::Time::getMillisecondCounterHiRes() < until)
                juce::Thread::sleep (1);

            *done = true;
        });

    const auto drums = soundPack.getFolder().getChildFile ("Drums");
    phazite::test::writeSound (drums.getChildFile ("Clap.wav"), 0.2);
    const auto listing = library->checkListing (drums, [] (std::optional<std::vector<phazite::LibraryItem>>) {});
    REQUIRE (phazite::test::dispatchMessagesUntil ([entered] { return entered->load(); }));

    library = nullptr;
    CHECK_FALSE (done->load()); // gone without waiting for it

    *going = true;
    REQUIRE (phazite::test::dispatchMessagesUntil ([done] { return done->load(); }));
}

TEST_CASE ("Phazite coming to the front looks again at the open folders in view, however soon: the user back from Explorer in two seconds")
{
    BrowserScreen screen;
    phazite::test::HeldClock clock;
    phazite::test::Mouse mouse (screen.browser.getTree());
    mouse.click (screen.getRow ("Drums").getCentre());
    waitForFolders (screen);

    // A clap saved into Drums in Explorer two seconds after it opened: the browser taking the keys again waits for its few
    // seconds; Phazite coming to the front doesn't.
    phazite::test::writeSound (screen.soundPack.getFolder().getChildFile ("Drums/Clap.wav"), 0.2);
    clock.advance (2000.0);
    CHECK (screen.browser.lookAgainAtOpenFolders (false) == 0);
    CHECK (screen.browser.lookAgainAtOpenFolders (true) >= 1);
    waitForFolders (screen);
    CHECK (screen.getRows().contains ("Clap"));
}

TEST_CASE ("A folder whose drive stopped answering gives its root's turn on after a few seconds: another folder of that root opens")
{
    BrowserScreen screen;
    waitForFolders (screen);
    screen.library.setListsFromIndex (false);
    phazite::test::HeldClock clock;
    HeldReads held (screen.library, { screen.soundPack.getFolder().getChildFile ("Drums") });
    phazite::test::Mouse mouse (screen.browser.getTree());
    mouse.click (screen.getRow ("Drums").getCentre());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&held] { return held.countHolding() == 1; })); // inside its system call

    // Loops, of the same root, waits its turn: Drums's read may yet answer.
    mouse.click (screen.getRow ("Loops").getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (countOf (screen.getRows(), "Reading this folder...") == 2);

    // Drums's read inside the system for longer than a few seconds: Loops is read, with nothing more done.
    clock.advance (4000.0);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return screen.getRows().contains ("Break 120 BPM, 120 BPM"); }));
    // Drums's line, which says its drive isn't answering.
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return screen.getRows().contains ("This drive isn't answering..."); }));
    CHECK (countReading (screen.getRows()) == 1);

    held.letGo();
    waitForFolders (screen);
    CHECK (screen.getRows().contains ("Kick"));
    screen.library.setListsFromIndex (true);
}

TEST_CASE ("Four roots whose drives stopped answering leave a fifth root's folder a thread of its own")
{
    phazite::test::TemporaryFolder others;
    juce::StringArray folders;
    juce::Array<juce::File> dead;

    for (int i = 0; i < 5; ++i)
    {
        const auto folder = others.folder.getChildFile ("Other " + juce::String (i));
        phazite::test::writeSound (folder.getChildFile ("Hat " + juce::String (i) + ".wav"), 0.1);
        folders.add (folder.getFullPathName());

        if (i < 4)
            dead.add (folder);
    }

    BrowserScreen screen (folders);
    waitForFolders (screen);
    screen.library.setListsFromIndex (false);
    phazite::test::HeldClock clock;
    HeldReads held (screen.library, dead);
    phazite::test::Mouse mouse (screen.browser.getTree());

    for (int i = 0; i < 4; ++i)
        mouse.click (screen.getRow ("Other " + juce::String (i)).getCentre());

    REQUIRE (phazite::test::dispatchMessagesUntil ([&held] { return held.countHolding() == 4; })); // every listing thread

    clock.advance (4000.0);
    mouse.click (screen.getRow ("Other 4").getCentre());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return screen.getRows().contains ("Hat 4"); }));
    CHECK (countReading (screen.getRows()) == 4);
    screen.library.setListsFromIndex (true);
}

TEST_CASE (
    "A file deleted after a folder's read patched the index, and before the walk listed that folder, doesn't come back with the walk")
{
    BrowserScreen screen;
    waitForFolders (screen);
    const auto drums = screen.soundPack.getFolder().getChildFile ("Drums");
    const auto clap = drums.getChildFile ("Clap.wav");
    phazite::test::writeSound (clap, 0.2);

    // A walk held as it comes to Drums.
    const auto atDrums = std::make_shared<std::atomic<bool>> (false), going = std::make_shared<std::atomic<bool>> (false);
    const juce::ScopeGuard letGo { [going] { *going = true; } };
    screen.library.setBeforeWalkListing (
        [drums, atDrums, going] (const juce::File& folder)
        {
            if (folder != drums)
                return;

            *atDrums = true;

            while (! going->load())
                juce::Thread::sleep (1);
        });

    screen.library.refresh();
    REQUIRE (phazite::test::dispatchMessagesUntil ([atDrums] { return atDrums->load(); }));

    // Drums read meanwhile: the clap patched into the index.
    const auto listing = screen.library.checkListing (drums, [] (std::optional<std::vector<phazite::LibraryItem>>) {});
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return ! screen.library.isReadingFolders() && screen.library.getWholeIndex()->find (clap.getFullPathName()).has_value(); }));

    // Deleted; then the walk lists Drums, and publishes what it found: no clap.
    REQUIRE (clap.deleteFile());
    *going = true;
    REQUIRE (phazite::test::runTimersUntil ([&screen] { return ! screen.library.isIndexing(); }));
    phazite::test::dispatchPendingMessages();
    screen.library.setBeforeWalkListing (nullptr);
    CHECK_FALSE (screen.library.getWholeIndex()->find (clap.getFullPathName()).has_value());
    const phazite::BrowserModel model { screen.library, screen.soundPack };
    CHECK (model.countSoundsOfType ("Clap") == 0);
}

TEST_CASE ("A folder patched within a user's folder that is inside another gets the outer folder's tags, as the walk gives them")
{
    phazite::test::TemporaryFolder others;
    const auto mine = others.folder.getChildFile ("Mine"), claps = mine.getChildFile ("Claps");
    phazite::test::writeSound (claps.getChildFile ("Hit A.wav"), 0.1);
    BrowserScreen screen (juce::StringArray { claps.getFullPathName(), mine.getFullPathName() }); // the inner one first
    waitForFolders (screen);
    const phazite::BrowserModel model { screen.library, screen.soundPack };
    REQUIRE (model.countSoundsOfType ("Clap") == 1); // its folder's word, from the outer folder

    phazite::test::writeSound (claps.getChildFile ("Hit B.wav"), 0.1);
    const auto listing = screen.library.checkListing (claps, [] (std::optional<std::vector<phazite::LibraryItem>>) {});
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&]
        {
            return ! screen.library.isReadingFolders()
                   && screen.library.getWholeIndex()->find (claps.getChildFile ("Hit B.wav").getFullPathName()).has_value();
        }));
    CHECK (model.countSoundsOfType ("Clap") == 2);
}

TEST_CASE ("Four roots whose drives stopped answering, opened with a fifth root's folder at the same moment, leave the fifth a thread")
{
    // As the browser restores every open folder as Phazite starts: the fifth's read was put in the four's queue before any of
    // them had been stuck for long, and stayed there.
    phazite::test::TemporaryFolder others;
    juce::StringArray folders;
    juce::Array<juce::File> dead;

    for (int i = 0; i < 5; ++i)
    {
        const auto folder = others.folder.getChildFile ("Other " + juce::String (i));
        phazite::test::writeSound (folder.getChildFile ("Hat " + juce::String (i) + ".wav"), 0.1);
        folders.add (folder.getFullPathName());

        if (i < 4)
            dead.add (folder);
    }

    BrowserScreen screen (folders);
    waitForFolders (screen);
    screen.library.setListsFromIndex (false);
    phazite::test::HeldClock clock;
    HeldReads held (screen.library, dead);
    phazite::test::Mouse mouse (screen.browser.getTree());

    for (int i = 0; i < 5; ++i)
        mouse.click (screen.getRow ("Other " + juce::String (i)).getCentre());

    REQUIRE (phazite::test::dispatchMessagesUntil ([&held] { return held.countHolding() == 4; }));
    clock.advance (4000.0);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return screen.getRows().contains ("Hat 4"); }));
    screen.library.setListsFromIndex (true);
}

TEST_CASE ("Past the spare threads, a folder whose read has waited a few seconds says its drive isn't answering")
{
    // One user's folder, every folder in it on a drive that doesn't answer: the four listing threads, then spare ones up to
    // the roots of the reads out and one (two here), each taken as the one before has been stuck for a few seconds. The next
    // waits, and its listing, once late, says so; as does each stuck one.
    phazite::test::TemporaryFolder others;
    const auto dead = others.folder.getChildFile ("Dead");
    juce::Array<juce::File> subfolders;

    for (int i = 0; i < 7; ++i)
    {
        subfolders.add (dead.getChildFile ("Folder " + juce::String (i)));
        phazite::test::writeSound (subfolders.getLast().getChildFile ("Sound.wav"), 0.05);
    }

    BrowserScreen screen (juce::StringArray { dead.getFullPathName() });
    waitForFolders (screen);
    phazite::test::HeldClock clock;
    HeldReads held (screen.library, subfolders);
    std::vector<std::unique_ptr<phazite::Library::FolderListing>> listings;

    for (int i = 0; i < 6; ++i)
    {
        CAPTURE (i);
        listings.push_back (screen.library.checkListing (subfolders[i], [] (std::optional<std::vector<phazite::LibraryItem>>) {}));
        REQUIRE (phazite::test::dispatchMessagesUntil ([&held, i] { return held.countHolding() == i + 1; }));
        clock.advance (4000.0);
    }

    // The seventh: no thread for it; late after a few seconds.
    listings.push_back (screen.library.checkListing (subfolders[6], [] (std::optional<std::vector<phazite::LibraryItem>>) {}));
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (listings.back()->isLate());
    clock.advance (4000.0);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&listings] { return listings.back()->isLate(); }));
    CHECK (held.countHolding() == 6);
    CHECK (listings.front()->isLate());
}

TEST_CASE ("A folder's rows come while a new sound's header is read for the index")
{
    // A reviewer, of the known bugs' round (2026-11-16): a few thousand sounds bounced into a folder held its rows back until
    // each one's header had been read.
    BrowserScreen screen;
    waitForFolders (screen);
    const auto entered = std::make_shared<std::atomic<bool>> (false), going = std::make_shared<std::atomic<bool>> (false);
    const juce::ScopeGuard letGo { [going] { *going = true; } };
    screen.library.setBeforeHeader (
        [entered, going] (const juce::File&)
        {
            *entered = true;

            while (! going->load())
                juce::Thread::sleep (1);
        });

    phazite::test::writeSound (screen.soundPack.getFolder().getChildFile ("Drums/Clap.wav"), 0.2);
    phazite::test::Mouse mouse (screen.browser.getTree());
    mouse.click (screen.getRow ("Drums").getCentre());
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return screen.getRows().contains ("Clap"); }));
    CHECK (entered->load()); // its header being read still
    CHECK_FALSE (
        screen.library.getWholeIndex()->find (screen.soundPack.getFolder().getChildFile ("Drums/Clap.wav").getFullPathName()).has_value());

    *going = true;
    waitForFolders (screen);
    screen.library.setBeforeHeader (nullptr);
    CHECK (
        screen.library.getWholeIndex()->find (screen.soundPack.getFolder().getChildFile ("Drums/Clap.wav").getFullPathName()).has_value());
}

TEST_CASE ("A new sound's header that doesn't answer counts as stuck: its root's next folder is read after a few seconds")
{
    // A drive whose folders answer from a cache while its files hang: the patch's header read held its root's turn for ever.
    BrowserScreen screen;
    waitForFolders (screen);
    screen.library.setListsFromIndex (false);
    phazite::test::HeldClock clock;
    const auto entered = std::make_shared<std::atomic<bool>> (false), going = std::make_shared<std::atomic<bool>> (false);
    const juce::ScopeGuard letGo { [going] { *going = true; } };
    screen.library.setBeforeHeader (
        [entered, going] (const juce::File&)
        {
            *entered = true;

            while (! going->load())
                juce::Thread::sleep (1);
        });

    phazite::test::writeSound (screen.soundPack.getFolder().getChildFile ("Drums/Clap.wav"), 0.2);
    phazite::test::Mouse mouse (screen.browser.getTree());
    mouse.click (screen.getRow ("Drums").getCentre());
    REQUIRE (phazite::test::dispatchMessagesUntil ([entered] { return entered->load(); }));

    // Loops, of the same root, waits; once the header's read has been held a few seconds, it is read.
    mouse.click (screen.getRow ("Loops").getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (screen.getRows().contains ("Break 120 BPM, 120 BPM"));
    clock.advance (4000.0);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return screen.getRows().contains ("Break 120 BPM, 120 BPM"); }));

    *going = true;
    waitForFolders (screen);
    screen.library.setBeforeHeader (nullptr);
    screen.library.setListsFromIndex (true);
}

TEST_CASE ("Presets in a folder the index doesn't hold show what each is, and a file that isn't a preset doesn't show")
{
    // As in a library larger than the index holds, and in the first seconds of every start: the rows were made without the
    // presets' headers, so a track's showed as an effect's, without its tag, and a file that wasn't one was listed.
    BrowserScreen screen;
    waitForFolders (screen);
    phazite::test::TemporaryFolder others;
    const auto folder = others.folder.getChildFile ("Presets");
    REQUIRE (folder.createDirectory());
    REQUIRE (folder.getChildFile ("Warm.phazitepreset")
                 .replaceWithText (
                     R"({ "kind": "device", "device": { "type": "phaziteEq", "name": "EQ", "instrument": false, "parameters": {} } })"));
    REQUIRE (folder.getChildFile ("Lead Track.phazitepreset").replaceWithText (R"({ "kind": "track", "track": { "name": "Lead" } })"));
    REQUIRE (folder.getChildFile ("Broken.phazitepreset").replaceWithText ("not a preset"));

    std::optional<std::vector<phazite::LibraryItem>> listed;
    const auto listing = screen.library.checkListing (folder, [&listed] (std::optional<std::vector<phazite::LibraryItem>> items)
                                                      { listed = items.value_or (std::vector<phazite::LibraryItem>()); });
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return listed.has_value() && ! screen.library.isReadingFolders(); }));

    juce::StringArray names;

    for (const auto& item : *listed)
    {
        names.add (item.name);
        CAPTURE (item.name);
        REQUIRE (item.preset.has_value());
        CHECK (item.tags.contains (item.name == juce::String ("Warm") ? "EQ" : "Track"));
    }

    CHECK (names == juce::StringArray { "Lead Track", "Warm" });
}

TEST_CASE ("The plug-in picker finds a device as its name is typed, and Enter adds it")
{
    phazite::Settings settings;
    phazite::Library library (phazite::test::getEngineContext(), settings, {});
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
    const auto song = phazite::createSong();                             // 120 BPM, 4/4
    phazite::ProjectContext context (phazite::test::getEngineContext()); // the open project, as the app has it
    auto& selection = getSelection (context);
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands (registry);
    phazite::ui::Playlist playlist (commands, context);
    phazite::test::TestWindow window (playlist, 1000, 600);
    setSong (context, song.get());
    phazite::test::dispatchPendingMessages();

    auto* lanes = phazite::test::findComponentWithTitle (playlist, "Tracks");
    REQUIRE (lanes != nullptr);
    const auto track = phazite::getTrackIDs (*song)[0];
    juce::DragAndDropTarget& target = playlist;
    const auto startOf = [&song] (phazite::ClipID clip) { return phazite::getClipRange (*song, clip).getStart().inBeats(); };

    // A sound dropped on the first track at beat 4.
    phazite::test::TemporaryFolder temp;
    phazite::LibraryItem kick;
    kick.name = "Kick";
    kick.file = phazite::test::writeSound (temp.folder.getChildFile ("Kick.wav"), 0.3);
    const auto sound = phazite::ui::toDragDescription (kick);
    const auto beat4 = juce::roundToInt (playlist.getView().beatToX (phazite::BeatPosition::fromBeats (4.0)));
    const juce::DragAndDropTarget::SourceDetails onTrack (sound, &playlist, playlist.getLocalPoint (lanes, juce::Point<int> (beat4, 10)));

    juce::Array<juce::File> landed; // the browser's sounds in the song, as the playlist tells it
    playlist.onBrowserSoundPlaced = [&landed] (const juce::File& file) { landed.add (file); };
    REQUIRE (target.isInterestedInDragSource (onTrack));
    target.itemDropped (onTrack);
    CHECK (landed.isEmpty());                  // not yet: once it is in the song
    phazite::test::waitForAudioWork (context); // a sound is read in the background, its clip added once it is (known bug 123)
    const auto kickClips = phazite::getClips (*song, track);
    REQUIRE (kickClips.size() == 1);
    CHECK (startOf (kickClips[0]) == doctest::Approx (4.0));
    CHECK (landed == juce::Array<juce::File> { kick.file });
    CHECK (selection.getClips() == std::vector { kickClips[0] }); // the selection was the drop's still: the new clip

    // An instrument dropped below the tracks gets a track of its own.
    phazite::LibraryItem synth;
    synth.kind = phazite::LibraryItem::Kind::instrument;
    synth.device = phazite::getDeviceTypes (context).at (0);
    synth.name = synth.device->name;
    REQUIRE (synth.device->isInstrument);
    const auto device = phazite::ui::toDragDescription (synth);
    const juce::DragAndDropTarget::SourceDetails below (device, &playlist, playlist.getLocalPoint (lanes, juce::Point<int> (beat4, 500)));

    REQUIRE (target.isInterestedInDragSource (below));
    target.itemDropped (below);
    const auto tracks = phazite::getTrackIDs (*song);
    REQUIRE (tracks.size() == 2);
    CHECK (phazite::getInstrument (*song, tracks[1]).isValid());
    CHECK (phazite::ui::wasPlaced (device));

    // A loop that knows its tempo follows the song's, as it played in the preview: two bars at 120 BPM span two bars
    // at 100.
    phazite::setTempo (*song, 100.0);
    phazite::LibraryItem breakLoop;
    breakLoop.name = "Break 120 BPM";
    breakLoop.file = phazite::test::writeSound (temp.folder.getChildFile ("Break 120 BPM.wav"), 4.0);
    const auto beat16 = juce::roundToInt (playlist.getView().beatToX (phazite::BeatPosition::fromBeats (16.0)));
    target.itemDropped (
        { phazite::ui::toDragDescription (breakLoop), &playlist, playlist.getLocalPoint (lanes, juce::Point<int> (beat16, 10)) });
    phazite::test::waitForAudioWork (context);
    const auto withLoop = phazite::getClips (*song, track);
    REQUIRE (withLoop.size() == 2);
    const auto loopID = withLoop[0] == kickClips[0] ? withLoop[1] : withLoop[0];
    CHECK (phazite::isAudioState (phazite::findClipState (*song, loopID)));
    CHECK (phazite::getClipTempo (*song, loopID) == doctest::Approx (120.0)); // it follows the song's tempo, from its own
    CHECK (startOf (loopID) == doctest::Approx (16.0));
    CHECK (phazite::getClipRange (*song, loopID).getLength().inBeats() == doctest::Approx (8.0));

    // Enter in the browser puts a sound on the selected track, where Space plays from.
    phazite::movePlayhead (*song, phazite::BeatPosition::fromBeats (8.0));
    selection.selectOnly (track);
    REQUIRE (playlist.placeFile (kick.file));

    // Something other than the user changes the selection while it reads: the new clip doesn't replace that.
    selection.deselectAll();
    phazite::test::waitForAudioWork (context);
    CHECK (selection.getClips().empty());
    CHECK (selection.getTracks().empty());
    const auto clips = phazite::getClips (*song, track);
    REQUIRE (clips.size() == 3);
    CHECK (std::any_of (clips.begin(), clips.end(), [&startOf] (phazite::ClipID clip) { return std::abs (startOf (clip) - 8.0) < 0.001; }));

    setSong (context, nullptr);
}

TEST_CASE ("Until Phazite Sounds are installed, the browser offers them, and lists them once a click has downloaded them")
{
    phazite::test::PackRelease source;
    phazite::test::TemporaryFolder packs;
    phazite::SoundPack soundPack (packs.folder, source.release);
    phazite::Settings settings;
    phazite::Library library (phazite::test::getEngineContext(), settings, soundPack.getFolder());
    const auto previewerOwner = phazite::createPreviewer (phazite::test::getEngineContext());
    auto& previewer = *previewerOwner;
    const phazite::ProjectContext noProject (phazite::test::getEngineContext());
    phazite::ui::Browser browser (library, previewer, soundPack, settings, noProject);
    phazite::test::TestWindow window (browser, 320, 700);
    bool pageShown = false;
    browser.onShowSoundPack = [&pageShown] { pageShown = true; };
    phazite::test::dispatchPendingMessages();

    // Their place has a row that downloads them, which says how large they are.
    const auto rows = getRows (browser);
    const auto place = rows.indexOf ("Phazite Sounds");
    REQUIRE (place >= 0);
    REQUIRE (rows.size() > place + 2);
    CHECK (rows[place + 1].startsWith (phazite::SoundPack::canDownload() ? "Download (" : "Get Them ("));
    CHECK (rows[place + 2] == juce::String ("Free drums, loops and effects to start with"));

    // A click downloads them. Where Phazite can't download, it shows the page instead, and the user installs the zip
    // downloaded in a browser.
    phazite::test::Mouse mouse (browser.getTree());
    mouse.click (getRow (browser, rows[place + 1]).getCentre());
    CHECK (pageShown != phazite::SoundPack::canDownload());

    if (! phazite::SoundPack::canDownload())
        soundPack.installFromFile (source.zip);

    REQUIRE (phazite::test::waitForSoundPack (soundPack));
    REQUIRE (soundPack.isInstalled());

    // The place lists them now (after the pack's change message, which its thread posts after it sets the state), and
    // search finds them once they are indexed.
    CHECK (phazite::test::dispatchMessagesUntil ([&browser] { return getRows (browser).contains ("Drums"); }));
    REQUIRE (phazite::test::runTimersUntil ([&library] { return ! library.isIndexing(); }));
    CHECK_FALSE (library.search ("kick").empty());
}

TEST_CASE ("A dragged item says what it is, and remembers being placed")
{
    // A sound: its file and its key, and no device.
    phazite::LibraryItem kick;
    kick.name = "Kick";
    kick.file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Kick.wav");

    const auto sound = phazite::ui::toDragDescription (kick);
    CHECK (phazite::ui::getDraggedFile (sound).value_or (juce::File()) == kick.file);
    CHECK (phazite::ui::getDraggedKey (sound) == kick.getKey());
    CHECK_FALSE (phazite::ui::getDraggedDevice (sound).has_value());

    // Whatever puts it into the song marks it, and every copy of the description sees that.
    const auto copy = sound;
    CHECK_FALSE (phazite::ui::wasPlaced (copy));
    phazite::ui::markPlaced (sound);
    CHECK (phazite::ui::wasPlaced (copy));

    // A device: its plug-in description, which says whether it is an instrument.
    juce::PluginDescription plugin;
    plugin.name = "Test Synth";
    plugin.pluginFormatName = "VST3";
    plugin.fileOrIdentifier = "Test Synth.vst3";
    plugin.isInstrument = true;

    phazite::LibraryItem synth;
    synth.kind = phazite::LibraryItem::Kind::instrument;
    synth.name = plugin.name;
    synth.device = phazite::makeDeviceType (plugin);

    const auto dragged = phazite::ui::toDragDescription (synth);
    const auto device = phazite::ui::getDraggedDevice (dragged);
    REQUIRE (device.has_value());
    CHECK (device->name == juce::String ("Test Synth"));
    CHECK (device->isInstrument);
    CHECK (device->category == juce::String ("Instrument"));
    CHECK (phazite::ui::getDraggedKey (dragged) == synth.getKey());
    CHECK_FALSE (phazite::ui::getDraggedFile (dragged).has_value());

    // A file is only taken with its full path.
    auto* relative = new juce::DynamicObject();
    relative->setProperty ("file", "Kick.wav");
    CHECK_FALSE (phazite::ui::getDraggedFile (juce::var (relative)).has_value());
}

TEST_CASE ("A device's presets menu lists Phazite's for it by kind of sound, then the user's, and loads the one chosen")
{
    // Phazite's presets, in a folder of their own, and the user's: an EQ's in each, and a compressor's that doesn't belong.
    phazite::test::TemporaryFolder temp;
    const auto factory = temp.folder.getChildFile ("Factory"), own = temp.folder.getChildFile ("Presets");
    const auto writePreset = [] (const juce::File& file, const juce::String& type, const juce::String& name, float lowShelf)
    {
        const auto text = "{ \"kind\": \"device\", \"device\": { \"type\": \"" + type + "\", \"name\": \"" + name
                          + "\", \"instrument\": false, \"parameters\": { \"band2Gain\": " + juce::String (lowShelf) + " } } }";
        REQUIRE (file.getParentDirectory().createDirectory());
        REQUIRE (file.replaceWithText (text));
    };

    writePreset (factory.getChildFile ("EQ/Mixing/Warm Lows.phazitepreset"), "phaziteEq", "EQ", 4.0f);
    writePreset (factory.getChildFile ("EQ/Mastering/Gentle Tilt.phazitepreset"), "phaziteEq", "EQ", 1.0f);
    writePreset (own.getChildFile ("EQ/Mine.phazitepreset"), "phaziteEq", "EQ", -2.0f);
    writePreset (own.getChildFile ("EQ/Squash.phazitepreset"), "phaziteCompressor", "Compressor", 0.0f);

    phazite::Settings settings;
    phazite::Library library { phazite::test::getEngineContext(), settings, {}, own, factory };
    REQUIRE (phazite::test::runTimersUntil ([&library] { return ! library.isIndexing(); }));

    juce::File loaded;
    const auto menu = phazite::ui::makeDevicePresetMenu (library, "EQ", "EQ", [&loaded] (const juce::File& file) { loaded = file; });

    // Phazite's kinds of sound first, each a submenu; then the user's own, the compressor's left out.
    juce::StringArray top, mixing;
    juce::PopupMenu::Item* warm = nullptr;

    for (juce::PopupMenu::MenuItemIterator item (menu); item.next();)
    {
        top.add (item.getItem().isSectionHeader ? "[" + item.getItem().text + "]" : item.getItem().text);

        if (item.getItem().text == "Mixing" && item.getItem().subMenu != nullptr)
            for (juce::PopupMenu::MenuItemIterator inside (*item.getItem().subMenu); inside.next();)
            {
                mixing.add (inside.getItem().text);
                warm = &inside.getItem();
            }
    }

    CHECK (top == juce::StringArray { "Mastering", "Mixing", "[Your Presets]", "Mine" });
    CHECK (mixing == juce::StringArray { "Warm Lows" });
    REQUIRE (warm != nullptr);
    REQUIRE (warm->action != nullptr);
    warm->action();
    CHECK (loaded == factory.getChildFile ("EQ/Mixing/Warm Lows.phazitepreset"));

    // None for a device without presets.
    CHECK (phazite::ui::makeDevicePresetMenu (library, "Reverb", "Reverb", [] (const juce::File&) {}).getNumItems() == 0);
}

TEST_CASE ("The browser is laid out as VS Code's Explorer: its title, its rows' height, each level further in, the outline after keys")
{
    namespace ListRow = phazite::ui::ListRow;
    namespace SideBar = phazite::ui::SideBar;
    BrowserScreen screen;
    auto& tree = screen.browser.getTree();

    // Its title along the top, the search under it, and the tree as wide as the panel.
    const auto titles = phazite::test::findAll<phazite::ui::ViewTitle> (screen.browser);
    REQUIRE (titles.size() == 1);
    CHECK (titles[0]->getName() == juce::String ("Browser"));
    CHECK (titles[0]->getBounds() == screen.browser.getLocalBounds().withHeight (SideBar::titleHeight));
    CHECK (screen.browser.getSearchField().getY() >= titles[0]->getBottom());
    CHECK (screen.browser.getSearchField().getHeight() == SideBar::controlHeight);
    CHECK (tree.getX() == 0);
    CHECK (tree.getWidth() == screen.browser.getWidth());

    // Rows as tall as VS Code's, each level a step further in, the first at the tree's edge.
    CHECK (screen.getRow ("Phazite Sounds").getHeight() == ListRow::height);
    CHECK (screen.getRow ("Phazite Sounds").getX() == 0);
    CHECK (screen.getRow ("Drums").getX() == SideBar::indent);

    // The selected row's outline, at the row's inset edge, in the focus colour: not after a click, which selects it; once
    // the keys move the selection, there.
    const auto focus = phazite::ui::getPalette (tree).focus;
    const auto edgeOf = [&screen, &tree] (const juce::String& name)
    {
        juce::Image image (juce::Image::ARGB, tree.getWidth(), tree.getHeight(), true, juce::SoftwareImageType());
        {
            juce::Graphics g (image);
            tree.paintEntireComponent (g, false);
        }

        return image.getPixelAt ((int) ListRow::inset, screen.getRow (name).getCentreY());
    };

    phazite::test::Mouse mouse (tree);
    mouse.click (screen.getRow ("Drums").getCentre()); // a folder: it opens
    phazite::test::dispatchPendingMessages();
    REQUIRE (tree.hasKeyboardFocus (true));
    CHECK (edgeOf ("Drums") != focus);
    screen.window.pressKey (downKey);
    REQUIRE (screen.browser.getSelectedItem().has_value());
    CHECK (screen.browser.getSelectedItem()->name == juce::String ("Kick"));
    CHECK (edgeOf ("Kick") == focus);
}

TEST_CASE ("Enter in the search acts on the newest text's results, which it waits for; the same text after the index changed is "
           "looked for again")
{
    BrowserScreen screen;
    auto& search = screen.browser.getSearchField();
    search.grabKeyboardFocus();
    search.setText ("break", true);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return screen.getRows().contains ("Break 120 BPM, 120 BPM"); }));

    // The next text's search held until let go: Enter meanwhile waits for its results, and acts on them.
    std::atomic<bool> letGo { false };
    screen.library.setBeforeSearch (
        [&letGo]
        {
            while (! letGo)
                juce::Thread::sleep (1);
        });

    search.setText ("kick", true);
    phazite::test::dispatchPendingMessages(); // the field tells of its change, and the search starts
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::returnKey));
    CHECK_FALSE (screen.browser.getTree().hasKeyboardFocus (true));

    letGo = true;
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return screen.browser.getSelectedItem().has_value(); }));
    screen.library.setBeforeSearch (nullptr);
    CHECK (screen.browser.getSelectedItem()->name == juce::String ("Kick"));
    CHECK (screen.browser.getTree().hasKeyboardFocus (true));

    // The text taken away, the index made again with another kick: the same text finds it, not the rows from before.
    search.setText ({}, true);
    phazite::test::dispatchPendingMessages();
    phazite::test::writeSound (screen.soundPack.getFolder().getChildFile ("Drums/Kick Two.wav"), 0.3);
    screen.library.refresh();
    REQUIRE (phazite::test::runTimersUntil ([&screen] { return ! screen.library.isIndexing(); }, 60000));
    phazite::test::dispatchPendingMessages(); // the index's change told
    search.setText ("kick", true);
    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&screen]
        {
            const auto rows = screen.getRows();
            return std::any_of (rows.begin(), rows.end(), [] (const juce::String& row) { return row.startsWith ("Kick Two"); });
        }));
}

TEST_CASE ("Typing on after Enter, before the results are in, keeps the keys in the search field")
{
    BrowserScreen screen;
    auto& search = screen.browser.getSearchField();
    search.grabKeyboardFocus();

    std::atomic<bool> letGo { false };
    screen.library.setBeforeSearch (
        [&letGo]
        {
            while (! letGo)
                juce::Thread::sleep (1);
        });

    search.setText ("kick", true);
    phazite::test::dispatchPendingMessages();
    screen.window.pressKey (juce::KeyPress (juce::KeyPress::returnKey)); // waits for "kick"'s results
    search.setText ("kicks", true);                                      // and typing goes on
    phazite::test::dispatchPendingMessages();

    letGo = true;
    REQUIRE (phazite::test::dispatchMessagesUntil ([&screen] { return ! screen.getRows().contains ("Drums"); }));
    phazite::test::dispatchPendingMessages();
    screen.library.setBeforeSearch (nullptr);
    CHECK (search.hasKeyboardFocus (true));
    CHECK_FALSE (screen.browser.getTree().hasKeyboardFocus (true));
}
