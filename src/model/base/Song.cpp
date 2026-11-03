// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Song.h"

namespace phazite
{
Song::Song (juce::ValueTree s)
    : state (std::move (s))
{
    undoManager.setMaxNumberOfStoredUnits (30000, 30);
}

Song::~Song() = default;

void Song::setTracktionIDs (std::function<std::uint64_t()> takeNext)
{
    takeTracktionID = std::move (takeNext);
}

void Song::setMirror (std::function<void (Part, std::uint64_t)> reconcile)
{
    mirror = std::move (reconcile);
}

void Song::reconcile (Part part, std::uint64_t id)
{
    if (mirror != nullptr)
        mirror (part, id);
}
} // namespace phazite
