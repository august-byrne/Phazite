// SPDX-License-Identifier: AGPL-3.0-or-later
// Where playback loops (docs/design.md, section 6). As in FL Studio's song mode, the loop is the whole song
// unless something else is looped on purpose: from the song's start to the end of the bar its last clip ends
// in, so the loop grows as clips go later. A time range selected in the playlist, or a section, can be looped
// instead. Tracktion keeps the loop in the transport, without undo steps.
#pragma once

#include "model/arrangement/Selection.h"
#include "model/base/Tracktion.h"

#include <optional>

namespace phazite
{
class CommandRegistry;

// Where the song ends: at the end of the bar its last clip ends in, as a song in FL Studio ends with the last bar that
// has something in it, or at its start if it has no clips. Every clip on the song's tracks counts, muted or not, in
// folders too; sections don't, since an outline can run longer than what is in it so far. Not Tracktion's
// Edit::getLength(), which counts sections. Go to End goes here, and the song's loop ends here.
te::TimePosition getSongEnd (const te::Edit&);

// The song as a loop: from its start to its end (getSongEnd()), and at least a bar long, so that a song without clips
// loops its first bar.
te::TimeRange getSongLoopRange (const te::Edit&);

// Whether the loop is the song's, and follows it, rather than a range looped on purpose. Kept in the project.
bool doesLoopFollowSong (const te::Edit&);

// Loops the song, or a range of it. Both turn looping on.
void loopSong (te::Edit&);
void loopRange (te::Edit&, te::TimeRange);

// Puts the loop around the song again if it follows the song and the song's end has moved.
void updateSongLoop (te::Edit&);

//==============================================================================
// Where playback starts: where the playhead was last put (a click in a ruler, Home or End), or the song's start when a
// project opens. Stopping goes back there, as in FL Studio and Ableton Live, so that a part can be heard again and
// again while it is tweaked; pausing leaves the playhead where it is. It is Tracktion's transport start position.
te::TimePosition getPlayStart (const te::Edit&);

// Puts the playhead somewhere, as a click in a ruler does: playback starts there from now on (and goes on from there,
// if it is playing).
void movePlayhead (te::Edit&, te::TimePosition);

// Home: puts the playhead at the song's start, as a click in a ruler would (movePlayhead()): playback starts there.
void goToStart (te::Edit&);

// End: moves the playhead to the song's end (getSongEnd()), as a click in a ruler would: playback starts there.
void goToEnd (te::Edit&);

// Space: plays from where playback starts, or stops and goes back there.
void playOrStop (te::Edit&);

// Plays from where the playhead is, or stops it where it is, as Shift+Space does in Ableton Live.
void playOrPause (te::Edit&);

// Stops playback and puts the playhead back where playback starts.
void stopAndReturn (te::Edit&);

//==============================================================================
// Follows where an Edit's song ends, as clips are added, moved, trimmed or deleted and the tempo changes: keeps the
// loop around the song while the loop follows it, and stops playback that doesn't loop at the song's end, rather
// than playing on into silence (as Logic does), going back to where playback starts. Each open project has one.
class SongFollower : private juce::ValueTree::Listener,
                     private juce::AsyncUpdater,
                     private juce::Timer
{
public:
    explicit SongFollower (te::Edit&);
    ~SongFollower() override;

private:
    te::Edit& edit;
    juce::ValueTree editState;
    te::TimePosition songEnd;                     // where the song's loop ends: getSongEnd(), or a bar in for a song without clips
    std::optional<te::TimePosition> lastPosition; // while playing without a loop: where the playhead was last time

    void changed (const juce::ValueTree&);
    void handleAsyncUpdate() override;
    void timerCallback() override; // stops playback at the song's end
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override { changed (tree); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child) override { changed (child); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int) override { changed (child); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongFollower)
};

//==============================================================================
// The transport's commands, for the selection manager's Edit: Play / Stop, Play / Pause, Go to Start and Go to End.
void addTransportCommands (CommandRegistry&, te::SelectionManager&);

// The loop's commands: Loop (on or off), Loop Selection (the time range selected in the playlist), Loop
// Section (the selected sections) and Loop the Song.
void addLoopCommands (CommandRegistry&, te::SelectionManager&, RangeSelection&);
} // namespace phazite
