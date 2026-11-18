// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation written by hand while Phazite's engine plays a project, as the write modes say
// (model/automation/AutomationValues.h). Each move is written where it was heard as it was made, the playhead heard: the
// engine's, earlier by the output's latency and the song's delay compensation (Engine::getPlayheadBeatBefore()), so that it
// plays back where the user heard it, as other DAWs write it. On a track that writes, a parameter punches in there: in touch,
// as its control is grabbed (or, grabbed before, as playback starts); in latch, as it is first moved; in write, every
// automated parameter of the track as playback starts. Meanwhile it plays its value set by hand rather than its lane, from
// the lane's value there. What it did becomes the song's curve as it punches out: in touch, as its control is let go; in
// write, likewise, the track in latch after; and anything still written as playback stops. A pass writes the value where it
// punched in, each move after, and where it punched out, the value it had and the lane's value there again (a jump back to
// it), in place of the points in between; around the loop, a pass to the loop's end and another from its start; and at a
// jump of the playhead, a pass to where the song was heard and another from where it lands. Its moves are thinned to those
// that change the line. The loop going round is known from the engine's rounds (PlayheadStamps::Place), not guessed. What a
// control's pass writes joins its drag's undo step; what is still written as playback stops, or at a jump, is one "Record
// Automation" step; and during a drag, what a pass writes at the loop's end or a jump waits for the drag to end (its own
// parameter's joining its step as it ends), and what a stop or another parameter's end writes waits until the drag has
// ended, committed or taken back, and is a step of its own (Gesture::whenEnded()), so that Escape takes back only what the
// drag did. Message thread.
#pragma once

#include "engine/core/PlayheadClock.h"
#include "model/automation/AutomationValues.h"
#include "model/base/Song.h"

#include <juce_events/juce_events.h>

#include <utility>
#include <vector>

namespace phazite
{
class PlaybackSession;

class AutomationWriter : private juce::AsyncUpdater
{
public:
    AutomationWriter (Song&, PlaybackSession&);
    ~AutomationWriter() override;

    // The session's transport, as it starts and before it stops; and as the playhead is put elsewhere, before the engine is
    // asked (PlaybackSession::locate()).
    void playbackStarted();
    void playbackStopped();
    void playheadJumped (BeatPosition to);

    // A control grabbed, and let go (model/devices/ParameterHandle.h), while the session plays the project.
    void gestureBegan (const ParameterRef&);
    void gestureEnded (const ParameterRef&);

    // A control's drag taken back (Escape: ParameterHandle::cancelGesture()), just before its undo step is rolled back (what
    // waits for the drag to end is written after that, in a step of its own): what its pass
    // recorded since the grab goes, and the pass is as it was before the grab: in touch, as if never grabbed; in latch, back to
    // what it held; in write, on from the value before the drag; what the pass wrote at the loop's end or a jump meanwhile,
    // up to the grab, in a step of its own. rememberGrab() notes the pass as the control is grabbed, just before
    // gestureBegan(), and again as playback starts, for a control grabbed while it was stopped; the note goes as the drag
    // ends. Ended as the drag was, the points it recorded came back as automation (latch), or what write had recorded since
    // playback started went with the rolled-back step (2026-11-14).
    void rememberGrab (const ParameterRef&);
    void gestureCancelled (const ParameterRef&);

    // A value set by hand changed by an undo or a redo while the song plays: each pass writes what plays from then (known
    // bug 174). What that finishes (the loop gone round since the pass's last move) is written once the undo or the redo has
    // returned, in a "Record Automation" step, as nothing may write through the song's history while it undoes or redoes.
    // Message thread.
    void followValuesSetByHand();

    // The same by a plug-in's own window (the session tells it): what it writes as it is let go is one "Record
    // Automation" step, as no control's drag holds one; let go during a control's drag, once that drag has ended.
    void pluginGestureChanged (const ParameterRef&, bool starting);

    // A parameter set by hand, by a control or a plug-in's own window (the session tells it).
    void parameterChanged (const ParameterRef&, float valueSetByHand);

    // Whether a pass writes a parameter now, which plays its value set by hand meanwhile: a value set by hand moves no
    // curve's one point then (model/devices/internal/Parameters.h).
    bool isWriting (const ParameterRef&) const;

    // A parameter that its curve now names otherwise (a folder's volume, its fader of the other kind: model/mixer/
    // internal/Buses.h): a pass that writes it goes on writing it by its new name, so that what was ridden is written.
    void parameterRenamed (const ParameterRef& from, const ParameterRef& to);

    // What sets many values at once sets them meanwhile (model/devices/internal/Parameters.h: ManyValuesSetByHand): no pass
    // punches in for them, and those already writing take none of them.
    void beginManyValues() noexcept { ++settingManyValues; }
    void endManyValues() noexcept { --settingManyValues; }

private:
    struct Pass
    {
        ParameterRef parameter;
        AutomationMode mode = AutomationMode::read;
        bool held = false;                   // its control is grabbed
        devices::Travel travel;              // its parameter's control's, which its points are positions along
        bool discrete = false;               // its parameter steps (each point holds, then jumps)
        std::vector<AutomationPoint> points; // from where it punched in, or the loop's start
        std::uint32_t round = 0;             // the engine's round of the loop its last point was heard on
    };

    Song& song;
    PlaybackSession& session;
    std::vector<Pass> passes;

    struct Grab
    {
        ParameterRef parameter;
        bool hadPass = false;  // a pass wrote it before the grab
        size_t points = 0;     // and its points then
        float position = 0.0f; // and its position then
    };

    std::vector<Grab> grabs;   // the controls grabbed now (rememberGrab())
    bool settingValue = false; // as a pass starts from its lane's value
    int settingManyValues = 0; // as beginManyValues() and endManyValues() count

    // What passes finished while a drag's step was open (Gesture), each a pass with the points it wrote and where it ended,
    // in order: at the loop's end or a jump, written as that parameter's own drag ends, into its step; the rest (another
    // parameter's, or what a stop ended: ownStep), once the drag has ended, committed or taken back, in a step of their own
    // (Gesture::whenEnded()); each before anything its pass writes later, which comes after it in the song's time.
    struct Part
    {
        Pass pass;
        BeatPosition end;
        bool ownStep = false;
    };

    std::vector<Part> waiting;
    bool writeAsked = false; // once the drag under way ends

    // Where the song is heard now, and the round of the loop it is on (the class's comment says why).
    engine::PlayheadStamps::Place getHeard() const;

    // The loop gone round since a pass's last move, as the engine counts its rounds: the pass so far to the loop's end, a
    // whole time round after it held at its value if the loop went round twice, and the pass on from the loop's start.
    void goRound (Pass&, std::uint32_t round);

    Pass* findPass (const ParameterRef&);
    Pass* punchIn (const ParameterRef&, AutomationMode, bool byMove);
    void record (Pass&, float value);
    void punchOut (Pass, bool intoDrag); // the caller forgets it; into the drag's step, if it is that parameter's own
    void gestureEnded (const ParameterRef&, bool ownDrag);
    void finishPart (const Pass&, std::vector<AutomationPoint>, BeatPosition end, bool intoDrag = false, bool ownStep = false);
    void writeWaiting (const ParameterRef* only = nullptr); // one parameter's, those not of a step of their own
    void writeWaitingAfterDrag();
    void handleAsyncUpdate() override; // what finished during an undo or a redo, written after it (finishPart())
    void keepPassesValues();           // each pass's value set by hand what it writes, after a drag's rollback
    void write (const Pass&, std::vector<AutomationPoint>, BeatPosition end);

    JUCE_DECLARE_NON_COPYABLE (AutomationWriter)
};
} // namespace phazite
