// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SongRange.h"
#include "internal/SongRange.h"
#include "internal/Tracks.h"

namespace phazite
{
struct SongRange::Impl
{
    RangeSelection& range;
    const Selection& selection;
};

std::unique_ptr<SongRange> createSongRange (RangeSelection& range, const Selection& selection)
{
    return std::make_unique<SongRange> (std::make_unique<SongRange::Impl> (range, selection));
}

SongRange::SongRange (std::unique_ptr<Impl> i)
    : impl (std::move (i))
{
}

SongRange::~SongRange() = default;

bool SongRange::isEmpty() const
{
    return impl->range.isEmpty();
}

BeatRange SongRange::getRange() const
{
    return impl->range.getRange();
}

std::vector<TrackID> SongRange::getTracks() const
{
    return impl->range.getTracks();
}

void SongRange::set (BeatRange newRange, const std::vector<TrackID>& ids)
{
    // Those the song holds.
    std::vector<TrackID> tracks;

    if (auto* song = impl->selection.getSong())
        for (const auto id : ids)
            if (findTrackState (*song, id).isValid())
                tracks.push_back (id);

    impl->range.set (newRange, tracks);
}

void SongRange::clear()
{
    impl->range.clear();
}

void SongRange::addChangeListener (juce::ChangeListener* listener)
{
    impl->range.addChangeListener (listener);
}

bool SongRange::isSource (const juce::ChangeBroadcaster* source) const noexcept
{
    return source == &impl->range;
}

void SongRange::removeChangeListener (juce::ChangeListener* listener)
{
    impl->range.removeChangeListener (listener);
}
} // namespace phazite
