// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TimelineGesture.h"

#include <algorithm>

namespace phazite::ui
{
SelectionAtPress SelectionAtPress::take (ProjectContext& context)
{
    const auto& selection = context.getSongSelection();
    const auto& songRange = context.getSongRange();
    SelectionAtPress taken;
    taken.tracks = selection.getTracks();
    taken.clips = selection.getClips();
    taken.sections = selection.getSections();
    taken.devices = selection.getDevices();
    taken.noteClips = selection.getNoteClips();
    taken.notes = selection.getNotes();
    taken.points = selection.getSelectedAutomationPoints();

    if (! songRange.isEmpty())
        taken.range = std::pair (songRange.getRange(), songRange.getTracks());

    return taken;
}

void SelectionAtPress::putBack (ProjectContext& context) const
{
    const auto now = take (context);
    auto& selection = context.getSongSelection();

    // Devices, notes and points are selected on their own (SongSelection.h): what was selected is one of them, or tracks,
    // clips and sections. Notes are of the clips an editor showed: those go back first, which the piano roll then shows.
    if (now.tracks != tracks || now.clips != clips || now.sections != sections || now.devices != devices || now.notes != notes
        || now.points.parameter != points.parameter || now.points.indices != points.indices)
    {
        selection.clear();

        for (const auto track : tracks)
            selection.add (track);

        for (const auto clip : clips)
            selection.add (clip);

        for (const auto section : sections)
            selection.add (section);

        for (const auto device : devices)
            selection.add (device);

        if (! notes.empty())
        {
            selection.setNoteClips (noteClips);
            selection.selectNotes (notes);
        }
        else if (points.parameter.isValid())
        {
            selection.selectAutomationPoints (points.parameter, points.indices);
        }
    }

    if (now.range == range)
        return;

    if (range.has_value())
        context.getSongRange().set (range->first, range->second);
    else
        context.getSongRange().clear();
}

juce::Point<int> TimelineGesture::getStart (const TimelineView& view, int scrollYNow, int unscrolledHeight) const
{
    // In the column the beat's own place rounds to, as lines and edges are (TimelineView::beatToColumn()): rounded where it is
    // in view, a start half a pixel in went one way at one step of a scroll and the other at the next.
    const auto y = startPosition.y < unscrolledHeight ? startPosition.y : startPosition.y + startScrollY - scrollYNow;
    return { view.beatToColumn (startBeat), y };
}

void TimelineGesture::beginStep (ProjectContext& context, const juce::String& name)
{
    if (step == nullptr)
        step = std::make_unique<SongGesture> (context, name);
}

void TimelineGesture::rollBack()
{
    if (step != nullptr)
        step->rollBack();

    step = nullptr;
}

bool TimelineGesture::doOnce (NoteRef note)
{
    if (std::find (doneNotes.begin(), doneNotes.end(), note) != doneNotes.end())
        return false;

    doneNotes.push_back (note);
    return true;
}
} // namespace phazite::ui
