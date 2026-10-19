// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Freeze.h"
#include "FreezeModel.h"
#include "internal/Mixer.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"

#include <utility>

namespace phazite
{
namespace
{
    te::AudioTrack* findAudioTrack (const ProjectContext& context, TrackID id)
    {
        auto* edit = getEdit (context);
        return edit != nullptr && id.isValid() ? dynamic_cast<te::AudioTrack*> (te::findTrackForID (*edit, toTracktion (id))) : nullptr;
    }

    // Tracktion's AudioTrack renders its frozen file on the message thread when its frozen switch turns on, and undoes the
    // freeze through the undo manager when it turns off (tracktion_AudioTrack.cpp: freezeTrack(), unFreezeTrack()). Phazite
    // renders the file in the background and sets the rest itself, so the track mustn't hear its switch: it listens to its
    // own state through a protected base, which a C-style cast reaches.
    void setFrozenSwitch (te::AudioTrack& track, bool frozen)
    {
        auto* trackItself = (juce::ValueTree::Listener*) &track;
        track.state.setPropertyExcludingListener (trackItself, te::IDs::frozenIndividually, frozen, nullptr);
    }

    // Why a track can't be rendered on its own, frozen or bounced (what it would be, and what is done to it); empty if it
    // can.
    juce::String getWhyNotRender (const te::Track* track, const juce::String& verb, const juce::String& done)
    {
        if (track == nullptr)
            return "Select a track to " + verb + ".";

        if (track->isFolderTrack())
            return "Folders and buses can't be " + done + ": their sound is their tracks'.";

        if (getReturnBus (*track) >= 0)
            return "Returns can't be " + done + ": their sound comes from the tracks that send to them.";

        auto* audioTrack = dynamic_cast<const te::AudioTrack*> (track);

        if (audioTrack == nullptr)
            return track->getName() + " can't be " + done + ".";

        if (isRecording (track->edit))
            return "Stop recording first.";

        if (audioTrack->getClips().isEmpty())
            return track->getName() + " has nothing to " + verb + ": it has no clips.";

        if (audioTrack->getOutput().getDestinationTrack() != nullptr)
            return track->getName() + "'s sound goes into another track, so it can't be " + done + " on its own.";

        return {};
    }

    // Properties written into a device's state without the device's sound changing: Tracktion's frozen switch, a plug-in
    // window's place, and a plug-in's own saved state, which every save stores again (model/devices/internal/
    // DeviceStates.h); and the IDs Phazite gives notes.
    bool isIgnoredProperty (const juce::Identifier& property)
    {
        return property == te::IDs::frozen || schema::isDeviceViewProperty (property) || property == te::IDs::state
               || property == schema::phaziteNoteID;
    }

    bool isSameSound (const juce::ValueTree& a, const juce::ValueTree& b)
    {
        if (a.getType() != b.getType() || a.getNumChildren() != b.getNumChildren())
            return false;

        const auto sameProperties = [] (const juce::ValueTree& one, const juce::ValueTree& other)
        {
            for (int i = 0; i < one.getNumProperties(); ++i)
            {
                const auto name = one.getPropertyName (i);

                if (! isIgnoredProperty (name) && ! (other.hasProperty (name) && one[name] == other[name] && other[name] == one[name]))
                    return false;
            }

            return true;
        };

        if (! sameProperties (a, b) || ! sameProperties (b, a))
            return false;

        for (int i = 0; i < a.getNumChildren(); ++i)
            if (! isSameSound (a.getChild (i), b.getChild (i)))
                return false;

        return true;
    }

    // What a frozen track's file holds, as its state shows it: its clips, its frozen devices, and its modulators and
    // macros, which can move them.
    juce::Array<juce::ValueTree> getWhatTheFreezeHolds (const te::AudioTrack& track)
    {
        juce::Array<juce::ValueTree> parts;

        for (const auto& child : track.state)
        {
            const auto isFrozenPlugin = child.hasType (te::IDs::PLUGIN) && (bool) child.getProperty (te::IDs::frozen, false);

            if (te::Clip::isClipState (child) || isFrozenPlugin || child.hasType (te::IDs::MODIFIERS)
                || child.hasType (te::IDs::MACROPARAMETERS))
                parts.add (child);
        }

        return parts;
    }

    juce::ValueTree copyWhatTheFreezeHolds (const te::AudioTrack& track)
    {
        juce::ValueTree copy ("FROZEN");

        for (const auto& part : getWhatTheFreezeHolds (track))
            copy.appendChild (part.createCopy(), nullptr);

        return copy;
    }

    bool holdsWhatWasFrozen (const te::AudioTrack& track, const juce::ValueTree& frozen)
    {
        const auto parts = getWhatTheFreezeHolds (track);

        if (parts.size() != frozen.getNumChildren())
            return false;

        for (int i = 0; i < parts.size(); ++i)
            if (! isSameSound (parts[i], frozen.getChild (i)))
                return false;

        return true;
    }

    // The rate a track renders at: the audio device's, as the file plays alongside it (44.1 kHz without a device).
    double getRenderSampleRate (te::Edit& edit)
    {
        const auto rate = edit.engine.getDeviceManager().getSampleRate();
        return rate > 7000.0 ? rate : 44100.0;
    }

    // The track a tree is in, or an invalid tree.
    juce::ValueTree findTrackState (juce::ValueTree tree)
    {
        for (; tree.isValid(); tree = tree.getParent())
            if (te::TrackList::isTrack (tree))
                return tree;

        return {};
    }
} // namespace

//==============================================================================
int getFreezePointIndex (const te::AudioTrack& track)
{
    const auto& list = track.pluginList;

    for (int i = 0; i < list.size(); ++i)
    {
        auto* plugin = list[i];
        const auto isSharedContainer = [plugin]
        {
            auto* rack = dynamic_cast<te::RackInstance*> (plugin);
            return rack != nullptr && rack->type != nullptr && te::getRackInstancesInEditForType (*rack->type).size() > 1;
        };

        if (dynamic_cast<te::VolumeAndPanPlugin*> (plugin) != nullptr || dynamic_cast<te::AuxSendPlugin*> (plugin) != nullptr
            || dynamic_cast<te::FreezePointPlugin*> (plugin) != nullptr || plugin->getSidechainSourceID().isValid() || isSharedContainer())
            return i;
    }

    return list.size();
}

void freezeFromFile (te::AudioTrack& track, const juce::File& rendered)
{
    auto& engine = track.edit.engine;
    const auto frozenFile = te::TemporaryFileManager::getFreezeFileForTrack (track);
    frozenFile.getParentDirectory().createDirectory();
    te::AudioFile (engine, frozenFile).deleteFile(); // and what Tracktion read of an earlier one

    if (! rendered.moveFileTo (frozenFile))
        rendered.copyFileTo (frozenFile);

    engine.getAudioFileManager().forceFileUpdate (te::AudioFile (engine, frozenFile));

    // The freeze point, which Tracktion keeps a frozen track by, before the devices that stay live.
    auto& list = track.pluginList;
    const auto point = getFreezePointIndex (track);

    if (dynamic_cast<te::FreezePointPlugin*> (list[point]) == nullptr)
    {
        // Switched on already: a new plug-in switches itself on through the undo manager.
        auto state = te::FreezePointPlugin::create();
        state.setProperty (te::IDs::enabled, true, nullptr);
        auto freezePoint = track.edit.getPluginCache().getOrCreatePluginFor (state);
        const auto treeIndex = point < list.size() ? list.state.indexOf (list[point]->state) : -1;
        list.state.addChild (freezePoint->state, treeIndex, nullptr);
    }

    for (int i = 0; i < list.size(); ++i)
    {
        list[i]->state.setProperty (te::IDs::frozen, i < point, nullptr);

        // A frozen plug-in's window shuts: what it changed would no longer be heard.
        if (i < point && list[i]->windowState != nullptr)
            list[i]->windowState->closeWindowExplicitly();
    }

    setFrozenSwitch (track, true); // last: the FreezeKeeper takes what the file holds then
    track.changed();
}

void unfreeze (te::AudioTrack& track)
{
    setFrozenSwitch (track, false); // first: the FreezeKeeper lets go of it

    auto& list = track.pluginList;

    for (int i = list.size(); --i >= 0;)
    {
        if (dynamic_cast<te::FreezePointPlugin*> (list[i]) != nullptr)
            list.state.removeChild (list[i]->state, nullptr);
        else
            list[i]->state.setProperty (te::IDs::frozen, false, nullptr);
    }

    track.changed();
}

bool isFrozenDevice (const te::Plugin& plugin)
{
    auto* track = dynamic_cast<te::AudioTrack*> (plugin.getOwnerTrack());
    return track != nullptr && track->isFrozen (te::Track::individualFreeze) && plugin.isFrozen();
}

juce::Array<te::AudioTrack*> getFrozenTracks (const te::Edit& edit)
{
    juce::Array<te::AudioTrack*> frozen;

    for (auto* track : te::getAudioTracks (edit))
        if (track->isFrozen (te::Track::individualFreeze))
            frozen.add (track);

    return frozen;
}

//==============================================================================
juce::Array<te::EditItemID> takeFrozenTracks (juce::ValueTree& state, const juce::File& cacheFolder)
{
    juce::Array<te::EditItemID> taken;
    juce::Array<juce::ValueTree> tracks;

    const std::function<void (const juce::ValueTree&)> findTracks = [&] (const juce::ValueTree& parent)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (te::IDs::TRACK))
                tracks.add (child);

            if (te::TrackList::isTrack (child))
                findTracks (child); // a folder's tracks
        }
    };

    findTracks (state);

    for (auto track : tracks)
    {
        if (! (bool) track.getProperty (te::IDs::frozenIndividually, false))
            continue;

        const auto id = te::EditItemID::fromID (track);
        const auto file = cacheFolder.getChildFile ("trackFreeze_0_" + id.toString() + ".freeze"); // as TemporaryFileManager names it

        track.setProperty (te::IDs::frozenIndividually, false, nullptr);

        if (cacheFolder != juce::File() && file.existsAsFile())
        {
            taken.add (id);
            continue;
        }

        // Its file is gone: it opens unfrozen, without its freeze point.
        for (int i = track.getNumChildren(); --i >= 0;)
        {
            auto child = track.getChild (i);

            if (child.hasType (te::IDs::PLUGIN) && child[te::IDs::type].toString() == te::FreezePointPlugin::xmlTypeName)
                track.removeChild (i, nullptr);
            else if (child.hasType (te::IDs::PLUGIN))
                child.setProperty (te::IDs::frozen, false, nullptr);
        }
    }

    return taken;
}

void restoreFrozenTracks (te::Edit& edit, const juce::Array<te::EditItemID>& taken)
{
    for (const auto& id : taken)
        if (auto* track = dynamic_cast<te::AudioTrack*> (te::findTrackForID (edit, id)))
            setFrozenSwitch (*track, true);
}

void purgeCache (te::Edit& edit)
{
    te::TemporaryFileManager::purgeOrphanFreezeAndProxyFiles (edit);

    // Tracktion leaves the frozen files of tracks that are gone.
    const auto prefix = juce::String ("trackFreeze_0_");

    for (const auto& entry : juce::RangedDirectoryIterator (edit.getTempDirectory (false), false, prefix + "*.freeze"))
    {
        const auto id = te::EditItemID::fromVar (entry.getFile().getFileNameWithoutExtension().substring (prefix.length()));
        auto* track = dynamic_cast<te::AudioTrack*> (te::findTrackForID (edit, id));

        if (track == nullptr || ! track->isFrozen (te::Track::individualFreeze))
            te::AudioFile (edit.engine, entry.getFile()).deleteFile();
    }
}

//==============================================================================
FreezeKeeper::FreezeKeeper (te::Edit& e)
    : edit (&e),
      state (e.state)
{
    for (auto* track : getFrozenTracks (e))
        frozen[track->itemID] = copyWhatTheFreezeHolds (*track);

    state.addListener (this);
}

FreezeKeeper::~FreezeKeeper()
{
    state.removeListener (this);
}

void FreezeKeeper::check (juce::ValueTree& changed)
{
    auto* e = edit.get(); // gone first if the project context outlives it

    if (checking || e == nullptr)
        return;

    const juce::ScopedValueSetter<bool> busy (checking, true);
    const auto trackState = findTrackState (changed);

    // A track frozen or unfrozen, or tracks added, removed or moved (undo brings a frozen track back frozen): the tracks
    // that are frozen now, each with what its file holds, kept from before if it was frozen then.
    if (! trackState.isValid() || trackState == changed)
    {
        std::map<te::EditItemID, juce::ValueTree> now;

        for (auto* track : getFrozenTracks (*e))
        {
            const auto known = frozen.find (track->itemID);
            now[track->itemID] = known != frozen.end() ? known->second : copyWhatTheFreezeHolds (*track);
        }

        frozen = std::move (now);

        if (! trackState.isValid())
            return;
    }

    const auto id = te::EditItemID::fromID (trackState);
    const auto known = frozen.find (id);

    if (known == frozen.end())
        return;

    // What the freeze holds changed: the track plays it live again.
    if (auto* track = dynamic_cast<te::AudioTrack*> (te::findTrackForID (*e, id));
        track != nullptr && ! holdsWhatWasFrozen (*track, known->second))
    {
        frozen.erase (known);
        unfreeze (*track);
    }
}

//==============================================================================
struct TrackRender::Impl
{
    virtual ~Impl() = default;
    virtual float getProgress() const = 0;
    virtual juce::String getTitle() const = 0;
};

namespace
{
    // What a render changes in the Edit while it renders a track, and puts back after: the devices from the end of what it
    // renders on are off, the track isn't muted and nothing is soloed, as Tracktion's own freeze has it; and the tracks
    // that feed the track's sidechains and sends play muted. None of it is an undo step or a change.
    class RenderSetup
    {
    public:
        RenderSetup (te::AudioTrack& t, int firstDeviceLeftOut, const juce::Array<te::EditItemID>& tracksToMute)
            : track (t),
              wasChanged (t.edit.hasChangedSinceSaved()),
              wasMuted (t.isMuted (false)),
              unsoloer (std::make_unique<te::FreezePointPlugin::ScopedTrackUnsoloer> (t.edit))
        {
            auto& list = track.pluginList;

            for (int i = firstDeviceLeftOut; i < list.size(); ++i)
            {
                if (list[i]->isEnabled())
                {
                    leftOut.add (list[i]);
                    list[i]->state.setProperty (te::IDs::enabled, false, nullptr);
                }
            }

            if (wasMuted)
                track.setMute (false);

            if (! tracksToMute.isEmpty())
                muter = std::make_unique<te::ScopedTrackMuter> (track.edit, tracksToMute);
        }

        ~RenderSetup()
        {
            muter = nullptr;
            unsoloer = nullptr;

            if (wasMuted)
                track.setMute (true);

            for (auto& plugin : leftOut)
                plugin->state.setProperty (te::IDs::enabled, true, nullptr);

            if (! wasChanged)
                track.edit.resetChangedStatus();
        }

    private:
        te::AudioTrack& track;
        const bool wasChanged, wasMuted;
        juce::ReferenceCountedArray<te::Plugin> leftOut;
        std::unique_ptr<te::FreezePointPlugin::ScopedTrackUnsoloer> unsoloer;
        std::unique_ptr<te::ScopedTrackMuter> muter;

        JUCE_DECLARE_NON_COPYABLE (RenderSetup)
    };

    // A track rendered to a temporary file in the background (Tracktion's EditRenderer), then made something of on the
    // message thread: frozen, or bounced.
    class RenderOfTrack : public TrackRender::Impl
    {
    public:
        RenderOfTrack (ProjectContext& c, te::AudioTrack& t, juce::String title, std::function<void (const juce::Result&)> callback)
            : context (c),
              edit (t.edit),
              trackID (t.itemID),
              renderTitle (std::move (title)),
              onFinished (std::move (callback)),
              alive (std::make_shared<RenderOfTrack*> (this))
        {
        }

        ~RenderOfTrack() override
        {
            alive = nullptr; // no callback from here on
            endRender();

            if (temporary.existsAsFile())
                temporary.deleteFile();
        }

        float getProgress() const override { return handle != nullptr ? handle->getProgress() : 0.0f; }
        juce::String getTitle() const override { return renderTitle; }
        bool hasStarted() const noexcept { return handle != nullptr; }

    protected:
        ProjectContext& context;
        te::Edit& edit;
        const te::EditItemID trackID;
        juce::File temporary; // what the render writes, until it is used

        // Starts the render, with the setup held while it runs.
        void start (te::Renderer::Parameters params, std::unique_ptr<RenderSetup> heldWhileRendering)
        {
            if (edit.getTransport().isPlaying())
                stopAndReturn (edit);

            restorePlayback = edit.getTransport().isPlayContextActive(); // the render frees it
            setup = std::move (heldWhileRendering);
            temporary = params.destFile;
            temporary.getParentDirectory().createDirectory();

            handle = te::EditRenderer::render (
                std::move (params),
                [weakAlive = std::weak_ptr<RenderOfTrack*> (alive)] (tl::expected<juce::File, std::string> rendered)
                {
                    const auto result = rendered.has_value() ? juce::Result::ok() : juce::Result::fail (juce::String (rendered.error()));
                    juce::MessageManager::callAsync (
                        [weakAlive, result]
                        {
                            if (const auto locked = weakAlive.lock())
                                (*locked)->finish (result);
                        });
                });
        }

        te::AudioTrack* findTrack() const { return dynamic_cast<te::AudioTrack*> (te::findTrackForID (edit, trackID)); }

        // What a rendered file becomes, on the message thread once the render has finished and the Edit is as it was.
        virtual juce::Result use (te::AudioTrack&, const juce::File& rendered) = 0;

    private:
        const juce::String renderTitle;
        const std::function<void (const juce::Result&)> onFinished;
        std::shared_ptr<RenderOfTrack*> alive; // what a callback on its way holds on to
        std::shared_ptr<te::EditRenderer::Handle> handle;
        std::unique_ptr<RenderSetup> setup;
        bool restorePlayback = false;

        void endRender()
        {
            handle.reset(); // waits for the render thread
            setup = nullptr;

            if (std::exchange (restorePlayback, false))
                edit.getTransport().ensureContextAllocated(); // live MIDI and the meters play again
        }

        void finish (juce::Result result)
        {
            endRender();

            if (result.wasOk())
            {
                if (auto* track = findTrack(); track != nullptr && temporary.existsAsFile())
                    result = use (*track, temporary);
                else
                    result = juce::Result::fail ("The track is gone, or Phazite couldn't render it.");
            }

            if (temporary.existsAsFile())
                temporary.deleteFile();

            if (const auto callback = onFinished; callback != nullptr)
                callback (result); // last: it may delete this
        }

        JUCE_DECLARE_NON_COPYABLE (RenderOfTrack)
    };

    //==============================================================================
    // Freezing: the track up to its freeze point, as Tracktion's own freeze renders it (tracktion_AudioTrack.cpp:
    // freezeTrack()), at the audio device's rate, in Tracktion's frozen-file format.
    class FreezeRender final : public RenderOfTrack
    {
    public:
        FreezeRender (ProjectContext& c, te::AudioTrack& track, std::function<void (const juce::Result&)> callback)
            : RenderOfTrack (c, track, "Freezing " + track.getName(), std::move (callback))
        {
            auto& devices = edit.engine.getDeviceManager();
            juce::BigInteger trackBit;
            trackBit.setBit (track.getIndexInEditTrackList());
            juce::Array<te::EditItemID> trackIDs { track.itemID };

            for (auto* input : track.getInputTracks())
                trackIDs.addIfNotAlreadyThere (input->itemID);

            te::Renderer::Parameters params (edit);
            params.tracksToDo = trackBit;
            params.destFile =
                te::TemporaryFileManager::getFreezeFileForTrack (track).getSiblingFile ("rendering.freeze").getNonexistentSibling (false);
            params.audioFormat = edit.engine.getAudioFileFormatManager().getFrozenFileFormat();
            params.bitDepth = 32;
            params.blockSizeForAudio = devices.getBlockSize() > 0 ? devices.getBlockSize() : 512;
            params.sampleRateForAudio = getRenderSampleRate (edit);
            params.time = { te::TimePosition(), track.getLengthIncludingInputTracks() };
            params.endAllowance = te::RenderOptions::findEndAllowance (edit, &trackIDs, nullptr);
            params.canRenderInMono = true;
            params.mustRenderInMono = false;
            params.usePlugins = true;
            params.useMasterPlugins = false;

            start (std::move (params), std::make_unique<RenderSetup> (track, getFreezePointIndex (track), juce::Array<te::EditItemID>()));
        }

    private:
        juce::Result use (te::AudioTrack& track, const juce::File& rendered) override
        {
            freezeFromFile (track, rendered);
            return track.isFrozen (te::Track::individualFreeze) ? juce::Result::ok()
                                                                : juce::Result::fail ("Phazite couldn't keep the frozen file.");
        }
    };

    //==============================================================================
    // Bouncing: the track before its fader, with the tracks that feed its sidechains playing muted, from the song's start to
    // the end of its last clip and its tail, into a WAV file of floating-point samples in the project's Audio folder.
    class BounceRender final : public RenderOfTrack
    {
    public:
        BounceRender (ProjectContext& c, te::AudioTrack& track, std::function<void (const juce::Result&)> callback)
            : RenderOfTrack (c, track, "Bouncing " + track.getName(), std::move (callback)),
              name (track.getName() + " (Bounce)")
        {
            const auto folder = edit.engine.getEngineBehaviour().getDefaultFolderForAudioRecordings (edit);
            target = folder.getNonexistentChildFile (juce::File::createLegalFileName (name), ".wav", false);

            te::RenderSpecification spec;
            spec.tracks = { track.itemID };
            spec.includeSourceTracks = true;
            spec.time = te::TimeRange (te::TimePosition(), track.getLengthIncludingInputTracks());
            spec.includeTails = true;
            spec.destination =
                target.getSiblingFile (target.getFileNameWithoutExtension() + " (rendering).wav").getNonexistentSibling (false);
            spec.format = te::RenderFormat::wav;
            spec.sampleRate = getRenderSampleRate (edit);
            spec.bitDepth = 32;
            spec.channelLayout = "stereo";
            spec.useMasterPlugins = false;

            auto job = te::createRenderJob (edit, spec);

            if (! job.has_value())
                return;

            const auto fader = track.pluginList.indexOf (track.getVolumePlugin());
            start (std::move (job->params),
                   std::make_unique<RenderSetup> (track, fader >= 0 ? fader : track.pluginList.size(), job->tracksToMute));
        }

    private:
        const juce::String name;
        juce::File target;

        juce::Result use (te::AudioTrack& track, const juce::File& rendered) override
        {
            if (! rendered.moveFileTo (target))
                return juce::Result::fail ("Phazite couldn't write " + target.getFullPathName() + ".");

            te::AudioFile file (edit.engine, target);
            const auto length = te::TimeDuration::fromSeconds (file.getLength());

            if (length <= te::TimeDuration())
                return juce::Result::fail ("Phazite couldn't read the bounced file back.");

            // One undo step: the new track under the original, with the original's color and mixer settings, and the
            // original muted and collapsed.
            const UndoStep step (edit, "Bounce in Place");
            auto bounced = edit.insertNewAudioTrack (getInsertPointAfter (track), nullptr);

            if (bounced == nullptr)
                return juce::Result::fail ("Phazite couldn't add a track for the bounce.");

            bounced->setName (name);
            setTrackColours ({ bounced.get() }, getTrackColour (track, getPlaylistTracks (edit).indexOf (&track)));

            if (auto* from = track.getVolumePlugin(); from != nullptr)
            {
                if (auto* to = bounced->getVolumePlugin(); to != nullptr)
                {
                    to->setVolumeDb (from->getVolumeDb());
                    to->setPan (from->getPan());
                }
            }

            for (auto* send : getSends (track))
                if (auto* copy = addSend (*bounced, send->getBusNumber(), isPreFader (*send)))
                    copy->setGainDb (send->getGainDb());

            if (bounced->insertWaveClip (name, target, { te::TimeRange (te::TimePosition(), length) }, false) == nullptr)
                return juce::Result::fail ("Phazite couldn't put the bounced file on its track.");

            track.state.setProperty (te::IDs::mute, true, &edit.getUndoManager());
            setCollapsed (track, true);
            getSelection (context).selectOnly (bounced.get());
            return juce::Result::ok();
        }
    };

    te::AudioTrack* getSelectedAudioTrack (const ProjectContext& context)
    {
        const auto selected = getSelectedTracks (getSelection (context));
        return selected.isEmpty() ? nullptr : dynamic_cast<te::AudioTrack*> (selected.getFirst());
    }

    TrackID getSelectedTrackID (const ProjectContext& context)
    {
        const auto selected = getSelectedTracks (getSelection (context));
        return selected.isEmpty() ? TrackID() : getTrackID (*selected.getFirst());
    }

    // Starts a render in the host, or tells the host why it can't.
    void runOrExplain (TrackRenderHost& host, const juce::String& title, const juce::String& whyNot,
                       const std::function<std::unique_ptr<TrackRender> (std::function<void (const juce::Result&)>)>& start)
    {
        if (whyNot.isNotEmpty())
        {
            host.renderFinished (title, juce::Result::fail (whyNot));
            return;
        }

        auto render = start ([&host, title] (const juce::Result& result) { host.renderFinished (title, result); });

        if (render != nullptr)
            host.runRender (std::move (render));
        else
            host.renderFinished (title, juce::Result::fail ("Phazite couldn't start rendering."));
    }
} // namespace

TrackRender::TrackRender (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

TrackRender::~TrackRender() = default;

float TrackRender::getProgress() const
{
    return impl->getProgress();
}

juce::String TrackRender::getTitle() const
{
    return impl->getTitle();
}

//==============================================================================
bool isFrozen (const ProjectContext& context, TrackID id)
{
    auto* track = findAudioTrack (context, id);
    return track != nullptr && track->isFrozen (te::Track::individualFreeze);
}

bool isFrozenDevice (const ProjectContext& context, DeviceID id)
{
    auto* edit = getEdit (context);
    const auto plugin = edit != nullptr && id.isValid() ? te::findPluginForID (*edit, toTracktion (id)) : te::Plugin::Ptr();
    return plugin != nullptr && isFrozenDevice (*plugin);
}

juce::String getWhyNotFreeze (const ProjectContext& context, TrackID id)
{
    auto* edit = getEdit (context);
    auto* track = edit != nullptr && id.isValid() ? te::findTrackForID (*edit, toTracktion (id)) : nullptr;

    if (auto* audioTrack = dynamic_cast<te::AudioTrack*> (track);
        audioTrack != nullptr && audioTrack->isFrozen (te::Track::individualFreeze))
        return audioTrack->getName() + " is frozen already.";

    return getWhyNotRender (track, "freeze", "frozen");
}

juce::String getWhyNotBounce (const ProjectContext& context, TrackID id)
{
    auto* edit = getEdit (context);
    return getWhyNotRender (edit != nullptr && id.isValid() ? te::findTrackForID (*edit, toTracktion (id)) : nullptr, "bounce", "bounced");
}

std::unique_ptr<TrackRender> startFreeze (ProjectContext& context, TrackID id, std::function<void (const juce::Result&)> onFinished)
{
    auto* track = findAudioTrack (context, id);

    if (track == nullptr || getWhyNotFreeze (context, id).isNotEmpty())
        return {};

    auto render = std::make_unique<FreezeRender> (context, *track, std::move (onFinished));
    return render->hasStarted() ? std::make_unique<TrackRender> (std::move (render)) : nullptr;
}

std::unique_ptr<TrackRender> startBounceInPlace (ProjectContext& context, TrackID id, std::function<void (const juce::Result&)> onFinished)
{
    auto* track = findAudioTrack (context, id);

    if (track == nullptr || getWhyNotBounce (context, id).isNotEmpty())
        return {};

    auto render = std::make_unique<BounceRender> (context, *track, std::move (onFinished));
    return render->hasStarted() ? std::make_unique<TrackRender> (std::move (render)) : nullptr;
}

void unfreezeTrack (ProjectContext& context, TrackID id)
{
    if (auto* track = findAudioTrack (context, id); track != nullptr && track->isFrozen (te::Track::individualFreeze))
        unfreeze (*track);
}

//==============================================================================
void toggleFreeze (ProjectContext& context, TrackRenderHost& host)
{
    const auto id = getSelectedTrackID (context);

    if (isFrozen (context, id))
    {
        unfreezeTrack (context, id);
        return;
    }

    auto* track = getSelectedAudioTrack (context);
    const auto title = "Freezing " + (track != nullptr ? track->getName() : juce::String ("the track"));
    runOrExplain (host, title, getWhyNotFreeze (context, id),
                  [&context, id] (std::function<void (const juce::Result&)> done) { return startFreeze (context, id, std::move (done)); });
}

bool canToggleFreeze (const ProjectContext& context)
{
    const auto id = getSelectedTrackID (context);
    return isFrozen (context, id) || getWhyNotFreeze (context, id).isEmpty();
}

bool isSelectedTrackFrozen (const ProjectContext& context)
{
    return isFrozen (context, getSelectedTrackID (context));
}

juce::String getFreezeCommandName (const ProjectContext& context)
{
    return isSelectedTrackFrozen (context) ? "Unfreeze Track" : "Freeze Track";
}

void bounceSelectedTrack (ProjectContext& context, TrackRenderHost& host)
{
    const auto id = getSelectedTrackID (context);
    auto* track = getSelectedAudioTrack (context);
    const auto title = "Bouncing " + (track != nullptr ? track->getName() : juce::String ("the track"));
    runOrExplain (host, title, getWhyNotBounce (context, id), [&context, id] (std::function<void (const juce::Result&)> done)
                  { return startBounceInPlace (context, id, std::move (done)); });
}

bool canBounceSelectedTrack (const ProjectContext& context)
{
    return getWhyNotBounce (context, getSelectedTrackID (context)).isEmpty();
}

void addFreezeCommands (CommandRegistry& registry, ProjectContext& context, TrackRenderHost& host)
{
    registry.add ({ .id = "track.freeze",
                    .name = "Freeze Track",
                    .category = "Track",
                    .description = "Plays the selected track from a file of its sound up to its fader, so that its instrument and "
                                   "effects take no CPU; again to play it live. Changing what it froze unfreezes it.",
                    .synonyms = { "unfreeze", "render", "cpu", "commit", "snowflake" },
                    .perform = [&context, &host] { toggleFreeze (context, host); },
                    .isEnabled = [&context] { return canToggleFreeze (context); },
                    .isTicked = [&context] { return isSelectedTrackFrozen (context); },
                    .currentName = [&context] { return getFreezeCommandName (context); } });

    registry.add ({ .id = "track.bounceInPlace",
                    .name = "Bounce in Place",
                    .category = "Track",
                    .description = "Renders the selected track through its instrument and effects onto a new audio track under it, "
                                   "and mutes the original.",
                    .synonyms = { "render", "consolidate", "resample", "print", "audio", "flatten", "commit" },
                    .perform = [&context, &host] { bounceSelectedTrack (context, host); },
                    .isEnabled = [&context] { return canBounceSelectedTrack (context); } });
}
} // namespace phazite
