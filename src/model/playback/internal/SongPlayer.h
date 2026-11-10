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

class SongPlayer : private juce::ValueTree::Listener
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
    // their devices, plug-ins among them, their automation and the faders', into the master; or in a render, a part of it.
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
    // modulation or without (nothing for one it doesn't play). A fader's volume is in its positions, as its automation is.
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
    std::map<std::int64_t, std::shared_ptr<engine::ContainerState>> containers;
    std::map<std::int64_t, std::shared_ptr<engine::ChainState>> chains; // each container's chain's, by its ID
    std::set<std::pair<std::int64_t, std::int64_t>> usedDelays;
    std::set<std::int64_t> usedSends, usedContainers, usedChains;

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
    std::set<std::int64_t> usedRegions;
    engine::FileReader* reader = nullptr;
    const std::atomic<std::uint64_t>* blocks = nullptr;

    // The song's automation, each curve by its device's ID and its parameter's, read again for each plan.
    std::map<std::pair<std::uint64_t, juce::String>, std::shared_ptr<const engine::Curve>> curves;
    std::map<std::uint64_t, std::int64_t> faders; // each fader's track, by the fader's ID (the master's under 0)
    void readCurves();
    std::shared_ptr<const engine::Curve> findCurve (std::uint64_t device, const juce::String& parameter) const;
    void addFaderAutomation (engine::Plan::Strip&, const juce::ValueTree& channelState, bool isMaster);

    // Each track's notes as the plans play them, by its ID, compiled again only once something in its clips changed
    // (the song tells the player, which listens to it itself, so that every player's plans follow it): in a big song a
    // change is in one track, and its notes are many.
    std::map<std::int64_t, std::shared_ptr<const engine::NoteSequence>> notes;
    std::set<std::int64_t> notesChanged, usedNotes;
    juce::ValueTree songState;
    std::shared_ptr<const engine::NoteSequence> getNotes (const juce::ValueTree& trackState);
    void markNotes (const juce::ValueTree& changed);    // the track of the clip it is, or is in
    void markTracksIn (const juce::ValueTree& element); // it, if a track, and the tracks it holds
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override { markNotes (tree); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override;

    std::shared_ptr<engine::AudioFile> getAudioFile (const juce::File&);
    engine::AudioRegion makeRegion (std::int64_t id, std::shared_ptr<engine::AudioFile>);
    void addAudio (engine::Plan::Strip&, const juce::ValueTree& trackState);
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
    std::shared_ptr<engine::DeviceNode> getDevice (const juce::ValueTree& deviceState);
    std::shared_ptr<engine::DeviceNode> getPluginDevice (const juce::ValueTree& deviceState);
    static int findParameter (const engine::DeviceNode&, const juce::String& id); // its index, or -1

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
    std::shared_ptr<engine::Delay> getDelay (std::pair<std::int64_t, std::int64_t> key, int length); // none for none
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
    std::shared_ptr<engine::ModulatorNode> getModulator (const juce::ValueTree& modulatorState);
    void setModulatorValues (engine::ModulatorNode&, const juce::ValueTree& modulatorState);
    void addModulation (engine::Plan::Strip&, const juce::ValueTree& channelState, const std::vector<std::int64_t>& deviceIDs);
    void setValues (engine::DeviceNode&, const juce::ValueTree& deviceState);
    void loadSounds (engine::DeviceNode&, const juce::ValueTree& deviceState);
    void loadSound (engine::DeviceNode&, const juce::ValueTree& deviceState, const juce::Identifier& property); // its pad's or oscillator's
    void setChannel (const juce::ValueTree& channelState, bool isMaster);
    void updateAudibility();
};

// A file's samples as JUCE's formats read them, for the engine's reading threads (or a render's): the song's, and the
// previews' (model/engine/Preview.cpp).
std::unique_ptr<engine::SampleReader> makeSampleReader (std::unique_ptr<juce::AudioFormatReader>);
} // namespace phazite
