// SPDX-License-Identifier: AGPL-3.0-or-later
// The playlist as the UI shows it (docs/design.md, section 6), by IDs and plain values (architecture phase A3: no
// Tracktion here): the tracks' rows, the clips on them and what the playlist draws of them, the edits its tools make,
// and what is dropped on it. Each function takes the open project's context, and gives nothing, or does nothing, for an
// ID of something that is gone. Edits are undo steps of their own where the model's functions they call are (Tracks.h,
// Clips.h); while a gesture is open (SongGesture.h), its edits join its one step.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"

#include <juce_graphics/juce_graphics.h>

#include <optional>
#include <vector>

namespace phazite
{
class ProjectContext;

//==============================================================================
// The tracks, top to bottom as the playlist shows them: tracks and folders (Tracks.h), with the tracks in collapsed
// folders left out.
enum class TrackType
{
    track,
    folder,
    returnTrack
};

struct TrackRow
{
    TrackID id {};
    juce::String name {};
    juce::Colour colour {}; // its own, or else its row's in the palette
    TrackType kind = TrackType::track;
    int depth = 0;                  // how many folders it is in
    bool pinned = false;            // it stays in view while the others scroll
    bool collapsed = false;         // a thin row, or a folder whose tracks are hidden
    std::optional<int> savedHeight; // as the project keeps it, in pixels (view state); the playlist keeps it in range
    bool hasInstrument = false;     // a double-click on it adds a MIDI clip
};

std::vector<TrackRow> getPlaylistRows (const ProjectContext&);
std::vector<TrackID> getAllPlaylistTracks (const ProjectContext&);    // those in collapsed folders too
std::optional<TrackRow> getTrackRow (const ProjectContext&, TrackID); // any of them, shown or not

// Whether a track is inside a folder, at any depth.
bool isInside (const ProjectContext&, TrackID track, TrackID folder);

// View state, written without undo steps (Tracks.h).
void setCollapsed (ProjectContext&, TrackID, bool);
void setSavedTrackHeight (ProjectContext&, TrackID, int height);

// Track edits, each one undo step (Tracks.h): a name (an empty one gives the track its number back); a color; tracks
// moved, in their order, to right after another (an empty ID: to the start); and a track or folder added after the
// last, and selected.
void renameTrack (ProjectContext&, TrackID, const juce::String& name);
void setTrackColours (ProjectContext&, const std::vector<TrackID>&, juce::Colour);
void moveTracksAfter (ProjectContext&, const std::vector<TrackID>& moving, TrackID previous);
TrackID addTrackAtEnd (ProjectContext&, bool folder);

//==============================================================================
// The clips on a track, and what the playlist draws of them.
enum class ClipType
{
    midi,
    audio
};

enum class FadeShape
{
    linear,
    convex,
    concave,
    sCurve
};

struct ClipInfo
{
    ClipID id {};
    TrackID track {};
    ClipType kind = ClipType::midi;
    juce::String name {};
    BeatRange range {}; // where it is in the song
    bool muted = false;
    bool linked = false; // an alias: what it plays is another clip's too

    // An audio clip's fades, each as a part of the clip's length (0 to 1), and their shapes.
    double fadeIn = 0.0, fadeOut = 0.0;
    FadeShape fadeInShape = FadeShape::linear, fadeOutShape = FadeShape::linear;
};

std::vector<ClipInfo> getClips (const ProjectContext&, TrackID); // left to right
std::optional<ClipInfo> getClip (const ProjectContext&, ClipID);
bool canHoldClips (const ProjectContext&, TrackID); // a track, not a folder

// A fade's gain at a point through it, from 0 (its start) to 1 (its end), as Tracktion plays it.
float getFadeGain (FadeShape, float alpha);

// A MIDI clip's notes as the playlist draws them: each one's pitch, and where it starts and ends, in beats from the
// clip's start (outside the clip for notes it doesn't play).
struct NoteShape
{
    int pitch = 60;
    double start = 0.0, end = 0.0;
};

std::vector<NoteShape> getNoteShapes (const ProjectContext&, ClipID);

//==============================================================================
// The tools' edits of clips (Clips.h).

// The clip that Draw and Paint place, a copy of the last one used, or else a new MIDI clip on a track with an
// instrument, starting there. Returns it, or an empty ID if nothing could be placed.
ClipID placeClip (ProjectContext&, TrackID, BeatPosition start, ClipID lastUsed);

void moveClip (ProjectContext&, ClipID, BeatPosition start, TrackID);
void trimClipStart (ProjectContext&, ClipID, BeatPosition);
void trimClipEnd (ProjectContext&, ClipID, BeatPosition);

// An audio clip's fade, from its start (fadeIn) or end to a place in the song.
void fadeClipTo (ProjectContext&, ClipID, bool fadeIn, BeatPosition);

// Slip: what is inside a clip moves by the time between two places, from its offset when the drag began (in seconds:
// getSlipOffset()), while the clip stays where it is.
double getSlipOffset (const ProjectContext&, ClipID);
void slipClip (ProjectContext&, ClipID, double startOffset, BeatPosition from, BeatPosition to);

std::vector<ClipID> splitClips (ProjectContext&, const std::vector<ClipID>&, BeatPosition); // returns the right halves
std::vector<ClipID> copyClips (ProjectContext&, const std::vector<ClipID>&, bool asAliases);
void deleteClips (ProjectContext&, const std::vector<ClipID>&, bool closeGap);
void toggleClipsMuted (ProjectContext&, const std::vector<ClipID>&);
void nudgeClips (ProjectContext&, const std::vector<ClipID>&, BeatDuration);

// A new, empty MIDI clip a bar long on a track, selected. Returns it, or an empty ID.
ClipID addMidiClipAt (ProjectContext&, TrackID, BeatPosition start);

// Crossfades: where two audio clips on a track meet (they touch, or overlap in a crossfade already), the boundary
// between them; a drag from there crossfades them, as long as twice the distance from the boundary to the pointer.
// Clips a little apart (gap: a pixel of the time line, say) count as touching.
struct CrossfadeSpot
{
    ClipID first {}, second {};
    BeatPosition boundary {};
};

std::vector<CrossfadeSpot> getCrossfadeSpots (const ProjectContext&, TrackID, BeatDuration gap);
void setCrossfade (ProjectContext&, const CrossfadeSpot&, BeatPosition pointer);

//==============================================================================
// Files dropped on the playlist, or placed from the browser (Import.h): audio and MIDI files, onto a track that suits
// them or onto new tracks, starting at a place in the song. Returns the clips added.
bool canImport (const ProjectContext&, const juce::File&);
std::vector<ClipID> importFiles (ProjectContext&, const juce::StringArray& paths, TrackID target, BeatPosition start);

// The song's sections (Sections.h), in its order, as the playlist draws them in its time bar and behind the tracks.
struct SongSection
{
    SectionID id {};
    juce::String name {};
    BeatRange range {};
    juce::Colour colour {}; // its own, or its role's
    juce::String tip;       // a one-line suggestion of what to put in it, for the hint bar
    float energy = 0.5f;    // how intense it is, from 0 to 1, for the energy curve
};

std::vector<SongSection> getSongSections (const ProjectContext&);
std::optional<SongSection> getSongSection (const ProjectContext&, SectionID);

// Edits of sections, each one undo step (Sections.h). A section added where the user asks for one, selected alone
// (an empty ID if there is no room); renamed (an empty name or the same one changes nothing); or given another color.
SectionID addSectionAt (ProjectContext&, BeatPosition);
void renameSection (ProjectContext&, SectionID, const juce::String& name);
void setSectionColour (ProjectContext&, SectionID, juce::Colour);

// Edits of a section and everything in it, on every track. The destinations are section boundaries: the start of each
// section and the end of the last, places in the song as it is before the edit. A copy is returned; with asAliases, its
// clips are copied as aliases. Resizing makes it at least a beat long; with moveRest, what comes after it moves along.
std::vector<BeatPosition> getSectionBoundaries (const ProjectContext&);
void moveSection (ProjectContext&, SectionID, BeatPosition destination);
SectionID copySection (ProjectContext&, SectionID, BeatPosition destination, bool asAliases);
void resizeSection (ProjectContext&, SectionID, BeatDuration newLength, bool moveRest);

// Where Space plays from (Playback.h), and how far the time line goes: everything on it, sections included (not the
// song's end, which sections don't make longer), so that an outline can be scrolled to.
BeatPosition getPlayStartBeat (const ProjectContext&);
BeatPosition getTimelineEnd (const ProjectContext&);
} // namespace phazite
