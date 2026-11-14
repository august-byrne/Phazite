// SPDX-License-Identifier: AGPL-3.0-or-later
// The long run: a song played through Phazite's engine block by block, as the audio device plays it, for an hour of
// music, while it is edited as a user edits it, from a sequence of edits drawn from a fixed seed (notes, clips, values set
// by hand, effects added and removed, Undo and Redo, the loop and the tempo changed, the playhead moved, a save now and
// then). Along the way it checks that nothing allocates or frees on the audio thread (in Windows Debug builds, where
// AudioThreadMonitor.h counts), that the process's memory doesn't grow past an allowance, that what plays stays finite and
// within reason, and that the song's IDs stay whole (SongIDs.h); at the end, that the song saved and opened again is the
// same song. Under linux-asan and linux-tsan the same run is watched by the sanitizers.
//
// ctest skips it. Run it with
//   phazite_tests --test-suite=long-run --no-skip
// PHAZITE_LONG_RUN_SECONDS sets how much music it plays (3600 unless set: 60 tries it quickly), and PHAZITE_LONG_RUN_SEED
// the seed of its edits (1 unless set). The nightly workflow (.github/workflows/nightly.yml) runs it.
#include "AudioThreadMonitor.h"
#include "BenchmarkSong.h"
#include "EnginePlayback.h"
#include "ProcessMemory.h"
#include "SongIDs.h"
#include "TestClips.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "devices/delay/Delay.h"
#include "devices/dynamics/Compressor.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/base/internal/SongText.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/NotesModel.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Tempo.h"

#include <chrono>
#include <cmath>
#include <cstdlib>

using phazite::test::at;
using phazite::test::lasting;

namespace
{
namespace dev = phazite::devices;

double getSetting (const char* name, double otherwise)
{
    const auto value = juce::SystemStats::getEnvironmentVariable (name, {});
    return value.isNotEmpty() ? value.getDoubleValue() : otherwise;
}

phazite::Song& getSong (phazite::test::TestSong& s)
{
    auto* song = phazite::findSong (s.context);
    REQUIRE (song != nullptr);
    return *song;
}

juce::String getSongText (const phazite::Song& song)
{
    juce::MemoryOutputStream text;
    phazite::writeProjectText (text, song.getState());
    return text.toString();
}

// Where two texts first differ, by line, for a failure's message: the whole of a song is too long to read.
juce::String describeFirstDifference (const juce::String& a, const juce::String& b)
{
    const auto linesOfA = juce::StringArray::fromLines (a), linesOfB = juce::StringArray::fromLines (b);

    for (int i = 0; i < std::max (linesOfA.size(), linesOfB.size()); ++i)
        if (linesOfA[i] != linesOfB[i])
            return "line " + juce::String (i + 1) + ": \"" + linesOfA[i].trim() + "\" before, \"" + linesOfB[i].trim() + "\" after";

    return "none";
}

juce::String megabytes (std::int64_t bytes)
{
    return juce::String ((double) bytes / (1024.0 * 1024.0), 1) + " MB";
}

// The memory the process may gain after the run has warmed up: its undo history keeps every edit (up to the 30,000 steps'
// worth that Song.cpp keeps), and the sanitizers keep freed memory aside for a while (AddressSanitizer's quarantine,
// 256 MB by default) and hold their own records of what the program did.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr std::int64_t memoryAllowance = 512LL * 1024 * 1024;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr std::int64_t memoryAllowance = 512LL * 1024 * 1024;
#else
constexpr std::int64_t memoryAllowance = 64LL * 1024 * 1024;
#endif
#else
constexpr std::int64_t memoryAllowance = 64LL * 1024 * 1024;
#endif
constexpr std::int64_t memoryPerEdit = 16LL * 1024; // what each edit's undo step may keep, at most, on average
} // namespace

TEST_CASE ("Long run: an hour of music played block by block while the song is edited" * doctest::test_suite ("long-run") * doctest::skip())
{
    const auto secondsOfMusic = getSetting ("PHAZITE_LONG_RUN_SECONDS", 3600.0);
    const auto seed = (juce::int64) getSetting ("PHAZITE_LONG_RUN_SEED", 1.0);
    REQUIRE (secondsOfMusic > 0.0);

    constexpr double rate = 48000.0;
    constexpr int blockSize = 256;
    constexpr int blocksPerEdit = 375; // an edit every two seconds of music
    constexpr int editsPerSave = 60;   // a save every two minutes
    constexpr int checkpoints = 10;

    phazite::test::TestSong s;
    const auto built = phazite::test::buildBenchmarkSong (s, 8, 32.0);
    auto& song = getSong (s);
    REQUIRE (s.getProject().save().wasOk());

    // On a device at 48 kHz in blocks of up to 256 samples, which the run plays itself.
    phazite::test::EnginePlayback device (phazite::test::EnginePlayback::Input::silence, phazite::test::EnginePlayback::Timing::realTime,
                                          rate, blockSize);
    auto& engine = phazite::getPlaybackHost (phazite::test::getEngineContext()).getEngine();
    phazite::loopSong (song);
    phazite::playOrStop (song);
    phazite::test::dispatchPendingMessages();
    REQUIRE (phazite::isSongPlaying (song));

    juce::Random random (seed);
    std::vector<phazite::DeviceID> added; // effects the run added, which it removes again
    int edits = 0, saves = 0, undos = 0, redos = 0;
    juce::StringArray editsMade; // the last few, for a failure's message

    // One of the edits, drawn from the seed.
    const auto edit = [&]
    {
        const auto index = (size_t) random.nextInt ((int) built.tracks.size());
        const auto track = built.tracks[index];
        const auto clip = built.clips[index];
        juce::String what;

        switch (random.nextInt (12))
        {
            case 0:
                what = "a note added";
                phazite::addNote (s.context, clip, 48 + random.nextInt (36), at (random.nextInt (128) * 0.25), lasting (0.25), 100);
                break;

            case 1:
                if (const auto notes = phazite::getNotes (s.context, { clip }); ! notes.empty())
                {
                    what = "a note deleted";
                    phazite::deleteNotes (s.context, { notes[(size_t) random.nextInt ((int) notes.size())].getRef() });
                }

                break;

            case 2:
                what = "a clip moved";
                phazite::moveClip (s.context, clip, at (random.nextInt (5) * 1.0), track);
                break;

            case 3:
                what = "a track's volume set";
                s.setVolume (track, -24.0f + 24.0f * random.nextFloat());
                break;

            case 4:
            {
                // The compressor's first parameter, anywhere in its range.
                const auto& parameter = dev::Compressor::info.parameters.front();
                what = "a device's parameter set";
                s.setParameter (phazite::getEffects (s.context, track)[1].id, juce::String (std::string (parameter.id)),
                                parameter.minimum + (parameter.maximum - parameter.minimum) * random.nextFloat());
                break;
            }

            case 5:
                if (added.size() < 16)
                {
                    const char* types[] = { dev::Delay::typeID, dev::Equalizer::typeID, dev::FilterEffect::typeID };
                    what = "an effect added";
                    added.push_back (s.addEffect (track, types[random.nextInt (3)]));
                }

                break;

            case 6:
                if (! added.empty())
                {
                    const auto effect = added.back();
                    added.pop_back();

                    // One that an Undo took away already is gone.
                    if (phazite::getDeviceChannel (s.context, effect).has_value())
                    {
                        what = "an effect removed";
                        phazite::removeDevice (s.context, effect);
                    }
                }

                break;

            case 7:
                what = "Undo";
                phazite::undo (song);
                ++undos;
                break;

            case 8:
                what = "Redo";
                phazite::redo (song);
                ++redos;
                break;

            case 9:
            {
                const auto start = random.nextInt (16) * 1.0;
                what = "the loop changed";
                phazite::loopBeatRange (song, start, start + 4.0 + random.nextInt (16));
                break;
            }

            case 10:
                what = "the tempo changed";
                s.setTempo (90.0 + random.nextInt (70));
                break;

            default:
                what = "the playhead moved";
                phazite::movePlayhead (song, at (random.nextInt (32) * 1.0));
                break;
        }

        if (++edits % editsPerSave == 0)
        {
            what << ", and the project saved";
            REQUIRE (s.getProject().save().wasOk());
            ++saves;
        }

        editsMade.add (what);

        if (editsMade.size() > 8)
            editsMade.remove (0);

        // Whatever the edit did to the transport (an Undo of the loop's change), it plays on.
        phazite::test::dispatchPendingMessages();

        if (! phazite::isSongPlaying (song))
            phazite::playOrStop (song);
    };

    const auto totalSamples = (std::int64_t) (secondsOfMusic * rate);
    const auto checkpointSamples = totalSamples / checkpoints;
    std::int64_t played = 0;
    int blocks = 0, blocksSinceEdit = 0;
    float peak = 0.0f;
    std::int64_t memoryAfterWarmUp = 0, mostMemory = 0;
    int editsAfterWarmUp = 0;
    juce::String memoryReport;
    juce::AudioBuffer<float> output (2, blockSize), input (2, blockSize);
    input.clear();

    phazite::test::AudioThreadMonitor::reset();
    const auto started = std::chrono::steady_clock::now();

    for (int checkpoint = 1; checkpoint <= checkpoints; ++checkpoint)
    {
        for (; played < checkpointSamples * checkpoint; ++blocks)
        {
            // Mostly whole blocks, and now and then a shorter one, as some devices give.
            const auto length = random.nextInt (8) == 0 ? 1 + random.nextInt (blockSize) : blockSize;
            const float* inputs[] = { input.getReadPointer (0), input.getReadPointer (1) };
            float* channels[] = { output.getWritePointer (0), output.getWritePointer (1) };

            {
                const phazite::test::AudioThreadMonitor::ScopedAudioThread audioThread;
                engine.process (inputs, 2, channels, 2, length);
            }

            for (int channel = 0; channel < 2; ++channel)
            {
                for (int i = 0; i < length; ++i)
                {
                    const auto sample = output.getSample (channel, i);

                    if (! std::isfinite (sample))
                    {
                        INFO ("the last edits: " << editsMade.joinIntoString ("; ").toStdString());
                        FAIL ("a sample that isn't a number, at " << played + i << " samples");
                    }

                    peak = std::max (peak, std::abs (sample));
                }
            }

            played += length;

            if (const auto counts = phazite::test::AudioThreadMonitor::getCounts(); counts.allocations + counts.frees > 0)
            {
                INFO ("the last edits: " << editsMade.joinIntoString ("; ").toStdString());
                INFO ("where:\n" << phazite::test::AudioThreadMonitor::describeCallSites().joinIntoString ("\n").toStdString());
                FAIL ("the audio thread allocated " << counts.allocations << " times and freed " << counts.frees << " times, in block "
                                                    << blocks);
            }

            // The message thread's work meanwhile (what the engine let go of, files read), and an edit now and then.
            if (blocks % 16 == 0)
                phazite::test::dispatchPendingMessages();

            if (++blocksSinceEdit >= blocksPerEdit)
            {
                blocksSinceEdit = 0;
                edit();
            }
        }

        // At each tenth of the run: the song whole, and the process's memory.
        phazite::test::checkIDs (song);
        const auto memory = phazite::test::getProcessMemoryBytes();
        memoryReport << megabytes (memory) << " ";

        if (checkpoint == 1)
        {
            memoryAfterWarmUp = memory;
            editsAfterWarmUp = edits;
        }

        mostMemory = std::max (mostMemory, memory);
        const auto allowed = memoryAllowance + memoryPerEdit * (edits - editsAfterWarmUp);
        INFO ("the process's memory at each tenth: " << memoryReport.toStdString());
        CHECK (memory - memoryAfterWarmUp <= allowed);
    }

    const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();
    phazite::stopAndReturn (song);
    phazite::test::dispatchPendingMessages();

    // Loud, but never wild: nothing past +24 dBFS.
    CHECK (peak > 0.001f);
    CHECK (peak < 16.0f);

    // Saved and opened again, the same song: from its start, where a project opens (Project.cpp: open()).
    phazite::movePlayhead (song, at (0.0));
    REQUIRE (s.getProject().save().wasOk());
    const auto before = getSongText (song);
    s.reopen();
    const auto after = getSongText (getSong (s));
    INFO ("the first line that differs: " << describeFirstDifference (before, after).toStdString());
    CHECK (after == before);
    phazite::test::checkIDs (getSong (s));

    MESSAGE ("Played " << juce::String (played / rate, 1) << " s of music in " << blocks << " blocks, in " << juce::String (elapsed, 1)
                       << " s (" << juce::String (played / rate / elapsed, 2) << " times as fast as it plays); " << edits << " edits ("
                       << undos << " Undo, " << redos << " Redo), " << saves << " saves; peak "
                       << juce::String (juce::Decibels::gainToDecibels (peak), 1)
                       << " dBFS; the process's memory at each tenth: " << memoryReport << "(the most " << megabytes (mostMemory) << ")"
                       << (phazite::test::AudioThreadMonitor::isAvailable() ? "; nothing allocated on the audio thread"
                                                                            : "; allocations not counted in this build"));
}
