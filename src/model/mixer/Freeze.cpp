// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Freeze.h"
#include "FreezeModel.h"
#include "internal/Mixer.h"
#include "internal/Sidechain.h"
#include "model/arrangement/internal/AudioClips.h"
#include "model/arrangement/internal/ClipRenders.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/base/internal/Schema.h"
#include "model/commands/CommandRegistry.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/AudioIO.h"
#include "model/playback/internal/OfflineRender.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/AudioRecorder.h"
#include "model/transport/internal/Playback.h"
#include "model/transport/internal/Recording.h"
#include "model/transport/internal/Tempo.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <set>
#include <utility>

namespace phazite
{
namespace
{
    constexpr const char* frozenFilePrefix = "Frozen ";

    bool isTrackState (const juce::ValueTree& state)
    {
        return state.hasType (schema::TRACK);
    }

    // A track that can be frozen or bounced: one of the playlist's with sound of its own, not a folder or a return.
    bool canRender (const juce::ValueTree& state)
    {
        return isTrackState (state) && ! isFolderState (state) && ! isReturnState (state);
    }

    // Whether a device, or a device in a container, listens to another track through its sidechain.
    bool listensToSidechain (const Song& song, const juce::ValueTree& device)
    {
        if (getSidechainSource (song, getDeviceID (device)).isValid())
            return true;

        const auto inside = getDeviceStates (device);
        return std::any_of (inside.begin(), inside.end(),
                            [&song] (const juce::ValueTree& state) { return getSidechainSource (song, getDeviceID (state)).isValid(); });
    }

    // Why a track can't be rendered on its own, frozen or bounced (what it would be, and what is done to it); empty if it
    // can.
    juce::String getWhyNotRender (const Song* song, TrackID track, const juce::String& verb, const juce::String& done)
    {
        const auto state = song != nullptr ? findTrackState (*song, track) : juce::ValueTree();

        if (! isTrackState (state))
            return "Select a track to " + verb + ".";

        if (isFolderState (state))
            return "Folders and buses can't be " + done + ": their sound is their tracks'.";

        if (isReturnState (state))
            return "Returns can't be " + done + ": their sound comes from the tracks that send to them.";

        if (isRecording (*song))
            return "Stop recording first.";

        if (getClipStates (state).empty())
            return getTrackName (*song, track) + " has nothing to " + verb + ": it has no clips.";

        return {};
    }

    // Properties written into a device's element without its sound changing: where its window was, and a plug-in's own
    // state, which every save stores again (model/plugins/internal/PluginInstances.h).
    bool isIgnoredProperty (const juce::Identifier& property)
    {
        return schema::isDeviceViewProperty (property) || property == schema::pluginState;
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

    // What a frozen track's file holds, as the song has it: the track's clips, its frozen devices, its modulators and
    // macros, which can move them, the frozen devices' automation (of the devices in its frozen containers too), and the
    // song's tempos, which put its notes and clips where they are in seconds.
    std::vector<juce::ValueTree> getWhatTheFreezeHolds (const Song& song, TrackID track)
    {
        std::vector<juce::ValueTree> parts;
        const auto state = findTrackState (song, track);

        for (const auto& clip : getClipStates (state))
            parts.push_back (clip);

        std::set<DeviceID> devices;

        for (const auto id : getFrozenDevices (song, track))
        {
            const auto device = findDeviceState (song, id);
            parts.push_back (device);
            devices.insert (id);

            for (const auto& inside : getDeviceStates (device))
                devices.insert (getDeviceID (inside));
        }

        for (const auto& child : state)
            if (child.hasType (schema::MODULATOR) || child.hasType (schema::MACRO))
                parts.push_back (child);

        for (const auto& curve : song.getState().getChildWithName (schema::AUTOMATION))
            if (devices.contains (DeviceID::fromRaw ((std::uint64_t) (juce::int64) curve[schema::curveDevice])))
                parts.push_back (curve);

        for (const auto& tempo : song.getState().getChildWithName (schema::TEMPOMAP))
            if (tempo.hasType (schema::TEMPO))
                parts.push_back (tempo);

        return parts;
    }

    juce::ValueTree copyWhatTheFreezeHolds (const Song& song, TrackID track)
    {
        juce::ValueTree copy ("FROZEN");

        for (const auto& part : getWhatTheFreezeHolds (song, track))
            copy.appendChild (part.createCopy(), nullptr);

        return copy;
    }

    bool holdsWhatWasFrozen (const Song& song, TrackID track, const juce::ValueTree& frozen)
    {
        const auto parts = getWhatTheFreezeHolds (song, track);

        if ((int) parts.size() != frozen.getNumChildren())
            return false;

        for (int i = 0; i < (int) parts.size(); ++i)
            if (! isSameSound (parts[(size_t) i], frozen.getChild (i)))
                return false;

        return true;
    }

    // The element of a type a tree is in (itself, if it is one), or an invalid one.
    juce::ValueTree findEnclosing (juce::ValueTree tree, const juce::Identifier& type)
    {
        for (; tree.isValid(); tree = tree.getParent())
            if (tree.hasType (type))
                return tree;

        return {};
    }

    // Where a track's sound ends in the song, before its tails: the end of its last clip, or of the last clip of a track
    // whose sound reaches it (the source of one of its sidechains), which plays through it in its render.
    BeatPosition getTrackEnd (const Song& song, TrackID track)
    {
        BeatPosition end;

        for (const auto& other : getTrackStates (song))
        {
            if (const auto id = getTrackID (other); id != track)
                if (const auto fed = getTracksFedBy (song, id); std::find (fed.begin(), fed.end(), track) == fed.end())
                    continue;

            for (const auto& clip : getClipStates (other))
                end = std::max (end, getClipRange (song, clip).getEnd());
        }

        return end;
    }

    // The rate a track renders at, and its blocks: the audio device's, as its file plays alongside what it plays
    // (model/engine/internal/AudioIO.h), or 44.1 kHz in blocks of 256 without one.
    struct RenderFormat
    {
        double sampleRate = 44100.0;
        int blockSize = 256;
    };

    RenderFormat getRenderFormat (const PlaybackSession* session)
    {
        RenderFormat format;

        if (auto* device = session != nullptr ? session->getAudioIO().getDevices().getCurrentAudioDevice() : nullptr)
        {
            if (device->getCurrentSampleRate() > 7000.0)
                format.sampleRate = device->getCurrentSampleRate();

            if (device->getCurrentBufferSizeSamples() > 0)
                format.blockSize = device->getCurrentBufferSizeSamples();
        }

        return format;
    }

    // A file's length in seconds, as JUCE's formats read it; 0 for one they can't.
    double getFileSeconds (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        return reader != nullptr && reader->sampleRate > 0.0 ? (double) reader->lengthInSamples / reader->sampleRate : 0.0;
    }
} // namespace

//==============================================================================
std::vector<DeviceID> getFrozenDevices (const Song& song, TrackID track)
{
    std::vector<DeviceID> devices;
    const auto state = findTrackState (song, track);

    if (! canRender (state))
        return devices;

    for (const auto& device : getDeviceStates (state))
    {
        if (listensToSidechain (song, device))
            break;

        devices.push_back (getDeviceID (device));
    }

    return devices;
}

juce::File getFrozenFile (const Song& song, TrackID track)
{
    return song.getCacheFolder().getChildFile (frozenFilePrefix + juce::String ((juce::int64) track.getRaw()) + ".wav");
}

void freezeFromFile (Song& song, TrackID track, const juce::File& rendered)
{
    auto state = findTrackState (song, track);

    if (! canRender (state))
        return;

    // The rendered file takes the place of an earlier one.
    const auto file = getFrozenFile (song, track);
    file.getParentDirectory().createDirectory();

    if (rendered != file && ! rendered.moveFileTo (file) && ! rendered.copyFileTo (file))
        return;

    state.setProperty (schema::trackFrozen, true, nullptr);
}

void unfreeze (Song& song, TrackID track)
{
    if (auto state = findTrackState (song, track); isTrackState (state))
        state.removeProperty (schema::trackFrozen, nullptr);
}

bool isFrozenDevice (const Song& song, DeviceID id)
{
    auto device = findDeviceState (song, id);

    // A device in a container is frozen with its container.
    if (isContainerState (device.getParent()))
        device = device.getParent();

    const auto track = device.getParent();

    if (! isTrackState (track) || ! (bool) track[schema::trackFrozen])
        return false;

    const auto frozen = getFrozenDevices (song, getTrackID (track));
    return std::find (frozen.begin(), frozen.end(), getDeviceID (device)) != frozen.end();
}

std::vector<TrackID> getFrozenTracks (const Song& song)
{
    std::vector<TrackID> frozen;

    for (const auto& state : getTrackStates (song))
        if ((bool) state[schema::trackFrozen])
            frozen.push_back (getTrackID (state));

    return frozen;
}

//==============================================================================
void forgetMissingFrozenFiles (Song& song)
{
    for (auto state : getTrackStates (song))
        if ((bool) state[schema::trackFrozen] && ! getFrozenFile (song, getTrackID (state)).existsAsFile())
            state.removeProperty (schema::trackFrozen, nullptr);
}

void purgeCache (Song& song)
{
    purgeClipRenders (song);

    // The frozen files of tracks that aren't frozen, or are gone, and what a freeze left half rendered.
    std::set<juce::String> used;

    for (const auto track : getFrozenTracks (song))
        used.insert (getFrozenFile (song, track).getFileName());

    if (const auto folder = song.getCacheFolder(); folder.isDirectory())
        for (const auto& entry : juce::RangedDirectoryIterator (folder, false, juce::String (frozenFilePrefix) + "*.wav"))
            if (! used.contains (entry.getFile().getFileName()))
                entry.getFile().deleteFile();
}

//==============================================================================
FreezeKeeper::FreezeKeeper (Song& s)
    : song (s),
      state (s.getState())
{
    takeFrozenTracks();
    state.addListener (this);
}

FreezeKeeper::~FreezeKeeper()
{
    state.removeListener (this);
}

void FreezeKeeper::takeFrozenTracks()
{
    // The tracks that are frozen now, each with what its file holds, kept from before if it was frozen then.
    std::map<TrackID, juce::ValueTree> now;

    for (const auto track : getFrozenTracks (song))
    {
        const auto known = frozen.find (track);
        now[track] = known != frozen.end() ? known->second : copyWhatTheFreezeHolds (song, track);
    }

    frozen = std::move (now);
}

void FreezeKeeper::compare (TrackID track)
{
    // What the freeze holds changed: the track plays it live again.
    if (const auto known = frozen.find (track); known != frozen.end() && ! holdsWhatWasFrozen (song, track, known->second))
    {
        frozen.erase (known);
        unfreeze (song, track);
    }
}

void FreezeKeeper::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // How tracks show and where windows were change nothing that plays.
    if (! schema::isTrackViewProperty (property) && ! isIgnoredProperty (property))
        check (tree);
}

void FreezeKeeper::check (const juce::ValueTree& changed)
{
    if (checking)
        return;

    const juce::ScopedValueSetter<bool> busy (checking, true);
    const auto track = findEnclosing (changed, schema::TRACK);

    // A track frozen or unfrozen, or tracks added, removed or moved (an undo brings a frozen track back frozen).
    if (changed == state || changed.hasType (schema::TRACKS) || (track.isValid() && changed == track))
        takeFrozenTracks();

    if (track.isValid())
    {
        compare (getTrackID (track));
        return;
    }

    // A curve of a device whose sound no frozen file holds changes none of them.
    if (const auto curve = findEnclosing (changed, schema::CURVE);
        curve.isValid() && ! isFrozenDevice (song, DeviceID::fromRaw ((std::uint64_t) (juce::int64) curve[schema::curveDevice])))
        return;

    // The song's automation or its tempos, which any frozen track's file may hold.
    if (changed == state || findEnclosing (changed, schema::AUTOMATION).isValid() || findEnclosing (changed, schema::TEMPOMAP).isValid())
    {
        std::vector<TrackID> tracks;

        for (const auto& entry : frozen)
            tracks.push_back (entry.first);

        for (const auto frozenTrack : tracks)
            compare (frozenTrack);
    }
}

//==============================================================================
namespace
{
    // A track rendered by Phazite's engine in the background (model/playback/internal/OfflineRender.h), before its fader,
    // through its devices or some of them, what feeds it playing only through it, from the song's start to the end of its
    // last clip and on as long as its tails sound, into a WAV file of 32-bit floats; then made something of on the message
    // thread: frozen, or bounced. Nothing of the song changes meanwhile.
    class RenderOfTrack : public TrackRender::Impl
    {
    public:
        RenderOfTrack (ProjectContext& c, Song& s, TrackID t, juce::String title, std::function<void (const juce::Result&)> callback)
            : context (c),
              song (&s),
              track (t),
              renderTitle (std::move (title)),
              onFinished (std::move (callback)),
              alive (std::make_shared<RenderOfTrack*> (this))
        {
        }

        ~RenderOfTrack() override
        {
            alive = nullptr; // no callback from here on
            render = nullptr;

            if (rendered.existsAsFile())
                rendered.deleteFile();
        }

        float getProgress() const override { return render != nullptr ? render->getProgress() : 0.0f; }
        juce::String getTitle() const override { return renderTitle; }
        bool hasStarted() const noexcept { return render != nullptr; }

    protected:
        ProjectContext& context;
        const Song::Ptr song;
        const TrackID track;
        juce::File rendered; // what the engine plays into, used once it is complete

        // Starts the render: of the track, with so much of its devices (all of them for none), at the audio device's rate.
        void start (std::optional<std::set<std::int64_t>> devices)
        {
            if (isSongPlaying (*song))
                stopAndReturn (*song);

            rendered.getParentDirectory().createDirectory();
            renderMissingClipAudio (*song); // the engine plays a clip whose render isn't there silent

            auto* session = findSession (*song);
            const auto format = getRenderFormat (session);
            const OfflineRender::Job job { rendered, toSeconds (*song, getTrackEnd (*song, track)), tailSeconds,
                                           SongPlayer::RenderOf { (std::int64_t) track.getRaw(), true, std::move (devices) } };
            const OfflineRender::Setup setup {
                .session = session,
                .makePlayer = [played = song, session]
                { return std::make_unique<SongPlayer> (*played, session != nullptr ? &session->getPlugins() : nullptr); },
                .numThreads = session != nullptr ? session->getHost().getNumThreads() : 1,
                .sampleRate = format.sampleRate,
                .blockSize = format.blockSize
            };

            render = std::make_unique<OfflineRender> (setup, std::vector { job }, OfflineRender::Rendered(),
                                                      [weakAlive = std::weak_ptr<RenderOfTrack*> (alive)] (const juce::Result& result)
                                                      {
                                                          juce::MessageManager::callAsync (
                                                              [weakAlive, result]
                                                              {
                                                                  if (const auto locked = weakAlive.lock())
                                                                      (*locked)->finish (result);
                                                              });
                                                      });
        }

        // What the rendered file becomes, on the message thread once the render has finished.
        virtual juce::Result use (const juce::File&) = 0;

    private:
        // How long a track's tails may sound after its last clip, at most.
        static constexpr double tailSeconds = 10.0;

        const juce::String renderTitle;
        const std::function<void (const juce::Result&)> onFinished;
        std::shared_ptr<RenderOfTrack*> alive; // what a callback on its way holds on to
        std::unique_ptr<OfflineRender> render;

        void finish (juce::Result result)
        {
            render = nullptr; // the project plays its plug-ins again

            if (result.wasOk())
            {
                if (canRender (findTrackState (*song, track)) && rendered.existsAsFile())
                    result = use (rendered);
                else
                    result = juce::Result::fail ("The track is gone, or Phazite couldn't render it.");
            }

            if (rendered.existsAsFile())
                rendered.deleteFile();

            if (const auto callback = onFinished; callback != nullptr)
                callback (result); // last: it may delete this
        }

        JUCE_DECLARE_NON_COPYABLE (RenderOfTrack)
    };

    //==============================================================================
    // Freezing: the track through its frozen devices (getFrozenDevices()), at the audio device's rate, into the file that
    // becomes its frozen file.
    class FreezeRender final : public RenderOfTrack
    {
    public:
        FreezeRender (ProjectContext& c, Song& s, TrackID t, std::function<void (const juce::Result&)> callback)
            : RenderOfTrack (c, s, t, "Freezing " + getTrackName (s, t), std::move (callback))
        {
            const auto frozenFile = getFrozenFile (s, t);
            rendered =
                frozenFile.getSiblingFile (frozenFile.getFileNameWithoutExtension() + " (rendering).wav").getNonexistentSibling (false);

            std::set<std::int64_t> frozen;

            for (const auto device : getFrozenDevices (s, t))
                frozen.insert ((std::int64_t) device.getRaw());

            start (std::move (frozen));
        }

    private:
        juce::Result use (const juce::File& file) override
        {
            freezeFromFile (*song, track, file);
            return (bool) findTrackState (*song, track)[schema::trackFrozen]
                       ? juce::Result::ok()
                       : juce::Result::fail ("Phazite couldn't keep the frozen file.");
        }
    };

    //==============================================================================
    // Bouncing: the track before its fader, through all its devices, into a WAV file of floating-point samples in the
    // song's recordings folder (its project's Audio folder).
    class BounceRender final : public RenderOfTrack
    {
    public:
        BounceRender (ProjectContext& c, Song& s, TrackID t, std::function<void (const juce::Result&)> callback)
            : RenderOfTrack (c, s, t, "Bouncing " + getTrackName (s, t), std::move (callback)),
              name (getTrackName (s, t) + " (Bounce)")
        {
            target = getRecordingsFolder (s).getNonexistentChildFile (juce::File::createLegalFileName (name), ".wav", false);
            rendered = target.getSiblingFile (target.getFileNameWithoutExtension() + " (rendering).wav").getNonexistentSibling (false);
            start (std::nullopt);
        }

    private:
        const juce::String name;
        juce::File target;

        juce::Result use (const juce::File& file) override
        {
            if (! file.moveFileTo (target))
                return juce::Result::fail ("Phazite couldn't write " + target.getFullPathName() + ".");

            const auto length = getFileSeconds (target);

            if (length <= 0.0)
                return juce::Result::fail ("Phazite couldn't read the bounced file back.");

            // One undo step: the new track under the original, with the original's color and mixer settings, and the
            // original muted and collapsed.
            auto& bouncedSong = *song;
            const UndoStep step (bouncedSong, "Bounce in Place");
            const auto bounced = insertPlainTrack (bouncedSong, track);

            if (! bounced.isValid())
                return juce::Result::fail ("Phazite couldn't add a track for the bounce.");

            const auto tracks = getTrackIDs (bouncedSong);
            const auto row = (int) std::distance (tracks.begin(), std::find (tracks.begin(), tracks.end(), track));
            setTrackName (bouncedSong, bounced, name);
            setTrackColour (bouncedSong, bounced, getTrackColour (bouncedSong, track, row));

            setChannelVolume (bouncedSong, bounced, getChannelVolume (bouncedSong, track));
            setChannelPan (bouncedSong, bounced, getChannelPan (bouncedSong, track));

            for (const auto send : getSends (bouncedSong, track))
                if (const auto copy = addSend (bouncedSong, bounced, getSendTarget (bouncedSong, send), isPreFader (bouncedSong, send));
                    copy.isValid())
                    setSendLevel (bouncedSong, copy, getSendLevel (bouncedSong, send));

            if (! placeClipState (bouncedSong, makeAudioClipState (bouncedSong, name, target, BeatPosition(), { 0.0, length }), bounced)
                      .isValid())
                return juce::Result::fail ("Phazite couldn't put the bounced file on its track.");

            setTrackMuted (bouncedSong, track, true, &bouncedSong.getUndoManager());
            setCollapsed (bouncedSong, track, true);
            getSelection (context).selectOnly (bounced);
            return juce::Result::ok();
        }
    };

    TrackID getSelectedTrackID (const ProjectContext& context)
    {
        const auto selected = getSelectedTracks (getSelection (context));
        return selected.empty() ? TrackID() : selected.front();
    }

    // What a render of a track is called before it starts: "Freezing Bass", or "Freezing the track" without one.
    juce::String getRenderTitle (const ProjectContext& context, TrackID track, const juce::String& doing)
    {
        auto* song = findSong (context);
        const auto isTrack = song != nullptr && isTrackState (findTrackState (*song, track));
        return doing + " " + (isTrack ? getTrackName (*song, track) : juce::String ("the track"));
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
    auto* song = findSong (context);
    return song != nullptr && id.isValid() && (bool) findTrackState (*song, id)[schema::trackFrozen];
}

bool isFrozenDevice (const ProjectContext& context, DeviceID id)
{
    auto* song = findSong (context);
    return song != nullptr && isFrozenDevice (*song, id);
}

juce::String getWhyNotFreeze (const ProjectContext& context, TrackID id)
{
    auto* song = findSong (context);

    if (isFrozen (context, id))
        return getTrackName (*song, id) + " is frozen already.";

    return getWhyNotRender (song, id, "freeze", "frozen");
}

juce::String getWhyNotBounce (const ProjectContext& context, TrackID id)
{
    return getWhyNotRender (findSong (context), id, "bounce", "bounced");
}

std::unique_ptr<TrackRender> startFreeze (ProjectContext& context, TrackID id, std::function<void (const juce::Result&)> onFinished)
{
    auto* song = findSong (context);

    if (song == nullptr || getWhyNotFreeze (context, id).isNotEmpty())
        return {};

    auto render = std::make_unique<FreezeRender> (context, *song, id, std::move (onFinished));
    return render->hasStarted() ? std::make_unique<TrackRender> (std::move (render)) : nullptr;
}

std::unique_ptr<TrackRender> startBounceInPlace (ProjectContext& context, TrackID id, std::function<void (const juce::Result&)> onFinished)
{
    auto* song = findSong (context);

    if (song == nullptr || getWhyNotBounce (context, id).isNotEmpty())
        return {};

    auto render = std::make_unique<BounceRender> (context, *song, id, std::move (onFinished));
    return render->hasStarted() ? std::make_unique<TrackRender> (std::move (render)) : nullptr;
}

void unfreezeTrack (ProjectContext& context, TrackID id)
{
    if (auto* song = findSong (context); song != nullptr && isFrozen (context, id))
        unfreeze (*song, id);
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

    runOrExplain (host, getRenderTitle (context, id, "Freezing"), getWhyNotFreeze (context, id),
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
    runOrExplain (host, getRenderTitle (context, id, "Bouncing"), getWhyNotBounce (context, id),
                  [&context, id] (std::function<void (const juce::Result&)> done)
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
