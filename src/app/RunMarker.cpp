// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RunMarker.h"
#include "model/base/WholeFile.h"

namespace phazite
{
RunMarker::RunMarker (juce::File f)
    : file (std::move (f)),
      lastRunEndedBadly (file.existsAsFile())
{
    // Whole or not at all; one that couldn't be written only leaves the next start's crash check out.
    file.getParentDirectory().createDirectory();
    const juce::String text ("Phazite is running, or didn't close properly.");

    if (const auto written = writeWholeFile (file, text.toRawUTF8(), text.getNumBytesAsUTF8()); written.failed())
        juce::Logger::writeToLog ("Phazite couldn't write its run marker: " + written.getErrorMessage());
}

RunMarker::~RunMarker()
{
    file.deleteFile();
}
} // namespace phazite
