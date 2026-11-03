// SPDX-License-Identifier: AGPL-3.0-or-later
// The song as Phazite's own engine plays it (docs/tracktion-migration.md, T2): compiled into plans (engine/core/Plan.h),
// with what each plan points at and sounds on from one to the next (each track's strip and channel, each of Phazite's
// devices) kept by the song's IDs, and made anew for another sample rate or block size. Values set by hand (a device's
// parameter or switch, a channel's volume, pan, mute and solo, and a plug-in's parameter, by hand or by the plug-in
// itself) reach what plays at once, without a new plan. A playback session has one for its project (PlaybackSession.h),
// and a render one of its own. Message thread.
#pragma once

#include "engine/audio/FileReader.h"
#include "engine/core/Plan.h"
#include "model/base/internal/Tracktion.h"
#include "model/devices/ParameterRef.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
class SongPlayer
{
public:
    explicit SongPlayer (te::Edit&);

    // What a render plays of the song, when not all of it through the master: one channel alone, by its ID, as it reaches
    // the master (an export's stem, model/project/Export.h); or before its fader, heard whatever its mute switch or a solo
    // says, through some of its devices and containers, by their IDs, or all of them (a freeze, a bounce:
    // model/mixer/FreezeModel.h). What feeds it plays only through it, and the master adds it up without its own devices
    // or fader.
    struct RenderOf
    {
        std::int64_t channel = 0;
        bool beforeFader = false;
        std::optional<std::set<std::int64_t>> devices;
    };

    // What the engine plays of the song now, at a rate and in blocks of up to so many samples: the tempo map; the loop and
    // the metronome, live (a render plays the song once, from its start, without the metronome); and its channels, with
    // their devices, plug-ins among them, their automation and the faders', into the master; or in a render, a part of it.
    std::unique_ptr<engine::Plan> compile (double sampleRate, int maximumBlockSize, bool isRender,
                                           const std::optional<RenderOf>& of = std::nullopt);

    // Live, the audio files it plays are read ahead of the audio thread by a reading thread, and what the engine played long
    // ago is let go of after the engine's blocks pass (engine/audio/AudioFile.h); without, as in a render, each is read as it
    // is first needed and kept.
    void readLive (engine::FileReader&, const std::atomic<std::uint64_t>& blocksPlayed);

    // Takes a change of the song that is a value set by hand into what plays at once; false for any other change, which
    // needs a new plan.
    bool takeValue (const juce::ValueTree& tree, const juce::Identifier& property);

    // What plays a device (by its ID), and a track's strip (by its ID; 0 for the master's), for what shows them (meters,
    // editors' displays); nullptr for none.
    engine::DeviceNode* findDevice (std::int64_t id) const;
    engine::StripState* findStrip (std::int64_t id) const;

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

    // A plug-in's parameter set by hand, or by the plug-in itself (which Tracktion takes in as set by hand): the value its
    // modulation moves from, and which it plays while it is written by hand.
    void takeValueSetByHand (const ParameterRef&, float value);

    // What a plug-in's own window grabs and lets go of (in a helper: SandboxedInstance.h), for the automation written by
    // hand; told nothing once this has gone.
    void setPluginGestureListener (std::function<void (const ParameterRef&, bool starting)>);

    // The plug-ins it plays told that nothing of them moves now, before it goes while their Edit stays.
    void letGoOfPlugins();

    // What plays its plug-ins made anew by the next plan, prepared again (a render prepared them for itself), while no plan
    // plays them.
    void makePluginsAnew();

private:
    te::Edit& edit;
    double preparedRate = 0.0;
    int preparedBlockSize = 0;

    std::map<std::int64_t, std::shared_ptr<engine::DeviceNode>> devices;
    std::map<std::int64_t, std::shared_ptr<engine::ModulatorNode>> modulators;
    std::set<std::int64_t> usedModulators;
    std::set<std::pair<std::uint64_t, juce::String>> modulated;         // the parameters the plan being compiled modulates
    std::map<std::int64_t, std::shared_ptr<engine::StripState>> strips; // the master's under 0
    // What joins paths, kept by where they join: each delay by what it delays into what (a strip's ID, or a device's for
    // its sidechain, then the strip it takes in: twice its ID, plus one before its fader); each send's level and each
    // container's mix by their IDs.
    std::map<std::pair<std::int64_t, std::int64_t>, std::shared_ptr<engine::Delay>> delays;
    std::map<std::int64_t, std::shared_ptr<engine::SendLevel>> sendLevels;
    std::map<std::int64_t, std::shared_ptr<engine::ContainerState>> containers;
    std::set<std::pair<std::int64_t, std::int64_t>> usedDelays;
    std::set<std::int64_t> usedSends, usedContainers;
    std::set<std::int64_t> used; // the devices the plan being compiled plays
    bool lowLatency = false;     // as the plan being compiled sees it

    // The audio files the plans play, by their paths, and each audio clip's place in its file, by its ID.
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

    std::shared_ptr<engine::AudioFile> getAudioFile (const juce::File&);
    engine::AudioRegion makeRegion (std::int64_t id, std::shared_ptr<engine::AudioFile>);
    void addAudio (engine::Plan::Strip&, const juce::ValueTree& trackState);
    bool addFrozenFile (engine::Plan::Strip&, const juce::ValueTree& trackState);

    // The audio device's inputs a track hears: its input, while it is armed or always hears it (RecordingModel.h).
    std::vector<int> getHeardInput (const juce::ValueTree& trackState) const;

    std::shared_ptr<engine::StripState> getStrip (std::int64_t id);
    std::shared_ptr<engine::DeviceNode> getDevice (const juce::ValueTree& deviceState);
    std::shared_ptr<engine::DeviceNode> getPluginDevice (const juce::ValueTree& deviceState);
    static int findParameter (const engine::DeviceNode&, const juce::String& id); // its index, or -1

    // A plug-in in a helper: the parameters whose values the engine sends it with each block, which the plan's automation
    // (but while written by hand) and modulation move; and where its window's gestures go, which its instance holds as
    // long as it lives.
    void tellWhatMoves (const engine::Plan::Strip&);
    std::vector<int> moved; // the plug-in's being told
    std::shared_ptr<std::function<void (const ParameterRef&, bool)>> pluginGestures;
    // A track, a bus or a return: where its sound goes (a bus's ID, or 0 for the master), what it takes in (its tracks, what
    // sends to it, what its sidechains listen to), and what of that it refuses, which would feed itself round a loop.
    struct ChannelInfo
    {
        juce::ValueTree state {};
        std::int64_t id = 0, output = 0;
        std::vector<std::int64_t> needs {};
        std::set<std::int64_t> refused {};

        bool refuses (std::int64_t other) const { return refused.contains (other); }
    };

    std::vector<ChannelInfo> getChannels() const; // in an order in which each comes after what it takes in
    void addInputs (engine::Plan&, engine::Plan::Strip&, std::vector<engine::Plan::Input>);
    std::shared_ptr<engine::Delay> getDelay (std::pair<std::int64_t, std::int64_t> key, int length); // none for none
    std::shared_ptr<engine::SendLevel> getSendLevel (const juce::ValueTree& sendState);
    std::vector<std::int64_t> addDevices (engine::Plan&, const std::map<std::int64_t, int>& places, const ChannelInfo&,
                                          engine::Plan::Strip&, const juce::ValueTree& channelState, bool skipFrozen = false,
                                          const std::set<std::int64_t>* only = nullptr); // their IDs
    std::shared_ptr<engine::ModulatorNode> getModulator (const juce::ValueTree& modulatorState);
    void setModulatorValues (engine::ModulatorNode&, const juce::ValueTree& modulatorState);
    void addModulation (engine::Plan::Strip&, const juce::ValueTree& channelState, const std::vector<std::int64_t>& deviceIDs);
    void setValues (engine::DeviceNode&, const juce::ValueTree& deviceState);
    void loadSounds (engine::DeviceNode&, const juce::ValueTree& deviceState);
    void setChannel (const juce::ValueTree& channelState, bool isMaster);
    void updateAudibility();
};

// A file's samples as JUCE's formats read them, for the engine's reading threads (or a render's): the song's, and the
// previews' (model/engine/Preview.cpp).
std::unique_ptr<engine::SampleReader> makeSampleReader (std::unique_ptr<juce::AudioFormatReader>);
} // namespace phazite
