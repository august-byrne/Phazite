// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's items as the UI and the model's public functions name them: by IDs, which stay the same while the project
// is open and when it is saved and opened again (Tracktion's EditItemIDs, saved with each item), never by Tracktion's
// objects (docs/design.md, section 24, rule 4). An ID of 0 names nothing. Each kind of item has an ID type of its own,
// so that a clip's ID can't go where a track's is meant.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace phazite
{
template <typename Kind>
class ItemID
{
public:
    constexpr ItemID() noexcept = default;

    static constexpr ItemID fromRaw (std::uint64_t raw) noexcept
    {
        ItemID id;
        id.raw = raw;
        return id;
    }

    constexpr std::uint64_t getRaw() const noexcept { return raw; }
    constexpr bool isValid() const noexcept { return raw != 0; }

    constexpr auto operator<=> (const ItemID&) const noexcept = default;

private:
    std::uint64_t raw = 0;
};

using TrackID = ItemID<struct TrackKind>;     // a track, a folder or a return track
using ClipID = ItemID<struct ClipKind>;       // an audio or MIDI clip
using SectionID = ItemID<struct SectionKind>; // a section of the song (model/arrangement/internal/Sections.h)
using DeviceID = ItemID<struct DeviceKind>;   // a device on a track or on the master, or a send

// A note, among its clip's notes: no other note of that clip has its ID, but a copy of the clip (an alias too) keeps it
// (model/notes/internal/Notes.h). Not one of Tracktion's EditItemIDs, so a note is named by its clip and its ID together.
using NoteID = ItemID<struct NoteKind>;

// A note by its clip and its ID in it.
struct NoteRef
{
    ClipID clip {};
    NoteID note {};

    constexpr bool isValid() const noexcept { return clip.isValid() && note.isValid(); }
    constexpr auto operator<=> (const NoteRef&) const noexcept = default;
};
} // namespace phazite

template <typename Kind>
struct std::hash<phazite::ItemID<Kind>>
{
    std::size_t operator() (phazite::ItemID<Kind> id) const noexcept { return std::hash<std::uint64_t>() (id.getRaw()); }
};

template <>
struct std::hash<phazite::NoteRef>
{
    std::size_t operator() (const phazite::NoteRef& ref) const noexcept
    {
        return std::hash<std::uint64_t>() (ref.clip.getRaw() * 0x9e3779b97f4a7c15ull + ref.note.getRaw());
    }
};
