// SPDX-License-Identifier: AGPL-3.0-or-later
// The browser grown on its folders (docs/design.md, section 11; roadmap, After v1): its filters, which narrow a big folder to
// the kicks in one key alone; what its index reads of loop data, once; collections kept by file, a renamed sound in them
// still; and Swap, which tries a pad's kicks in place with the keys and keeps one, or puts the old one back, and shows a
// device's presets alone to try them.
#include "SoundPackTestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/drums/DrumSampler.h"
#include "devices/reverb/Reverb.h"
#include "devices/synth/Synth.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/DeviceHandle.h"
#include "model/devices/SwapModel.h"
#include "model/engine/internal/Preview.h"
#include "model/mixer/MixerModel.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "services/BrowserModel.h"
#include "ui/browser/Browser.h"
#include "ui/devices/DrumSamplerEditor.h"

#include <algorithm>

namespace
{
using phazite::SoundKey;
using Filter = phazite::ui::FilterBar::Filter;
using Kind = phazite::BrowserPlace::Kind;

const juce::KeyPress downKey (juce::KeyPress::downKey), upKey (juce::KeyPress::upKey), returnKey (juce::KeyPress::returnKey),
    escapeKey (juce::KeyPress::escapeKey);

// A big folder of the user's, laid out as sample packs lay theirs out: kicks tuned to notes, snares, 808s and loops in keys,
// each a sound of its own (sets know a sound by what it holds: two the same are one).
juce::File writeBigFolder (const juce::File& folder)
{
    using phazite::test::writeSound;
    writeSound (folder.getChildFile ("Drums/Kicks/Kick 01 (F).wav"), 0.3, 87.3);
    writeSound (folder.getChildFile ("Drums/Kicks/Kick 02 (G).wav"), 0.3, 98.0);
    writeSound (folder.getChildFile ("Drums/Kicks/Kick 03 - F.wav"), 0.3, 43.7);
    writeSound (folder.getChildFile ("Drums/Kicks/Kick 04.wav"), 0.3, 60.0);
    writeSound (folder.getChildFile ("Drums/Snares/Snare 01 (F).wav"), 0.3, 174.6);
    writeSound (folder.getChildFile ("Drums/Snares/Snare 02.wav"), 0.3, 200.0);
    writeSound (folder.getChildFile ("808s/808 Long F.wav"), 1.0, 43.7);
    writeSound (folder.getChildFile ("Loops/Bass Loop Fm 120 BPM.wav"), 2.0, 87.3);
    writeSound (folder.getChildFile ("Loops/Keys Loop Am 90 BPM.wav"), 2 * 4 * 60.0 / 90.0, 440.0);
    return folder;
}

// Some seconds of silence in a WAV file, with these loop data.
juce::File writeWithLoopData (const juce::File& file, double seconds, const std::unordered_map<juce::String, juce::String>& data)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::AudioBuffer<float> silence (1, (int) (seconds * 44100.0));
    silence.clear();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (auto writer = juce::WavAudioFormat().createWriterFor (
            stream,
            juce::AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (16).withMetadataValues (
                data)))
        writer->writeFromAudioSampleBuffer (silence, 0, silence.getNumSamples());

    return file;
}

juce::StringArray getNames (const std::vector<phazite::LibraryItem>& items)
{
    juce::StringArray names;

    for (const auto& item : items)
        names.add (item.name);

    return names;
}

juce::StringArray getTitles (const std::vector<phazite::BrowserPlace>& places)
{
    juce::StringArray titles;

    for (const auto& place : places)
        titles.add (place.name);

    return titles;
}

bool waitForIndex (const phazite::Library& library)
{
    const auto done = phazite::test::runTimersUntil ([&library] { return ! library.isIndexing(); }, 10000);
    phazite::test::dispatchPendingMessages(); // what the index tells as it ends
    return done;
}

// A library of a big folder of the user's, without Phazite Sounds, and the browser's model of it.
struct Libraries
{
    Libraries()
    {
        library.addUserFolder (folder);
        REQUIRE (waitForIndex (library));
    }

    std::optional<phazite::LibraryItem> find (const juce::String& path) const
    {
        return library.findItem (folder.getChildFile (path).getFullPathName());
    }

    phazite::test::TemporaryFolder temp;
    const juce::File folder = writeBigFolder (temp.folder.getChildFile ("Big Folder"));
    phazite::SoundPack pack { temp.folder.getChildFile ("Packs") }; // not installed
    phazite::Settings settings;
    phazite::Library library { phazite::test::getEngineContext(), settings };
    phazite::BrowserModel model { library, pack };
};

// The rows of a browser's tree, as screen readers read them, and where the first row of that name is.
juce::StringArray getRows (phazite::ui::Browser& browser)
{
    juce::StringArray rows;
    auto& tree = browser.getTree();

    for (int i = 0; i < tree.getNumRowsInTree(); ++i)
        rows.add (tree.getItemOnRow (i)->getAccessibilityName());

    return rows;
}

juce::Rectangle<int> getRow (phazite::ui::Browser& browser, const juce::String& name)
{
    auto& tree = browser.getTree();

    for (int i = 0; i < tree.getNumRowsInTree(); ++i)
        if (auto* item = tree.getItemOnRow (i); item->getAccessibilityName().upToFirstOccurrenceOf (",", false, false) == name)
            return item->getItemPosition (true).withHeight (item->getItemHeight());

    FAIL_CHECK ("no row called " << name);
    return {};
}

// The rows of a browser's tree that are in view, from the top of the view, as screen readers read them.
juce::StringArray getRowsInView (phazite::ui::Browser& browser)
{
    juce::StringArray rows;
    auto& tree = browser.getTree();
    const auto* viewport = tree.getViewport();
    const auto top = viewport->getViewPositionY(), bottom = top + viewport->getViewHeight();

    for (int i = 0; i < tree.getNumRowsInTree(); ++i)
        if (auto* item = tree.getItemOnRow (i))
            if (const auto y = item->getItemPosition (false).getY(); y >= top && y + item->getItemHeight() <= bottom)
                rows.add (item->getAccessibilityName());

    return rows;
}

// A device's preset, as Save Preset writes one: the values of some of its parameters ("\"cutoff\": 400"), each other at
// its default.
void writeDevicePreset (const juce::File& file, const char* type, const juce::String& name, bool instrument,
                        const juce::String& parameters = {})
{
    REQUIRE (file.getParentDirectory().createDirectory());
    REQUIRE (file.replaceWithText ("{ \"kind\": \"device\", \"device\": { \"type\": \"" + juce::String (type) + "\", \"name\": \"" + name
                                   + "\", \"instrument\": " + (instrument ? "true" : "false") + ", \"parameters\": { " + parameters
                                   + " } } }"));
}
} // namespace

//==============================================================================
TEST_CASE ("The filters narrow every place to the sounds with their values, and offer only what is there, with counts")
{
    Libraries libraries;
    const auto& model = libraries.model;

    // Over everything: each type, loops and one-shots, tempos and keys there are, with how many have them.
    const auto all = model.getFilterOptions ({}, {});
    const auto countOf = [] (const std::vector<std::pair<juce::String, int>>& types, const juce::String& type)
    {
        const auto found = std::find_if (types.begin(), types.end(), [&type] (const auto& t) { return t.first == type; });
        return found != types.end() ? found->second : 0;
    };

    CHECK (countOf (all.types, "Kick") == 4);
    CHECK (countOf (all.types, "Snare") == 2);
    CHECK (countOf (all.types, "Bass") == 2); // the 808, and the bass loop
    CHECK (countOf (all.types, "Clap") == 0);
    CHECK (std::none_of (all.types.begin(), all.types.end(), [] (const auto& t) { return t.second == 0; })); // only what is there
    CHECK (all.loops == 2);
    CHECK (all.oneShots == 7);
    CHECK (all.tempos == std::vector<std::pair<int, int>> { { 90, 1 }, { 120, 1 } });
    CHECK (all.keys
           == std::vector<std::pair<SoundKey, int>> { { { 5, SoundKey::Mode::note }, 4 },
                                                      { { 5, SoundKey::Mode::minor }, 1 },
                                                      { { 7, SoundKey::Mode::note }, 1 },
                                                      { { 9, SoundKey::Mode::minor }, 1 } });

    // Kicks: the keys among them; the other types as the key and tempo leave them, so that another can be chosen.
    phazite::BrowserFilters kicks;
    kicks.type = "Kick";
    const auto amongKicks = model.getFilterOptions ({}, kicks);
    CHECK (amongKicks.keys
           == std::vector<std::pair<SoundKey, int>> { { { 5, SoundKey::Mode::note }, 2 }, { { 7, SoundKey::Mode::note }, 1 } });
    CHECK (countOf (amongKicks.types, "Snare") == 2);
    CHECK (amongKicks.loops == 0);
    CHECK (amongKicks.oneShots == 4);

    // The kicks in F: the places that have some, each with how many, and the big folder as one list of them.
    auto kicksInF = kicks;
    kicksInF.key = SoundKey { 5, SoundKey::Mode::note };
    const auto places = model.getPlaces (kicksInF);
    REQUIRE (getTitles (places) == juce::StringArray { "Big Folder" });
    CHECK (places[0].count == 2);
    CHECK (getNames (model.getContents (places[0], kicksInF).items) == juce::StringArray { "Kick 01 (F)", "Kick 03 - F" });

    // Search finds them with the filters, and says when the filters leave nothing.
    CHECK (getNames (model.search ("kick", kicksInF).items) == juce::StringArray { "Kick 01 (F)", "Kick 03 - F" });
    CHECK (model.search ("snare", kicksInF).messages.contains ("Nothing matches \"snare\" with the filters set"));
    CHECK (countOf (model.getFilterOptions ("snare", {}).types, "Snare") == 2);

    // Filters take sounds and MIDI files alone: no folder, device or preset has their values.
    CHECK_FALSE (kicks.accepts (*libraries.find ("Drums")));
    CHECK (phazite::BrowserFilters().accepts (*libraries.find ("Drums")));

    // A loop's row says its key and tempo.
    CHECK (phazite::BrowserModel::getDetail (*libraries.find ("Loops/Bass Loop Fm 120 BPM.wav"))
           == juce::String::fromUTF8 ("Fm \xc2\xb7 120 BPM"));
}

TEST_CASE ("The index reads a sound's loop data once, and keeps what it read for the next start")
{
    phazite::test::TemporaryFolder temp;
    const auto folder = temp.folder.getChildFile ("Sounds");
    const auto factsFile = temp.folder.getChildFile ("Library facts.txt");

    // Eight beats in four seconds, on A: a loop at 120 BPM in A, though its name says none of it.
    const auto pad = writeWithLoopData (folder.getChildFile ("Texture.wav"), 4.0,
                                        { { juce::WavAudioFormat::acidBeats, "8" },
                                          { juce::WavAudioFormat::acidOneShot, "0" },
                                          { juce::WavAudioFormat::acidRootSet, "1" },
                                          { juce::WavAudioFormat::acidRootNote, "57" } });
    phazite::Settings settings;
    settings.set (phazite::Settings::browserFolders, juce::StringArray { folder.getFullPathName() });

    {
        phazite::Library library (phazite::test::getEngineContext(), settings, {}, {}, {}, factsFile);
        REQUIRE (waitForIndex (library));
        const auto item = library.findItem (pad.getFullPathName());
        REQUIRE (item.has_value());
        CHECK (item->tags == juce::StringArray { "Loop", "120 BPM", "A" });
        CHECK (item->key == SoundKey { 9, SoundKey::Mode::note });
        REQUIRE (library.getIndex()->size() == 1);
        CHECK (library.getIndex()->front().tags.contains ("Loop"));
        CHECK (factsFile.existsAsFile());
    }

    // The next start takes what the facts file says of a file that is as it was then, without reading it again: here the
    // file says it is in C# minor.
    juce::StringArray lines;
    factsFile.readLines (lines);
    lines.removeEmptyStrings();
    REQUIRE (lines.size() == 1);
    auto fields = juce::StringArray::fromTokens (lines[0], "\t", "");
    REQUIRE (fields.size() == 8);
    fields.set (5, "1");
    fields.set (6, juce::String ((int) SoundKey::Mode::minor));
    factsFile.replaceWithText (fields.joinIntoString ("\t") + "\n");

    phazite::Library again (phazite::test::getEngineContext(), settings, {}, {}, {}, factsFile);
    REQUIRE (waitForIndex (again));
    CHECK (again.findItem (pad.getFullPathName())->key == SoundKey { 1, SoundKey::Mode::minor });
}

TEST_CASE ("Collections keep their sounds by file: one renamed or moved stays in them, as in favorites")
{
    Libraries libraries;
    auto& library = libraries.library;
    const auto kick = libraries.find ("Drums/Kicks/Kick 01 (F).wav");
    REQUIRE (kick.has_value());

    // Named as asked, a taken name made unique, an empty one refused.
    CHECK (library.addCollection ("Dark") == juce::String ("Dark"));
    CHECK (library.addCollection (" Dark ") == juce::String ("Dark 2"));
    CHECK (library.addCollection ("  ").isEmpty());
    CHECK (library.getCollections() == juce::StringArray { "Dark", "Dark 2" });

    library.setInCollection (*kick, "Dark", true);
    library.setFavorite (*kick, true);
    CHECK (library.isInCollection (*kick, "Dark"));
    CHECK_FALSE (library.isInCollection (*kick, "Dark 2"));

    // Renamed where it is: found again, at its new name, in its collection and its favorites.
    const auto renamed = libraries.folder.getChildFile ("Drums/Kicks/Kick 01 Dark (F).wav");
    REQUIRE (kick->file.moveFileTo (renamed));
    CHECK (getNames (library.getCollection ("Dark")) == juce::StringArray { "Kick 01 Dark (F)" });
    CHECK (getNames (library.getFavorites()) == juce::StringArray { "Kick 01 Dark (F)" });
    CHECK (library.isFavorite (*library.findItem (renamed.getFullPathName())));

    // Moved to another of the library's folders: found again once the folders are indexed again.
    const auto moved = libraries.folder.getChildFile ("Moved/Kick X.wav");
    moved.getParentDirectory().createDirectory();
    REQUIRE (renamed.moveFileTo (moved));
    library.refresh();
    REQUIRE (waitForIndex (library));
    CHECK (getNames (library.getCollection ("Dark")) == juce::StringArray { "Kick X" });

    // Kept in the settings, for the next start.
    phazite::Library again (phazite::test::getEngineContext(), libraries.settings);
    REQUIRE (waitForIndex (again));
    CHECK (again.getCollections() == juce::StringArray { "Dark", "Dark 2" });
    CHECK (getNames (again.getCollection ("Dark")) == juce::StringArray { "Kick X" });

    // Renamed, not to a name that is taken, and deleted: its sounds stay where they are.
    CHECK (library.renameCollection ("Dark 2", "Bright"));
    CHECK_FALSE (library.renameCollection ("Bright", "Dark"));
    library.setInCollection (*library.findItem (moved.getFullPathName()), "Dark", false);
    CHECK (library.getCollection ("Dark").empty());
    library.removeCollection ("Bright");
    CHECK (library.getCollections() == juce::StringArray { "Dark" });
    CHECK (moved.existsAsFile());
}

//==============================================================================
namespace
{
// A browser in a window over the big folder, with a project open.
struct FilterScreen
{
    FilterScreen()
    {
        setSong (context, song.get());
        library.addUserFolder (libraries.folder);
        REQUIRE (waitForIndex (library));
        browser.setBounds (0, 0, 320, 700);
    }

    ~FilterScreen() { previewer.stop(); }

    Libraries libraries;
    phazite::Song::Ptr song = phazite::createSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::Library& library = libraries.library;
    std::unique_ptr<phazite::Previewer> previewerOwner = phazite::createPreviewer (phazite::test::getEngineContext());
    phazite::Previewer& previewer = *previewerOwner;
    phazite::ui::Browser browser { library, previewer, libraries.pack, libraries.settings, context };
    phazite::test::TestWindow window { browser, 320, 700 };

    juce::StringArray getRows() { return ::getRows (browser); }
    juce::Rectangle<int> getRow (const juce::String& name) { return ::getRow (browser, name); }

    void choose (Filter filter, const juce::String& value)
    {
        REQUIRE (phazite::test::runMenuItem (browser.getFilterBar().getMenu (filter), { value }));
        phazite::test::dispatchPendingMessages();
    }
};
} // namespace

TEST_CASE ("A beat maker narrows a big folder down to the kicks in one key with the filters alone")
{
    FilterScreen screen;
    auto& bar = screen.browser.getFilterBar();
    CHECK_FALSE (bar.getClearButton().isVisible());

    // The type's menu offers the types there are, each with how many.
    const auto typeMenu = bar.getMenu (Filter::type);
    const auto* kickItem = phazite::test::findMenuItem (typeMenu, { "Kick" });
    REQUIRE (kickItem != nullptr);
    CHECK (kickItem->shortcutKeyDescription == juce::String ("4"));
    CHECK (phazite::test::findMenuItem (typeMenu, { "Clap" }) == nullptr);

    // Kicks: then the keys among them, F twice and G once.
    screen.choose (Filter::type, "Kick");
    CHECK (bar.getChip (Filter::type).getTitle() == juce::String ("Type filter: Kick"));
    CHECK (bar.getClearButton().isVisible());
    CHECK (screen.getRows() == juce::StringArray { "Big Folder, 4" });

    const auto keyMenu = bar.getMenu (Filter::key);
    REQUIRE (phazite::test::findMenuItem (keyMenu, { "F" }) != nullptr);
    CHECK (phazite::test::findMenuItem (keyMenu, { "F" })->shortcutKeyDescription == juce::String ("2"));
    CHECK (phazite::test::findMenuItem (keyMenu, { "G" })->shortcutKeyDescription == juce::String ("1"));
    CHECK (phazite::test::findMenuItem (keyMenu, { "F minor" }) == nullptr); // a bass loop's: no kick has it

    // The kicks in F, as one list in the big folder, its folders' within it.
    screen.choose (Filter::key, "F");
    CHECK (screen.getRows() == juce::StringArray { "Big Folder, 2" });
    phazite::test::Mouse mouse (screen.browser.getTree());
    mouse.click (screen.getRow ("Big Folder").getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getRows() == juce::StringArray { "Big Folder, 2", "Kick 01 (F), F", "Kick 03 - F, F" });

    // The type's x takes it away: everything in F.
    phazite::test::Mouse chip (bar.getChip (Filter::type));
    chip.click (bar.getClearArea (Filter::type).getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (screen.browser.getFilters().type.isEmpty());
    CHECK (screen.getRows().contains ("Big Folder, 4"));

    // Clear takes the rest away, with the search's words: the folders open again as they were before the filters.
    screen.browser.getSearchField().setText ("kick", true);
    phazite::test::dispatchPendingMessages();
    phazite::test::Mouse clear (bar.getClearButton());
    clear.click (bar.getClearButton().getLocalBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (screen.browser.getFilters().isEmpty());
    CHECK (screen.browser.getSearchField().isEmpty());
    CHECK_FALSE (bar.getClearButton().isVisible());
    CHECK (screen.getRows().contains ("Big Folder"));
    CHECK_FALSE (screen.getRows().contains ("Kick 01 (F), F"));
}

TEST_CASE ("The four filters fit the side panel's width in one row, each name whole, a set one's value too")
{
    phazite::ui::FilterBar bar;
    bar.show ({}, false);

    // The browser's row at the side panel's width at first (as the README's picture shows it).
    constexpr int width = 218;
    CHECK (bar.getHeightForWidth (width) == phazite::ui::FilterChipTint::height);
    bar.setBounds (0, 0, width, bar.getHeightForWidth (width));

    const juce::Font font { juce::FontOptions (phazite::ui::TextSize::small) };
    const auto fits = [&] (Filter filter, const juce::String& text)
    {
        INFO (text.toStdString());
        const auto room = bar.getChip (filter).getWidth() - 2 * phazite::ui::FilterChipTint::textInset;
        CHECK (juce::GlyphArrangement::getStringWidth (font, text) <= (float) room);
    };

    fits (Filter::type, "Type");
    fits (Filter::loops, "Loop");
    fits (Filter::tempo, "Tempo");
    fits (Filter::key, "Key");

    // Set, a chip shows its value and its x; Clear comes after the chips.
    phazite::BrowserFilters set;
    set.type = "Percussion";
    bar.show (set, false);
    const auto& chip = bar.getChip (Filter::type);
    CHECK (juce::GlyphArrangement::getStringWidth (font, "Percussion")
           <= (float) (chip.getWidth() - bar.getClearArea (Filter::type).getWidth() - phazite::ui::FilterChipTint::textInset));
    CHECK (bar.getClearButton().isVisible());
    CHECK (bar.getClearButton().getX() >= 0);
}

TEST_CASE ("A sound goes into a new collection from its menu, named in a field over the tree, and shows there")
{
    FilterScreen screen;
    auto& browser = screen.browser;
    const auto kick = screen.libraries.find ("Drums/Kicks/Kick 01 (F).wav");
    REQUIRE (kick.has_value());

    REQUIRE (phazite::test::runMenuItem (browser.getItemMenu (*kick, false), { "Add to Collection", "New Collection..." }));
    auto* field = browser.getNameField();
    REQUIRE (field != nullptr);
    CHECK (field->hasKeyboardFocus (false));
    field->selectAll();
    field->insertTextAtCaret ("Dark Kicks");
    phazite::test::dispatchPendingMessages();
    screen.window.pressKey (returnKey);
    phazite::test::dispatchPendingMessages();

    CHECK (screen.library.getCollections() == juce::StringArray { "Dark Kicks" });
    CHECK (screen.library.isInCollection (*kick, "Dark Kicks"));

    // Collections hold it; its menu ticks the collection, which chosen again takes it out.
    phazite::test::Mouse mouse (browser.getTree());
    mouse.click (screen.getRow ("Collections").getCentre());
    phazite::test::dispatchPendingMessages();
    mouse.click (screen.getRow ("Dark Kicks").getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (screen.getRows().contains ("Kick 01 (F), F"));

    const auto menu = browser.getItemMenu (*kick, false);
    REQUIRE (phazite::test::findMenuItem (menu, { "Add to Collection", "Dark Kicks" }) != nullptr);
    CHECK (phazite::test::findMenuItem (menu, { "Add to Collection", "Dark Kicks" })->isTicked);
    REQUIRE (phazite::test::runMenuItem (menu, { "Add to Collection", "Dark Kicks" }));
    CHECK_FALSE (screen.library.isInCollection (*kick, "Dark Kicks"));

    // Esc names nothing.
    REQUIRE (phazite::test::runMenuItem (browser.getItemMenu (*kick, false), { "Add to Collection", "New Collection..." }));
    REQUIRE (browser.getNameField() != nullptr);
    screen.window.pressKey (escapeKey);
    phazite::test::dispatchPendingMessages();
    CHECK (screen.library.getCollections() == juce::StringArray { "Dark Kicks" });
}

TEST_CASE ("A pad's kick is swapped for another and back with the keys, kept with Enter as one step, or put back with Esc")
{
    phazite::test::TestSong song;
    const auto drums = song.setInstrument (song.firstTrack, phazite::devices::DrumSampler::typeID);
    const auto pack = song.getSoundsFolder().getChildFile ("Pack");
    phazite::test::writeSound (pack.getChildFile ("Kick One.wav"), 0.2);
    phazite::test::writeSound (pack.getChildFile ("Kick Two.wav"), 0.3);
    phazite::test::writeSound (pack.getChildFile ("Snare One.wav"), 0.2);

    phazite::test::TemporaryFolder temp;
    phazite::SoundPack sounds { temp.folder.getChildFile ("Packs") };
    phazite::Settings settings;
    phazite::Library library (phazite::test::getEngineContext(), settings);
    library.addUserFolder (pack);
    REQUIRE (waitForIndex (library));
    const auto previewerOwner = phazite::createPreviewer (phazite::test::getEngineContext());
    phazite::ui::Browser browser (library, *previewerOwner, sounds, settings, song.context);
    phazite::test::TestWindow window (browser, 320, 700);
    auto started = 0;
    browser.onSwapStarted = [&started] { ++started; };

    const auto handle = phazite::getDeviceHandle (song.context, drums);
    const auto before = handle.getPadName (36);
    auto& undo = *song.context.getUndoManager();
    const auto padName = [&handle] { return handle.getPadName (36); };

    // Swap on the kick's pad: the browser shows kicks, its strip says what it swaps, and it has the keys.
    browser.startSwap (drums, 36);
    phazite::test::dispatchPendingMessages();
    REQUIRE (browser.isSwapping());
    CHECK (started == 1);
    CHECK (browser.getSwapStrip().isVisible());
    CHECK (browser.getFilters().type == juce::String ("Kick"));
    CHECK (browser.getTree().hasKeyboardFocus (true));

    phazite::test::Mouse mouse (browser.getTree());
    mouse.click (getRow (browser, "Pack").getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (getRows (browser).contains ("Kick One"));
    CHECK_FALSE (getRows (browser).contains ("Snare One"));

    // Each kick the keys move to plays on the pad; Up goes back; nothing plays as a preview meanwhile.
    window.pressKey (downKey);
    CHECK (padName() == juce::String ("Kick One"));
    window.pressKey (downKey);
    CHECK (padName() == juce::String ("Kick Two"));
    window.pressKey (upKey);
    CHECK (padName() == juce::String ("Kick One"));
    CHECK_FALSE (previewerOwner->isPlaying());

    // Enter keeps it, as one step; the browser is as it was.
    window.pressKey (returnKey);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (browser.isSwapping());
    CHECK_FALSE (browser.getSwapStrip().isVisible());
    CHECK (browser.getFilters().isEmpty());
    CHECK (padName() == juce::String ("Kick One"));
    CHECK (undo.getUndoDescription() == juce::String ("Put Kick One on Pad 36"));

    // Again, and Esc puts the one before back, adding no step.
    browser.startSwap (drums, 36);
    phazite::test::dispatchPendingMessages();
    mouse.click (getRow (browser, "Pack").getCentre());
    phazite::test::dispatchPendingMessages();
    window.pressKey (downKey);
    window.pressKey (downKey);
    CHECK (padName() == juce::String ("Kick Two"));
    window.pressKey (escapeKey);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (browser.isSwapping());
    CHECK (padName() == juce::String ("Kick One"));
    CHECK (undo.getUndoDescription() == juce::String ("Put Kick One on Pad 36"));

    phazite::undo (*phazite::findSong (song.context));
    CHECK (padName() == before);
}

TEST_CASE ("A drum pad's Swap asks for its pad, and is lit while that pad is being swapped")
{
    phazite::test::TestSong song;
    const auto drums = song.setInstrument (song.firstTrack, phazite::devices::DrumSampler::typeID);
    phazite::ui::DrumSamplerEditor editor (phazite::getDeviceHandle (song.context, drums));
    phazite::test::TestWindow window (editor, editor.getWidth(), editor.getHeight());

    // Only while something can swap.
    editor.refresh();
    CHECK_FALSE (editor.getSwapButton().isVisible());
    auto asked = -1;
    editor.onSwap = [&asked] (int pad) { asked = pad; };
    editor.refresh();
    REQUIRE (editor.getSwapButton().isVisible());

    editor.selectPad (38);
    phazite::test::Mouse mouse (editor.getSwapButton());
    mouse.click (editor.getSwapButton().getLocalBounds().getCentre());
    CHECK (asked == 38);

    phazite::Swap swap (song.context, drums, 38);
    editor.refresh();
    CHECK (editor.getSwapButton().getToggleState());
    editor.selectPad (39);
    CHECK_FALSE (editor.getSwapButton().getToggleState());
}

TEST_CASE ("A device's Swap shows its presets alone, from the top, Down trying each; then the browser is as it was")
{
    phazite::test::TestSong song;
    const auto synth = song.setInstrument (song.firstTrack, phazite::devices::Synth::typeID);
    const auto reverb = song.addEffect (song.firstTrack, phazite::devices::Reverb::typeID);

    // Phazite's presets, a folder for each of many devices before the synth's (as they come with Phazite, it far down the
    // tree), the synth's by kind of sound; and the user's, one of them the synth's. None for the reverb.
    phazite::test::TemporaryFolder temp;
    const auto factory = temp.folder.getChildFile ("Factory"), own = temp.folder.getChildFile ("Presets");

    for (const auto* other : { "Chorus", "Compressor", "Delay", "Drum Sampler", "EQ", "Filter", "Flanger", "FM", "Limiter", "Multiband",
                               "Phaser", "Sampler", "Saturator", "Stereo Expander" })
        writeDevicePreset (factory.getChildFile (other).getChildFile ("Default").getChildFile (juce::String (other) + " One.phazitepreset"),
                           "phaziteEq", "EQ", false);

    const auto synthPreset = [&] (const juce::String& path) { return factory.getChildFile ("Synth/" + path + ".phazitepreset"); };

    auto cutoff = 0; // each its own, so that each one tried changes the synth

    for (const auto* path : { "Bass/Acid Bass", "Bass/Deep Sub", "Lead/Saw Lead", "Pad/Warm Pad" })
        writeDevicePreset (synthPreset (path), phazite::devices::Synth::typeID, "Synth", true,
                           "\"cutoff\": " + juce::String (cutoff += 500));

    writeDevicePreset (own.getChildFile ("Synth/My Synth.phazitepreset"), phazite::devices::Synth::typeID, "Synth", true);
    writeDevicePreset (own.getChildFile ("EQ/Mine.phazitepreset"), "phaziteEq", "EQ", false);

    phazite::SoundPack sounds { temp.folder.getChildFile ("Packs") }; // not installed
    phazite::Settings settings;
    phazite::Library library (phazite::test::getEngineContext(), settings, {}, own, factory);
    REQUIRE (waitForIndex (library));

    // The places that hold a device's presets, each as its folder for the device, with how many: both of them for the synth,
    // neither for the reverb.
    const phazite::BrowserModel model { library, sounds };
    const auto places = model.getPresetPlaces ("Synth");
    REQUIRE (places.size() == 2);
    CHECK (places[0].kind == Kind::factoryPresets);
    CHECK (places[0].folder == factory.getChildFile ("Synth"));
    CHECK (places[0].count == 4);
    CHECK (places[1].kind == Kind::presets);
    CHECK (places[1].folder == own.getChildFile ("Synth"));
    CHECK (places[1].count == 1);
    CHECK (model.getPresetPlaces ("Reverb").empty());

    const auto previewerOwner = phazite::createPreviewer (phazite::test::getEngineContext());
    phazite::ui::Browser browser (library, *previewerOwner, sounds, settings, song.context);
    phazite::test::TestWindow window (browser, 240, 420); // narrow and short, as a side panel on a small screen
    auto& tree = browser.getTree();
    auto& viewport = *tree.getViewport();
    phazite::test::dispatchPendingMessages(); // the tree makes its rows' components

    // The browser as it is used: Phazite Presets opened with a click, and scrolled down some.
    phazite::test::Mouse mouse (tree);
    mouse.click (getRow (browser, "Phazite Presets").getCentre());
    phazite::test::dispatchPendingMessages();
    viewport.setViewPosition (0, 4 * phazite::ui::Browser::rowHeight);
    REQUIRE (viewport.getViewPositionY() == 4 * phazite::ui::Browser::rowHeight);
    const auto rowsBefore = getRows (browser);
    const auto inViewBefore = getRowsInView (browser);
    REQUIRE (tree.getSelectedItem (0) != nullptr);
    CHECK (tree.getSelectedItem (0)->getAccessibilityName() == juce::String ("Phazite Presets"));

    // The synth's Swap: its presets alone, Phazite's then the user's, every folder open, in view from the top.
    browser.startSwap (synth);
    phazite::test::dispatchPendingMessages();
    REQUIRE (browser.isSwapping());
    const juce::StringArray presets { "Phazite Presets, 4", "Bass", "Acid Bass, Synth", "Deep Sub, Synth", "Lead",
                                      "Saw Lead, Synth",    "Pad",  "Warm Pad, Synth",  "Presets, 1",      "My Synth, Synth" };
    CHECK (getRows (browser) == presets);
    CHECK (viewport.getViewPositionY() == 0);
    const auto inView = getRowsInView (browser);
    REQUIRE (inView.size() >= 4);
    CHECK (inView[0] == juce::String ("Phazite Presets, 4"));
    CHECK (inView.contains ("Acid Bass, Synth"));
    CHECK (tree.getNumSelectedItems() == 0);
    CHECK (tree.hasKeyboardFocus (true));
    CHECK_FALSE (browser.getFilterBar().isVisible()); // presets have none of the filters' values

    // Down goes to the first preset and tries it, then on through the next, past a kind's folder.
    const auto tried = [&browser] { return browser.getSwap() != nullptr ? browser.getSwap()->getTried() : juce::File(); };
    window.pressKey (downKey);
    CHECK (tried() == synthPreset ("Bass/Acid Bass"));
    window.pressKey (downKey);
    CHECK (tried() == synthPreset ("Bass/Deep Sub"));
    window.pressKey (downKey);
    CHECK (tried() == synthPreset ("Bass/Deep Sub")); // on Lead's folder
    window.pressKey (downKey);
    CHECK (tried() == synthPreset ("Lead/Saw Lead"));

    // Esc puts the old one back, and the browser is as it was: the same rows, the same one selected, scrolled as far.
    window.pressKey (escapeKey);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (browser.isSwapping());
    CHECK (getRows (browser) == rowsBefore);
    CHECK (viewport.getViewPositionY() == 4 * phazite::ui::Browser::rowHeight);
    CHECK (getRowsInView (browser) == inViewBefore);
    REQUIRE (tree.getSelectedItem (0) != nullptr);
    CHECK (tree.getSelectedItem (0)->getAccessibilityName() == juce::String ("Phazite Presets"));
    CHECK (browser.getFilterBar().isVisible());

    // A device without presets says so, with nothing of the synth's left over.
    browser.startSwap (reverb);
    phazite::test::dispatchPendingMessages();
    REQUIRE (browser.isSwapping());
    CHECK (getRows (browser) == juce::StringArray { "No presets for Reverb yet" });
    window.pressKey (escapeKey);
    phazite::test::dispatchPendingMessages();
    CHECK (getRows (browser) == rowsBefore);

    // A double-click on one keeps it, as one step, ending the swap in its row's own callback.
    browser.startSwap (synth);
    phazite::test::dispatchPendingMessages();
    mouse.doubleClick (getRow (browser, "Warm Pad").getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (browser.isSwapping());
    CHECK (song.context.getUndoManager()->getUndoDescription() == juce::String ("Load Preset Warm Pad"));
    CHECK (getRows (browser) == rowsBefore);
}
