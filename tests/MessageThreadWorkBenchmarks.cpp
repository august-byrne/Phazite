// SPDX-License-Identifier: AGPL-3.0-or-later
// Benchmarks (docs/performance.md) of what the message thread is kept for by work that reads much: the browser's search at
// each key typed, in a library of 50,000 sounds and of 500,000; and a ten-minute recording imported (with a file of the
// same name in the project already), normalized and quantized (known bugs 128 and 123).
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/drums/DrumSampler.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/SwapModel.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "services/BrowserModel.h"
#include "services/Library.h"
#include "services/SoundPack.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
// A sound's name and folder as sample packs name them: "Pack 12/Drums/Kicks/Deep Kick 07.flac".
juce::String makeSoundPath (int i)
{
    static const char* kinds[] = { "Kick", "Snare", "Hat", "Clap", "Bass", "Pad", "Lead", "Vox", "FX", "Perc", "Tom", "Ride" };
    static const char* folders[] = { "Drums/Kicks",  "Drums/Snares", "Drums/Hats", "Drums/Claps",      "Bass",       "Synths/Pads",
                                     "Synths/Leads", "Vocals",       "FX",         "Drums/Percussion", "Drums/Toms", "Drums/Cymbals" };
    static const char* words[] = { "Deep", "Dusty", "Punchy", "Warm", "Dark", "Bright", "Tight", "Wide", "Lofi", "Crisp" };
    const auto kind = i % 12;
    return "Pack " + juce::String (i / 2000) + "/" + folders[kind] + "/" + words[(i / 12) % 10] + " " + kinds[kind] + " "
           + juce::String (i % 97);
}

double now()
{
    return juce::Time::getMillisecondCounterHiRes();
}

const juce::StringArray& getKeysTyped()
{
    static const juce::StringArray typed { "k", "ki", "kic", "kick", "kick ", "kick d", "kick de", "kick dee", "kick deep" };
    return typed;
}

// What the message thread spends while it waits for a condition: the time in the timers and messages it runs meanwhile.
double busyUntil (const std::function<bool()>& condition)
{
    auto busy = 0.0;

    while (! condition())
    {
        juce::Thread::sleep (1);
        const auto started = now();
        juce::Timer::callPendingTimersSynchronously();
        phazite::test::dispatchPendingMessages();
        busy += now() - started;
    }

    return busy;
}
} // namespace

TEST_CASE ("Benchmark: the browser's search at each key typed, in a library of 50,000 sounds and of 500,000"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    phazite::test::TemporaryFolder temp;
    constexpr int sounds = 50000;

    for (int i = 0; i < sounds; ++i)
    {
        const auto file = temp.folder.getChildFile (makeSoundPath (i) + ".flac");
        file.getParentDirectory().createDirectory();
        file.create();
    }

    phazite::Settings settings;
    phazite::Library library (phazite::test::getEngineContext(), settings, {});
    library.addUserFolder (temp.folder);
    REQUIRE (phazite::test::runTimersUntil ([&library] { return ! library.isIndexing(); }));
    REQUIRE ((int) library.getIndex()->size() == sounds);

    // At each key, as the browser searches: what the call keeps the message thread for, and how long until the results are
    // there; and what a search costs where it runs (at once, as before, it ran on the message thread).
    juce::String report (
        "\n| Typed | 50,000: message thread per key (ms) | 50,000: results there after (ms) | 50,000: the search itself (ms) "
        "| 500,000: the search itself (ms) | Matches (50,000) |\n|---|---|---|---|---|---|\n");

    // 500,000, made here: the same names ten times over, in ten roots.
    auto big = std::make_shared<phazite::LibraryIndex>();

    for (int copy = 0; copy < 10; ++copy)
    {
        const auto root = temp.folder.getChildFile ("Root " + juce::String (copy));

        for (const auto& item : *library.getIndex())
        {
            auto moved = item;
            moved.file = root.getChildFile (item.file.getRelativePathFrom (temp.folder));
            big->addFile (std::move (moved), root);
        }
    }

    REQUIRE (big->files.size() == 10 * (size_t) sounds);
    const auto devices = library.getAllDevices();

    for (const auto& typed : getKeysTyped())
    {
        std::optional<phazite::SearchResults> arrived;
        auto started = now();
        library.startSearch (typed, 200, nullptr, [&arrived] (phazite::SearchResults found) { arrived = std::move (found); });
        const auto call = now() - started;
        REQUIRE (phazite::test::dispatchMessagesUntil ([&arrived] { return arrived.has_value(); }));
        const auto there = now() - started;

        started = now();
        const auto found = phazite::Library::searchIndex (*library.getWholeIndex(), devices, typed, 200, nullptr);
        const auto search = now() - started;

        started = now();
        phazite::Library::searchIndex (*big, devices, typed, 200, nullptr);
        const auto bigSearch = now() - started;

        report << "| \"" << typed << "\" | " << juce::String (call, 2) << " | " << juce::String (there, 1) << " | "
               << juce::String (search, 1) << " | " << juce::String (bigSearch, 1) << " | " << found.total << " |\n";
    }

    MESSAGE (report);
}

TEST_CASE ("Benchmark: the browser's filters' counts and its filtered places, in a library of 50,000 sounds"
           * doctest::test_suite ("benchmark") * doctest::skip())
{
    phazite::test::TemporaryFolder temp;
    constexpr int sounds = 50000;

    for (int i = 0; i < sounds; ++i)
    {
        const auto file = temp.folder.getChildFile (makeSoundPath (i) + ".flac");
        file.getParentDirectory().createDirectory();
        file.create();
    }

    phazite::Settings settings;
    phazite::Library library (phazite::test::getEngineContext(), settings, {});
    library.addUserFolder (temp.folder);
    REQUIRE (phazite::test::runTimersUntil ([&library] { return ! library.isIndexing(); }));
    REQUIRE ((int) library.getIndex()->size() == sounds);
    const phazite::SoundPack pack { temp.folder.getChildFile ("No Packs") };
    const phazite::BrowserModel model { library, pack };

    phazite::BrowserFilters kicks;
    kicks.type = "Kick";

    // What a filter's menu kept the message thread for as it opened (counted there, as before), and what it keeps it for
    // now: the call that starts the count, and how long until the counts are there; then the filtered places, which are
    // still listed on the message thread as a filter is set.
    juce::String report ("\n| Words, filters | Counted on the message thread (ms) | The call now (ms) | Counts there after (ms) |\n"
                         "|---|---|---|---|\n");

    for (const auto& [words, filters] :
         { std::pair { juce::String(), phazite::BrowserFilters() }, std::pair { juce::String ("kick"), phazite::BrowserFilters() },
           std::pair { juce::String(), kicks }, std::pair { juce::String ("deep"), kicks } })
    {
        auto started = now();
        const auto atOnce = model.getFilterOptions (words, filters);
        const auto before = now() - started;
        juce::ignoreUnused (atOnce);

        std::optional<phazite::FilterOptions> arrived;
        started = now();
        model.startFilterOptions (words, filters, [&arrived] (phazite::FilterOptions found) { arrived = std::move (found); });
        const auto call = now() - started;
        REQUIRE (phazite::test::dispatchMessagesUntil ([&arrived] { return arrived.has_value(); }));
        const auto there = now() - started;

        report << "| \"" << words << "\", " << (filters.isEmpty() ? "none" : "Kick") << " | " << juce::String (before, 1) << " | "
               << juce::String (call, 2) << " | " << juce::String (there, 1) << " |\n";
    }

    // The places the Type filter set to Kick leaves: before, listed on the message thread; now, the call that starts the
    // listing, and how long until the places are there.
    auto started = now();
    const auto places = model.getPlaces (kicks);
    const auto atOnce = now() - started;

    std::optional<phazite::BrowserModel::FilteredPlaces> listed;
    started = now();
    model.startFilteredPlaces (kicks, [&listed] (phazite::BrowserModel::FilteredPlaces found) { listed = std::move (found); });
    const auto call = now() - started;
    REQUIRE (phazite::test::dispatchMessagesUntil ([&listed] { return listed.has_value(); }));
    const auto there = now() - started;
    REQUIRE (listed->places.size() == places.size());

    report << "\nThe places with the Type filter set to Kick (" << (int) places.size() << "): listed on the message thread "
           << juce::String (atOnce, 1) << " ms; the call now " << juce::String (call, 2) << " ms, the places there after "
           << juce::String (there, 1) << " ms\n";
    MESSAGE (report);
}

TEST_CASE ("Benchmark: a ten-minute recording imported, normalized and quantized, on the message thread" * doctest::test_suite ("benchmark")
           * doctest::skip())
{
    phazite::test::TestSong song;

    // Ten minutes of stereo at 48 kHz, 24-bit, a click on each beat at 120 BPM.
    const auto take = song.getSoundsFolder().getChildFile ("Take.wav");
    {
        constexpr double rate = 48000.0;
        constexpr int seconds = 600, block = 48000;
        take.getParentDirectory().createDirectory();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (take);
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
        REQUIRE (writer != nullptr);
        juce::AudioBuffer<float> audio (2, block);

        for (int second = 0; second < seconds; ++second)
        {
            audio.clear();

            for (int beat = 0; beat < 2; ++beat)
                for (int i = 0; i < 200; ++i)
                    for (int channel = 0; channel < 2; ++channel)
                        audio.setSample (channel, beat * 24000 + i, 0.5f * std::exp ((float) -i / 40.0f) * ((i % 2) == 0 ? 1.0f : -1.0f));

            writer->writeFromAudioSampleBuffer (audio, 0, block);
        }
    }

    // A file of the same name in the project already: the same file, imported before.
    const auto audioFolder = song.getProject().getAudioFolder();
    audioFolder.createDirectory();
    REQUIRE (take.copyFileTo (audioFolder.getChildFile ("Take.wav")));

    // Each command: what its call keeps the message thread for, what the message thread spends until its change is made
    // (the calls and the timers and messages it runs meanwhile, its change among them), and how long until then.
    juce::String report ("\n| Command | The call (ms) | The message thread in all (ms) | Done after (ms) |\n|---|---|---|---|\n");
    const auto measure = [&song, &report] (const char* name, const std::function<void()>& command)
    {
        const auto started = now();
        command();
        const auto call = now() - started;
        const auto busy = busyUntil ([&song] { return ! phazite::isReadingAudio (song.context); });
        report << "| " << name << " | " << juce::String (call, 2) << " | " << juce::String (call + busy, 1) << " | "
               << juce::String (now() - started, 0) << " |\n";
    };

    measure ("Import (a file of the same name there)",
             [&]
             {
                 REQUIRE (
                     phazite::importFilesInBackground (song.context, { take.getFullPathName() }, song.firstTrack, phazite::BeatPosition()));
             });

    const auto clips = phazite::getClips (song.context, song.firstTrack);
    REQUIRE (clips.size() == 1);
    const auto clip = clips[0].id;
    measure ("Normalize", [&] { phazite::normalizeClip (song.context, clip); });
    measure ("Quantize Audio", [&] { phazite::quantizeAudio (song.context, { clip }, phazite::BeatDuration::fromBeats (1.0)); });
    MESSAGE (report);
}

TEST_CASE ("Benchmark: Quantize Audio's edit of 1,000 and 5,000 hits, on the message thread" * doctest::test_suite ("benchmark")
           * doctest::skip())
{
    phazite::test::TestSong song;

    // Ten minutes, mono at 8 kHz: what the clip plays matters, not what it holds.
    const auto take = song.getSoundsFolder().getChildFile ("Long.wav");
    {
        take.getParentDirectory().createDirectory();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (take);
        auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (8000.0).withNumChannels (1).withBitsPerSample (16));
        REQUIRE (writer != nullptr);
        juce::AudioBuffer<float> silence (1, 8000);
        silence.clear();

        for (int second = 0; second < 600; ++second)
            writer->writeFromAudioSampleBuffer (silence, 0, 8000);
    }

    const auto clip = song.addAudioClip (song.firstTrack, take, 0.0);
    auto& s = *phazite::findSong (song.context);
    juce::String report ("\n| Hits | The edit (ms) | The message thread after it, until quiet (ms) |\n|---|---|---|\n");

    for (const auto count : { 1000, 5000 })
    {
        // Each a little off the beat it is nearest, so that each moves.
        std::vector<double> hits;

        for (int i = 0; i < count; ++i)
            hits.push_back ((i + 0.5) * 590.0 / count + 0.013);

        auto started = now();
        {
            const phazite::UndoStep step (s, "Quantize Audio");
            phazite::quantizeClipAudio (s, clip, hits, phazite::BeatDuration::fromBeats (0.25), 1.0);
        }
        const auto edit = now() - started;

        // What the timers and messages make of it (the plan, the clip's render), for a second.
        started = now();
        const auto busy = busyUntil ([&started] { return now() - started > 1000.0; });
        report << "| " << count << " | " << juce::String (edit, 1) << " | " << juce::String (busy, 1) << " |\n";
        phazite::undo (s);
        busyUntil ([&started] { return now() - started > 2000.0; });
    }

    MESSAGE (report);
}

TEST_CASE ("Benchmark: a sound tried on a drum pad by Swap" * doctest::test_suite ("benchmark") * doctest::skip())
{
    phazite::test::TestSong song;
    const auto drums = song.setInstrument (song.firstTrack, phazite::devices::DrumSampler::typeID);
    juce::Array<juce::File> sounds;

    for (int i = 0; i < 20; ++i)
        sounds.add (phazite::test::writeSound (song.getSoundsFolder().getChildFile ("Kick " + juce::String (i) + ".wav"), 0.5, 60.0 + i));

    phazite::Swap swap (song.context, drums, 36);
    REQUIRE (swap.isOpen());
    std::vector<double> tries;

    for (const auto& sound : sounds)
    {
        const auto started = now();
        REQUIRE (swap.tryFile (sound));
        tries.push_back (now() - started);
    }

    swap.keep();
    phazite::test::waitForAudioWork (song.context); // its sound copied in, in the background
    std::sort (tries.begin(), tries.end());
    MESSAGE ("\nA try: median " << juce::String (tries[tries.size() / 2], 2) << " ms, slowest " << juce::String (tries.back(), 2) << " ms");
}
