// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio inputs of one Edit (docs/design.md, section 17; roadmap step 3.4), the Tracktion side of RecordingModel.h, which
// the project context keeps while it holds the Edit: which tracks are armed, and the audio device's inputs opened while a
// track is armed or always hears its input (and closed otherwise, so that no microphone is open for nothing). Phazite's
// engine plays what comes in and records it (Take.h), and placeTake() puts what a take recorded where it was played.
// Message thread only.
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/Tracktion.h"

#include <optional>
#include <set>
#include <vector>

namespace phazite
{
class AudioRecorder : private juce::ChangeListener,     // Tracktion's device manager, as the audio device changes
                      private juce::ValueTree::Listener // the tracks' inputs and monitoring
{
public:
    explicit AudioRecorder (te::Edit&);
    ~AudioRecorder() override;

    // The recorder of an Edit the project context holds, or nullptr.
    static AudioRecorder* findFor (const te::Edit&);

    te::Edit& getEdit() const noexcept { return edit; }

    // Arming (RecordingModel.h): not saved, not an undo step. A track without an input can't be armed.
    bool isArmed (const te::AudioTrack&) const;
    bool isArmed (TrackID) const;
    void setArmed (te::AudioTrack&, bool);
    juce::Array<te::AudioTrack*> getArmedTracks() const;

    // Whether Record would record audio: a track is armed, with an input the audio device has.
    bool canRecord() const;

    // The audio device's inputs: those Phazite's engine is given while it plays the project, or Tracktion's.
    int getNumInputs() const;

    // Brings the inputs in line with the tracks now, as it does by itself when they change, but while a take records.
    void update();

    // The file a take of a track's input goes into: in the project's Audio folder, named after the track, a take not
    // there yet.
    juce::File getTakeFile (const te::AudioTrack&) const;

private:
    te::Edit& edit;
    te::Engine& engine;
    const te::SafeSelectable<te::Edit> alive; // gone first if the project context outlives the Edit
    const te::Edit* const key;                // where findFor() finds it
    juce::ValueTree editState;
    std::set<TrackID> armed;
    juce::BigInteger openedForTracks; // the audio device's inputs opened for the tracks, which close when they no longer need them
    bool updating = false;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;

    JUCE_DECLARE_NON_COPYABLE (AudioRecorder)
};

// Where a take's file goes in the song: where its file's first frame was played, and when recording started and stopped,
// as if nothing looped, in seconds; the range a punched take keeps; and the range it went round, with where, as if nothing
// looped, the first time round began.
struct TakePlace
{
    double sampleRate = 44100.0;
    juce::int64 frames = 0; // in the file
    double firstFrame = 0.0, start = 0.0, stop = 0.0;
    std::optional<juce::Range<double>> punch, loop;
    double firstLoopStart = 0.0;
};

// Makes the clips of a track's take, in the caller's undo step: what was played where it was played; punched in, only
// what falls in the range, in place of what was there, cross-fading at its edges; round a looped range, a take for each
// time round, in one clip over the range, the last whole one playing. The file goes if it keeps nothing.
void placeTake (te::Edit&, TrackID, const juce::File&, const TakePlace&);

// The channels of the audio device's inputs that a track's input names ("1" or "1+2"), from 0; none for an id that names
// none.
std::vector<int> getInputChannels (const juce::String& inputID);

// The audio settings' correction for recordings, in milliseconds: how much earlier what the audio inputs record goes than
// the delays their driver reports put it, for a driver that reports them wrongly; 0 at first. Tracktion keeps it for each
// input of each device.
double getRecordingCorrection (te::Engine&);
void setRecordingCorrection (te::Engine&, double milliseconds);
} // namespace phazite
