// SPDX-License-Identifier: AGPL-3.0-or-later
// What the song holds of its devices outside its undo history alone, as a value (roadmap step S5.20, the devices' area; held
// in an OutsideValue, OutsideValue.h): where each device's window was (Schema.h: deviceWindowX, deviceWindowY), view state,
// which counts as no change and no step holds (decision 4). The song's tree is still the storage, and the song holds no such
// value yet: the functions by ID that read and write a window's place on the tree are in model/plugins/internal/
// DeviceStore.h (getWindowPlace(), setWindowPlace()), which the store's switch points at this value. immer is the model's
// alone (tools/check-architecture.sh: immer-outside-model).
#pragma once

#include "model/base/IDs.h"
#include "model/base/internal/Immer.h"
#include "model/base/internal/MapChanges.h"

#include <cstdint>
#include <optional>

namespace phazite
{
// Where a device's window was: its left and its top on the screen, in pixels.
struct WindowPlace
{
    int x = 0;
    int y = 0;

    bool operator== (const WindowPlace&) const = default;
};

// Each device's window place by the device's ID, of those whose windows moved. Entries of devices the song doesn't hold (one
// a step took away, which its Undo may bring back) are kept, as the tree keeps a removed element's.
struct DevicesOutside
{
    immer::map<std::uint64_t, WindowPlace> windows {};

    // A device's window's place, none for one never moved.
    std::optional<WindowPlace> findWindow (DeviceID id) const
    {
        const auto* found = windows.find (id.getRaw());
        return found != nullptr ? std::optional (*found) : std::nullopt;
    }

    // The same with a device's window's place given, or taken away (none).
    DevicesOutside withWindow (DeviceID id, std::optional<WindowPlace> place) const
    {
        DevicesOutside next;
        next.windows = place.has_value() ? windows.set (id.getRaw(), *place) : windows.erase (id.getRaw());
        return next;
    }

    // The same without the places of devices whose IDs are at or above first, one walk of the map; the very map where it holds
    // none. What the song as copied drops as its count of IDs is set back to the copy's (model/versions/ApplySong.cpp:
    // makeSongAsCopied()): no step its history holds can bring such a device back.
    DevicesOutside withoutIDsFrom (std::uint64_t first) const
    {
        DevicesOutside kept;
        kept.windows = phazite::withoutIDsFrom (windows, first);
        return kept;
    }

    // The same map without a look inside (OutsideValue.h); and the same places.
    bool isSameAs (const DevicesOutside& other) const noexcept { return windows.identity() == other.windows.identity(); }
    bool operator== (const DevicesOutside& other) const { return windows == other.windows; }
};
} // namespace phazite
