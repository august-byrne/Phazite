// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's own audio recording (docs/design.md, section 17; roadmap step 3.4), the Tracktion side of RecordingModel.h,
// for one Edit, which the project context keeps while it holds the Edit.
//
// Tracktion lists the audio device's inputs, carries them to the tracks that hear them, and plays the transport with its
// count-in, loop and punch; but its own audio recording locks and allocates on the audio thread at every block, and its
// takes need a Tracktion project (docs/realtime-audit.md, finding 13). So Phazite records itself: each armed track's input
// is copied, block by block from the first the song plays, into a buffer of its own that no lock or allocation touches
// (Tracktion's input consumer, on the audio thread, in place before recording starts playback), which a thread of
// Phazite's writes into a WAV file in the project's Audio folder; when recording stops, the clips are made from those
// files, each placed where in the song its input came in (as Tracktion's own recording finds it, from the device's
// clock) moved earlier by the delays the audio device reports for its input and output, kept between where Tracktion's
// recording started and stopped, in the stop's undo step ("Record").
//
// It keeps the inputs as the tracks need them: the audio device's inputs open while a track is armed or always hears its
// input (and closed otherwise, so that no microphone is open for nothing), each of Tracktion's inputs a channel alone or
// a pair as the tracks choose, enabled while a track uses it, and carried to the tracks that hear it now. Message thread
// only, but for what the audio thread does in each input's consumer.
#pragma once

#include "model/base/internal/Tracktion.h"

#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace phazite
{
class AudioRecorder : private juce::ChangeListener,          // Tracktion's device manager, which remakes its inputs
                      private juce::ValueTree::Listener,     // the tracks' inputs and monitoring
                      private te::TransportControl::Listener // recording starting and stopping
{
public:
    explicit AudioRecorder (te::Edit&);
    ~AudioRecorder() override;

    // The recorder of an Edit the project context holds, or nullptr.
    static AudioRecorder* findFor (const te::Edit&);

    te::Edit& getEdit() const noexcept { return edit; }

    // Arming (RecordingModel.h): not saved, not an undo step. A track without an input can't be armed.
    bool isArmed (const te::AudioTrack&) const;
    void setArmed (te::AudioTrack&, bool);
    juce::Array<te::AudioTrack*> getArmedTracks() const;

    // Whether Record would record audio: a track is armed, with an input the audio device has.
    bool canRecord() const;

    // Whether audio is being recorded now, or is about to be.
    bool isRecording() const noexcept { return ! recordings.empty(); }

    // Before recording starts (startRecording(), Recording.h): a recording of each armed track's input, which takes in
    // what comes in from the first block the song plays. Tracktion starts playing before it says that recording started,
    // and the message thread can take blocks to hear it: recordings made only then would lose the start of their takes.
    // Recording starting starts them; if it doesn't start, discardRecordings() drops them.
    void prepareRecordings();
    void discardRecordings();

    // Brings the inputs in line with the tracks now, as it does by itself when they change.
    void update();

    // How many samples the last recording's audio was moved earlier by: the delay the audio device reports for its input
    // and output, the song's delay compensation and the setting's offset. For tests.
    juce::int64 getLastShift() const noexcept { return lastShift; }

    struct Recording; // one armed track's, while it records

private:
    class Writer;

    te::Edit& edit;
    te::Engine& engine;
    const te::SafeSelectable<te::Edit> alive; // gone first if the project context outlives the Edit
    const te::Edit* const key;                // where findFor() finds it
    juce::ValueTree editState;
    std::set<te::EditItemID> armed;
    std::vector<std::unique_ptr<Recording>> recordings;
    std::unique_ptr<Writer> writer;
    std::optional<te::SyncPoint> started;
    std::optional<te::TimeRange> punchRange;
    std::optional<te::TimeRange> loopRange; // while recording round a looped range
    juce::int64 lastShift = 0;
    juce::BigInteger openedForTracks; // the audio device's inputs opened for the tracks, which close when they no longer need them
    int regroupings = 0;              // asked of Tracktion in a row: it gets no more than a few tries
    bool updating = false;

    void startRecordings (te::SyncPoint, std::optional<te::TimeRange> punch); // those prepared
    void finishRecordings (te::SyncPoint stopped, bool discard);
    void removeConsumers();
    void makeClips (Recording&, te::SyncPoint stopped); // in the stop's undo step
    juce::File getTakeFile (const te::AudioTrack&) const;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;

    void recordingStarted (te::SyncPoint, std::optional<te::TimeRange> punch) override;
    void recordingStopped (te::SyncPoint, bool discardRecordings) override;
    void playbackContextChanged() override;

    JUCE_DECLARE_NON_COPYABLE (AudioRecorder)
};

// The channels of the audio device's inputs that a track's input names ("1" or "1+2"), from 0; none for an id that names
// none.
std::vector<int> getInputChannels (const juce::String& inputID);

// The audio settings' correction for recordings, in milliseconds: how much earlier what the audio inputs record goes than
// the delays their driver reports put it, for a driver that reports them wrongly; 0 at first. Tracktion keeps it for each
// input of each device.
double getRecordingCorrection (te::Engine&);
void setRecordingCorrection (te::Engine&, double milliseconds);
} // namespace phazite
