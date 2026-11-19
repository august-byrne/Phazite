// SPDX-License-Identifier: AGPL-3.0-or-later
#include "WholeFile.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace phazite
{
namespace
{
    // Why a stream failed, in the operating system's words, without the line end Windows puts after them.
    juce::Result getFailure (const juce::FileOutputStream& stream)
    {
        return juce::Result::fail (stream.getStatus().getErrorMessage().trim());
    }

    // The tests' hook in force (WholeFileHook), taken under its lock, as writes come from any thread, and asked outside it.
    std::mutex hookLock;
    std::shared_ptr<const std::function<bool (const juce::File&)>> hookInForce;

    bool isLetThrough (const juce::File& file)
    {
        std::shared_ptr<const std::function<bool (const juce::File&)>> hook;

        {
            const std::scoped_lock lock (hookLock);
            hook = hookInForce;
        }

        return hook == nullptr || (*hook) (file);
    }

    // The folders held (holdFolder()), once for each hold.
    std::mutex heldLock;
    std::vector<juce::File> heldFolders;
} // namespace

void holdFolder (const juce::File& folder, bool held)
{
    const std::scoped_lock lock (heldLock);

    if (held)
        heldFolders.push_back (folder);
    else if (const auto found = std::find (heldFolders.begin(), heldFolders.end(), folder); found != heldFolders.end())
        heldFolders.erase (found);
}

bool isInHeldFolder (const juce::File& file)
{
    const std::scoped_lock lock (heldLock);
    return std::any_of (heldFolders.begin(), heldFolders.end(), [&file] (const auto& folder) { return file.isAChildOf (folder); });
}

WholeFileHook::WholeFileHook (std::function<bool (const juce::File&)> asked)
{
    const std::scoped_lock lock (hookLock);
    jassert (hookInForce == nullptr); // one at a time
    hookInForce = std::make_shared<const std::function<bool (const juce::File&)>> (std::move (asked));
}

WholeFileHook::~WholeFileHook()
{
    const std::scoped_lock lock (hookLock);
    hookInForce = nullptr;
}

juce::Result writeWholeFile (const juce::File& file, const std::function<bool (juce::OutputStream&)>& write)
{
    jassert (! isInHeldFolder (file)); // a first save copies its folder (holdFolder())

    // Beside the file, so that taking its place is a rename on the same drive; deleted as this returns, unless it did.
    const juce::TemporaryFile temporary (file);
    juce::int64 size = 0;

    {
        juce::FileOutputStream out (temporary.getFile());

        if (out.failedToOpen())
            return getFailure (out);

        const auto written = write (out);
        out.flush(); // what the stream still holds, and to the disk what the operating system holds

        if (out.getStatus().failed())
            return getFailure (out);

        if (! written)
            return juce::Result::fail ("Not all of it could be written.");

        size = out.getPosition();
    }

    // A write that took fewer bytes than it was given, without an error.
    if (temporary.getFile().getSize() != size)
        return juce::Result::fail ("Not all of it could be written.");

    if (! isLetThrough (file))
        return juce::Result::fail ("There is not enough space on the disk."); // as a full disk fails it, for a test

    if (! temporary.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("The file couldn't be replaced.");

    return juce::Result::ok();
}

juce::Result writeWholeFile (const juce::File& file, const void* data, size_t size)
{
    return writeWholeFile (file, [data, size] (juce::OutputStream& out) { return size == 0 || out.write (data, size); });
}

juce::File makeFileOfFullPath (const juce::String& path)
{
#if JUCE_WINDOWS
    const auto isSeparator = [] (juce::juce_wchar character) { return character == '\\' || character == '/'; };

    // From a root, with no drive and no server before it: on the current drive, as juce::File has it.
    if (isSeparator (path[0]) && ! isSeparator (path[1]))
        return juce::File (juce::File::getCurrentWorkingDirectory().getFullPathName().substring (0, 2) + path);
#endif

    return juce::File (path);
}
} // namespace phazite

namespace phazite
{
bool isReservedFileName (const juce::String& name)
{
    const auto stem = name.upToFirstOccurrenceOf (".", false, false).trimEnd().toUpperCase();

    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
        return true;

    if (stem.length() != 4 || ! (stem.startsWith ("COM") || stem.startsWith ("LPT")))
        return false;

    const auto last = stem[3];
    return juce::CharacterFunctions::isDigit (last) || last == (juce::juce_wchar) 0x00b9 || last == (juce::juce_wchar) 0x00b2
           || last == (juce::juce_wchar) 0x00b3;
}

juce::String makeUsableFileName (const juce::String& name)
{
    auto legal = juce::File::createLegalFileName (name);

    // Dots alone are a step along a path ("..": the folder above), not a name; with spaces too, as Windows drops a name's
    // trailing dots and spaces (".. " is ".." there).
    if (legal.containsChar ('.') && legal.containsOnly (". "))
        legal = legal.replaceCharacter ('.', '_');

    // A name's trailing dots and spaces left out on every system, as Windows leaves them out (but of spaces alone).
    if (const auto trimmed = legal.trimCharactersAtEnd (". "); trimmed.isNotEmpty())
        legal = trimmed;

    if (! isReservedFileName (legal))
        return legal;

    // After what comes before its first dot, which is what Windows looks at.
    const auto dot = legal.indexOfChar ('.');
    return dot < 0 ? legal.trimEnd() + "_" : legal.substring (0, dot).trimEnd() + "_" + legal.substring (dot);
}
} // namespace phazite
