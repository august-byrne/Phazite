// SPDX-License-Identifier: AGPL-3.0-or-later
// The piano roll's rows: the pitch (or drum) each shows, how tall they are, and scrolling through them.
#include "PianoRollParts.h"

namespace phazite::ui
{
namespace
{
    constexpr int drumRowHeight = 22;
    constexpr int numPitches = 128;
    constexpr float shortestNote = 3.0f; // pixels: however short a note is, it shows
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

    // All of its row but the line under it (NoteGrid::paintRows()), so that it sits between the lines above and below it,
    // as the chord tones' tint does.
    const auto& timeline = getView();
    const auto top = (float) getRowTop (row);
    const auto height = (float) getRowHeight() - 1.0f;
    const auto left = (float) timeline.beatToX (note.start);

    // In drum mode a note is a hit, drawn as a diamond where it starts.
    if (drumMode)
        return { left - height / 2.0f, top, height, height };

    const auto right = (float) timeline.beatToX (note.getEnd());
    return { left, top, std::max (shortestNote, right - left), height };
}

juce::Rectangle<float> PianoRoll::getNoteShape (const NoteData& note, float scale) const
{
    const auto bounds = getNoteBounds (note);

    if (bounds.isEmpty())
        return {};

    // Rounded where the note is in the song, not where it is in view: a note's end half a pixel in, rounded in view, went one
    // way in one frame and the other in the next while the song scrolled continuously.
    const auto perPixel = std::max (1.0f, scale);
    const auto snap = [perPixel] (float position) { return std::round (position * perPixel) / perPixel; };
    const auto& timeline = getView();
    const auto left = (float) timeline.beatToEdge (note.start, perPixel);
    const auto right = std::max ((float) timeline.beatToEdge (note.getEnd(), perPixel), left + snap (shortestNote));
    const auto top = snap (bounds.getY());
    return { left, top, right - left, snap (bounds.getBottom()) - top };
}
} // namespace phazite::ui
