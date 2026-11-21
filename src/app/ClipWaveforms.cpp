// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ClipWaveforms.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/BackgroundJobs.h"
#include "model/base/internal/Schema.h"
#include "model/engine/AudioFiles.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Tempo.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // The levels of every file a waveform has drawn, by the file and when it last changed, kept in memory while any waveform
    // shows (the 200 most recently used), so that a clip that plays a file another has drawn draws without its file being
    // opened again: in its first paint where the file's identity on the disk is known, else once its hash is worked out.
    // Read on a thread of its own; and the files' hashes worked out and the files opened for the waveforms on threads of
    // their own, never in a paint, where an open waited for the drive (a share, a synced folder).
    struct Levels
    {
        juce::AudioThumbnailCache cache { 200 };

        // The hashes whose levels the waveforms put into the cache, which the openings ask (any thread): one of them is
        // taken from the cache, the file not opened. The cache lets the oldest go, so a hash found here may miss there: it is
        // forgotten, and the file opened.
        struct Stored
        {
            std::mutex lock;
            std::set<juce::int64> hashes;

            bool contains (juce::int64 hash)
            {
                const std::scoped_lock held (lock);
                return hashes.count (hash) > 0;
            }
        };

        const std::shared_ptr<Stored> stored = std::make_shared<Stored>();

        // The hash last worked out for a file, by the file and its identity on the disk as it was known
        // (findKnownFileIdentity()), which a paint may ask: a file written again has another identity. Message thread.
        std::map<juce::String, juce::int64> hashesByIdentity;

        static juce::String getKey (const juce::File& file, const juce::String& identity)
        {
            return file.getFullPathName() + "\n" + identity;
        }

        void remember (juce::int64 hash, const juce::File& file, const juce::String& identity)
        {
            {
                const std::scoped_lock held (stored->lock);
                stored->hashes.insert (hash);
            }

            if (identity.isNotEmpty())
                hashesByIdentity[getKey (file, identity)] = hash;
        }

        void forget (juce::int64 hash)
        {
            {
                const std::scoped_lock held (stored->lock);
                stored->hashes.erase (hash);
            }

            std::erase_if (hashesByIdentity, [hash] (const auto& entry) { return entry.second == hash; });
        }

        BackgroundJobs openings { "Phazite waveform files", 2, juce::Thread::Priority::normal };

        // As the last waveform goes (the project closes): its thread stopped first, however long the slice of a file it reads
        // takes, a read of 65,536 of a file's samples at most, through a reader already open: JUCE's own stop, as the cache
        // goes, ends a thread that hasn't stopped within 2 s by force, which can leave a lock it held held for ever. Then
        // the openings (its member, which goes after this) drop those that wait and wait for those that run, each the open of
        // a file and the read of its header, however long the drive takes. JUCE workaround W1 (docs/juce-workarounds.md).
        ~Levels() { cache.getTimeSliceThread().stopThread (-1); }
    };

    constexpr int samplesPerLevel = 256; // how many of a file's samples each level the cache keeps sums up

    // What the tests set to run before a file is opened (setBeforeWaveformFileOpens()).
    std::mutex beforeOpenLock;
    std::function<void (const juce::File&)> beforeOpen;

    std::function<void (const juce::File&)> getBeforeOpen()
    {
        const std::scoped_lock held (beforeOpenLock);
        return beforeOpen;
    }

    std::atomic<int> opens { 0 }, opensOnMessageThread { 0 }, readersAlive { 0 }; // countWaveformFiles()

    // A file's reader, counted as it is made and as it goes: the format's own, read whole through JUCE's reader of a part of
    // another.
    class CountedReader final : public juce::AudioSubsectionReader
    {
    public:
        explicit CountedReader (juce::AudioFormatReader* reader)
            : juce::AudioSubsectionReader (reader, 0, reader->lengthInSamples, true)
        {
            ++readersAlive;
        }

        ~CountedReader() override { --readersAlive; }
    };

    // The one place a waveform opens a file: none if it isn't sound.
    std::unique_ptr<juce::AudioFormatReader> openReader (const juce::File& file)
    {
        ++opens;

        if (juce::MessageManager::existsAndIsCurrentThread())
            ++opensOnMessageThread;

        if (auto* reader = getAudioFormats().createReaderFor (file))
            return std::make_unique<CountedReader> (reader);

        return {};
    }

    class Waveform : public ui::ClipWaveform,
                     private juce::ChangeListener,
                     private FileIdentityListener
    {
    public:
        Waveform (ProjectContext& c, ClipID id, std::function<void()> onLoaded)
            : context (c),
              clipID (id),
              loaded (std::move (onLoaded))
        {
            addFileIdentityListener (*this);
        }

        ~Waveform() override
        {
            removeFileIdentityListener (*this);
            forgetOpening();

            if (thumbnail != nullptr)
                thumbnail->removeChangeListener (this);
        }

        void paint (juce::Graphics& g, juce::Rectangle<int> area, int width) override
        {
            // The clip's width spans its beats.
            if (const auto clipState = findClip(); clipState.isValid())
            {
                const auto beats = getClipRange (*findSong (context), clipState);
                draw (clipState, g, area, [&beats, width] (int x)
                      { return beats.getStart() + BeatDuration::fromBeats (beats.getLength().inBeats() * x / std::max (1, width)); });
            }
        }

        void paintSpan (juce::Graphics& g, juce::Rectangle<int> area, BeatRange span) override
        {
            if (const auto clipState = findClip(); clipState.isValid())
            {
                const auto start = span.getStart();
                const auto length = span.getLength().inBeats();
                draw (clipState, g, area, [start, length, &area] (int x)
                      { return start + BeatDuration::fromBeats (length * (x - area.getX()) / std::max (1, area.getWidth())); });
            }
        }

        void forgetDrawing() override { mask = {}; }

    private:
        ProjectContext& context;
        const ClipID clipID;
        std::function<void()> loaded; // called as more of the levels load
        juce::SharedResourcePointer<Levels> levels;
        std::unique_ptr<juce::AudioThumbnail> thumbnail;
        juce::File shownFile;       // what it shows
        juce::String shownIdentity; // and the file's identity on the disk as it was known then (empty if it wasn't)
        int levelsChanged = 0;      // how often the thumbnail's levels changed: more of them read, or another file's

        // A file being opened for it in the background (open()): whether it still wants what the open makes, which the job
        // reads; and the waveform to take it, on the message thread, until it goes or wants another file.
        struct Opening
        {
            std::atomic<bool> wanted { true };
            Waveform* waveform = nullptr;
        };

        // What an open made: the hash that JUCE's own source of a file gives it, which the cache keeps the levels by; whether
        // the cache has them, so that the file wasn't opened; else the file's reader (none if it isn't there or isn't sound).
        struct Opened
        {
            juce::int64 hash = 0;
            bool cached = false;
            std::unique_ptr<juce::AudioFormatReader> reader;
        };

        std::shared_ptr<Opening> opening; // the open it waits for, if any

        // While the thumbnail reads the levels from a reader of the file: the hash they go into the cache by, and how many
        // samples the file has.
        std::optional<juce::int64> readingHash;
        juce::int64 readingLength = 0;

        // A stretch of the file that plays straight, where it is drawn: the file's times at its left and its right (the later
        // one at its left while the clip plays backwards), and the clip's gain.
        struct Stretch
        {
            juce::Rectangle<int> area;
            double from = 0.0, to = 0.0;
            float gain = 1.0f;
            bool backwards = false;

            bool isSameAs (const Stretch& other) const noexcept
            {
                return area == other.area && juce::exactlyEqual (from, other.from) && juce::exactlyEqual (to, other.to)
                       && juce::exactlyEqual (gain, other.gain) && backwards == other.backwards;
            }
        };

        // The waveform as it was drawn last: a mask over the area it was drawn in and a pixel more all round, see-through,
        // so that its edges stay whole wherever a part of a pixel moves it, at the display's scale; and what it shows.
        struct Mask
        {
            juce::Image image;
            juce::Rectangle<int> area;
            float scale = 0.0f;
            std::vector<Stretch> stretches;
            int levelsChanged = -1;

            bool shows (juce::Rectangle<int> shownArea, float shownScale, const std::vector<Stretch>& shown, int changes) const
            {
                return image.isValid() && area == shownArea && juce::exactlyEqual (scale, shownScale) && levelsChanged == changes
                       && std::equal (stretches.begin(), stretches.end(), shown.begin(), shown.end(),
                                      [] (const Stretch& a, const Stretch& b) { return a.isSameAs (b); });
            }
        };

        Mask mask;

        // More of the file has been read: what shows it shows it. Every level read, the reader goes (letGoOfReader()). JUCE's
        // levels hold a level for each whole 256 samples, read in blocks of 256 levels: the thumbnail has all it will have
        // once it has a level for each whole 256 of the file's samples.
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            ++levelsChanged;

            if (hasEveryLevel())
                letGoOfReader();

            if (loaded != nullptr)
                loaded();
        }

        // Whether the thumbnail, reading from a reader, has every level it will have.
        bool hasEveryLevel() const
        {
            return readingHash.has_value() && thumbnail->getNumSamplesFinished() >= readingLength / samplesPerLevel * samplesPerLevel;
        }

        // A thumbnail given a reader keeps it until it is cleared (it owns it: juce::AudioThumbnail::setReader()), which kept
        // the file open for as long as the waveform lived, one for each waveform, past the files a process may have open.
        // Its levels, every one read, go into the cache (storeThumb(), which JUCE's own thread also does as it finishes),
        // the thumbnail is cleared, which lets the reader go, and they come back from the cache, which is memory. The clear
        // waits for a slice of the cache's thread under way for this thumbnail, which, every level being in, reads nothing.
        // At a zoom finer than a level, JUCE draws from the levels once it has no reader, where it read the file's samples.
        // JUCE workaround W59 (docs/juce-workarounds.md).
        void letGoOfReader()
        {
            const auto hash = *std::exchange (readingHash, std::nullopt);
            levels->cache.storeThumb (*thumbnail, hash);
            thumbnail->clear();
            levels->cache.loadThumb (*thumbnail, hash);
            levels->remember (hash, shownFile, shownIdentity);
        }

        // Its file was found written again in its place, by what asked the disk (the player's next plan): its picture let
        // go, and painted again at once, which opens the file anew in the background (showFileOf()).
        void fileIdentityChanged (const juce::File& file) override
        {
            if (file != shownFile)
                return;

            mask = {};

            if (loaded != nullptr)
                loaded();
        }

        // The clip's element, if it is an audio clip of the open project's song.
        juce::ValueTree findClip() const
        {
            auto* song = findSong (context);
            const auto clipState = song != nullptr ? findClipState (*song, clipID) : juce::ValueTree();
            return isAudioState (clipState) ? clipState : juce::ValueTree();
        }

        // Draws the file over the part of an area that it has audio for, given the song beat at each x: over all of the area,
        // not only the part being painted, as a mask kept while what it shows stays the same, filled with the colour set. A
        // strip that repaints, such as under the playhead, draws of the mask and reads nothing again, while an area as wide
        // as the part being painted would read the levels at each paint (docs/archive/performance-audit.md, section 8). JUCE draws a
        // waveform as a list of rectangles a pixel wide, which its Direct2D renderer draws sharp or soft by how the rounding
        // of their edges falls, where a part of a pixel moves them: as the playlist glides the song under the playhead, a clip
        // it paints afresh at each frame (one that the view's edge cuts) blinked fainter about one frame in four (the owner,
        // 2026-11-09). JUCE's software renderer draws the mask the same every time, and moving it moves it whole.
        // JUCE workaround W13 (docs/juce-workarounds.md).
        void draw (const juce::ValueTree& clipState, juce::Graphics& g, juce::Rectangle<int> area,
                   const std::function<BeatPosition (int)>& beatAt)
        {
            if (! area.intersects (g.getClipBounds()))
                return;

            const auto& song = *findSong (context);
            showFileOf (song, clipState);
            const auto stretches = findStretches (song, area, beatAt);

            if (stretches.empty())
                return;

            const auto scale = g.getInternalContext().getPhysicalPixelScaleFactor();

            if (! mask.shows (area, scale, stretches, levelsChanged))
                drawMask (area, scale, stretches);

            // Its alpha filled with the colour set.
            const auto padded = area.expanded (1);
            const auto placed = juce::AffineTransform::scale (1.0f / scale).translated ((float) padded.getX(), (float) padded.getY());
            g.drawImageTransformed (mask.image, placed, true);
        }

        // The stretches of an area that the file plays straight, each over the part of it that the file has audio for: between
        // two of the clip's warp markers (without markers, all of it at once); and for a clip that plays the song's seconds
        // rather than its beats (one that doesn't follow the tempo's changes), between two of the song's tempo changes, and a
        // few pixels at a time where the tempo ramps, as seconds go evenly along beats only where the tempo holds. Spread over
        // the clip as if the tempo held, its sound was drawn away from where it plays (known bug 16 until 2026-11-11).
        std::vector<Stretch> findStretches (const Song& song, juce::Rectangle<int> area,
                                            const std::function<BeatPosition (int)>& beatAt) const
        {
            const auto left = beatAt (area.getX()).inBeats(), right = beatAt (area.getRight()).inBeats();
            const auto xAt = [&area, left, right] (double beat)
            { return area.getX() + juce::roundToInt ((beat - left) / (right - left) * area.getWidth()); };
            std::vector<int> edges { area.getX() };

            if (right > left)
            {
                for (const auto& marker : getWarpMarkers (song, clipID))
                    if (const auto beat = marker.beat.inBeats(); beat > left && beat < right)
                        edges.push_back (xAt (beat));

                if (const auto clipState = findClip(); clipState.isValid() && ! followsTempoChanges (song, clipState))
                    addTempoEdges (getTempoChanges (song), left, right, xAt, edges);
            }

            edges.push_back (area.getRight());
            std::sort (edges.begin(), edges.end());
            std::vector<Stretch> stretches;

            for (size_t i = 0; i + 1 < edges.size(); ++i)
                if (edges[i + 1] > edges[i])
                    if (const auto stretch = findStretch (song, area.withX (edges[i]).withRight (edges[i + 1]), beatAt))
                        stretches.push_back (*stretch);

            return stretches;
        }

        // Where the song's tempo changes between two beats, and every few pixels where it ramps or bends to the next tempo
        // (rather than holding, then stepping, or jumping at once): counted from the area's own coordinates' 0 (the clip's
        // left in the playlist), so that they stay put as the part of the clip in view changes.
        static void addTempoEdges (const std::vector<TempoChange>& changes, double left, double right,
                                   const std::function<int (double)>& xAt, std::vector<int>& edges)
        {
            constexpr int rampStep = 8; // pixels

            for (size_t i = 0; i < changes.size(); ++i)
            {
                const auto beat = changes[i].beat.inBeats();

                if (beat > left && beat < right)
                    edges.push_back (xAt (beat));

                if (const auto curve = changes[i].curve; i + 1 < changes.size() && curve > -1.0f && curve < 1.0f)
                {
                    const auto from = xAt (std::max (beat, left)), to = xAt (std::min (changes[i + 1].beat.inBeats(), right));

                    for (auto x = (int) std::floor ((double) from / rampStep + 1.0) * rampStep; x < to; x += rampStep)
                        edges.push_back (x);
                }
            }
        }

        // A stretch the file plays straight, over the part of an area that it has audio for: nothing if it has none there.
        std::optional<Stretch> findStretch (const Song& song, juce::Rectangle<int> area,
                                            const std::function<BeatPosition (int)>& beatAt) const
        {
            // The time in the file at an x, as the clip plays it: stretched to the song's tempo, say, or going back from
            // its end while the clip plays backwards.
            const auto fileTime = [&] (int x) { return getFileTimeAt (song, clipID, beatAt (x)); };
            const auto backwards = isClipReversed (song, clipID);
            auto drawn = area;

            // Only where the file has audio, once the thumbnail knows how long it is.
            if (const auto length = thumbnail->getTotalLength(); length > 0.0)
            {
                const auto left = fileTime (drawn.getX()), right = fileTime (drawn.getRight());

                if (juce::exactlyEqual (left, right) || std::max (left, right) <= 0.0 || std::min (left, right) >= length)
                    return {};

                const auto xAt = [&] (double time)
                { return drawn.getX() + (int) std::round ((time - left) / (right - left) * drawn.getWidth()); };

                if (const auto start = xAt (0.0), end = xAt (length); backwards)
                    drawn = drawn.withLeft (std::max (drawn.getX(), end)).withRight (std::min (drawn.getRight(), start));
                else
                    drawn = drawn.withLeft (std::max (drawn.getX(), start)).withRight (std::min (drawn.getRight(), end));
            }

            if (drawn.getWidth() <= 0)
                return {};

            // Backwards, the file is drawn from its later time to its earlier, mirrored into place.
            return Stretch { drawn, fileTime (backwards ? drawn.getRight() : drawn.getX()),
                             fileTime (backwards ? drawn.getX() : drawn.getRight()),
                             juce::Decibels::decibelsToGain (getClipGain (song, clipID)), backwards };
        }

        // Draws the stretches into a new mask, through JUCE's software renderer, at the display's scale.
        void drawMask (juce::Rectangle<int> area, float scale, const std::vector<Stretch>& stretches)
        {
            const auto padded = area.expanded (1);
            juce::Image drawn (juce::Image::SingleChannel, std::max (1, (int) std::ceil ((float) padded.getWidth() * scale)),
                               std::max (1, (int) std::ceil ((float) padded.getHeight() * scale)), true, juce::SoftwareImageType());
            {
                juce::Graphics g (drawn);
                g.addTransform (juce::AffineTransform::translation ((float) -padded.getX(), (float) -padded.getY()).scaled (scale));
                g.setColour (juce::Colours::white);

                for (const auto& stretch : stretches)
                {
                    const juce::Graphics::ScopedSaveState saved (g);

                    if (stretch.backwards)
                        g.addTransform (juce::AffineTransform::scale (-1.0f, 1.0f, stretch.area.toFloat().getCentreX(), 0.0f));

                    thumbnail->drawChannels (g, stretch.area, stretch.from, stretch.to, stretch.gain);
                }
            }

            // As the display's own image, which the graphics card keeps from one paint to the next.
            mask = { juce::NativeImageType().convert (drawn), area, scale, stretches, levelsChanged };
        }

        // The file the clip's audio comes from. Not the render it plays while stretched, pitched or reversed, which may not
        // be ready yet: the waveform shows the file unstretched, as Ableton Live shows a sample, and all of it.
        // Shown again once the file is known to be written again in its place (its identity on the disk as last asked:
        // model/engine/AudioFiles.h), which a paint never asks: a clip whose file isn't there would ask at every paint.
        void showFileOf (const Song& song, const juce::ValueTree& clipState)
        {
            const auto file = getClipFile (song, clipState);
            const auto identity = findKnownFileIdentity (file);

            if (thumbnail != nullptr && file == shownFile && (identity.isEmpty() || identity == shownIdentity))
            {
                // The thumbnail tells of its last levels just before it counts them (setLevels(), then the count, in
                // JUCE's readNextBlock()), so that message can come while one is still to count, and no other follows:
                // asked again here, as it paints, so that the reader never stays for that.
                if (hasEveryLevel())
                    letGoOfReader();

                return;
            }

            shownFile = file;
            shownIdentity = identity;
            ++levelsChanged;
            forgetOpening();
            readingHash.reset();

            if (thumbnail == nullptr)
            {
                thumbnail = std::make_unique<juce::AudioThumbnail> (samplesPerLevel, getAudioFormats(), levels->cache);
                thumbnail->addChangeListener (this); // repaints as it loads
            }
            else
            {
                thumbnail->clear(); // no other file's levels meanwhile, nor its reader
            }

            // Levels read before for the file as it is known: from the cache at once, in this paint, from memory (a copy of a
            // clip, the half of one split, a clip brought back by Undo).
            if (identity.isNotEmpty())
            {
                if (const auto found = levels->hashesByIdentity.find (Levels::getKey (file, identity));
                    found != levels->hashesByIdentity.end())
                {
                    if (levels->cache.loadThumb (*thumbnail, found->second))
                        return;

                    levels->forget (found->second);
                }
            }

            open (file);
        }

        // Works out the file's hash in the background, and opens it there unless the cache has its levels, for the thumbnail
        // to take on the message thread (take()): JUCE's thumbnail, given the file as a source, asks the disk when it was
        // written, opens it and reads its header on the thread that gives it the source, which was a paint's.
        void open (const juce::File& file)
        {
            forgetOpening();
            opening = std::make_shared<Opening>();
            opening->waveform = this;

            levels->openings.add (
                [file, wanted = opening, stored = levels->stored] (const BackgroundJobs::ShouldStop& shouldStop)
                {
                    if (shouldStop() || ! wanted->wanted)
                        return;

                    // The levels kept by the file and when it was written, as JUCE's source of a file keys them: by its path
                    // alone, the cache gave the levels of the file it replaced. A file written again in its place has another
                    // time, so new levels.
                    const auto opened = std::make_shared<Opened>();

                    if (file.existsAsFile())
                    {
                        opened->hash = juce::FileInputSource (file, true).hashCode();
                        opened->cached = stored->contains (opened->hash);

                        if (! opened->cached && ! shouldStop())
                        {
                            if (const auto before = getBeforeOpen())
                                before (file);

                            opened->reader = openReader (file);
                        }
                    }

                    // A waveform that has gone, or wants another file, takes nothing: the file opened is let go here, on this
                    // thread. If it goes after this, the message's end lets it go on the message thread, which closes it.
                    if (! wanted->wanted)
                        return;

                    juce::MessageManager::callAsync (
                        [wanted, opened]
                        {
                            if (wanted->waveform != nullptr)
                                wanted->waveform->take (*opened);
                        });
                });
        }

        // The file's levels from the cache; or, opened, read from it on the cache's thread (or found in the cache at once by
        // the thumbnail), the reader let go once they are in (changeListenerCallback()).
        void take (Opened& opened)
        {
            forgetOpening();

            if (opened.cached)
            {
                // Gone from the cache since (it keeps the most recently used): opened after all.
                if (! levels->cache.loadThumb (*thumbnail, opened.hash))
                {
                    levels->forget (opened.hash);
                    open (shownFile);
                    return;
                }

                levels->remember (opened.hash, shownFile, shownIdentity);
            }
            else
            {
                if (opened.reader != nullptr)
                {
                    readingHash = opened.hash;
                    readingLength = opened.reader->lengthInSamples;
                }

                thumbnail->setReader (opened.reader.release(), opened.hash);
            }

            ++levelsChanged;
            mask = {};

            if (loaded != nullptr)
                loaded();
        }
        // What it waited for opened is no longer wanted.
        void forgetOpening()
        {
            if (opening != nullptr)
            {
                opening->wanted = false;
                opening->waveform = nullptr;
                opening = nullptr;
            }
        }
    };
} // namespace

ui::ClipWaveformMaker getClipWaveformMaker()
{
    return [] (ProjectContext& context, ClipID clip, std::function<void()> loaded) -> std::unique_ptr<ui::ClipWaveform>
    { return std::make_unique<Waveform> (context, clip, std::move (loaded)); };
}

void setBeforeWaveformFileOpens (std::function<void (const juce::File&)> before)
{
    const std::scoped_lock held (beforeOpenLock);
    beforeOpen = std::move (before);
}

WaveformFiles countWaveformFiles()
{
    return { opens.load(), opensOnMessageThread.load(), readersAlive.load() };
}
} // namespace phazite
