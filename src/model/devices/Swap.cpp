// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Swap.h"
#include "DeviceHandle.h"
#include "SwapModel.h"
#include "devices/drums/DrumSampler.h"
#include "internal/DeviceFiles.h"
#include "internal/DeviceHandle.h"
#include "internal/Devices.h"
#include "internal/DrumKit.h"
#include "model/arrangement/internal/Import.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/presets/internal/Presets.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

#include <algorithm>
#include <vector>

namespace phazite
{
namespace
{
    constexpr int noteVelocity = 100;
    constexpr int noteMilliseconds = 600; // an instrument's note, while the song is stopped
    constexpr int instrumentNote = 60;    // C4; the drum sampler's first pad's for its kits

    // The files a device's sounds name outside the project's folder, as Swap tries them by their full paths, copied into its
    // Audio folder, each named by its copy then, in the caller's undo step.
    void copyFilesIn (Song& song, DeviceID id)
    {
        const auto sounds = findDeviceState (song, id).getChildWithName (schema::SOUNDS);

        if (! sounds.isValid())
            return;

        for (int i = 0; i < sounds.getNumProperties(); ++i)
        {
            const auto property = sounds.getPropertyName (i);

            if (const auto path = sounds[property].toString(); juce::File::isAbsolutePath (path))
                if (const juce::File file (path); file.existsAsFile())
                    if (const auto copy = copyIntoProject (song, file); copy != file)
                        setDeviceSound (song, id, property, getProjectPath (song, copy));
        }
    }
} // namespace

//==============================================================================
struct Swap::Impl : private juce::Timer
{
    Impl (ProjectContext& c, DeviceID d, int p)
        : context (c),
          song (findSong (c)),
          device (d),
          pad (p),
          projectNumber (c.getProjectNumber())
    {
        getOpen().push_back (this);
    }

    ~Impl() override
    {
        stopNote();
        auto& open = getOpen();
        open.erase (std::remove (open.begin(), open.end(), this), open.end());
    }

    static std::vector<Impl*>& getOpen()
    {
        static std::vector<Impl*> open;
        return open;
    }

    // The song, while it is the open project's and holds the device, of its kind.
    Song* getSong() const
    {
        auto* s = song.get();

        if (s == nullptr || ended || context.getProjectNumber() != projectNumber || findSong (context) != s)
            return nullptr;

        const auto state = findDeviceState (*s, device);

        if (pad >= 0)
            return isDrumSampler (state) && juce::isPositiveAndBelow (pad, devices::DrumSampler::numPads) ? s : nullptr;

        return findOwnDevice (state[schema::deviceType].toString().toStdString()) != nullptr ? s : nullptr;
    }

    // The step's name, after what it tries: "Put Kick 03 on Pad 36", "Load Preset Warm Pad".
    juce::String getStepName() const
    {
        const auto triedName = tried.getFileNameWithoutExtension();
        return pad >= 0 ? "Put " + triedName + " on Pad " + juce::String (pad) : "Load Preset " + triedName;
    }

    // Copies in what it tried, and names the step after it.
    void keep()
    {
        if (trial == nullptr)
            return;

        if (auto* s = getSong(); s != nullptr && trial->hasChanges())
        {
            trial->apply ([this, s] { copyFilesIn (*s, device); });
            trial->keep (getStepName());
        }
        else
        {
            trial->putBack();
        }

        end();
    }

    void putBack()
    {
        if (trial != nullptr)
            trial->putBack();

        end();
    }

    void end()
    {
        ended = true;
        stopNote();
    }

    // Heard at once while the song is stopped: the pad, or a short note on an instrument, through the track's devices.
    void playAlone (Song& s)
    {
        if (isSongPlaying (s))
            return;

        if (pad >= 0)
        {
            if (auto* drums = makeDeviceHandle (s, device).getProcessor<devices::DrumSampler>())
                drums->audition (pad, 1.0f);

            return;
        }

        const auto state = findDeviceState (s, device);

        if (! isInstrumentState (state))
            return;

        if (auto* session = findPlayingSession (s))
        {
            stopNote();
            noteTrack = getDeviceChannel (s, device);
            const auto note = isDrumSampler (state) ? devices::DrumSampler::kitFirstNote : instrumentNote;
            session->playGuideNotes ((std::int64_t) noteTrack.getRaw(), { note }, noteVelocity);
            startTimer (noteMilliseconds);
        }
    }

    void stopNote()
    {
        stopTimer();

        if (! noteTrack.isValid())
            return;

        if (auto* s = song.get())
            if (auto* session = findSession (*s))
                session->stopGuideNotes ((std::int64_t) noteTrack.getRaw());

        noteTrack = {};
    }

    void timerCallback() override { stopNote(); }

    ProjectContext& context;
    juce::WeakReference<Song> song;
    const DeviceID device;
    const int pad;
    const int projectNumber;
    std::unique_ptr<Trial> trial;
    juce::File tried;
    juce::String oldName;
    TrackID noteTrack; // the track an instrument's note plays on, until it stops
    bool ended = false;
};

//==============================================================================
Swap::Swap (ProjectContext& context, DeviceID device, int pad)
    : impl (std::make_unique<Impl> (context, device, pad))
{
    auto* song = impl->getSong();

    if (song == nullptr)
    {
        impl->ended = true;
        return;
    }

    impl->oldName = pad >= 0 ? getPadName (*song, device, pad) : juce::String();

    // Something else that changes the song keeps what is tried first, as a step of its own, and ends the swap.
    impl->trial = std::make_unique<Trial> (*song, pad >= 0 ? "Swap Pad " + juce::String (pad) : "Swap Preset", [this] { impl->keep(); });
}

Swap::~Swap()
{
    if (isOpen())
        putBack();
}

bool Swap::isOpen() const
{
    return impl->getSong() != nullptr && impl->trial != nullptr && impl->trial->isOpen();
}

DeviceID Swap::getDevice() const
{
    return impl->device;
}

int Swap::getPad() const
{
    return impl->pad;
}

juce::String Swap::getTargetName() const
{
    if (impl->pad >= 0)
        return "Pad " + juce::String (impl->pad);

    auto* song = impl->song.get();
    return song != nullptr ? getDeviceName (*song, impl->device) : juce::String();
}

juce::String Swap::getOldName() const
{
    return impl->oldName;
}

bool Swap::canTry (const juce::File& file) const
{
    auto* song = impl->getSong();

    if (song == nullptr || ! file.existsAsFile())
        return false;

    return impl->pad >= 0 ? getFileKind (file) == FileKind::audio : isPresetFor (*song, impl->device, file);
}

bool Swap::tryFile (const juce::File& file)
{
    if (! isOpen() || ! canTry (file))
        return false;

    auto& song = *impl->getSong();
    const auto device = impl->device;
    const auto pad = impl->pad;

    // A pad's sound by its full path, copied in once it is kept; a preset over the device as it was before the swap.
    impl->trial->apply (
        [&song, &file, device, pad]
        {
            if (pad >= 0)
                setDeviceSound (song, device, schema::getPadSoundFile (pad), getProjectPath (song, file));
            else
                tryPresetIn (song, device, file);
        },
        pad < 0);

    impl->tried = file;
    impl->trial->rename (impl->getStepName());
    impl->playAlone (song);
    return true;
}

juce::File Swap::getTried() const
{
    return isOpen() ? impl->tried : juce::File();
}

void Swap::keep()
{
    if (isOpen())
        impl->keep();
}

void Swap::putBack()
{
    if (isOpen())
        impl->putBack();
    else
        impl->end();
}

//==============================================================================
std::optional<int> findOpenSwap (const Song& song, DeviceID device)
{
    for (const auto* open : Swap::Impl::getOpen())
        if (open->song.get() == &song && open->device == device && open->getSong() != nullptr && open->trial != nullptr
            && open->trial->isOpen())
            return open->pad;

    return std::nullopt;
}

std::optional<int> findOpenSwap (const ProjectContext& context, DeviceID device)
{
    const auto* song = findSong (context);
    return song != nullptr ? findOpenSwap (*song, device) : std::nullopt;
}
} // namespace phazite
