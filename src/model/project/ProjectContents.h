// SPDX-License-Identifier: AGPL-3.0-or-later
// What the open project holds, as the Project view in the left panel shows it (docs/design.md, section 4: the Project view):
// its notes; the audio files its song plays, its audio clips' and its devices' sound files, each found or missing; and the
// plug-ins it holds, each playing or what is wrong with it. By plain values and IDs; each function gives nothing, or does
// nothing, while no project is open. The audio files in its folder that nothing plays are Clean Up Unused Audio's
// (UnusedAudio.h), and a crashed plug-in is reloaded by reloadDevice() (model/mixer/MixerModel.h).
#pragma once

#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace phazite
{
class ProjectContext;

// The notes, which the song keeps (Schema.h: notes), lines and all. Changed, they are a change of their own, "Change
// Notes", unless a gesture is open (model/commands/SongGesture.h), as while they are typed, whose one step they join.
juce::String getProjectNotes (const ProjectContext&);
void setProjectNotes (ProjectContext&, const juce::String&);

//==============================================================================
// An audio file the song plays: the path the song keeps for it (within the project's folder, "Audio/Kick.wav", or a full
// one), the file that names, whether it is missing, the channels that play it (their names, each once: a clip's track, or a
// device's track or the master), and how many clips and devices' sounds play it.
struct ProjectAudioFile
{
    juce::String path {};
    juce::File file {};
    bool missing = false;
    juce::StringArray channels {};
    int uses = 0;

    bool operator== (const ProjectAudioFile&) const = default;
};

// Each once, in the order of the channels that play it first, top to bottom, the master last. Without the disk: missing as
// the files given say (what a MissingAudioCheck found, below), none where none are given.
std::vector<ProjectAudioFile> getProjectAudioFiles (const ProjectContext&, const std::set<juce::File>& missing = {});

// What these functions ask of the disk on the message thread, counted (as the library counts its own): every such question
// here is noted first (noteProjectDiskQuery()), and getProjectAudioFiles(), which the Project view reads at each of its
// ticks, asks none (the tests hold it to that). Any thread.
void noteProjectDiskQuery() noexcept;
int getProjectDiskQueriesOnMessageThread() noexcept;

// Which of the files given are missing, asked of the disk on a thread of its own, so that the window never waits for a drive
// (a network share or a synced folder that is slow to answer, a disk spinning up: seconds for each file). One check at a
// time: check() while one is under way asks that one to stop at its next file and keeps the files given, the newest, for the
// check that starts once it has ended (a check stuck inside the system's call on one file holds back the next, never two of
// them). Nothing waits for a check: as this goes, one under way is left to end by itself, telling nothing. onChecked is told
// on the message thread once the newest check is done; getMissing() keeps what the last check that was done found until the
// next is. Message thread.
class MissingAudioCheck
{
public:
    MissingAudioCheck();
    ~MissingAudioCheck();

    void check (const juce::Array<juce::File>&);
    bool isChecking() const noexcept { return checking; }
    const std::set<juce::File>& getMissing() const noexcept { return missing; }
    std::function<void()> onChecked;

    // Tests: what the next checks' threads do before they ask about each file (held until let go, as a drive slow to answer);
    // how many checks this started; and how many checks' threads, of every MissingAudioCheck, are under way (what the test
    // program waits for before it ends). Any thread, the last.
    void setBeforeChecking (std::function<void (const juce::File&)> before) { beforeChecking = std::move (before); }
    int getChecksStarted() const noexcept { return started; }
    static int getChecksUnderWay() noexcept;

private:
    struct Running; // the check under way's: whether a newer one waits, or this went
    std::shared_ptr<Running> running;
    std::optional<juce::Array<juce::File>> next; // the newest asked for while one was under way
    std::function<void (const juce::File&)> beforeChecking;
    std::set<juce::File> missing;
    bool checking = false;
    int started = 0;

    void start (const juce::Array<juce::File>&);
    void ended (bool stopped, std::set<juce::File>);

    JUCE_DECLARE_WEAK_REFERENCEABLE (MissingAudioCheck)
    JUCE_DECLARE_NON_COPYABLE (MissingAudioCheck)
};

// A missing file found again, or another put in its place: the project's copy of the file found (copied into its Audio
// folder, as an import copies it) takes the path's place everywhere the song plays it, as one undo step, "Locate Audio File".
// Whether anything changed: nothing for a path the song doesn't play, a file that isn't there, or one at the same path.
// The file is copied in the background (model/arrangement/internal/AudioWork.h), the step made once it is: true once that
// started. locateAudioFileNow(): the change itself, at once, for a file that is the project's own already.
bool locateAudioFile (ProjectContext&, const juce::String& path, const juce::File& found);
bool locateAudioFileNow (ProjectContext&, const juce::String& path, const juce::File& found);

//==============================================================================
// How a plug-in plays now (model/plugins/internal/PluginInstances.h).
enum class PluginState
{
    playing,
    loading, // it opens in the background, as the project opened, and plays once it has
    crashed, // its helper went, or it crashed as it loaded: it plays nothing until it is reloaded (reloadDevice())
    missing, // it isn't installed here, or scanning hasn't found it
    off      // the project was opened with its plug-ins off, after a crash of Phazite's
};

// A plug-in the song holds: its device, the channel it is on (a track, or the master: its ID) with that channel's name, the
// plug-in as the song names it (its name, maker and format), whether it is an instrument, and how it plays.
struct ProjectPlugin
{
    DeviceID device {};
    TrackID channel {};
    juce::String channelName {}, name {}, maker {}, format {};
    bool isInstrument = false;
    PluginState state = PluginState::playing;

    bool operator== (const ProjectPlugin&) const = default;
};

// In the order of their channels, top to bottom, the master last, and on each in the order its sound goes through them,
// those in containers too.
std::vector<ProjectPlugin> getProjectPlugins (const ProjectContext&);
} // namespace phazite
