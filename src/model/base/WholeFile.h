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

// A folder nothing may write into for now: a project's Versions folder while a first save copies it, from the copy's start
// until it is over (VersionHistory::copyFolder()), as what was written after the copy would be lost with the folder the
// project leaves. Every write into a file inside a held folder asserts in Debug, where a JUCE assertion fails its test: here
// in writeWholeFile(), which every file of the versions' store is written with, and in the store's two writers that append
// (PieceStore::joinPacks(), VersionStore::logBranchMove()), so that a writer added later that forgets the history's hold is
// found. Any thread.
void holdFolder (const juce::File& folder, bool held);
bool isInHeldFolder (const juce::File& file);

// For tests: while one lives, `asked` is asked about each write of a whole file, on the writing thread, once what is written
// is whole in the temporary file beside it and before that takes the file's place: false makes the write fail, as a full
// disk does, leaving the file as it was; and it may take its time, to make a writer slow. One at a time.
class WholeFileHook
{
public:
    explicit WholeFileHook (std::function<bool (const juce::File&)> asked);
    ~WholeFileHook();

private:
    JUCE_DECLARE_NON_COPYABLE (WholeFileHook)
};

// The file of a full path that a file holds (a project's, a preset's, a version's: File::isAbsolutePath() says it is one),
// as juce::File makes it. On Windows a path from a root with no drive ("\Sounds\Kick.wav", or a full path of a Mac's or
// Linux's) counts as full too, and juce::File asserts before it takes it for one on the current drive: here it is that
// file without the assertion, as such a path is nothing Phazite did wrong. JUCE workaround W37 (docs/juce-workarounds.md).
juce::File makeFileOfFullPath (const juce::String& path);
} // namespace phazite
