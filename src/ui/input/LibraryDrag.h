// SPDX-License-Identifier: AGPL-3.0-or-later
// What an item dragged from the browser carries, in memory, to whatever it is dropped on: the playlist, a mixer
// channel or the browser itself.
#pragma once

#include "services/Library.h"

namespace phazite::ui
{
// An item dragged from the browser: its description says what it is, so that whatever it is dropped on can tell (a file,
// by its path, or a device, by its plug-in description), without asking the library.
juce::var toDragDescription (const LibraryItem&);
std::optional<juce::File> getDraggedFile (const juce::var& description);
std::optional<DeviceType> getDraggedDevice (const juce::var& description);
juce::String getDraggedKey (const juce::var& description); // the item's key (LibraryItem::getKey()), or empty

// Whatever put a dropped item into the song marks its description, so that the browser can note the item as used.
void markPlaced (const juce::var& description);
bool wasPlaced (const juce::var& description);
} // namespace phazite::ui
