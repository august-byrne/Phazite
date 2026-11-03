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

void Song::setMirror (std::function<void (Part)> reconcile)
{
    mirror = std::move (reconcile);
}

void Song::reconcile (Part part)
{
    if (mirror != nullptr)
        mirror (part);
}
} // namespace phazite
