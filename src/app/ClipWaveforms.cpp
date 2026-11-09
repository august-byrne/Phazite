// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ClipWaveforms.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/Clips.h"
#include "model/base/internal/Schema.h"
#include "model/engine/AudioFiles.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    // The levels of every file a waveform has drawn, by the file and when it last changed, kept in memory while any waveform
    // shows (the 200 most recently used), so that a clip that plays a file another has drawn draws at once. Loaded on a
    // thread of its own.
    struct Levels
    {
        juce::AudioThumbnailCache cache { 200 };
    };

    constexpr int samplesPerLevel = 256; // how many of a file's samples each level the cache keeps sums up

    class Waveform : public ui::ClipWaveform,
                     private juce::ChangeListener
    {
    public:
        Waveform (ProjectContext& c, ClipID id, std::function<void()> onLoaded)
            : context (c),
              clipID (id),
              loaded (std::move (onLoaded))
        {
        }

        ~Waveform() override
        {
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
        juce::File shownFile;  // what it shows
        int levelsChanged = 0; // how often the thumbnail's levels changed: more of them read, or another file's

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

        // More of the file has been read: what shows it shows it.
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            ++levelsChanged;

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
        // as the part being painted would read the levels at each paint (docs/performance-audit.md, section 8). JUCE draws a
        // waveform as a list of rectangles a pixel wide, which its Direct2D renderer draws sharp or soft by how the rounding
        // of their edges falls, where a part of a pixel moves them: as the playlist glides the song under the playhead, a clip
        // it paints afresh at each frame (one that the view's edge cuts) blinked fainter about one frame in four (the owner,
        // 2026-11-09). JUCE's software renderer draws the mask the same every time, and moving it moves it whole.
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

        // The stretches of an area that the file plays straight, between two of the clip's warp markers (without markers, all
        // of it at once), each over the part of it that the file has audio for.
        std::vector<Stretch> findStretches (const Song& song, juce::Rectangle<int> area,
                                            const std::function<BeatPosition (int)>& beatAt) const
        {
            const auto left = beatAt (area.getX()).inBeats(), right = beatAt (area.getRight()).inBeats();
            std::vector<int> edges { area.getX() };

            for (const auto& marker : getWarpMarkers (song, clipID))
                if (const auto beat = marker.beat.inBeats(); right > left && beat > left && beat < right)
                    edges.push_back (area.getX() + juce::roundToInt ((beat - left) / (right - left) * area.getWidth()));

            edges.push_back (area.getRight());
            std::sort (edges.begin(), edges.end());
            std::vector<Stretch> stretches;

            for (size_t i = 0; i + 1 < edges.size(); ++i)
                if (edges[i + 1] > edges[i])
                    if (const auto stretch = findStretch (song, area.withX (edges[i]).withRight (edges[i + 1]), beatAt))
                        stretches.push_back (*stretch);

            return stretches;
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
        void showFileOf (const Song& song, const juce::ValueTree& clipState)
        {
            const auto file = getClipFile (song, clipState);

            if (thumbnail != nullptr && file == shownFile)
                return;

            shownFile = file;
            ++levelsChanged;

            if (thumbnail == nullptr)
            {
                thumbnail = std::make_unique<juce::AudioThumbnail> (samplesPerLevel, getAudioFormats(), levels->cache);
                thumbnail->addChangeListener (this); // repaints as it loads
            }

            thumbnail->setSource (file.existsAsFile() ? new juce::FileInputSource (file) : nullptr);
        }
    };
} // namespace

ui::ClipWaveformMaker getClipWaveformMaker()
{
    return [] (ProjectContext& context, ClipID clip, std::function<void()> loaded) -> std::unique_ptr<ui::ClipWaveform>
    { return std::make_unique<Waveform> (context, clip, std::move (loaded)); };
}
} // namespace phazite
