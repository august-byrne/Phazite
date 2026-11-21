// SPDX-License-Identifier: AGPL-3.0-or-later
// The waveforms of audio clips (src/app/ClipWaveforms.h), from Phazite's own cache of their files' levels: one opens its
// file and loads in the background and draws where the clip's file has audio, nothing past it; a second clip of the same
// file draws as soon as its file is open, its levels from the cache.
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "app/ClipWaveforms.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/engine/AudioFiles.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <atomic>

namespace
{
// Writes a sound of silence but for a burst at full scale from one time to another, in seconds.
juce::File writeBurst (const juce::File& file, double seconds, double from, double to)
{
    constexpr double sampleRate = 44100.0;
    juce::AudioBuffer<float> buffer (1, (int) (seconds * sampleRate));
    buffer.clear();

    for (auto i = (int) (from * sampleRate); i < (int) (to * sampleRate); ++i)
        buffer.setSample (0, i, (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / sampleRate));

    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (auto writer = juce::WavAudioFormat().createWriterFor (
            stream, juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (16)))
        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());

    return file;
}

// Writes a tone over a file in its place, as long as it was, without deleting it first (a thumbnail's reader may still hold
// it open, which Windows lets write to but not delete), and gives it a modification time a minute later.
void writeToneInPlace (const juce::File& file, double seconds, float amplitude)
{
    constexpr double sampleRate = 44100.0;
    const auto modified = file.getLastModificationTime();
    juce::AudioBuffer<float> buffer (1, (int) (seconds * sampleRate));

    for (int i = 0; i < buffer.getNumSamples(); ++i)
        buffer.setSample (0, i, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / sampleRate));

    {
        auto out = std::make_unique<juce::FileOutputStream> (file);
        REQUIRE (out->openedOk());
        REQUIRE (out->setPosition (0));
        REQUIRE (out->truncate().wasOk());
        std::unique_ptr<juce::OutputStream> stream = std::move (out);

        if (auto writer = juce::WavAudioFormat().createWriterFor (
                stream, juce::AudioFormatWriterOptions {}.withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (16)))
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    REQUIRE (file.setLastModificationTime (modified + juce::RelativeTime::minutes (1.0)));
}

// How many pixels of an image's columns, from one to another, a waveform lit.
int countLit (const juce::Image& image, int fromX, int toX)
{
    auto lit = 0;

    for (int x = fromX; x < toX; ++x)
        for (int y = 0; y < image.getHeight(); ++y)
            if (image.getPixelAt (x, y).getAlpha() > 0)
                ++lit;

    return lit;
}

// A waveform painted over an image, its span reaching a clip's length past the clip's end.
juce::Image paint (phazite::ui::ClipWaveform& waveform, phazite::BeatRange span)
{
    juce::Image image (juce::Image::ARGB, 400, 60, true);
    juce::Graphics g (image);
    g.setColour (juce::Colours::white);
    waveform.paintSpan (g, image.getBounds(), span);
    return image;
}
} // namespace

TEST_CASE ("A clip's waveform loads in the background and draws where its file has audio; another clip of the file draws from the "
           "cache")
{
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Waves", error);
    REQUIRE (project != nullptr);
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, &project->getSong());

    // A second of sound at 120 BPM: two beats, from the song's start.
    const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 1.0);
    const auto track = phazite::getTrackIDs (project->getSong()).front();
    const auto clips = phazite::importFiles (context, { file.getFullPathName() }, track, phazite::BeatPosition());
    REQUIRE (clips.size() == 1);

    // What shows it is told as its levels load, to paint it again.
    auto loads = 0;
    const auto make = phazite::getClipWaveformMaker();
    auto waveform = make (context, clips[0], [&loads] { ++loads; });
    REQUIRE (waveform != nullptr);

    // The clip's two beats, then two more past its end, where its file has no audio.
    const phazite::BeatRange span { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (4.0) };
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return countLit (paint (*waveform, span), 0, 190) > 190 * 10; }));
    CHECK (phazite::test::dispatchMessagesUntil ([&loads] { return loads > 0; })); // its message can come after the levels
    const auto image = paint (*waveform, span);
    CHECK (countLit (image, 210, 400) == 0);

    // Another waveform of the same file has its levels from the cache as soon as its file is open: at once, without a
    // message of more levels read.
    auto loadsAgain = 0;
    auto again = make (context, clips[0], [&loadsAgain] { ++loadsAgain; });
    paint (*again, span);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&loadsAgain] { return loadsAgain > 0; }));
    CHECK (countLit (paint (*again, span), 0, 190) > 190 * 10);

    setSong (context, nullptr);
}

TEST_CASE ("A clip's waveform shows its file anew once an ask of the disk finds it written again, and a paint never asks the disk "
           "(known bug 198)")
{
    // The second review of known bug 198: each paint asked the disk about the clip's file, twice at every paint for a file
    // that isn't there (which the record of identities never trusts, model/engine/AudioFiles.h), and a file written again
    // showed only at the clip's next paint. Now a paint reads what the record knows, and the record's listeners tell the
    // waveform of a file found written again (by the player's next plan, here an ask as it makes).
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Waves", error);
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, &song);

    // A second of a loud tone: two beats at 120 BPM, its file asked about as a plan asks.
    const auto track = phazite::getTrackIDs (song).front();
    const auto clips = phazite::importFiles (
        context, { phazite::test::writeSound (temp.folder.getChildFile ("Tone.wav"), 1.0, 220.0, 0.9f).getFullPathName() }, track,
        phazite::BeatPosition());
    REQUIRE (clips.size() == 1);
    const auto file = phazite::getClipFile (song, phazite::findClipState (song, clips[0]));
    phazite::askFileIdentity (file);

    auto loads = 0;
    auto waveform = phazite::getClipWaveformMaker() (context, clips[0], [&loads] { ++loads; });
    REQUIRE (waveform != nullptr);
    const phazite::BeatRange span { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (2.0) };
    const auto lit = [&] { return countLit (paint (*waveform, span), 0, 400); };
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return lit() > 400 * 40; }));
    phazite::test::dispatchPendingMessages(); // every level loaded

    // Written again in its place, quiet, a minute later: the waveform is told to paint again as an ask finds it, and shows
    // the file as it is now.
    writeToneInPlace (file, 1.0, 0.05f);
    const auto loadsBefore = loads;
    phazite::askFileIdentity (file);
    // Nothing shows while it is opened again, then its new levels: the cache doesn't give the old ones.
    CHECK (loads > loadsBefore);
    CHECK (phazite::test::dispatchMessagesUntil (
        [&]
        {
            const auto now = lit();
            return now > 0 && now < 400 * 10;
        }));

    // A clip of a file that isn't there, painted again and again: the disk never asked.
    const auto goneClips =
        phazite::importFiles (context, { phazite::test::writeSound (temp.folder.getChildFile ("Gone.wav"), 1.0).getFullPathName() }, track,
                              phazite::BeatPosition::fromBeats (4.0));
    REQUIRE (goneClips.size() == 1);
    const auto gone = phazite::getClipFile (song, phazite::findClipState (song, goneClips[0]));
    REQUIRE (gone.deleteFile());
    phazite::askFileIdentity (gone);
    auto missing = phazite::getClipWaveformMaker() (context, goneClips[0], {});
    REQUIRE (missing != nullptr);
    const phazite::BeatRange goneSpan { phazite::BeatPosition::fromBeats (4.0), phazite::BeatPosition::fromBeats (6.0) };
    paint (*missing, goneSpan);
    const auto asked = phazite::countFileIdentityAsks();

    for (int i = 0; i < 20; ++i)
        paint (*missing, goneSpan);

    CHECK (phazite::countFileIdentityAsks() == asked);

    setSong (context, nullptr);
}

TEST_CASE ("A clip's waveform opens its file in the background, never in a paint, and shows once it is open")
{
    // The threading audit, 2026-11-20: the waveform gave JUCE's thumbnail the clip's file as a source inside a paint, which
    // opened it and read its header there (and asked the disk whether it was there and when it was written), so that the
    // first paint of a project, on a share or a synced folder, waited for every audio clip's file in view.
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Waves", error);
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, &song);

    const auto track = phazite::getTrackIDs (song).front();
    const auto clips =
        phazite::importFiles (context,
                              { phazite::test::writeSound (temp.folder.getChildFile ("Slow.wav"), 1.0, 220.0, 0.9f).getFullPathName(),
                                phazite::test::writeSound (temp.folder.getChildFile ("Other.wav"), 1.0, 220.0, 0.9f).getFullPathName() },
                              track, phazite::BeatPosition());
    REQUIRE (clips.size() == 2);

    // Their files asked about as a plan asks, so that what is known of them doesn't change while they show (a file found
    // otherwise than it was known is opened again).
    for (const auto clip : clips)
        phazite::askFileIdentity (phazite::getClipFile (song, phazite::findClipState (song, clip)));

    // Each open waits until the test lets it go, as a drive that is slow to wake. Where a reader is made, the thread is
    // counted (countWaveformFiles()).
    const auto before = phazite::countWaveformFiles();
    const auto opensOnMessageThread = [&before]
    { return phazite::countWaveformFiles().opensOnMessageThread - before.opensOnMessageThread; };
    std::atomic<int> held { 0 };
    juce::WaitableEvent letGo { true };
    phazite::setBeforeWaveformFileOpens (
        [&] (const juce::File&)
        {
            ++held;
            letGo.wait (phazite::test::hangTimeout);
        });
    const juce::ScopeGuard noLongerHeld { [&letGo]
                                          {
                                              phazite::setBeforeWaveformFileOpens ({});
                                              letGo.signal();
                                          } };

    auto loads = 0;
    auto waveform = phazite::getClipWaveformMaker() (context, clips[0], [&loads] { ++loads; });
    REQUIRE (waveform != nullptr);
    const phazite::BeatRange span { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (2.0) };
    const auto lit = [&] { return countLit (paint (*waveform, span), 0, 400); };

    // A paint opens nothing on the message thread, and draws no waveform while the file is being opened, however often.
    CHECK (lit() == 0);
    REQUIRE (opensOnMessageThread() == 0);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&held] { return held == 1; }));

    for (int i = 0; i < 10; ++i)
        CHECK (lit() == 0);

    CHECK (held == 1);

    // Once it is open, the waveform is told to paint again, and shows the file.
    letGo.signal();
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return lit() > 400 * 40; }));
    CHECK (loads > 0);
    CHECK (opensOnMessageThread() == 0);

    // A waveform that goes while its file is being opened takes nothing: the file opened is let go where it was opened.
    letGo.reset();
    auto gone = phazite::getClipWaveformMaker() (context, clips[1], {});
    REQUIRE (gone != nullptr);
    paint (*gone, span);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&held] { return held == 2; }));
    gone.reset();
    letGo.signal();

    // Another waveform of that file opens it again and shows it.
    auto again = phazite::getClipWaveformMaker() (context, clips[1], {});
    REQUIRE (again != nullptr);
    paint (*again, span);
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return countLit (paint (*again, span), 0, 400) > 400 * 40; }));
    CHECK (opensOnMessageThread() == 0);

    setSong (context, nullptr);
}

TEST_CASE ("A clip's waveform lets its file go once its levels are read, and one of a file whose levels are kept opens nothing")
{
    // The review of the threading audit's fix, 2026-11-20: a thumbnail given a reader keeps it, so each waveform kept its file
    // open for as long as it lived, one open file for each audio clip that had come into view, past the files a process may
    // have open. Once its levels are read, the waveform keeps them without the file; and a clip of a file whose levels are
    // kept draws them without opening it: in its first paint where the file is known on the disk (a copy, a split, Undo),
    // else once its hash is worked out.
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Waves", error);
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, &song);

    const auto track = phazite::getTrackIDs (song).front();
    juce::StringArray paths;

    for (int i = 0; i < 14; ++i)
        paths.add (phazite::test::writeSound (temp.folder.getChildFile ("Sound " + juce::String (i) + ".wav"), 1.0, 220.0, 0.9f)
                       .getFullPathName());

    const auto clips = phazite::importFiles (context, paths, track, phazite::BeatPosition());
    REQUIRE (clips.size() == 14);

    // The first file known on the disk, as a plan asks; the second not.
    phazite::askFileIdentity (phazite::getClipFile (song, phazite::findClipState (song, clips[0])));
    const auto before = phazite::countWaveformFiles();
    const auto opened = [&before] { return phazite::countWaveformFiles().opens - before.opens; };
    const auto readersAlive = [] { return phazite::countWaveformFiles().readersAlive; };
    const phazite::BeatRange span { phazite::BeatPosition(), phazite::BeatPosition::fromBeats (2.0) };
    const auto litIn = [&span] (phazite::ui::ClipWaveform& waveform) { return countLit (paint (waveform, span), 0, 400); };

    // Read, its file opened once, and let go once every level is in.
    auto first = phazite::getClipWaveformMaker() (context, clips[0], {});
    REQUIRE (first != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return litIn (*first) > 400 * 40; }));
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return readersAlive() == before.readersAlive; }));
    CHECK (opened() == 1);
    CHECK (litIn (*first) > 400 * 40); // its levels kept

    // Another clip of the file, known on the disk: its levels in its first paint, nothing opened.
    auto copy = phazite::getClipWaveformMaker() (context, clips[0], {});
    REQUIRE (copy != nullptr);
    CHECK (litIn (*copy) > 400 * 40);
    phazite::test::dispatchPendingMessages();
    CHECK (opened() == 1);
    CHECK (readersAlive() == before.readersAlive);

    // A file not known on the disk: read once; another of its clips takes its levels from the cache once its hash is worked
    // out, nothing opened.
    auto second = phazite::getClipWaveformMaker() (context, clips[1], {});
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return litIn (*second) > 400 * 40; }));
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return readersAlive() == before.readersAlive; }));
    CHECK (opened() == 2);
    auto secondCopy = phazite::getClipWaveformMaker() (context, clips[1], {});
    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return litIn (*secondCopy) > 400 * 40; }));
    CHECK (opened() == 2);
    CHECK (readersAlive() == before.readersAlive);

    // Many waveforms of many files: none keeps a reader once its levels are in.
    std::vector<std::unique_ptr<phazite::ui::ClipWaveform>> many;

    for (size_t i = 2; i < clips.size(); ++i)
        many.push_back (phazite::getClipWaveformMaker() (context, clips[i], {}));

    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&] { return std::all_of (many.begin(), many.end(), [&] (const auto& waveform) { return litIn (*waveform) > 400 * 40; }); }));
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return readersAlive() == before.readersAlive; }));
    CHECK (opened() == 14);

    setSong (context, nullptr);
}
TEST_CASE ("A clip that doesn't follow the tempo draws its sound where it plays it, across the song's tempo changes")
{
    // Known bug 16 until 2026-11-11: such a clip plays the song's seconds, which a change of tempo puts at other beats, but its
    // waveform was spread over the clip as if the tempo held, its sound drawn away from where it plays.
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Waves", error);
    REQUIRE (project != nullptr);
    auto& song = project->getSong();
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, &song);

    // Three seconds of silence but for a burst at a second and a half, at its own speed, under 120 BPM for a beat and then 60.
    const auto file = writeBurst (temp.folder.getChildFile ("Burst.wav"), 3.0, 1.45, 1.55);
    const auto track = phazite::getTrackIDs (song).front();
    const auto clips = phazite::importFiles (context, { file.getFullPathName() }, track, phazite::BeatPosition());
    REQUIRE (clips.size() == 1);
    phazite::setClipFollowsTempo (song, clips[0], false);
    phazite::addTempoChange (song, phazite::BeatPosition::fromBeats (1.0), 60.0);
    REQUIRE_FALSE (phazite::followsTempoChanges (song, phazite::findClipState (song, clips[0])));

    // Painted as the playlist paints a clip, over all of it.
    constexpr int width = 400, height = 40;
    auto waveform = phazite::getClipWaveformMaker() (context, clips[0], {});
    REQUIRE (waveform != nullptr);
    const auto paintAll = [&waveform]
    {
        juce::Image image (juce::Image::ARGB, width, height, true);
        juce::Graphics g (image);
        g.setColour (juce::Colours::white);
        waveform->paint (g, { 0, 0, width, height }, width);
        return image;
    };

    // The columns where the clip plays the burst's middle and the last of it, as the model places them.
    const auto range = phazite::getClipRange (song, clips[0]);
    const auto columnPlaying = [&] (double fileSeconds)
    {
        for (int x = 0; x < width; ++x)
            if (const auto beat = range.getStart() + phazite::BeatDuration::fromBeats (range.getLength().inBeats() * (x + 0.5) / width);
                phazite::getFileTimeAt (song, clips[0], beat) >= fileSeconds)
                return x;

        return -1;
    };

    const auto expected = columnPlaying (1.5), last = columnPlaying (1.55) - 1;
    REQUIRE (expected > 0);
    REQUIRE (last > expected);

    // Where it draws the burst: the middle of the columns it lights more than a few pixels tall.
    const auto burstAt = [] (const juce::Image& image)
    {
        auto weighed = 0.0, total = 0.0;

        for (int x = 0; x < image.getWidth(); ++x)
            if (const auto lit = countLit (image, x, x + 1); lit > height / 4)
            {
                weighed += (x + 0.5) * lit;
                total += lit;
            }

        return total > 0.0 ? weighed / total : -1.0;
    };

    // JUCE's thumbnail reads the file's levels in pieces from its start, here a second and a half of it at a time, and each
    // is drawn as it comes: the burst straddles the first two, so its start can show before its end (CI's windows-debug
    // measured it half drawn, 2026-11-12). Once the column that plays the last of it lights, all of it is drawn.
    CHECK (phazite::test::dispatchMessagesUntil ([&] { return countLit (paintAll(), last, last + 1) > height / 4; }));
    CAPTURE (expected);
    CHECK (std::abs (burstAt (paintAll()) - (expected + 0.5)) <= 2.0);

    setSong (context, nullptr);
}

TEST_CASE ("A clip's waveform draws the same at every part of a pixel the playlist moves it by")
{
    // The owner, 2026-11-09: near the lo-fi example's end, the rain sample's waveform blinked. The view's left edge was
    // crossing its clip, which the playlist then paints afresh at each frame while the song glides under the playhead by
    // parts of a pixel (TimelineEditor::setSubPixel()). JUCE's Direct2D renderer drew the waveform's columns, a list of
    // rectangles a pixel wide, sharp at most of those parts and soft at some, as their edges' rounding fell: about one frame
    // in four looked fainter. On Linux, JUCE's software renderer draws the same at every part, before and after the fix.
    phazite::test::TemporaryFolder temp;
    juce::String error;
    auto project = phazite::Project::create (temp.folder, "Waves", error);
    REQUIRE (project != nullptr);
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    setSong (context, &project->getSong());

    // Two seconds of a quiet tone, as thin a waveform as the rain's.
    const auto file = phazite::test::writeSound (temp.folder.getChildFile ("Rain.wav"), 2.0, 220.0, 0.05f);
    const auto track = phazite::getTrackIDs (project->getSong()).front();
    const auto clips = phazite::importFiles (context, { file.getFullPathName() }, track, phazite::BeatPosition());
    REQUIRE (clips.size() == 1);
    auto waveform = phazite::getClipWaveformMaker() (context, clips[0], {});
    REQUIRE (waveform != nullptr);

    // Painted as the playlist paints a clip: the lanes placed in the window, moved by a part of a pixel, and the clip in them,
    // across x = 512, where a column's edges round differently either side.
    const auto paintAt = [&waveform] (float part)
    {
        juce::Image image (juce::Image::ARGB, 1100, 40, true);
        juce::Graphics g (image);
        g.setOrigin ({ 3, 0 });
        g.addTransform (juce::AffineTransform::translation (-part, 0.0f));
        g.setOrigin ({ 487, 0 });
        g.setColour (juce::Colours::white);
        waveform->paint (g, { 0, 0, 400, 40 }, 400);
        return image;
    };

    const auto drawn = [] (const juce::Image& image)
    {
        const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);
        auto total = 0.0;

        for (int y = 0; y < data.height; ++y)
            for (int x = 0; x < data.width; ++x)
                total += data.getPixelColour (x, y).getFloatAlpha();

        return total;
    };

    REQUIRE (phazite::test::dispatchMessagesUntil ([&] { return countLit (paintAt (0.0f), 490, 890) > 400; }));
    phazite::test::dispatchPendingMessages(); // every level loaded

    // How much it draws in all is the same at every part of a pixel.
    const auto whole = drawn (paintAt (0.0f));
    juce::Random random (7);
    juce::String report;
    auto most = 0.0;

    for (int i = 0; i < 48; ++i)
    {
        const auto part = random.nextFloat();
        const auto total = drawn (paintAt (part));
        report << juce::String (part, 4) << ": " << juce::String (total, 1) << "\n";
        most = std::max (most, std::abs (total - whole));
    }

    CAPTURE (whole);
    CAPTURE (report.toStdString());
    CHECK (whole > 400.0);
    CHECK (most < whole * 0.002);

    setSong (context, nullptr);
}
