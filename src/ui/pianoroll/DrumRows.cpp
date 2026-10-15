// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DrumRows.h"

#include <algorithm>

namespace phazite::ui
{
std::vector<DrumRow> getDrumRows (const std::optional<std::vector<DrumRow>>& samplerSounds, const std::vector<int>& notesPlayed,
                                  const std::vector<int>& order, const std::vector<int>& hidden)
{
    std::vector<DrumRow> rows;
    const auto contains = [&rows] (int note)
    { return std::any_of (rows.begin(), rows.end(), [note] (const DrumRow& row) { return row.note == note; }); };

    // The sampler's pads, or else the usual drums.
    if (samplerSounds.has_value())
    {
        rows = *samplerSounds;
    }
    else
    {
        for (const auto note : { 36, 37, 38, 39, 42, 46, 41, 45, 48, 49, 51 })
            rows.push_back ({ note, getDrumName (note) });
    }

    // Any other notes the clips play.
    for (const auto note : notesPlayed)
        if (! contains (note))
            rows.push_back ({ note, getDrumName (note) });

    // The track's order first; rows it doesn't mention keep theirs, after them.
    const auto rank = [&order] (int note)
    {
        const auto found = std::find (order.begin(), order.end(), note);
        return found != order.end() ? (int) (found - order.begin()) : (int) order.size();
    };

    std::stable_sort (rows.begin(), rows.end(), [&rank] (const DrumRow& a, const DrumRow& b) { return rank (a.note) < rank (b.note); });

    rows.erase (std::remove_if (rows.begin(), rows.end(), [&hidden] (const DrumRow& row)
                                { return std::find (hidden.begin(), hidden.end(), row.note) != hidden.end(); }),
                rows.end());
    return rows;
}

std::vector<int> getDrumOrderAfterMove (const std::vector<DrumRow>& shown, int note, int newIndex)
{
    std::vector<int> order;

    for (const auto& row : shown)
        if (row.note != note)
            order.push_back (row.note);

    order.insert (order.begin() + juce::jlimit (0, (int) order.size(), newIndex), note);
    return order;
}
} // namespace phazite::ui
