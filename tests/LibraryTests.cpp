// SPDX-License-Identifier: AGPL-3.0-or-later
// The browser's model: sounds by folder and by search, tags from names, favorites and recent items, and previews.
#include "TestHelpers.h"
#include "model/engine/Preview.h"
#include "services/Library.h"

using phazite::test::TemporaryFolder;
using phazite::test::writeSound;

TEST_CASE ("A sound's tags come from the words in its name and its folders")
{
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("PhaziteTags");
    const auto tagsOf = [&root] (const char* path) { return phazite::getSoundTags (root.getChildFile (path), root); };

    CHECK (tagsOf ("Drums/Kicks/Deep 01.wav") == juce::StringArray { "Kick", "One-shot" });
    CHECK (tagsOf ("OpenHat.wav") == juce::StringArray { "Hat", "One-shot" });
    CHECK (tagsOf ("808Bass_C.wav") == juce::StringArray { "Bass", "One-shot" });
    CHECK (tagsOf ("FX/Riser 8 bars.flac") == juce::StringArray { "FX", "One-shot" });
    CHECK (tagsOf ("Loops/Break 120 BPM.wav") == juce::StringArray { "Loop", "120 BPM" });
    CHECK (tagsOf ("kick_loop_128bpm.wav") == juce::StringArray { "Kick", "Loop", "128 BPM" });

    // "808" alone is a bass, "808 Clap" a clap; two words that make a tag word count as one; an effect with a tempo is
    // still a one-shot.
    CHECK (tagsOf ("808 Clap.wav") == juce::StringArray { "Clap", "One-shot" });
    CHECK (tagsOf ("808 C.wav") == juce::StringArray { "Bass", "One-shot" });
    CHECK (tagsOf ("Bass Drum 01.wav") == juce::StringArray { "Kick", "One-shot" });
    CHECK (tagsOf ("bassdrum.wav") == juce::StringArray { "Kick", "One-shot" });
    CHECK (tagsOf ("Hi Hat Open.wav") == juce::StringArray { "Hat", "One-shot" });
    CHECK (tagsOf ("Riser 4 Bars 128 BPM.wav") == juce::StringArray { "FX", "One-shot", "128 BPM" });

    CHECK (phazite::getTempoFromName ("BPM 95 groove") == std::optional<double> (95.0));
    CHECK_FALSE (phazite::getTempoFromName ("Kick 808").has_value());
    CHECK_FALSE (phazite::getTempoFromName ("Loop 400 BPM").has_value());
}

TEST_CASE ("The library lists folders, finds sounds and devices, and keeps favorites and recent items")
{
    TemporaryFolder temp;
    writeSound (temp.folder.getChildFile ("Drums/Kicks/Deep Kick.wav"), 0.5);
    writeSound (temp.folder.getChildFile ("Drums/Snare Tight.wav"), 0.3);
    writeSound (temp.folder.getChildFile ("Loops/Break 120 BPM.wav"), 4.0);
    temp.folder.getChildFile ("Drums/notes.txt").replaceWithText ("not a sound");

    juce::PropertySet settings;
    phazite::Library library (phazite::test::getEngine(), settings, {});
    library.addUserFolder (temp.folder);
    CHECK (library.getUserFolders() == juce::Array<juce::File> { temp.folder });

    // A folder shows its folders first, then its sounds, each by name; other files don't show.
    const auto drums = library.listFolder (temp.folder.getChildFile ("Drums"));
    REQUIRE (drums.size() == 2);
    CHECK (drums[0].name == juce::String ("Kicks"));
    CHECK (drums[0].kind == phazite::LibraryItem::Kind::folder);
    CHECK (drums[1].name == juce::String ("Snare Tight"));
    CHECK (drums[1].kind == phazite::LibraryItem::Kind::sound);
    CHECK (drums[1].tags.contains ("Snare"));

    // Search finds what the background index has found, and devices.
    REQUIRE (phazite::test::runTimersUntil ([&library] { return ! library.isIndexing(); }, 10000));
    const auto kicks = library.search ("kick");
    REQUIRE (! kicks.empty());
    CHECK (kicks[0].name == juce::String ("Deep Kick"));
    CHECK (library.search ("loop 120").size() == 1);
    CHECK (library.search ("reverb").at (0).kind == phazite::LibraryItem::Kind::effect);
    CHECK (library.search ("nothing is called this").empty());

    // Favorites and recent items stay in the settings, as long as their files are there.
    library.setFavorite (kicks[0], true);
    CHECK (library.isFavorite (kicks[0]));
    library.noteUsed (drums[1]);
    library.noteUsed (kicks[0]);

    phazite::Library again (phazite::test::getEngine(), settings, {});
    REQUIRE (again.getFavorites().size() == 1);
    CHECK (again.getFavorites()[0].name == juce::String ("Deep Kick"));
    REQUIRE (again.getRecent().size() == 2);
    CHECK (again.getRecent()[0].name == juce::String ("Deep Kick"));

    temp.folder.getChildFile ("Drums/Snare Tight.wav").deleteFile();
    CHECK (again.getRecent().size() == 1);

    library.removeUserFolder (temp.folder);
    CHECK (library.getUserFolders().isEmpty());
}

TEST_CASE ("A loop previews in time with the project, and a one-shot as it is")
{
    TemporaryFolder temp;
    auto project = te::Edit::createSingleTrackEdit (phazite::test::getEngine());
    project->tempoSequence.getTempos()[0]->setBpm (100.0);

    // Two bars at the tempo in its name.
    const auto loop = writeSound (temp.folder.getChildFile ("Break 120 BPM.wav"), 4.0);
    const auto kick = writeSound (temp.folder.getChildFile ("Kick.wav"), 0.4);

    phazite::Previewer previewer (phazite::test::getEngine());
    REQUIRE (previewer.play (loop, project.get()));
    CHECK (previewer.isInTime());
    CHECK (previewer.getFile() == loop);

    REQUIRE (previewer.play (kick, project.get()));
    CHECK_FALSE (previewer.isInTime());

    previewer.stop();
    CHECK (previewer.getFile() == juce::File());
    CHECK_FALSE (previewer.play (temp.folder.getChildFile ("Missing.wav"), project.get()));
}

TEST_CASE ("Sounds named as Phazite Sounds names them get their folders' tags, and those with a tempo play in time")
{
    // A few sounds laid out as the pack lays out its own (recipes.json in the phazite-sounds repository).
    TemporaryFolder temp;
    const auto& pack = temp.folder;
    const auto kick = writeSound (pack.getChildFile ("Drums/Kicks/Kick Punchy.wav"), 0.5);
    const auto bass = writeSound (pack.getChildFile ("808s/808 Long F.wav"), 2.0);
    const auto riser = writeSound (pack.getChildFile ("FX/Riser 4 Bars 128 BPM.wav"), 4 * 4 * 60.0 / 128.0);
    const auto loop = writeSound (pack.getChildFile ("Loops/House 124 BPM.wav"), 2 * 4 * 60.0 / 124.0);

    CHECK (phazite::getSoundTags (kick, pack).contains ("Kick"));
    CHECK (phazite::getSoundTags (bass, pack).contains ("Bass"));

    const auto riserTags = phazite::getSoundTags (riser, pack);
    CHECK (riserTags.contains ("FX"));
    CHECK (riserTags.contains ("One-shot"));

    const auto loopTags = phazite::getSoundTags (loop, pack);
    CHECK (loopTags.contains ("Loop"));
    CHECK (loopTags.contains ("124 BPM"));

    // A sound whose name gives a tempo, and which lasts whole bars at it, plays in time.
    for (const auto& file : { riser, loop })
    {
        INFO (file.getFileName());
        CHECK (te::AudioFile (phazite::test::getEngine(), file).getInfo().loopInfo.getNumBeats() > 0.0);
    }
}
