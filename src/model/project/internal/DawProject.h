// SPDX-License-Identifier: AGPL-3.0-or-later
// DAWproject (https://github.com/bitwig/dawproject, its version 1.0, which Bitwig Studio, Studio One, Cubase and others
// open), as Phazite writes a song into one (docs/versions.md: DAWproject with versions; step D1): a zip of project.xml,
// metadata.xml and the files they name.
//
// What goes in: the song's tracks, its folders (their tracks in them, a bus's channel a submix), its returns (effect
// channels, with the tracks' sends to them) and the master, each channel's volume, pan, mute and solo and where its sound
// goes; its MIDI clips' notes and its audio clips' files (in audio/), with their fades; the tempo and the time signature,
// with their changes; its sections, as markers; the faders' volume and pan automation; and its VST3 plug-ins, each with its
// state as its .vstpreset (in plugins/), taken from the plug-in's instance, and its automation. What another DAW can't play
// (Phazite's own devices and containers, a plug-in of another format, modulation, an audio clip's gain, stretch, pitch,
// reverse or takes, a frozen track) is bounced: a track's or a return's sound before its fader, through its devices, on an
// audio track of its own beside it (in bounces/), the track itself muted, its notes and clips kept. A bus's or the master's
// devices that can't be played, and what DAWproject has no place for (a note's mute, slide and glide, a clip's controller
// events, a fade's shape), are left out; the metadata's comment says what was.
#pragma once

#include "engine/core/TempoMap.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"

#include <functional>
#include <memory>
#include <vector>

namespace phazite
{
class PluginHost;
class PluginInstances;
class ProjectContext;
class TrackRender;

// What a song is as a DAWproject: project.xml and metadata.xml; the files that go beside them in the zip, from disk and from
// memory, by their paths there; and the tracks to bounce, each into bounces/<its ID>.wav, with their clips in project.xml,
// which writeDawProject() fits to the bounces' lengths, by the song's tempo map (in seconds).
struct DawProjectContent
{
    std::unique_ptr<juce::XmlElement> project, metadata;
    std::vector<std::pair<juce::String, juce::File>> files;
    std::vector<std::pair<juce::String, juce::MemoryBlock>> blobs;
    std::vector<std::pair<TrackID, juce::XmlElement*>> bounces; // each bounced track and its clip
    engine::TempoMap tempo;
};

// The song as a DAWproject, titled and with a comment, its VST3s' presets the states the song holds of them, which are
// their preset files (sandbox/core/Vst3State.h; store the plug-ins' states first: Song::storeDeviceStates()). Message thread.
DawProjectContent describeDawProject (const Song&, const juce::String& title, const juce::String& comment);

// Writes it into a file, its bounces taken from a folder (<the track's ID>.wav): into a temporary file next to it, which
// takes its place once complete. Any thread.
juce::Result writeDawProject (DawProjectContent&, const juce::File& bouncesFolder, const juce::File& file);

// A song exported as a DAWproject into a file, as a TrackRender (its progress window's): its bounces rendered by Phazite's
// engine on a thread of its own, with the song's session's plug-ins (which it lends them, playback stopped for it), or for a
// song that no session plays (an old version's) with plug-in instances of its own in the plug-in host, in the background;
// then the zip written. It calls back on the message thread, once, unless it is deleted first, which cancels it. Message
// thread; the song must outlive it.
std::unique_ptr<TrackRender> startDawProjectExport (Song&, PluginHost*, const juce::File&, const juce::String& title,
                                                    const juce::String& comment, std::function<void (const juce::Result&)> onFinished);

//==============================================================================
// Reading one (step D2): its project.xml and metadata.xml, from its zip; the version it was exported from, which the comment
// of a DAWproject Phazite wrote names ("(<the version's id>)"), or none; and why it couldn't be read.
struct DawProjectFile
{
    juce::File file;
    std::unique_ptr<juce::XmlElement> project, metadata;
    juce::String fromVersion;
    juce::String error;
};

DawProjectFile readDawProject (const juce::File&);

// What a DAWproject holds, taken into a song, which then is what it says; what it couldn't take, said. Its tracks are matched
// to the song's by the IDs Phazite wrote, where they were kept, or else by their names, the bounces Phazite added left out:
// a track it has none of goes, and one it adds comes, with its clips' notes and its audio clips' files (copied out of the
// zip into the song's Audio folder) and its channel's volume, pan, mute and solo. Of a track both have, its notes, its
// volume, pan and mute are taken, each compared with what the song is written as (describeDawProject()) within what a
// DAWproject's numbers keep, so that what didn't change stays as it was, to the bit; its devices, automation and audio clips
// stay the song's. Into a new song, its tempo, time signature and markers (as sections) come too. So it opens as a new
// project's song, or comes back into a copy of the version it was exported from, with what another DAW changed in it.
juce::StringArray takeDawProject (Song&, const DawProjectFile&);

// The same into a new branch from the version it came from, of the open project, whose versions must have it: the version's
// song with what the DAWproject holds taken in, which the song then switches to, as it switches to any branch, the project
// saved after it (model/versions/VersionsModel.h: switchBranch(), with the app's save). False, with why, if it can't be; else
// what it couldn't take.
bool importDawProjectAsBranch (ProjectContext&, const DawProjectFile&,
                               const std::function<void (std::function<void (bool saved)>)>& saveProjectThen, juce::StringArray& said);
} // namespace phazite
