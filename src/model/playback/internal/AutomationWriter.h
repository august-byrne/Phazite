// SPDX-License-Identifier: AGPL-3.0-or-later
// Automation written by hand while Phazite's engine plays a project (docs/tracktion-migration.md, T2 step 4), as the write
// modes say (model/automation/AutomationValues.h). On a track that writes, a parameter punches in at the engine's playhead:
// in touch, as its control is grabbed; in latch, as it is first moved; in write, every automated parameter of the track as
// playback starts. Meanwhile it plays its value set by hand rather than its lane, from the lane's value there. What it did
// becomes the song's curve as it punches out: in touch, as its control is let go; in write, likewise, the track in latch
// after; and anything still written as playback stops. A pass writes the value where it punched in, each move after, and
// where it punched out, the value it had and the lane's value there again (a jump back to it), in place of the points in
// between; around the loop, a pass to the loop's end and another from its start. Its moves are thinned to those that change
// the line. What a control's pass writes joins its drag's undo step; what is still written as playback stops is one
// "Record Automation" step. Message thread.
#pragma once

#include "model/automation/AutomationValues.h"
#include "model/base/Song.h"
#include "model/base/internal/Tracktion.h"

#include <vector>

namespace phazite
{
class PlaybackSession;

class AutomationWriter
{
public:
    AutomationWriter (te::Edit&, PlaybackSession&);
    ~AutomationWriter();

    // The session's transport, as it starts and before it stops.
    void playbackStarted();
    void playbackStopped();

    // A control grabbed, and let go (model/devices/ParameterHandle.h), while the session plays the project.
    void gestureBegan (const ParameterRef&);
    void gestureEnded (const ParameterRef&);

    // The same by a plug-in's own window (the session tells it): what it writes as it is let go is one "Record
    // Automation" step, as no control's drag holds one.
    void pluginGestureChanged (const ParameterRef&, bool starting);

    // A parameter changed, by hand or by its automation, to a value set by hand (the session tells it).
    void parameterChanged (const ParameterRef&, float valueSetByHand, bool isFollowingAutomation);

    // Whether a pass writes a parameter now, which plays its value set by hand meanwhile: a value set by hand moves no
    // curve's one point then (model/devices/internal/Parameters.h).
    bool isWriting (const ParameterRef&) const;

private:
    struct Pass
    {
        ParameterRef parameter;
        AutomationMode mode = AutomationMode::read;
        bool held = false;                   // its control is grabbed
        float range = 1.0f;                  // how far its parameter's value goes
        bool discrete = false;               // its parameter steps (each point holds, then jumps)
        std::vector<AutomationPoint> points; // from where it punched in, or the loop's start
    };

    te::Edit& edit; // the session's, which can go before the session does (PlaybackSession::hasEdit()): its song and transport
    PlaybackSession& session;
    std::vector<Pass> passes;
    bool settingValue = false; // as a pass starts from its lane's value

    Pass* findPass (const ParameterRef&);
    Pass* punchIn (const ParameterRef&, AutomationMode, bool byMove);
    void record (Pass&, float value);
    void punchOut (const Pass&); // writes it: the caller forgets it
    void write (const Pass&, std::vector<AutomationPoint>, BeatPosition end);

    JUCE_DECLARE_NON_COPYABLE (AutomationWriter)
};
} // namespace phazite
