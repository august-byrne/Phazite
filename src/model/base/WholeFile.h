// SPDX-License-Identifier: AGPL-3.0-or-later
// A file written whole or not at all: what the project file, the autosave and the versions' files are written with, so
// that a write that fails (a full disk, a drive that went) never leaves a file cut short where a whole one was, and never
// passes for one that worked.
#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace phazite
{
// Replaces a file with what `write` writes into the stream it is given, or leaves the file as it was (none, if there was
// none): written into a temporary file beside it, flushed to the disk, and put in the file's place only once every write
// worked and the temporary file holds all of it. Fails, with the reason (the operating system's, where it gave one), and
// leaves no temporary file behind, if `write` returns false, if anything couldn't be written or if the file couldn't be
// replaced. The file's folder must exist. Any thread.
//
// JUCE's own File::replaceWithData() and replaceWithText() don't look at whether their write worked before they move what
// was written into place, and say that it did; and a FileOutputStream that closes hands its bytes to the operating system
// without asking for them to reach the disk. JUCE workaround W30 (docs/juce-workarounds.md).
juce::Result writeWholeFile (const juce::File& file, const std::function<bool (juce::OutputStream&)>& write);

// The same, for bytes that are at hand.
juce::Result writeWholeFile (const juce::File& file, const void* data, size_t size);
} // namespace phazite
