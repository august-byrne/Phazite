// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ClipWaveforms.h"
#include "model/base/internal/Tracktion.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

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
            auto* audio = dynamic_cast<te::AudioClipBase*> (findClip());
            const auto visible = area.getIntersection (g.getClipBounds());

            if (audio == nullptr || visible.isEmpty())
                return;

            showFileOf (*audio);

            // The time in the audio file at an x: from the clip's offset, at its speed. The clip's width spans its
            // beats, which the tempo turns into times. A time-stretched clip plays a proxy file that is already at the
            // Edit's tempo.
            const auto& tempo = audio->edit.tempoSequence;
            const auto position = audio->getPosition();
            const auto speed = audio->usesTimeStretchedProxy() ? 1.0 : audio->getSpeedRatio();
            const auto xToTime = [&] (int x)
            {
                const auto beat =
                    audio->getStartBeat() + te::BeatDuration::fromBeats (audio->getLengthInBeats().inBeats() * x / std::max (1, width));
                return tempo.toTime (beat).inSeconds();
            };

            const auto sourceTime = [&] (int x)
            {
                return te::TimePosition::fromSeconds ((xToTime (x) - position.getStart().inSeconds() + position.getOffset().inSeconds())
                                                      * speed);
            };

            thumbnail->drawChannels (g, visible, { sourceTime (visible.getX()), sourceTime (visible.getRight()) }, audio->getGain());
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

        // What the clip plays now, which changes when it is stretched to another tempo, say.
        void showFileOf (te::AudioClipBase& audio)
        {
            const auto file = audio.getPlaybackFile();

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
