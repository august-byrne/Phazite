// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TransportModel.h"
#include "internal/Playback.h"
#include "internal/Recording.h"
#include "internal/Tempo.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/LiveInput.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
bool isPlaying (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr && isSongPlaying (*song);
}

bool isLoopOn (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr && isLoopOn (*song);
}

bool pausePlayback (ProjectContext& context)
{
    auto* song = findSong (context);

    if (song == nullptr || ! isSongPlaying (*song) || isRecording (*song))
        return false;

    stopPlaying (*song);
    return true;
}

void resumePlayback (ProjectContext& context)
{
    if (auto* song = findSong (context); song != nullptr && ! isSongPlaying (*song))
        playOrPause (*song); // from where the playhead is
}

std::optional<TimeSignature> getTimeSignature (const ProjectContext& context)
{
    if (auto* song = findSong (context))
        return getTimeSignature (*song);

    return {};
}

void setTimeSignature (ProjectContext& context, TimeSignature timeSignature)
{
    if (auto* song = findSong (context))
        setTimeSignature (*song, timeSignature);
}

TrackID getLiveMidiTrack (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? getLiveMidiTrack (*song) : TrackID();
}

//==============================================================================
struct TempoHandle::Impl
{
    juce::WeakReference<Song> song;
    std::unique_ptr<Gesture> gesture;
    BeatPosition gestureBeat; // where the playhead was as the drag began: the tempo there changes all through it

    BeatPosition getBeat() const { return gesture != nullptr ? gestureBeat : getPlayheadBeat (*song); }
};

TempoHandle getTempoHandle (ProjectContext& context)
{
    auto* song = findSong (context);

    if (song == nullptr)
        return {};

    auto impl = std::make_unique<TempoHandle::Impl>();
    impl->song = song;
    return TempoHandle (std::move (impl));
}

TempoStretch getTempoStretch (const ProjectContext& context)
{
    auto* song = findSong (context);
    return song != nullptr ? getTempoStretch (*song) : TempoStretch {};
}

TempoHandle::TempoHandle() = default;

TempoHandle::TempoHandle (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

TempoHandle::TempoHandle (TempoHandle&&) noexcept = default;

TempoHandle& TempoHandle::operator= (TempoHandle&& other) noexcept
{
    if (this != &other)
    {
        endGesture();
        impl = std::move (other.impl);
    }

    return *this;
}

TempoHandle::~TempoHandle()
{
    endGesture();
}

bool TempoHandle::isValid() const
{
    return impl != nullptr && impl->song.get() != nullptr;
}

double TempoHandle::getValue() const
{
    auto* song = impl != nullptr ? impl->song.get() : nullptr;
    return song != nullptr ? getTempoAt (*song, impl->getBeat()) : 120.0;
}

void TempoHandle::beginGesture()
{
    auto* song = impl != nullptr ? impl->song.get() : nullptr;

    if (song != nullptr && impl->gesture == nullptr)
    {
        impl->gestureBeat = getPlayheadBeat (*song);
        impl->gesture = std::make_unique<Gesture> (*song, "Change Tempo");
    }
}

void TempoHandle::setValue (double bpm)
{
    if (auto* song = impl != nullptr ? impl->song.get() : nullptr)
        setTempoAt (*song, impl->getBeat(), bpm); // joins the drag's step, if one is in progress
}

void TempoHandle::endGesture()
{
    if (impl != nullptr)
        impl->gesture.reset(); // which keeps the drag's change as its step
}

bool TempoHandle::isInGesture() const
{
    return impl != nullptr && impl->gesture != nullptr;
}
} // namespace phazite
