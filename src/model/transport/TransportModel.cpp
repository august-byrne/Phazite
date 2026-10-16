// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TransportModel.h"
#include "internal/Playback.h"
#include "internal/Tempo.h"
#include "model/commands/internal/Undo.h"
#include "model/engine/internal/LiveInput.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/ProjectContext.h"

namespace phazite
{
bool isPlaying (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    return edit != nullptr && edit->getTransport().isPlaying();
}

bool isLoopOn (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    return edit != nullptr && isLoopOn (*edit);
}

std::optional<TimeSignature> getTimeSignature (const ProjectContext& context)
{
    if (auto* edit = getEdit (context))
        return getTimeSignature (*edit);

    return {};
}

void setTimeSignature (ProjectContext& context, TimeSignature timeSignature)
{
    if (auto* edit = getEdit (context))
        setTimeSignature (*edit, timeSignature);
}

TrackID getLiveMidiTrack (const ProjectContext& context)
{
    auto* edit = getEdit (context);
    auto* track = edit != nullptr ? getLiveMidiTrack (*edit) : nullptr;
    return track != nullptr ? getTrackID (*track) : TrackID();
}

//==============================================================================
struct TempoHandle::Impl
{
    te::SafeSelectable<te::Edit> edit;
    std::unique_ptr<Gesture> gesture;
};

TempoHandle getTempoHandle (ProjectContext& context)
{
    auto* edit = getEdit (context);

    if (edit == nullptr)
        return {};

    auto impl = std::make_unique<TempoHandle::Impl>();
    impl->edit = edit;
    return TempoHandle (std::move (impl));
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
    return impl != nullptr && impl->edit.get() != nullptr;
}

double TempoHandle::getValue() const
{
    auto* edit = impl != nullptr ? impl->edit.get() : nullptr;
    return edit != nullptr ? getTempo (*edit) : 120.0;
}

void TempoHandle::beginGesture()
{
    auto* edit = impl != nullptr ? impl->edit.get() : nullptr;

    if (edit != nullptr && impl->gesture == nullptr)
        impl->gesture = std::make_unique<Gesture> (*edit, "Change Tempo");
}

void TempoHandle::setValue (double bpm)
{
    if (auto* edit = impl != nullptr ? impl->edit.get() : nullptr)
        setTempo (*edit, bpm); // joins the drag's step, if one is in progress
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
