// SPDX-License-Identifier: AGPL-3.0-or-later
// DAWproject (https://github.com/bitwig/dawproject, its version 1.0, which Bitwig Studio, Studio One, Cubase and others
// open), as Phazite writes a song into one (docs/design/versions.md: DAWproject with versions; step D1): a zip of project.xml,
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
#include <map>
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
// takes its place once complete; refused before anything is written where the archive wouldn't fit what the zip writer
// can hold (checkArchiveFits()). Any thread.
juce::Result writeDawProject (DawProjectContent&, const juce::File& bouncesFolder, const juce::File& file);

// An entry of a DAWproject's archive: its path in it, how many bytes it holds, and whether it is packed (deflated) or
// stored as it is (audio, which barely packs).
struct ArchiveEntry
{
    juce::String path;
    juce::int64 bytes = 0;
    bool packed = false;
};

// Whether the archive of these entries fits what JUCE's zip writer can hold: it writes each size and place in 32 bits and
// the count of entries in 16, without ZIP64, so that an archive or an entry of 4 GB or more, or more than 65,535 entries,
// comes out damaged while the write says it worked; and 0xFFFF entries, or 0xFFFFFFFF bytes, are what a reader takes for
// ZIP64's marker (Java's, which Bitwig Studio's is, among them): so at most 65,534 entries, and less than 4 GB. JUCE
// workaround W41 (docs/juce-workarounds.md). If not, why, and what to do instead.
inline constexpr juce::int64 maxArchiveBytes = (juce::int64) 0xfffffffe;
inline constexpr int maxArchiveEntries = 0xfffe;
juce::Result checkArchiveFits (const std::vector<ArchiveEntry>&);

// The same for a song's DAWproject before its bounces are rendered: its texts, files and presets as they are, and each
// bounce as large as a render of so many seconds can be (two channels of 32 bits at the bounces' rate), so that a song too
// large for one is refused at once rather than after minutes of rendering.
juce::Result checkDawProjectFits (const DawProjectContent&, double bounceSeconds);

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

// What a DAWproject unpacks to at most as Phazite reads it, whoever made it: its project.xml and metadata.xml no larger
// than maxDawProjectText each, and every file it copies out of it no larger than its entry says, nor, all together, than
// an archive of its size can hold (getUnpackLimit(): 1100 times its size, as deflate packs at most about 1032 to 1, and 64
// MB at least), so that one whose entries share their compressed bytes never fills the memory or the disk. What goes past
// is no DAWproject, or an audio file not taken.
inline constexpr juce::int64 maxDawProjectText = (juce::int64) 256 << 20;
juce::int64 getUnpackLimit (const juce::File& archive);

// What a DAWproject holds, taken into a song, which then is what it says; what it couldn't take, said. Its tracks are matched
// to the song's by the IDs Phazite wrote, where they were kept, or else by their names, the bounces Phazite added left out:
// a track it has none of goes, and one it adds comes, with its clips' notes and its audio clips' files (copied out of the
// zip into the song's Audio folder) and its channel's volume, pan, mute and solo. Of a track both have, its notes, its
// volume, pan and mute are taken, each compared with what the song is written as (describeDawProject()) within what a
// DAWproject's numbers keep, so that what didn't change stays as it was, to the bit; its devices, automation and audio clips
// stay the song's. Into a new song, its tempo, time signature and markers (as sections) come too. So it opens as a new
// project's song, or comes back into a copy of the version it was exported from, with what another DAW changed in it.
juce::StringArray takeDawProject (Song&, const DawProjectFile&);

// The audio files a DAWproject's clips name unpacked into a folder (a project's Audio folder), for any thread, before the file
// is taken in, so that the message thread unpacks none (found in review: a large file's audio kept the window for seconds):
// each once, the archive opened once, and not at all where the folder has a file of its name that holds the same; within what
// an archive of its size can hold (getUnpackLimit()); in blocks, until shouldStop says stop (a cancelled import, which takes
// back the file it was unpacking), telling how far it is (0 to 1). What it unpacked, by the paths inside the archive; the
// files it made, to take away again where the import doesn't finish; and whether it stopped before the end. Then takeDawProject()
// with them, which unpacks nothing more; and the files it made that a song doesn't play (its file given, which its paths
// start from) deleted, as an import that didn't finish, or took fewer clips than the file has, leaves them. And how many
// bytes were unpacked on the message thread since the program started, which the performance tests count.
struct UnpackedAudio
{
    std::map<juce::String, juce::File> files;
    std::vector<juce::File> made;
    bool stopped = false;
};

UnpackedAudio unpackDawProjectAudio (const DawProjectFile&, const juce::File& audioFolder, const std::function<bool()>& shouldStop = {},
                                     const std::function<void (float)>& progress = {});
juce::StringArray takeDawProject (Song&, const DawProjectFile&, const UnpackedAudio&);
void forgetUnusedAudio (const UnpackedAudio&, const juce::ValueTree& song, const juce::File& projectFile);
juce::int64 countBytesUnpackedOnMessageThread();

// The same into a new branch from the version it came from, of the open project, whose versions must have it: the version's
// song with what the DAWproject holds taken in, which the song then switches to, as it switches to any branch, the project
// saved after it (model/versions/VersionsModel.h: switchBranch(), with the app's save). In steps, the commit read and the
// branch's song split on the history's thread, the window held throughout (ProjectContext::holdForWork()), whose Cancel leaves
// nothing behind (the branch made is taken away again). False if it can't start; then, or once it is over, done is told
// whether it was imported, with what it couldn't take, or why not.
using ImportDone = std::function<void (bool imported, const juce::StringArray& said)>;
bool importDawProjectAsBranch (ProjectContext&, std::shared_ptr<const DawProjectFile>,
                               const std::function<void (std::function<void (bool saved)>)>& saveProjectThen, ImportDone done);
} // namespace phazite
