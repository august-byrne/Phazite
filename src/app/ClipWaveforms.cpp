// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ClipWaveforms.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/base/internal/Tracktion.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

#include <cmath>
#include <functional>

namespace phazite
{
namespace
{
    class Waveform : public ui::ClipWaveform
    {
    public:
        Waveform (ProjectContext& c, ClipID id, juce::Component& component)
            : context (c),
              clipID (id),
              toRepaint (component)
        {
        }

        void paint (juce::Graphics& g, juce::Rectangle<int> area, int width) override
        {
            // The clip's width spans its beats.
            if (auto* audio = dynamic_cast<te::AudioClipBase*> (findClip()))
            {
                const auto beats = audio->getEditBeatRange();
                draw (*audio, g, area, [&beats, width] (int x)
                      { return beats.getStart() + te::BeatDuration::fromBeats (beats.getLength().inBeats() * x / std::max (1, width)); });
            }
        }

        void paintSpan (juce::Graphics& g, juce::Rectangle<int> area, BeatRange span) override
        {
            if (auto* audio = dynamic_cast<te::AudioClipBase*> (findClip()))
            {
                const auto start = toTracktion (span.getStart());
                const auto length = span.getLength().inBeats();
                draw (*audio, g, area, [start, length, &area] (int x)
                      { return start + te::BeatDuration::fromBeats (length * (x - area.getX()) / std::max (1, area.getWidth())); });
            }
        }

    private:
        ProjectContext& context;
        const ClipID clipID;
        juce::Component& toRepaint;
        std::unique_ptr<te::SmartThumbnail> thumbnail;
        juce::File shownFile; // what it shows

        te::Clip* findClip() const
        {
            auto* edit = getEdit (context);
            return edit != nullptr ? te::findClipForID (*edit, toTracktion (clipID)) : nullptr;
        }

        // Draws the file over the part of an area that is being painted and that the file has audio for, given the song beat
        // at each x.
        void draw (te::AudioClipBase& audio, juce::Graphics& g, juce::Rectangle<int> area,
                   const std::function<te::BeatPosition (int)>& beatAt)
        {
            auto visible = area.getIntersection (g.getClipBounds());

            if (visible.isEmpty())
                return;

            showFileOf (audio);

            // The time in the file at an x, as the clip plays it: stretched to the song's tempo, say.
            const auto fileTime = [&] (int x) { return getFileTimeAt (audio, beatAt (x)); };

            // Only where the file has audio, once the thumbnail knows how long it is.
            if (const auto length = thumbnail->getTotalLength(); length > 0.0)
            {
                const auto left = fileTime (visible.getX()), right = fileTime (visible.getRight());

                if (right <= 0.0 || left >= length || right <= left)
                    return;

                const auto xAt = [&] (double time)
                { return visible.getX() + (int) std::round ((time - left) / (right - left) * visible.getWidth()); };

                if (left < 0.0)
                    visible.setLeft (xAt (0.0));

                if (right > length)
                    visible.setRight (xAt (length));
            }

            if (visible.getWidth() > 0)
                thumbnail->drawChannels (g, visible,
                                         { te::TimePosition::fromSeconds (fileTime (visible.getX())),
                                           te::TimePosition::fromSeconds (fileTime (visible.getRight())) },
                                         audio.getGain());
        }

        // The file the clip's audio comes from, which changes when it is reversed. Not the proxy it plays while stretched or
        // pitched, which holds only what the clip plays, and only once it is ready: the waveform shows it unstretched, as
        // Ableton Live shows a sample, and all of it.
        void showFileOf (te::AudioClipBase& audio)
        {
            const auto file = audio.getAudioFile();

            if (thumbnail != nullptr && file.getFile() == shownFile)
                return;

            shownFile = file.getFile();

            if (thumbnail == nullptr)
                thumbnail = std::make_unique<te::SmartThumbnail> (audio.edit.engine, file, toRepaint, &audio.edit); // repaints as it loads
            else
                thumbnail->setNewFile (file);
        }
    };
} // namespace

ui::ClipWaveformMaker getClipWaveformMaker()
{
    return [] (ProjectContext& context, ClipID clip, juce::Component& toRepaint) -> std::unique_ptr<ui::ClipWaveform>
    { return std::make_unique<Waveform> (context, clip, toRepaint); };
}
} // namespace phazite
