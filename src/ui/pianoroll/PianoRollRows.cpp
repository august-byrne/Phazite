// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's rows: the pitch (or drum) each shows, how tall they are, and scrolling through them.
#include "PianoRollParts.h"

namespace phazite::ui
{
namespace
{
    constexpr int drumRowHeight = 22;
    constexpr int numPitches = 128;
} // namespace

int PianoRoll::getRowHeight() const
{
    return drumMode ? drumRowHeight : rowHeight;
}

int PianoRoll::getNumRows() const
{
    return drumMode ? (int) drumRows.size() : numPitches;
}

int PianoRoll::getRowForPitch (int pitch) const
{
    if (! drumMode)
        return juce::isPositiveAndBelow (pitch, numPitches) ? numPitches - 1 - pitch : -1;

    for (size_t i = 0; i < drumRows.size(); ++i)
        if (drumRows[i].note == pitch)
            return (int) i;

    return -1;
}

int PianoRoll::getPitchForRow (int row) const
{
    if (! juce::isPositiveAndBelow (row, getNumRows()))
        return -1;

    return drumMode ? drumRows[(size_t) row].note : numPitches - 1 - row;
}

int PianoRoll::getRowTop (int row) const
{
    return row * getRowHeight() - scrollY;
}

int PianoRoll::getRowAt (int y) const
{
    const auto position = y + scrollY;
    const auto row = position >= 0 ? position / getRowHeight() : -1;
    return juce::isPositiveAndBelow (row, getNumRows()) ? row : -1;
}

void PianoRoll::setScrollY (int newScrollY)
{
    const auto maxScroll = std::max (0, getNumRows() * getRowHeight() - grid->getHeight());
    scrollY = juce::jlimit (0, maxScroll, newScrollY);
    updateVerticalScrollBar();
    keys->repaint();
    grid->repaint();
}

void PianoRoll::scrollToNotes()
{
    // The middle of the notes' range, or middle C (or the first drum) without notes.
    auto lowest = 127, highest = 0;

    for (const auto& n : getAllNotes())
    {
        lowest = std::min (lowest, n.pitch);
        highest = std::max (highest, n.pitch);
    }

    if (drumMode)
    {
        setScrollY (0);
        return;
    }

    const auto middle = lowest <= highest ? (lowest + highest) / 2 : 60;
    setScrollY (getRowTop (getRowForPitch (middle)) + scrollY - grid->getHeight() / 2);
}

void PianoRoll::updateVerticalScrollBar()
{
    const auto total = std::max (1, getNumRows() * getRowHeight());
    verticalScrollBar.setRangeLimits (0.0, total, juce::dontSendNotification);
    verticalScrollBar.setCurrentRange (scrollY, std::max (1, grid->getHeight()), juce::dontSendNotification);
    verticalScrollBar.setSingleStepSize (scrollPerNotch);
}

juce::Rectangle<float> PianoRoll::getNoteBounds (const NoteData& note) const
{
    const auto row = getRowForPitch (note.pitch);

    if (row < 0)
        return {};

    const auto& timeline = getView();
    const auto top = (float) getRowTop (row) + 1.0f;
    const auto height = (float) getRowHeight() - 2.0f;
    const auto left = (float) timeline.beatToX (note.start);

    // In drum mode a note is a hit, drawn as a diamond where it starts.
    if (drumMode)
        return { left - height / 2.0f, top, height, height };

    const auto right = (float) timeline.beatToX (note.getEnd());
    return { left, top, std::max (3.0f, right - left), height };
}
} // namespace phazite::ui
