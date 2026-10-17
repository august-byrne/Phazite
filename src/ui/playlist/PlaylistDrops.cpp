// SPDX-License-Identifier: AGPL-3.0-or-later
// What is dropped on the playlist: audio and MIDI files from Explorer, and the browser's sounds and devices.
#include "PlaylistParts.h"
#include "model/mixer/MixerModel.h"
#include "ui/input/LibraryDrag.h"

namespace phazite::ui
{
BeatPosition Playlist::getFileDropBeat (int x) const
{
    const auto beat = BeatPosition::fromBeats (std::max (0.0, getView().xToBeat (x).inBeats()));
    return snapDown (beat, juce::ModifierKeys::currentModifiers);
}

void Playlist::importFiles (const juce::StringArray& paths, juce::Point<int> position)
{
    // Files dropped on the master's row, which holds no clips, go on new tracks, as below the tracks.
    const auto row = getRowAt (position.y);
    const auto onTrack = row >= 0 && rows[(size_t) row].track.kind != TrackType::master;
    addFiles (paths, onTrack ? rows[(size_t) row].track.id : TrackID(), getFileDropBeat (position.x));
}

bool Playlist::placeFile (const juce::File& file)
{
    if (! context.hasProject())
        return false;

    const auto tracks = selection.getTracks();
    return addFiles ({ file.getFullPathName() }, tracks.empty() ? TrackID() : tracks.front(), getPlayStartBeat (context));
}

bool Playlist::addFiles (const juce::StringArray& paths, TrackID target, BeatPosition start)
{
    if (! context.hasProject())
        return false;

    const auto added = phazite::importFiles (context, paths, target, start);
    range.clear();
    selection.clear();

    for (const auto clip : added)
        selection.add (clip);

    if (added.empty())
        return false;

    lastClip = added.front(); // what Draw and Paint place next
    return true;
}

bool Playlist::isInterestedInFileDrag (const juce::StringArray& paths)
{
    return std::any_of (paths.begin(), paths.end(), [this] (const juce::String& path) { return canImport (context, juce::File (path)); });
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
    if (! context.hasProject())
        return false;

    if (const auto file = getDraggedFile (details.description))
        return canImport (context, *file);

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

    if (! context.hasProject())
        return;

    // A device dropped below the tracks goes on a new track (an instrument), or on the master (an effect).
    const auto position = lanes->getLocalPoint (this, details.localPosition);
    const auto row = getRowAt (position.y);
    const auto track = row >= 0 ? rows[(size_t) row].track.id : TrackID();
    auto placed = false;

    if (const auto file = getDraggedFile (details.description))
        placed = addFiles ({ file->getFullPathName() }, row >= 0 && rows[(size_t) row].track.kind == TrackType::master ? TrackID() : track,
                           getFileDropBeat (std::max (0, position.x)));
    else if (const auto device = getDraggedDevice (details.description))
        placed = addDeviceTo (context, track, *device);

    if (placed)
        markPlaced (details.description);
}
} // namespace phazite::ui
