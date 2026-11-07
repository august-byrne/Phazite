// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio clip editor (docs/design.md, section 7; roadmap step 2.6): one audio clip, large, in the editor dock's first
// tab while the selection is an audio clip or a track that holds audio, as the piano roll is for MIDI clips. It shows the
// clip's whole audio file, as wide as the editor, and never scrolls or zooms (the owner, 2026-10-30): the clip's part of it
// in its track's color, with what its edges trim off shaded; its ruler counts the song's bars where the audio plays, so
// that what is trimmed lines up with the song. Down the left, how the clip plays it: its gain, with Normalize beside it,
// which sets the gain from the clip's loudest peak, its pitch and fine tuning, Reverse, and Follow Tempo with the tempo the
// audio was played at. On the audio, the clip's edges, brackets with a grip,
// trim it, the round handles at its top corners fade it, and a drag on the clip's part moves it along the audio, the clip
// staying where it is in the song (as the Slip tool does in the playlist); a double-click on a fade's handle takes the fade
// away. Each fade's curve bends by the handle in its middle, dragged up or down, as an automation lane's line bends: a
// double-click straightens it, and a right-click switches it between a curve and an S-curve (PlaylistValues.h: FadeCurve). It
// has no tools to choose: those handles are its one way. It works by IDs, through the model's AudioClipModel.h and
// PlaylistModel.h.
#pragma once

#include "model/arrangement/AudioClipModel.h"
#include "model/commands/SongGesture.h"
#include "model/project/ProjectContext.h"
#include "ui/components/Controls.h"
#include "ui/theme/Icons.h"
#include "ui/timeline/TimelineEditor.h"

#include <memory>
#include <optional>

namespace phazite::ui
{
class AudioClipEditor : public TimelineEditor,
                        private ProjectContext::Listener,
                        private juce::ChangeListener
{
public:
    // Shows the open project's audio clips, and none while no project is open.
    AudioClipEditor (CommandBridge&, ProjectContext&);
    ~AudioClipEditor() override;

    // Shows an audio clip, with its audio in view; anything else shows nothing. The editor follows the song's selection by
    // itself (AudioClipModel.h: getAudioClipToEdit()); this is for opening a clip, as double-clicking it does.
    void showClip (ClipID);
    ClipID getClip() const; // the clip shown, while it is still an audio clip in the song

    // The handles on the clip's audio, and where each is, in the waveform's coordinates (for tests); empty while no clip
    // shows.
    enum class Handle
    {
        trimStart,
        trimEnd,
        fadeIn,
        fadeOut,
        bendIn, // in the middle of the fade's curve, while it is wide enough to bend
        bendOut
    };

    juce::Rectangle<int> getHandleBounds (Handle) const; // empty for one that doesn't show
    juce::Component& getWaveformComponent() noexcept;    // where the clip's audio is drawn, for tests

    // TimelineEditor. The view shows the clip's whole audio, always; nothing here is selected to nudge or quantize.
    bool isViewFixed() const override { return true; }
    bool canUseTool (Tool) const override { return false; }
    bool canNudgeSelection() const override { return false; }
    void nudgeSelection (int) override {}
    bool canQuantizeSelection() const override { return false; }
    void quantizeSelection() override {}

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Waveform; // the clip's audio, with its handles

    ProjectContext& context;
    int shownProject = -1; // the project shown (ProjectContext::getProjectNumber()), to tell another one from the same one
    ClipID clip;
    std::optional<AudioClipInfo> info; // the clip as it was shown last

    juce::Label gainLabel, pitchLabel, fineLabel, tempoLabel, clipLabel;
    NumberField gainField, pitchField, fineField, tempoField;
    IconButton normalizeButton { "Normalize", Icon::normalize }; // at the gain's right
    juce::TextButton reverseButton { "Reverse" }, followButton { "Follow tempo" };
    std::unique_ptr<SongGesture> fieldDrag; // while a field is dragged: the drag's one undo step
    std::unique_ptr<Waveform> waveform;
    juce::TimedCallback preparingCheck; // while Phazite renders the clip's audio, to show it once it is ready

    //==============================================================================
    void projectChanged() override; // another project, or none
    void songChanged (const SongChanges&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed

    void setClip (ClipID);
    void refresh(); // shows the clip as it is now
    void updateControls();
    void fitView(); // the clip's whole audio, as wide as the editor
    void setUpField (NumberField&, const juce::String& undoName, std::function<void()> change);
    void changePitch(); // to what the pitch and fine fields show
    bool isTypingInField() const;

    // TimelineEditor
    juce::Rectangle<int> getTimelineArea() const override;
    BeatRange getContentRange() const override;
    void timelineChanged() override;
};
} // namespace phazite::ui
