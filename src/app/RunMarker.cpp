// SPDX-License-Identifier: AGPL-3.0-or-later
#include "RunMarker.h"

namespace phazite
{
RunMarker::RunMarker (juce::File f)
    : file (std::move (f)),
      lastRunEndedBadly (file.existsAsFile())
{
    file.getParentDirectory().createDirectory();
    file.replaceWithText ("Phazite is running, or didn't close properly.");
}

RunMarker::~RunMarker()
{
    file.deleteFile();
}
} // namespace phazite
