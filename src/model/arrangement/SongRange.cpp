// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SongRange.h"
#include "internal/SongRange.h"

namespace phazite
{
struct SongRange::Impl
{
    RangeSelection& range;
    const te::SelectionManager& selection;
};

std::unique_ptr<SongRange> createSongRange (RangeSelection& range, const te::SelectionManager& selection)
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
    return fromTracktion (impl->range.getRange());
}

std::vector<TrackID> SongRange::getTracks() const
{
    std::vector<TrackID> ids;

    for (auto* track : impl->range.getTracks())
        ids.push_back (getTrackID (*track));

    return ids;
}

void SongRange::set (BeatRange newRange, const std::vector<TrackID>& ids)
{
    juce::Array<te::Track*> tracks;

    if (auto* edit = impl->selection.getEdit())
        for (const auto id : ids)
            if (auto* track = te::findTrackForID (*edit, toTracktion (id)))
                tracks.add (track);

    impl->range.set (toTracktion (newRange), tracks);
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
