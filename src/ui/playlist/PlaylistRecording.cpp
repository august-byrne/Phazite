// SPDX-License-Identifier: AGPL-3.0-or-later
// What is being recorded, on each track it records onto, as it comes in (model/transport/RecordingModel.h:
// getRecordingsInProgress()), as Ableton Live draws a clip growing while it records: a clip in the track's colour with its
// edge in the danger colour, from where recording started (round a looped range, where this time round began) to the
// playhead's line, an input's waveform drawn as it comes in and the notes as they are played. When recording stops, the
// clips the take made take their place.
#include "PlaylistParts.h"

#include <algorithm>

namespace phazite::ui
{
namespace
{
    // An input's waveform while it records, as the playlist's clips draw theirs (ClipWaveform.h): the loudest sample of what
    // has come in under each column, from the middle out, over the beats the clip that draws it shows.
    class RecordingWaveform final : public ClipWaveform
    {
    public:
        RecordingWaveform (ProjectContext& c, TrackID t, const ClipComponent& shownBy)
            : context (c),
              track (t),
              clip (shownBy)
        {
        }

        void paint (juce::Graphics& g, juce::Rectangle<int> area, int width) override
        {
            const auto range = clip.getInfo().range;
            const auto painted = area.getIntersection (g.getClipBounds());

            if (painted.isEmpty() || width <= 0 || range.isEmpty())
                return;

            const auto start = range.getStart().inBeats(), length = range.getLength().inBeats();
            const auto beatAt = [start, length, width] (int x) { return BeatPosition::fromBeats (start + length * x / width); };
            const auto levels =
                getRecordingLevels (context, track, BeatRange (beatAt (painted.getX()), beatAt (painted.getRight())), painted.getWidth());
            const auto middle = (float) area.getCentreY(), half = (float) area.getHeight() * 0.5f;

            for (int i = 0; i < painted.getWidth(); ++i)
                if (const auto height = std::min (1.0f, levels[(size_t) i]) * half; height > 0.0f)
                    g.fillRect ((float) (painted.getX() + i), middle - height, 1.0f, std::max (1.0f, 2.0f * height));
        }

        void paintSpan (juce::Graphics&, juce::Rectangle<int>, BeatRange) override {} // the audio clip editor shows no recording

    private:
        ProjectContext& context;
        const TrackID track;
        const ClipComponent& clip;
    };

    bool isSame (const RecordingInProgress& a, const RecordingInProgress& b)
    {
        return a.track == b.track && a.source == b.source;
    }
} // namespace

void Playlist::showRecordings()
{
    // Asked at every tick: cheap while nothing records (the session has no take).
    auto recordings = context.hasProject() ? getRecordingsInProgress (context) : std::vector<RecordingInProgress>();

    if (recordings.empty() && shownRecordings.empty())
        return;

    // Those no longer recorded go: once recording stops, the clips the take made show in their place.
    std::erase_if (shownRecordings,
                   [&recordings] (const ShownRecording& shown)
                   {
                       return std::none_of (recordings.begin(), recordings.end(), [&shown] (const RecordingInProgress& recording)
                                            { return isSame (recording, shown.recording); });
                   });

    for (auto& recording : recordings)
    {
        auto found = std::find_if (shownRecordings.begin(), shownRecordings.end(),
                                   [&recording] (const ShownRecording& shown) { return isSame (shown.recording, recording); });

        if (found == shownRecordings.end())
        {
            auto& added = shownRecordings.emplace_back();
            added.clip = std::make_unique<ClipComponent> (context, ClipID());

            if (recording.source == RecordSource::input)
            {
                added.waveform = std::make_unique<RecordingWaveform> (context, recording.track, *added.clip);
                added.clip->showWaveform (added.waveform.get());
            }

            found = std::prev (shownRecordings.end());
        }

        auto& shown = *found;
        shown.recording = std::move (recording);

        ClipInfo info;
        info.track = shown.recording.track;
        info.kind = shown.recording.source == RecordSource::input ? ClipType::audio : ClipType::midi;
        info.name = "Recording";
        info.range = shown.recording.range;

        auto& clip = *shown.clip;
        clip.showRecording (shown.recording.take);
        clip.showClip (info);

        if (info.kind == ClipType::midi)
            clip.showNotes (shown.recording.notes);

        showLookOf (clip);
    }

    layOutRecordings();
}

void Playlist::layOutRecordings()
{
    // Its end at the playhead's line while that is just past what has come in, or behind it: the line is what the eye
    // follows, and an input comes in a few milliseconds after it is heard.
    const auto playheadBeat = getShownPlayheadBeat();
    const auto& timeline = getView();
    const auto reach = BeatDuration::fromBeats (RecordingTint::reach / std::max (1.0e-6, timeline.pixelsPerBeat));

    for (auto& shown : shownRecordings)
    {
        auto& clip = *shown.clip;
        const auto row = getRowOf (shown.recording.track);

        if (row < 0)
        {
            clip.setVisible (false); // in a collapsed folder
            continue;
        }

        auto recorded = shown.recording.range;

        if (playheadBeat > recorded.getStart() && playheadBeat < recorded.getEnd() + reach)
            recorded = BeatRange (recorded.getStart(), playheadBeat);

        if (recorded != clip.getInfo().range)
        {
            auto info = clip.getInfo();
            info.range = recorded;
            clip.showClip (info);
        }

        // Over the clips already there, all of the row's height, as a clip that overlaps none has it.
        const auto& r = rows[(size_t) row];
        auto& layer = r.track.pinned ? pinnedClips : scrollingClips;
        const auto left = timeline.beatToColumn (recorded.getStart());
        const auto right = timeline.beatToColumn (recorded.getEnd());
        const juce::Rectangle<int> bounds (left, getRowTop (r), std::max (2, right - left), std::max (1, r.height - 1));

        if (clip.getParentComponent() != &layer)
            layer.addAndMakeVisible (clip);

        clip.setVisible (bounds.intersects (layer.getBounds()));
        clip.setBounds (bounds - layer.getPosition());
        clip.toFront (false);
    }
}

void Playlist::playheadShown()
{
    if (! shownRecordings.empty())
        layOutRecordings();
}
} // namespace phazite::ui
