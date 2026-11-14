// SPDX-License-Identifier: AGPL-3.0-or-later
#include "WholeFile.h"

namespace phazite
{
namespace
{
    // Why a stream failed, in the operating system's words, without the line end Windows puts after them.
    juce::Result getFailure (const juce::FileOutputStream& stream)
    {
        return juce::Result::fail (stream.getStatus().getErrorMessage().trim());
    }
} // namespace

juce::Result writeWholeFile (const juce::File& file, const std::function<bool (juce::OutputStream&)>& write)
{
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

    if (! temporary.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("The file couldn't be replaced.");

    return juce::Result::ok();
}

juce::Result writeWholeFile (const juce::File& file, const void* data, size_t size)
{
    return writeWholeFile (file, [data, size] (juce::OutputStream& out) { return size == 0 || out.write (data, size); });
}
} // namespace phazite
