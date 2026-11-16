// SPDX-License-Identifier: AGPL-3.0-or-later
// What is dropped on the playlist: audio and MIDI files and presets from Explorer, and the browser's sounds, devices and presets.
#include "PlaylistParts.h"
#include "model/mixer/MixerModel.h"
#include "model/presets/PresetModel.h"
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
    // Files dropped on the master's row, which holds no clips, go on new tracks, as below the tracks; presets go where a
    // preset dropped from the browser goes.
    const auto row = getRowAt (position.y);
    const auto onTrack = row >= 0 && rows[(size_t) row].track.kind != TrackType::master;
    const auto track = onTrack ? rows[(size_t) row].track.id : TrackID();
    juce::StringArray sounds;

    for (const auto& path : paths)
    {
        if (isPresetFile (juce::File (path)))
            loadPreset (context, juce::File (path), track);
        else
            sounds.add (path);
    }

    if (! sounds.isEmpty())
        addFiles (sounds, track, getFileDropBeat (position.x));
}

bool Playlist::placeFile (const juce::File& file)
{
    if (! context.hasProject())
        return false;

    const auto tracks = selection.getTracks();
    return addFiles ({ file.getFullPathName() }, tracks.empty() ? TrackID() : tracks.front(), getPlayStartBeat (context), true);
}

bool Playlist::addFiles (const juce::StringArray& paths, TrackID target, BeatPosition start, bool fromBrowser)
{
    if (! context.hasProject())
        return false;

    // Read in the background (known bug 123): the clips come once the files are copied into the project, selected if the
    // selection is still the one of the drop (something other than the user may have changed it meanwhile).
    return importFilesInBackground (context, paths, target, start,
                                    [safeThis = juce::Component::SafePointer<Playlist> (this), clips = selection.getClips(),
                                     tracks = selection.getTracks(), paths, fromBrowser] (const std::vector<ClipID>& added)
                                    {
                                        if (safeThis == nullptr || added.empty())
                                            return;

                                        if (safeThis->selection.getClips() == clips && safeThis->selection.getTracks() == tracks)
                                        {
                                            safeThis->range.clear();
                                            safeThis->selection.clear();

                                            for (const auto clip : added)
                                                safeThis->selection.add (clip);
                                        }

                                        safeThis->lastClip = added.front(); // what Draw and Paint place next

                                        // What came from the browser goes into its recent items now that it is in the song.
                                        if (fromBrowser && safeThis->onBrowserSoundPlaced != nullptr)
                                            for (const auto& path : paths)
                                                safeThis->onBrowserSoundPlaced (juce::File (path));
                                    });
}

bool Playlist::isInterestedInFileDrag (const juce::StringArray& paths)
{
    return std::any_of (paths.begin(), paths.end(), [this] (const juce::String& path)
                        { return canImport (context, juce::File (path)) || isPresetFile (juce::File (path)); });
}

void Playlist::showFileDrop (const juce::File& first, juce::Point<int> position)
{
    // Where importFiles() would put the first file: on the track under the pointer if it suits it, or else on a new track
    // below, which the lanes show and the hint bar says, with why (an audio clip on a track with an instrument went there,
    // silenced by it, until 2026-11-14).
    fileDropRow = getRowAt (position.y);
    fileDropX = std::max (0, position.x);
    fileDropBeat = getFileDropBeat (fileDropX);

    const auto onRow =
        juce::isPositiveAndBelow (fileDropRow, (int) rows.size()) && rows[(size_t) fileDropRow].track.kind != TrackType::master;
    const auto track = onRow ? rows[(size_t) fileDropRow].track.id : TrackID();
    fileDropOnNewTrack = onRow && canImport (context, first) && ! importsOnto (context, first, track); // not a preset

    if (fileDropOnNewTrack)
    {
        const auto midi = first.hasFileExtension ("mid;midi");
        const auto row = rows[(size_t) fileDropRow].track;
        const auto why = row.kind == TrackType::returnTrack ? juce::String ("a return holds no clips")
                         : row.kind == TrackType::folder    ? juce::String ("a folder holds no clips")
                         : midi                             ? juce::String ("notes can't go on a track of audio")
                                                            : juce::String ("audio can't go on a track of notes or with an instrument");
        setDragReadout ("Goes on a new track below " + row.name + ": " + why);
    }
    else
    {
        setDragReadout ({});
    }

    lanes->repaint();
}

void Playlist::fileDragMove (const juce::StringArray& paths, int x, int y)
{
    showFileDrop (juce::File (paths[0]), lanes->getLocalPoint (this, juce::Point<int> (x, y)));
}

void Playlist::fileDragExit (const juce::StringArray&)
{
    fileDropBeat.reset();
    fileDropOnNewTrack = false;
    setDragReadout ({});
    lanes->repaint();
}

void Playlist::filesDropped (const juce::StringArray& paths, int x, int y)
{
    fileDragExit (paths);
    importFiles (paths, lanes->getLocalPoint (this, juce::Point<int> (x, y)));
}

bool Playlist::isInterestedInDragSource (const SourceDetails& details)
{
    if (! context.hasProject())
        return false;

    if (const auto file = getDraggedFile (details.description))
        return canImport (context, *file) || isPresetFile (*file);

    return getDraggedDevice (details.description).has_value();
}

void Playlist::itemDragMove (const SourceDetails& details)
{
    // Where it would go, as for files from Explorer: a file at a beat, a device or a preset onto a track.
    const auto position = lanes->getLocalPoint (this, details.localPosition);
    deviceDropping = getDraggedDevice (details.description).has_value() || getDraggedPreset (details.description).has_value();

    if (const auto file = getDraggedFile (details.description); file.has_value() && ! deviceDropping)
    {
        showFileDrop (*file, position);
        return;
    }

    fileDropRow = getRowAt (position.y);
    fileDropX = std::max (0, position.x);
    fileDropBeat.reset();
    fileDropOnNewTrack = false;
    lanes->repaint();
}

void Playlist::itemDragExit (const SourceDetails&)
{
    fileDropBeat.reset();
    fileDropOnNewTrack = false;
    deviceDropping = false;
    setDragReadout ({});
    lanes->repaint();
}

void Playlist::itemDropped (const SourceDetails& details)
{
    itemDragExit (details);

    if (! context.hasProject())
        return;

    // A device dropped below the tracks goes on a new track (an instrument), or on the master (an effect); a preset too,
    // and a track's preset makes a new track after the one it is dropped on, or at the end.
    const auto position = lanes->getLocalPoint (this, details.localPosition);
    const auto row = getRowAt (position.y);
    const auto track = row >= 0 ? rows[(size_t) row].track.id : TrackID();
    auto placed = false;

    if (const auto preset = getDraggedPreset (details.description))
        placed = loadPreset (context, *preset, row >= 0 && rows[(size_t) row].track.kind == TrackType::master ? TrackID() : track);
    else if (const auto file = getDraggedFile (details.description))
    {
        // Noted as placed once its clip is in the song (onBrowserSoundPlaced), not as it is dropped.
        addFiles ({ file->getFullPathName() }, row >= 0 && rows[(size_t) row].track.kind == TrackType::master ? TrackID() : track,
                  getFileDropBeat (std::max (0, position.x)), true);
    }
    else if (const auto device = getDraggedDevice (details.description))
        placed = addDeviceTo (context, track, *device);

    if (placed)
        markPlaced (details.description);
}
} // namespace phazite::ui
