// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio clip editor (docs/design.md, section 7; roadmap step 2.6): one audio clip, large, in the editor dock's first
// tab while the selection is an audio clip or a track that holds audio, as the piano roll is for MIDI clips. It counts
// the song's bars, as the playlist does, so that what is trimmed and faded lines up with the song: the clip's audio in its
// track's color, with what its edges trim off shaded; and down the left, how the clip plays it: its gain, its pitch and
// fine tuning, Reverse, and Follow Tempo with the tempo the audio was played at. On the audio, as on the clip in the
// playlist, the edges trim the clip, the handles at its top corners fade it, and a drag on the audio moves it inside the
// clip, as the Slip tool does; a double-click on a fade's handle takes the fade away. It has no tools to choose: those
// handles are its one way. It works by IDs, through the model's AudioClipModel.h and PlaylistModel.h.
#pragma once

#include "model/arrangement/AudioClipModel.h"
#include "model/commands/SongGesture.h"
#include "model/project/ProjectContext.h"
#include "ui/components/Controls.h"
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
        fadeOut
    };

    juce::Rectangle<int> getHandleBounds (Handle) const;
    juce::Component& getWaveformComponent() noexcept; // where the clip's audio is drawn, for tests

    // TimelineEditor. Nothing here is selected to nudge or quantize.
    bool canUseTool (Tool) const override { return false; }
    bool canNudgeSelection() const override { return false; }
    void nudgeSelection (int) override {}
    bool canQuantizeSelection() const override { return false; }
    void quantizeSelection() override {}

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Waveform; // the clip's audio, with its handles

    ProjectContext& context;
    int shownProject = -1; // the project shown (ProjectContext::getProjectNumber()), to tell another one from the same one
    ClipID clip;
    std::optional<AudioClipInfo> info; // the clip as it was shown last

    juce::Label gainLabel, pitchLabel, fineLabel, tempoLabel, clipLabel;
    NumberField gainField, pitchField, fineField, tempoField;
    juce::TextButton reverseButton { "Reverse" }, followButton { "Follow tempo" };
    std::unique_ptr<SongGesture> fieldDrag; // while a field is dragged: the drag's one undo step
    std::unique_ptr<Waveform> waveform;
    juce::TimedCallback preparingCheck; // while Tracktion prepares the clip's audio, to show it once it is ready

    //==============================================================================
    void projectChanged() override; // another project, or none
    void songChanged (const SongChanges&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed

    void setClip (ClipID, bool keepView);
    void refresh(); // shows the clip as it is now
    void updateControls();
    void zoomToClip(); // the clip in view, with some of the audio around it
    void setUpField (NumberField&, const juce::String& undoName, std::function<void()> change);
    void changePitch(); // to what the pitch and fine fields show
    bool isTypingInField() const;

    // TimelineEditor
    juce::Rectangle<int> getTimelineArea() const override;
    BeatRange getContentRange() const override;
    void timelineChanged() override;
};
} // namespace phazite::ui
