// SPDX-License-Identifier: AGPL-3.0-or-later
// The song as Phazite's own engine plays it: compiled into plans (engine/core/Plan.h), with what each plan points at
// and sounds on from one to the next (each track's strip and channel, each of Phazite's devices) kept by the song's
// IDs, and made anew for another sample rate or block size. Values set by hand (a device's parameter or switch, a
// channel's volume, pan, mute and solo, and a plug-in's parameter, by hand or by the plug-in itself) reach what plays
// at once, without a new plan. A playback session has one for its project (PlaybackSession.h), and a render one of its
// own. Message thread.
#pragma once

#include "engine/audio/FileReader.h"
#include "engine/core/Plan.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/SongValues.h"
#include "model/devices/ParameterRef.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <juce_audio_formats/juce_audio_formats.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
class PluginInstances;
struct ClipTempo;
struct ModulatorValue;
struct PlayedClip;
struct SongModulation;

class SongPlayer : private juce::ValueTree::Listener,
                   private SongValues::Listener
{
public:
    // A song's, with its plug-ins' instances (those of its session: none play without).
    explicit SongPlayer (Song&, PluginInstances* = nullptr);
    ~SongPlayer() override;

    // What a render plays of the song, when not all of it through the master: one channel alone, by its ID, as it reaches
    // the master (an export's stem, model/project/Export.h); or before its fader, heard whatever its mute switch or a solo
    // says, through some of its devices and containers, by their IDs, or all of them (a freeze, a bounce:
    // model/mixer/FreezeModel.h). What feeds it plays only through it, and the master adds it up without its own devices
    // or fader.
    struct RenderOf
    {
        std::int64_t channel = 0;
        bool beforeFader = false;
        std::optional<std::set<std::int64_t>> devices {};
    };

    // What plays live, as the session sees it: whether Low Latency is on (PlaybackHost.h), and the tracks armed to record,
    // which hear their inputs (model/transport/internal/AudioRecorder.h).
    struct Live
    {
        bool lowLatency = false;
        std::set<TrackID> armed {};
    };

    // What the engine plays of the song now, at a rate and in blocks of up to so many samples: the tempo map; live, the loop
    // and the metronome (a render plays the song once, from its start, without the metronome); and its channels, with
    // their devices, plug-ins among them, their automation and the faders' and sends', into the master; or in a render, a
    // part of it.
    std::unique_ptr<engine::Plan> compile (double sampleRate, int maximumBlockSize, const std::optional<Live>& live,
                                           const std::optional<RenderOf>& of = std::nullopt);

    // Live, the audio files it plays are read ahead of the audio thread by a reading thread, and what the engine played long
    // ago is let go of after the engine's blocks pass (engine/audio/AudioFile.h); without, as in a render, each is read as it
    // is first needed and kept.
    void readLive (engine::FileReader&, const std::atomic<std::uint64_t>& blocksPlayed);

    // Takes a change of the song that is a value set by hand into what plays at once; false for any other change, which
    // needs a new plan.
    bool takeValue (const juce::ValueTree& tree, const juce::Identifier& property);

    // What plays a device (by its ID), a track's strip (by its ID; 0 for the master's) and a container's chain (by its ID),
    // for what shows them (meters, editors' displays); nullptr for none.
    engine::DeviceNode* findDevice (std::int64_t id) const;
    engine::StripState* findStrip (std::int64_t id) const;
    engine::ChainState* findChain (std::int64_t id) const;

    // A parameter's automation as the plan has it, or nullptr; and its value as the engine played it last, with its
    // modulation or without (nothing for one it doesn't play). A fader's volume and a send's level are in the fader's
    // positions, as their automation is.
    const engine::Curve* findCurve (const ParameterRef&) const;
    std::optional<float> getPlayedValue (const ParameterRef&, bool withModulation) const;

    // Whether automation or modulation moves a parameter in the plan, so that its control shows what the engine played.
    bool isMoved (const ParameterRef&) const;

    // What plays a modulator or a macro (by its ID), for its editor and slot; nullptr for none.
    engine::ModulatorNode* findModulator (std::int64_t id) const;

    // Whether a parameter plays its value set by hand rather than its automation, while automation is written by hand
    // (AutomationWriter.h). It stays so from plan to plan, until it is written no more.
    void setWriting (const ParameterRef&, bool);

    // A plug-in's parameter set by hand, or by the plug-in itself: the value its modulation moves from, and which it plays
    // while it is written by hand.
    void takeValueSetByHand (const ParameterRef&, float value);

    // The plug-ins it plays told that nothing of them moves now, before it goes while their instances stay.
    void letGoOfPlugins();

    // What its devices were given and the audio thread has let go of since (a sound swapped out), freed now
    // (devices::Processor::releaseUnused()): its playback session asks, 30 times a second.
    void releaseUnused();

    // What plays its plug-ins made anew by the next plan, prepared again (a render prepared them for itself), while no plan
    // plays them.
    void makePluginsAnew();

    // A version's bounce compared with the song, which of the two is heard switched through its state
    // (engine/nodes/Comparison.h), in the live plans from the next on: its file, and the version's song (a song's state, of
    // which only its tempo map counts here); or none. The same file again keeps the comparison as it was, its loudness so far
    // too. Its state is none while there is no comparison.
    struct Comparison
    {
        juce::File file;
        juce::ValueTree song;
    };

    void setComparison (std::optional<Comparison>);
    engine::ComparisonState* getComparisonState() const noexcept { return comparisonState.get(); }

    // What a plan keeps from the plan before, unless the song changed there since (known bugs 124 and 145: every edit read
    // the whole song again, its devices' values above all): each track's notes and audio clips, and the values set by hand
    // of each of Phazite's own devices on a channel whose own elements didn't change. While checking is on (as the tests run
    // it: tests/TestMain.cpp), each kept part is compared with what reading the song again gives, and a difference is a JUCE
    // assertion, which fails the test it fires in: every part in a small song, a share of them in a big one. Off unless set;
    // any thread.
    static void setChecking (bool) noexcept;
    static bool isChecking() noexcept;

    // How many of Phazite's own devices had their values read from the song for a plan, how many automation curves were read,
    // and how many times the disk was asked about an audio file, by every player since the program started (the gates run
    // without checking): for the speed gates (tests/PerformanceGateTests.cpp), which count what an edit's plan reads.
    static std::uint64_t countDevicesRead() noexcept;
    static std::uint64_t countCurvesRead() noexcept;
    static std::uint64_t countFilesAsked() noexcept;

    // Whether the checks look at a part (by its place among those a plan keeps) of a plan (by its number), checking one part
    // in every so many: any `every` plans in a row look at each part once. For the tests of the choice.
    static bool isCheckedPart (std::uint64_t part, std::uint64_t plan, std::uint64_t every) noexcept;

private:
    Song& song;
    PluginInstances* const plugins;
    double preparedRate = 0.0;
    int preparedBlockSize = 0;

    std::map<std::int64_t, std::shared_ptr<engine::DeviceNode>> devices;
    std::map<std::int64_t, std::shared_ptr<engine::ModulatorNode>> modulators;
    std::set<std::int64_t> usedModulators;
    std::set<std::pair<std::uint64_t, juce::String>> modulated;         // the parameters the plan being compiled modulates
    std::map<std::int64_t, std::shared_ptr<engine::StripState>> strips; // the master's under 0
    // What joins paths, kept by where they join: each delay by what it delays into what (a strip's ID, or a device's for
    // its sidechain, then the strip it takes in: twice its ID, plus one before its fader; or a container's chain's ID, then
    // 0, for the delay that lines it up with its container's latest chain); each send's level and each container's mix by
    // their IDs.
    std::map<std::pair<std::int64_t, std::int64_t>, std::shared_ptr<engine::Delay>> delays;
    std::map<std::int64_t, std::shared_ptr<engine::SendLevel>> sendLevels;
    std::map<std::int64_t, std::vector<engine::Plan::Route>> sendModulation; // the plan being compiled's, by the sends' IDs
    std::map<std::int64_t, std::shared_ptr<engine::ContainerState>> containers;
    std::map<std::int64_t, std::shared_ptr<engine::ChainState>> chains; // each container's chain's, by its ID
    std::set<std::pair<std::int64_t, std::int64_t>> usedDelays;         // every path's, those that need no delay too
    std::set<std::int64_t> usedSends, usedContainers, usedChains;

    // How long each path's delay was in the plan compiled last, by the same keys (0 for none), and the plans' delay
    // compensation, numbered (engine::Plan::alignment), which moves on when a path changes its delay.
    std::map<std::pair<std::int64_t, std::int64_t>, int> pathLengths;
    std::uint64_t alignment = 0, departures = 0; // and the paths gone, numbered (engine::Plan::departures)
    bool compilingAfresh = false;                // no path the plan being compiled has is new to a song that plays
    bool realigned = false;                      // a path of the plan being compiled was lined up otherwise in the last
    bool compilingLive = false;                  // the plan being compiled plays live, not in a render

    // The strip each path adds its sound into, by the same keys (an input's strip, a container's chain's); and the strips
    // paths that left went into, each with when (juce::Time::getMillisecondCounter()), for a second (markWhatReadsLeaving()).
    std::map<std::pair<std::int64_t, std::int64_t>, std::int64_t> pathDestinations;
    std::map<std::int64_t, std::uint32_t> leftInto;

    // The strips' taps (engine::Plan::Strip::taps), by the strip's ID, the point and, for a device's part, the part
    // (-1 - the sampler's ID times engine::maximumParts plus its pad; 0 otherwise): each ring, which the paths from there
    // share, and its part's output. And the plan being compiled's paths that read one: each its tap, and whether it is new.
    struct TapKey
    {
        std::int64_t strip = 0;
        engine::Plan::Strip::TapPoint point = engine::Plan::Strip::TapPoint::output;
        std::int64_t part = 0;

        auto operator<=> (const TapKey&) const = default;
    };

    struct TappedPath
    {
        TapKey tap;
        bool isNew = false;
    };

    std::map<TapKey, std::shared_ptr<engine::DelayLine>> taps;
    std::map<TapKey, std::shared_ptr<engine::PartOutput>> tapParts;
    std::map<std::pair<std::int64_t, std::int64_t>, TappedPath> tappedPaths;

    // The outputs of pads on mixer channels of their own (engine/nodes/PartOutput.h), by their samplers' IDs and their pads:
    // those of this plan, by their samplers, and how late each comes out of its sampler (for its channel's delay).
    std::map<std::pair<std::int64_t, int>, std::shared_ptr<engine::PartOutput>> partOutputs;
    std::set<std::pair<std::int64_t, int>> usedPartOutputs;
    std::map<std::int64_t, std::set<int>> separatedPads; // each pad once, however many tracks name it
    std::map<const engine::PartOutput*, int> partLatencies;
    std::set<std::int64_t> used;      // the devices the plan being compiled plays
    bool lowLatency = false;          // as the plan being compiled sees it
    std::optional<RenderOf> rendered; // what a render plays, as the plan being compiled sees it: none live

    // The audio files the plans play, by their paths and versions (getAudioFile()), and each audio clip's place in its file,
    // by its ID.
    std::map<juce::String, std::shared_ptr<engine::AudioFile>> files;
    std::map<std::int64_t, std::shared_ptr<engine::RegionState>> regions;
    std::set<juce::String> usedFiles;
    std::set<juce::String> usedSources; // by their paths: the sources of the renders the plan plays (fileKeys)
    std::set<std::int64_t> usedRegions;
    engine::FileReader* reader = nullptr;
    const std::atomic<std::uint64_t>* blocks = nullptr;

    // The song's automation, each curve by its device's ID and its parameter's (those without points apart), read again
    // only once something of it changed (or every one, after a change of the curves themselves), or while the song hasn't
    // its parameter, whose control's travel it plays along.
    using CurveKey = std::pair<std::uint64_t, juce::String>;
    std::map<CurveKey, std::shared_ptr<const engine::Curve>> curves;
    std::set<CurveKey> emptyCurves, curvesWithoutTravel;
    std::map<std::uint64_t, std::int64_t> faders; // each fader's track, by the fader's ID (the master's under 0)
    void readCurves();
    std::shared_ptr<const engine::Curve> findCurve (std::uint64_t device, const juce::String& parameter) const;
    void addFaderAutomation (engine::Plan::Strip&, const juce::ValueTree& channelState, bool isMaster);

    // What changed in the song since the plan compiled last, as the player hears it (it listens to the song itself, so that
    // every player's plans follow it): the tracks whose clips changed (their notes and audio), the channels (by their
    // tracks' IDs, the master's 0) whose devices' values may have changed, the curves whose points changed, and whether
    // the tempo did; or all of a kind. Taken whole as a compile starts (compiling), so that a change heard meanwhile is
    // the next plan's.
    struct Changes
    {
        std::set<std::int64_t> notes, channels;
        std::vector<ClipID> noteClips; // the clips whose notes changed (SongValues.h), their tracks found as a compile starts
        // The devices whose sounds changed (SOUNDS), which a kept node loads again: sounds that came whole or otherwise
        // changed under it (an undo, a song applied); those set by hand (a property of SOUNDS itself); and of those, the ones
        // its session has loaded at once (takeValue()), which aren't loaded again. Two kinds apart, and what was loaded, so
        // that the order the song's listeners hear a change in (the player's own, the session's) doesn't matter.
        std::set<std::int64_t> sounds, soundsSet, soundsTaken;
        std::set<CurveKey> curves;
        bool allNotes = false, allChannels = false, tempo = false;

        bool hasNotes (std::int64_t track) const { return allNotes || notes.contains (track); }
        bool hasChannel (std::int64_t channel) const { return allChannels || channels.contains (channel); }
        bool hasCurve (const CurveKey& key) const { return curves.contains (key); }
    };

    Changes changes, compiling;

    // Each track's notes and audio clips as the plans play them, by its ID, compiled again only once something in its clips
    // changed: in a big song a change is in one track, and its notes are many.
    std::map<std::int64_t, std::shared_ptr<const engine::NoteSequence>> notes;
    std::set<std::int64_t> usedNotes;
    juce::ValueTree songState;
    std::shared_ptr<const engine::NoteSequence> getNotes (const juce::ValueTree& trackState);
    void markChange (const juce::ValueTree& changed, bool structure); // an element added, removed or moved in it
    void markTracksIn (const juce::ValueTree& element);               // it, if a track or the master, and the tracks it holds
    void markSoundsIn (const juce::ValueTree& element);               // the devices in it, as their sounds came with it
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int newIndex) override;
    void clipNotesChanged (const std::vector<ClipID>&) override;
    void clipsChanged (const std::vector<SongValues::ClipChange>&, const std::vector<TrackID>& reordered) override;
    void tempoMapChanged() override { changes.tempo = true; } // every track's audio, which plays in seconds

    // The chord lane (SongValues.h: chordLaneChanged()): nothing, as nothing a plan holds reads chords or keys; its tree
    // callbacks leave what is in the song's CHORDLANE alone (isHeardAsKept()). Nor the MIDI links (midiLinksChanged()), which
    // no plan holds (the session gives the MIDI tap them: PlaybackSession.h); its tree callbacks leave what is in the song's
    // MIDILINKS alone too.

    // The sections (SongValues.h: sectionsChanged()): nothing, as nothing a plan holds reads them; its tree callbacks leave what
    // is in the song's SECTIONS alone (isHeardAsKept()), and its SECTIONS's own coming and going marks nothing that plays.

    // The curves that changed (SongValues.h), each read again by the next compile.
    void curvesChanged (const std::vector<ParameterRef>&) override;

    // Whether a part kept from the plan before is checked against the song now (setChecking()): all of them while the
    // player plays few devices, a share otherwise, one part in every so many, its phase moving on with each plan.
    std::uint64_t plansCompiled = 0, checkedParts = 0, checkEvery = 1;
    bool shouldCheck();

    // A track's audio clips as its strip plays them, kept from plan to plan (above): their regions, each region's clip (by
    // its ID), file (by its key in files) and the file itself; the source of each that plays a render, with its key as the
    // render was named for it (getRenderFile()); and whether every clip's file was there (one that isn't, a render not made
    // yet, is looked for again by the next plan). Made again for every track once the tempo, the rate, the song's folders or
    // what reads the files changes, and for a track one of whose files or sources changed on the disk (isOnDiskAsKept()). A
    // frozen track's file isn't kept (it is written again in its place as the track is frozen again).
    struct TrackAudio
    {
        std::vector<engine::AudioRegion> regions;
        std::vector<std::int64_t> clips;
        std::vector<juce::String> files;
        std::vector<juce::File> played;
        std::vector<std::pair<juce::File, juce::String>> sources;
        bool whole = true;
    };

    std::map<std::int64_t, TrackAudio> audio;
    juce::String audioFolders; // the song's folders the kept tracks' files were found in
    // What this player knows of each file a plan played or named a render for (its key: its identity on the disk, from its
    // path, size and when it was written: model/engine/AudioFiles.h), by its path. The disk asked at most once for a plan
    // however many ask: for a file it doesn't know yet, the clips of a track that changed (the file a clip plays may have been
    // written again in its place), a frozen track's file and a version's bounce; for any other, only once what is known of
    // it is too old to trust (getFileIdentity(): trustFilesMilliseconds, or Phazite wrote a sound file meanwhile).
    std::map<juce::String, juce::String> fileKeys;
    std::set<juce::String> askedForPlan;
    bool filesChangedForPlan = false; // a file's key that this plan found changed (compile() makes trustedForPlan's again)
    // The tracks whose audio this plan kept, or made without asking the disk again about their files, with their elements.
    std::vector<std::pair<std::int64_t, juce::ValueTree>> trustedForPlan;
    juce::String getFileKey (const juce::File&, bool askAgain);
    bool isOnDiskAsKept (const TrackAudio&); // each of its files and sources as the plan before knew it (getFileKey())
    std::shared_ptr<engine::AudioFile> getAudioFile (const juce::File&, bool askAgain, juce::String* key = nullptr);
    engine::AudioRegion makeRegion (std::int64_t id, std::shared_ptr<engine::AudioFile>);
    void placeRegion (engine::AudioRegion&, const ClipValue&, const PlayedClip&) const;
    TrackAudio makeAudio (const juce::ValueTree& trackState, std::optional<ClipTempo>&, bool askAgain); // the tempo read once
    void addAudio (engine::Plan::Strip&, const juce::ValueTree& trackState, std::optional<ClipTempo>&);
    void checkAudio (const TrackAudio& kept, const juce::ValueTree& trackState, std::optional<ClipTempo>&);
    bool addFrozenFile (engine::Plan::Strip&, const juce::ValueTree& trackState);

    // The comparison, and what plays it from plan to plan: made anew for another rate or block size, still hearing what it
    // heard.
    std::optional<Comparison> comparison;
    std::shared_ptr<engine::ComparisonState> comparisonState;
    std::shared_ptr<engine::RegionState> comparisonRegion;
    void prepareComparison (double sampleRate, int maximumBlockSize);
    void addComparison (engine::Plan&, double sampleRate);

    // The audio device's inputs a track hears: its input, while it is armed or always hears it (RecordingModel.h).
    std::vector<int> getHeardInput (const juce::ValueTree& trackState) const;

    std::shared_ptr<engine::StripState> getStrip (std::int64_t id);
    // What plays a device, its values set by hand read again from the song if its channel changed (Changes::channels).
    std::shared_ptr<engine::DeviceNode> getDevice (const juce::ValueTree& deviceState, bool channelChanged);
    std::shared_ptr<engine::DeviceNode> getPluginDevice (const juce::ValueTree& deviceState);
    static int findParameter (const engine::DeviceNode&, const juce::String& id); // its index, or -1

    // A device's values set by hand, in its description's order, as the song holds them (readValueSetByHand()), each
    // parameter's name made once for each kind of device: a juce::Identifier made from a string is looked up in JUCE's pool
    // of names, under a lock, which was most of a big song's compile (known bug 145).
    std::map<const devices::DeviceInfo*, std::vector<juce::Identifier>> parameterNames;
    std::vector<float> readValues (const devices::DeviceInfo&, const juce::ValueTree& deviceState);
    void checkValues (const engine::DeviceNode&, const juce::ValueTree& deviceState); // kept ones, while checking

    // A plug-in in a helper: the parameters whose values the engine sends it with each block, which the plan's automation
    // (but while written by hand) and modulation move.
    void tellWhatMoves (const engine::Plan::Strip&);
    std::vector<int> moved;  // the plug-in's being told
    std::set<TrackID> armed; // as the plan being compiled sees them
    // A track, a bus or a return: where its sound goes (a bus's ID, or 0 for the master), what it takes in (its tracks, what
    // sends to it, what its sidechains listen to), and what of that it refuses, which would feed itself round a loop.
    struct ChannelInfo
    {
        juce::ValueTree state {};
        std::int64_t id = 0, output = 0;
        std::vector<juce::ValueTree> sends {}; // its SEND children, looked for once
        std::vector<std::int64_t> needs {};
        std::set<std::int64_t> refused {};
        std::int64_t padDevice = 0; // a pad track's drum sampler, by its ID, and its pad (schema::trackPadDevice, trackPad)
        int pad = -1;
        std::int64_t padSource = 0; // the track the sampler is on

        bool refuses (std::int64_t other) const { return refused.contains (other); }
    };

    std::vector<ChannelInfo> getChannels() const; // in an order in which each comes after what it takes in
    void addInputs (engine::Plan&, engine::Plan::Strip&, std::vector<engine::Plan::Input>);
    // One for every path (none for a render's that needs none): reading a strip's tap, or with a ring of its own. One that
    // doesn't come and go with what the song routes (a device's own sound held back) never fades in as a new path does.
    std::shared_ptr<engine::Delay> getDelay (std::pair<std::int64_t, std::int64_t> key, int length,
                                             std::optional<TapKey> tap = std::nullopt, std::shared_ptr<engine::PartOutput> part = {},
                                             bool comesAndGoes = true);
    void leaveOutWhatIsNotHeard (engine::Plan&); // a render's
    void finishRings (engine::Plan&);            // the taps the plan's paths read, on their strips, and the rings made
    void markWhatReadsLeaving (engine::Plan&);   // the delays whose growth would play again what paths that left added (live)
    void replace (std::shared_ptr<engine::Delay>& kept, std::shared_ptr<engine::Delay> by) const; // a path's delay
    std::shared_ptr<engine::SendLevel> getSendLevel (const juce::ValueTree& sendState);
    std::vector<std::int64_t> addDevices (engine::Plan&, const std::map<std::int64_t, int>& places, const ChannelInfo&,
                                          engine::Plan::Strip&, const juce::ValueTree& channelState, bool skipFrozen = false,
                                          const std::set<std::int64_t>* only = nullptr); // their IDs

    // A container on a strip, its chains' devices each added as add() adds a device; each chain's state, kept by its ID,
    // with its values set (its fader's volume and pan, its crossover, the notes it plays), and whether each is heard; and
    // a chain of the plan being compiled, by its ID, or nullptr.
    void addContainer (engine::Plan::Strip&, const juce::ValueTree& containerState,
                       const std::function<void (const juce::ValueTree&)>& add);
    std::shared_ptr<engine::ChainState> getChain (const juce::ValueTree& chainState);
    void setChainValues (engine::ChainState&, const juce::ValueTree& chainState);
    void updateChainAudibility (const juce::ValueTree& containerState);
    static engine::Plan::Container::Chain* findPlannedChain (engine::Plan::Strip&, std::int64_t id);
    // A modulator or a macro as the song holds it (model/base/internal/ModulationValue.h), its node kept by its ID; and a
    // channel's own and its containers' from the song's modulation, which the compile reads once.
    std::shared_ptr<engine::ModulatorNode> getModulator (const ModulatorValue&);
    void setModulatorValues (engine::ModulatorNode&, const ModulatorValue&);
    void addModulation (engine::Plan::Strip&, const SongModulation&, const juce::ValueTree& channelState,
                        const std::vector<std::int64_t>& deviceIDs);
    void loadSounds (engine::DeviceNode&, const juce::ValueTree& deviceState);
    void loadSound (engine::DeviceNode&, const juce::ValueTree& deviceState, const juce::Identifier& property); // its pad's or oscillator's
    void setChannel (const juce::ValueTree& channelState, bool isMaster);
    void updateAudibility();
};

// A file's samples as JUCE's formats read them, for the engine's reading threads (or a render's): the song's, and the
// previews' (model/engine/Preview.cpp).
std::unique_ptr<engine::SampleReader> makeSampleReader (std::unique_ptr<juce::AudioFormatReader>);
} // namespace phazite
