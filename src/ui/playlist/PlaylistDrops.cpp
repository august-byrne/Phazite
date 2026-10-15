// SPDX-License-Identifier: AGPL-3.0-or-later
// What is dropped on the playlist: audio and MIDI files from Explorer, and the browser's sounds and devices.
#include "PlaylistParts.h"
#include "model/arrangement/Import.h"
#include "model/arrangement/Tracks.h"
#include "model/mixer/Mixer.h"
#include "model/transport/Playback.h"
#include "ui/input/LibraryDrag.h"

namespace phazite::ui
{
te::BeatPosition Playlist::getFileDropBeat (int x) const
{
    const auto beat = te::BeatPosition::fromBeats (std::max (0.0, getView().xToBeat (x).inBeats()));
    return snapDown (beat, juce::ModifierKeys::currentModifiers);
}

void Playlist::importFiles (const juce::StringArray& paths, juce::Point<int> position)
{
    const auto row = getRowAt (position.y);
    addFiles (paths, row >= 0 ? rows[(size_t) row].track.get() : nullptr, getFileDropBeat (position.x));
}

bool Playlist::placeFile (const juce::File& file)
{
    auto* e = getEdit();

    if (e == nullptr)
        return false;

    return addFiles ({ file.getFullPathName() }, getSelectedTracks (selection).getFirst(), e->tempoSequence.toBeats (getPlayStart (*e)));
}

bool Playlist::addFiles (const juce::StringArray& paths, te::Track* target, te::BeatPosition start)
{
    auto* e = getEdit();

    if (e == nullptr)
        return false;

    const auto added = phazite::importFiles (*e, paths, target, start);
    range.clear();
    selection.deselectAll();

    for (auto* clip : added)
        selection.addToSelection (clip);

    if (added.isEmpty())
        return false;

    lastClip = added.getFirst(); // what Draw and Paint place next
    return true;
}

bool Playlist::isInterestedInFileDrag (const juce::StringArray& paths)
{
    auto* e = getEdit();

    if (e == nullptr)
        return false;

    return std::any_of (paths.begin(), paths.end(),
                        [e] (const juce::String& path) { return getFileKind (e->engine, juce::File (path)) != FileKind::other; });
}

void Playlist::fileDragMove (const juce::StringArray&, int x, int y)
{
    // Where importFiles() would put them.
    const auto position = lanes->getLocalPoint (this, juce::Point<int> (x, y));
    fileDropRow = getRowAt (position.y);
    fileDropBeat = getFileDropBeat (position.x);
    lanes->repaint();
}

void Playlist::fileDragExit (const juce::StringArray&)
{
    fileDropBeat.reset();
    lanes->repaint();
}

void Playlist::filesDropped (const juce::StringArray& paths, int x, int y)
{
    fileDropBeat.reset();
    lanes->repaint();
    importFiles (paths, lanes->getLocalPoint (this, juce::Point<int> (x, y)));
}

bool Playlist::isInterestedInDragSource (const SourceDetails& details)
{
    auto* e = getEdit();

    if (e == nullptr)
        return false;

    if (const auto file = getDraggedFile (details.description))
        return getFileKind (e->engine, *file) != FileKind::other;

    return getDraggedDevice (details.description).has_value();
}

void Playlist::itemDragMove (const SourceDetails& details)
{
    // Where it would go, as for files from Explorer: a file at a beat, a device onto a track.
    const auto position = lanes->getLocalPoint (this, details.localPosition);
    fileDropRow = getRowAt (position.y);
    deviceDropping = getDraggedDevice (details.description).has_value();

    if (deviceDropping)
        fileDropBeat.reset();
    else
        fileDropBeat = getFileDropBeat (std::max (0, position.x));

    lanes->repaint();
}

void Playlist::itemDragExit (const SourceDetails&)
{
    fileDropBeat.reset();
    deviceDropping = false;
    lanes->repaint();
}

void Playlist::itemDropped (const SourceDetails& details)
{
    itemDragExit (details);
    auto* e = getEdit();

    if (e == nullptr)
        return;

    const auto position = lanes->getLocalPoint (this, details.localPosition);
    const auto row = getRowAt (position.y);
    auto* track = row >= 0 ? rows[(size_t) row].track.get() : nullptr;
    auto placed = false;

    if (const auto file = getDraggedFile (details.description))
        placed = addFiles ({ file->getFullPathName() }, track, getFileDropBeat (std::max (0, position.x)));
    else if (const auto device = getDraggedDevice (details.description))
        placed = addDeviceTo (*e, track, *device, &selection) != nullptr;

    if (placed)
        markPlaced (details.description);
}
} // namespace phazite::ui
