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
#include "model/base/WholeFile.h"
#include "model/base/internal/DevicesValue.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/mixer/internal/Mixer.h"
#include "model/playback/internal/PlaybackSession.h"
#include "model/presets/internal/Presets.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/internal/Playback.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace phazite
{
namespace
{
    constexpr int noteVelocity = 100;
    constexpr int noteMilliseconds = 600; // an instrument's note, while the song is stopped
    constexpr int instrumentNote = 60;    // C4; the drum sampler's first pad's for its kits

    // Each of a device's sounds with its property (DevicesValue.h: getSoundPropertiesOf()), in their order: a copy of them,
    // which what writes a sound may outlive.
    std::vector<std::pair<juce::Identifier, juce::String>> getSounds (const Song& song, DeviceID id)
    {
        std::vector<std::pair<juce::Identifier, juce::String>> sounds;

        if (const auto* device = findDevice (song, id); device != nullptr && device->form == DeviceValue::Form::own)
        {
            const auto& properties = getSoundPropertiesOf (std::string_view (device->type.toRawUTF8()));

            std::vector<std::pair<std::uint16_t, juce::String>> held (device->sounds.begin(), device->sounds.end());
            std::sort (held.begin(), held.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });

            for (const auto& [index, path] : held)
                if (index < properties.size())
                    sounds.emplace_back (properties[index], path);
        }

        return sounds;
    }

    // The files a device's sounds name outside the project's folder, by their full paths, as Swap tries them.
    juce::Array<juce::File> findFilesOutside (const Song& song, DeviceID id)
    {
        juce::Array<juce::File> outside;

        for (const auto& [property, path] : getSounds (song, id))
            if (juce::File::isAbsolutePath (path))
                if (const auto file = makeFileOfFullPath (path); file.existsAsFile())
                    outside.addIfNotAlreadyThere (file);

        return outside;
    }

    // Those of them brought into the project (bringFilesIntoProject(), Import.h) each named by its copy, which
    // copyIntoProject() finds without reading, in the caller's undo step.
    void copyFilesIn (Song& song, DeviceID id, const juce::Array<juce::File>& brought)
    {
        for (const auto& [property, path] : getSounds (song, id))
        {
            if (juce::File::isAbsolutePath (path))
                if (const auto file = makeFileOfFullPath (path); brought.contains (file))
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

        const auto* value = findDevice (*s, device);

        if (value == nullptr || value->form != DeviceValue::Form::own)
            return nullptr;

        if (pad >= 0)
            return isDrumSampler (*value) && juce::isPositiveAndBelow (pad, devices::DrumSampler::numPads) ? s : nullptr;

        return findOwnDevice (std::string_view (value->type.toRawUTF8())) != nullptr ? s : nullptr;
    }

    // The step's name, after what it tries: "Put Kick 03 on Pad 36", "Load Preset Warm Pad".
    juce::String getStepName() const
    {
        const auto triedName = tried.getFileNameWithoutExtension();
        return pad >= 0 ? "Put " + triedName + " on Pad " + juce::String (pad) : "Load Preset " + triedName;
    }

    // Names what it tried by the copies brought in for it (Swap::keep()), and keeps the step under its name.
    void keep()
    {
        if (trial == nullptr)
            return;

        if (auto* s = getSong(); s != nullptr && trial->hasChanges())
        {
            trial->apply ([this, s] { copyFilesIn (*s, device, brought); });
            trial->keep (getStepName());
        }
        else
        {
            trial->putBack();
        }

        end();
    }

    // Kept first as another change begins (Trial's keepFirst): at once, so its sounds as tried, by their full paths outside
    // the project, as a sound that couldn't be copied is kept (no copy made there, on the message thread: known bug 183).
    // They are copied in the background then, and named by their copies once they are, as a step of its own.
    void keepForAnother()
    {
        auto* s = getSong();

        if (trial == nullptr || s == nullptr || ! trial->hasChanges())
        {
            putBack();
            return;
        }

        trial->keep (getStepName());
        end();

        const auto outside = findFilesOutside (*s, device);

        if (outside.isEmpty())
            return;

        bringFilesIntoProject (*s, { .files = outside,
                                     .title = "Copying " + tried.getFileName(),
                                     .problem = "What the swap kept plays its sounds from where they are: its device went before they "
                                                "were copied into the project.",
                                     .apply = [weak = song, id = device, outside]
                                     {
                                         auto* kept = weak.get();

                                         if (kept == nullptr || findDevice (*kept, id) == nullptr)
                                             return false;

                                         const UndoStep step (*kept, "Copy Sounds into the Project");
                                         copyFilesIn (*kept, id, outside);
                                         return true;
                                     } });
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

        const auto* value = findDevice (s, device);

        if (value == nullptr || ! value->isInstrument())
            return;

        const auto drums = isDrumSampler (*value);

        if (auto* session = findPlayingSession (s))
        {
            stopNote();
            noteTrack = getDeviceChannel (s, device);
            const auto note = drums ? devices::DrumSampler::kitFirstNote : instrumentNote;
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
    bool heldBack = false;            // closed from the start, as another's keep was under way (Swap::isHeldBack())
    bool keeping = false;             // its sounds being copied into the project, to keep once they are (Swap::keep())
    juce::Array<juce::File> brought;  // those copied in for its keep
    std::function<void()> afterwards; // what keep() was given, once kept
};

//==============================================================================
Swap::Swap (ProjectContext& context, DeviceID device, int pad)
    : impl (std::make_shared<Impl> (context, device, pad))
{
    auto* song = impl->getSong();

    // Not while another's keep is under way (its sounds copied in): it opens closed, and is asked for again once that is kept
    // (Browser::startSwap()).
    if (song == nullptr || Trial::isKeepPending (*song))
    {
        impl->ended = true;
        impl->heldBack = song != nullptr;
        return;
    }

    impl->oldName = pad >= 0 ? getPadName (*song, device, pad) : juce::String();

    // Something else that changes the song keeps what is tried first, as a step of its own, and ends the swap.
    impl->trial = std::make_unique<Trial> (*song, pad >= 0 ? "Swap Pad " + juce::String (pad) : "Swap Preset",
                                           [kept = impl.get()] { kept->keepForAnother(); });
}

Swap::~Swap()
{
    if (isOpen() && ! impl->keeping)
        putBack();
}

bool Swap::isOpen() const
{
    // Kept, its sounds being copied in: closed, as once kept.
    return ! impl->keeping && impl->getSong() != nullptr && impl->trial != nullptr && impl->trial->isOpen();
}

bool Swap::isHeldBack() const
{
    return impl->heldBack;
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

bool Swap::keep (std::function<void()> afterwards)
{
    if (! isOpen())
        return false;

    // The sounds it tried by their full paths, copied into the project in the background first, as a sound put on a pad is
    // (known bug 123): kept once they are, its trial open meanwhile, input held; put back if that is cancelled.
    auto& song = *impl->getSong();
    const auto outside = findFilesOutside (song, impl->device);
    impl->brought = outside;
    impl->keeping = true;
    bringFilesIntoProject (song, { .files = outside,
                                   .title = "Copying " + impl->tried.getFileName(),
                                   .problem = "What the swap tried wasn't kept.",
                                   .apply =
                                       [kept = impl]
                                   {
                                       kept->keeping = false;
                                       kept->trial->setKeepPending (false);
                                       kept->keep();

                                       if (const auto then = std::exchange (kept->afterwards, nullptr))
                                           then();

                                       return true;
                                   },
                                   .otherwise =
                                       [kept = impl]
                                   {
                                       kept->keeping = false;
                                       kept->trial->setKeepPending (false);
                                       kept->afterwards = nullptr;
                                       kept->putBack();
                                   },
                                   .forTheOpenTrial = true });

    // Its keep under way: kept once its sounds are in, and not a second time meanwhile (Trial::setKeepPending()).
    if (! impl->keeping)
        return false;

    impl->afterwards = std::move (afterwards);
    impl->trial->setKeepPending (true);
    return true;
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
        if (open->song.get() == &song && open->device == device && ! open->keeping && open->getSong() != nullptr && open->trial != nullptr
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
