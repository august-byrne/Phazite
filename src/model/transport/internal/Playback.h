// SPDX-License-Identifier: AGPL-3.0-or-later
// Where playback loops (docs/design.md, section 6). As in FL Studio's song mode, the loop is the whole song
// unless something else is looped on purpose: from the song's start to the end of the bar its last clip ends
// in, so the loop grows as clips go later. A time range selected in the playlist, or a section, can be looped
// instead. The song keeps the loop, in beats, and where playback starts (Schema.h: loop, playStart), without undo steps,
// so that both stay on their beats as the tempo changes. The transport is the song's playback session's
// (model/playback/internal/PlaybackSession.h): without one, nothing plays.
#pragma once

#include "model/arrangement/internal/Selection.h"
#include "model/base/Beats.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

#include <optional>
#include <utility>
#include <vector>

namespace phazite
{
class CommandRegistry;
class PlaybackHost;
struct Section;

// Where the song ends: at the end of the bar its last clip ends in, as a song in FL Studio ends with the last bar that
// has something in it, or at its start if it has no clips. Every clip on the song's tracks counts, muted or not, in
// folders too; sections don't, since an outline can run longer than what is in it so far. Go to End goes here, and the
// song's loop ends here.
BeatPosition getSongEnd (const Song&);

// The song as a loop: from its start to its end (getSongEnd()), and at least a bar long, so that a song without clips
// loops its first bar.
BeatRange getSongLoopBeats (const Song&);

// Whether the loop is the song's, and follows it, rather than a range looped on purpose. Kept in the project.
bool doesLoopFollowSong (const Song&);

// Loops the song, or a range of it (an empty range changes nothing). Both turn looping on.
void loopSong (Song&);
void loopBeats (Song&, BeatRange);

// The loop as a time line's ruler shows and changes it, in song beats: where it is while playback loops (nothing while
// it doesn't), and a range to loop, which turns looping on (an empty range changes nothing).
std::optional<std::pair<double, double>> getLoopBeatRange (const Song&);
void loopBeatRange (Song&, double startBeat, double endBeat);

// What the loop goes around: the song's loop range while the loop follows the song, or else the range looped on purpose,
// in song beats; and the same while looping is on, nothing while it is off (what plays looped).
BeatRange getLoopBeats (const Song&);
std::optional<BeatRange> getLoop (const Song&);

// What the loop commands do. Looping is on or off; turning it on puts the loop around the song again if it follows
// the song. The song is looping when looping is on and the loop follows it.
bool isLoopOn (const Song&);
void toggleLoop (Song&);
bool isLoopingSong (const Song&);

// Loops the time range selected in the playlist, or the song when none is.
void loopSelection (Song&, const RangeSelection&);

// Loops around the sections, from the earliest start to the latest end. Nothing without sections.
void loopSections (Song&, const std::vector<Section>&);

//==============================================================================
// Where playback starts: where the playhead was last put (a click in a ruler, Home or End), or the song's start when a
// project opens. Stopping goes back there, as in FL Studio and Ableton Live, so that a part can be heard again and
// again while it is tweaked; pausing leaves the playhead where it is. The song keeps it in beats.
BeatPosition getPlayStartBeat (const Song&);

// Puts the playhead somewhere, as a click in a ruler does: playback starts there from now on (and goes on from there,
// if it is playing).
void movePlayhead (Song&, BeatPosition);

// Home: puts the playhead at the song's start, as a click in a ruler would (movePlayhead()): playback starts there.
void goToStart (Song&);

// End: moves the playhead to the song's end (getSongEnd()), as a click in a ruler would: playback starts there.
void goToEnd (Song&);

// Space: plays from where playback starts, or stops and goes back there. A recording that these stop is kept as one
// undo step, "Record" (Recording.h).
void playOrStop (Song&);

// Plays from where the playhead is, or stops it where it is, as Shift+Space does in Ableton Live.
void playOrPause (Song&);

// Stops playback and puts the playhead back where playback starts.
void stopAndReturn (Song&);

// Stops playback where it is; a recording it stops is kept as one undo step, "Record" (Recording.h).
void stopPlaying (Song&);

// Whether the song plays, and where its playhead is, in its beats: its session's, or where playback starts without one.
bool isSongPlaying (const Song&);
BeatPosition getPlayheadBeat (const Song&);

// The Low Latency switch (PlaybackHost.h: setLowLatency()).
bool isLowLatencyOn (const PlaybackHost&);
void toggleLowLatency (PlaybackHost&);

//==============================================================================
// The transport's commands, and the loop's.
void addTransportCommands (CommandRegistry&, Selection&, PlaybackHost&);
void addLoopCommands (CommandRegistry&, Selection&, RangeSelection&);
} // namespace phazite
