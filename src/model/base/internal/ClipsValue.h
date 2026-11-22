// SPDX-License-Identifier: AGPL-3.0-or-later
// A clip as a value (roadmap step S5.20, the clips' area), every clip of a song (SongClips) with the helpers that alone write
// it, and the conversion between a CLIP element with its WARP children and the value, the one place that reads and writes
// those by name. The song holds its clips in an area of its values (SongValues.h; the area's parts in ClipsArea.h).
//
// Its own header, beside SongValues.h (one header for each area's values). immer is the model's alone
// (tools/check-architecture.sh: immer-outside-model): nothing outside src/model includes this.
#pragma once

#include "model/base/Beats.h"
#include "model/base/IDs.h"
#include "model/base/internal/ClipNotesValue.h"
#include "model/base/internal/Immer.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>

namespace phazite
{
// A clip as a value (roadmap step S5.20, the clips' area): what each property of its CLIP element (Schema.h, "On CLIP") and
// its WARP children hold, in Phazite's own types. The song holds its clips as these (SongClips, below), which the clips' code
// (model/arrangement/internal/Clips.h, AudioClips.h) reads (getClipValue()) and sets for the code outside it, by their IDs.

// A warp marker as an audio clip holds it (Schema.h: WARP): a time of its file as recorded, and where that time plays, as a
// time of the file played straight at the clip's tempo, both in seconds.
struct WarpValue
{
    double time = 0.0, playsAt = 0.0;

    bool operator== (const WarpValue& other) const noexcept
    {
        return juce::exactlyEqual (time, other.time) && juce::exactlyEqual (playsAt, other.playsAt);
    }
};

// A fade as an audio clip holds it (Schema.h: clipFadeIn, clipFadeInShape, clipFadeInBend and the fade-out's): how long it
// is, in seconds; whether its shape is an S-curve (Schema.h: fadeIsSCurve) or a curve; and how far it bends, from -1 to 1.
struct ClipFade
{
    double seconds = 0.0;
    bool sCurve = false;
    float bend = 0.0f;

    bool operator== (const ClipFade& other) const noexcept
    {
        return juce::exactlyEqual (seconds, other.seconds) && sCurve == other.sCurve && juce::exactlyEqual (bend, other.bend);
    }
};

// What the audio clips of an alias group share (Schema.h: clipLink): the file, its tempo while the clip follows the song's,
// its pitch, its direction and its warp markers, in their order. The clips of a group are to hold the same value, its
// markers one vector, shared, as a group's notes are one ClipNotes under each clip's ID.
struct ClipSound
{
    juce::String file;                   // clipFile, as the song holds it: a path relative to the project, or a full one
    double tempo = 0.0;                  // clipTempo: 0 for none (it plays at the file's own speed)
    float pitch = 0.0f;                  // clipPitch, in semitones
    bool reversed = false;               // clipReversed
    immer::flex_vector<WarpValue> warps; // WARP, in the order the clip holds them (the song holds none while tempo is 0)

    // The same markers without a look inside (one vector, shared) and the rest alike; and alike, compared.
    bool isSameAs (const ClipSound&) const noexcept;
    bool operator== (const ClipSound&) const;
};

// A clip as the song holds it: each property of its CLIP element as a plain value. What a clip's element hasn't (a property
// written only while it is set, or one a damaged file left out) holds its default here: its description's (Schema.h).
struct ClipValue
{
    // What it holds (Schema.h: clipKind): notes (clipIsMidi) or a part of an audio file (clipIsAudio). Its own name, as the
    // ClipID's tag (model/base/IDs.h) is phazite::ClipKind.
    enum class Kind : std::uint8_t
    {
        midi,
        audio
    };

    ClipID id;              // clipID
    Kind kind = Kind::midi; // clipKind
    TrackID track;          // the track it is on: none for a clip on none (a copy)
    juce::String name;      // clipName: empty for none (it is called after its track)
    BeatPosition start;     // clipStart

    // clipLength: a MIDI clip's. An audio clip has none (its range follows from its part of its file and its tempo:
    // AudioClips.h), so readClipValue() gives it 0, and == leaves it out of an audio clip's value, whatever it holds.
    // A MIDI clip whose element has none holds the description's 4 beats.
    BeatDuration length = BeatDuration::fromBeats (4.0);

    BeatDuration offset;                   // clipOffset: how far into its notes a MIDI clip starts
    bool muted = false;                    // clipMute
    std::uint64_t link = 0;                // clipLink: its alias group, 0 for none (the loader takes a link of 0 away: Project.cpp)
    ClipSound sound;                       // an audio clip's file and how it plays it
    double fileStart = 0.0, fileEnd = 0.0; // clipFileStart, clipFileEnd: the part of its file, in seconds
    float gain = 0.0f;                     // clipGain, in decibels
    ClipFade fadeIn, fadeOut;              // clipFadeIn, clipFadeInShape, clipFadeInBend and the fade-out's
    int takes = 0;                         // clipTakes: 0 for none (a clip recorded round a loop has 2 or more)
    int take = 1;                          // clipTake: the one it plays, from 1
    double takeLength = 0.0;               // clipTakeLength, in seconds

    bool isMidi() const noexcept { return kind == Kind::midi; }
    bool isAudio() const noexcept { return kind == Kind::audio; }

    // Written out, not defaulted: a defaulted one would compare doubles with ==, which GCC's -Wfloat-equal refuses.
    bool operator== (const ClipValue&) const;
};

// The shortest a MIDI clip is: what the clips' code keeps a clip's length to (keepWithinClip()).
inline constexpr double shortestClipBeats = 1.0 / 64.0;

// Every clip of a song, as the clips' area holds them: the clips by their IDs, each track's clips in their order (what a
// track's clips are written in, first among its elements, in a whole copy of the song: WholeState.h; a track without clips
// has no entry), and each alias group's clips (by its link) in the order they joined it (a clip in none is in none). The
// helpers below are the only writers of the three, and keep them in step: holdsItsLists() says whether they are.
struct SongClips
{
    immer::map<std::uint64_t, ClipValue> clips;
    immer::map<std::uint64_t, immer::flex_vector<ClipID>> tracks;
    immer::map<std::uint64_t, immer::flex_vector<ClipID>> groups;

    const ClipValue* find (ClipID id) const { return clips.find (id.getRaw()); }

    // The same three without a look inside: what an area compares (ValueArea.h).
    bool isSameAs (const SongClips& other) const noexcept
    {
        return clips.identity() == other.clips.identity() && tracks.identity() == other.tracks.identity()
               && groups.identity() == other.groups.identity();
    }
};

// A clip added, at a place in its track's list (from 0, or -1 for after the others there), or one of them changed: on another
// track, it leaves its old track's list for the new one's (at the place given, or at its end); in another alias group, it
// leaves its old group's list for the end of the new one's. A clip that stays on its track keeps its place there.
SongClips withClip (const SongClips&, ClipValue, int index = -1);

// A clip taken out, of its track's list and its group's too (nothing for one that isn't there).
SongClips withoutClip (const SongClips&, ClipID);

// A track's clips put in another order: the same clips (else nothing changes).
SongClips withTrackOrder (const SongClips&, TrackID, immer::flex_vector<ClipID> order);

// Whether the three agree: each clip once in its track's list and, in an alias group, once in its group's; each list
// holding clips of that track or group alone, and none of them empty. For the tests, and the assertions where it is cheap.
// A group of one clip is a group to these (the helpers leave what the caller makes): hasGroupsOfOne() says whether any is.
bool holdsItsLists (const SongClips&);

// Whether an alias group holds one clip alone, which the model never leaves (the last clip of a group leaves it as the
// next-to-last goes: Clips.h, eraseClip()); what a song's consistency check is to say there is none of.
bool hasGroupsOfOne (const SongClips&);

//==============================================================================
// The conversion between a clip's element and its value: the one place that reads and writes a CLIP's properties and its
// WARP children by name.

// A clip's value, read from its CLIP element and its WARP children: each number as its description reads it (Schema.h: one
// that is absent reads as its fallback, or else as none: a MIDI clip's length 4 beats, its start 0, its take 1, its tempo
// none; one outside its range kept within it), nothing of the other kind of clip's (an audio clip's length and offset 0, a
// MIDI clip's file, part, gain, fades and takes at their defaults, whatever its element holds), the markers in the order
// the element holds them. Its track is the TRACK the element is on (none for an element on none). Its NOTE and
// CONTROL children are its notes' value's (readClipNotes()), and a property or a child Phazite doesn't know is no part of
// it: the song keeps none of them (Schema.h, CLIP).
ClipValue readClipValue (const juce::ValueTree& clip);

// A clip's element as a whole copy of the song writes it (WholeState.h), new: each property of its kind only while it is set
// (its ID, kind and start always; a name while it isn't empty; a MIDI clip's length always and its offset while more than 0;
// its mute, link, an audio clip's tempo, pitch, direction, gain, fades and takes while they aren't their defaults; an audio
// clip's file and its part always), so that a property another program's file held at its default is not written again;
// then an audio clip's WARP elements, in their order; then its notes (writeClipNotes()), where given. Its value as it is:
// what keeps it within what a clip holds is the clips' code's (keepWithinClip()). Its track is where the element goes.
juce::ValueTree writeClipElement (const ClipValue&, const ClipNotes* notes = nullptr);

// A clip's value kept within what a clip holds, as the clips' code keeps what it sets, and as the song takes in a clip read
// from its element (Song::holdValues(), and the keeper's safety net): nothing of the other kind of clip's properties (each
// at its default, as readClipValue() reads them), its start at 0 or later, a MIDI clip's length at least shortestClipBeats,
// the part of its file starting at 0 or later and ending at its start or later, its fades' bends within -1 to 1, and its
// takes either none (0 takes, the first take, no take's length) or a count with the take it plays one of them.
ClipValue keepWithinClip (ClipValue);

// A clip's value with one of its properties (by its name in the song: schema::clipStart, clipMute...) given a value as the
// value would read it from an element that held it, or taken away (a void value: as if the element hadn't it): a merge's
// clash on a clip's property, chosen. Whether the property is a clip's.
bool setClipProperty (ClipValue&, const juce::Identifier& property, const juce::var& value);
} // namespace phazite
