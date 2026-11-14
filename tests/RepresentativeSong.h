// SPDX-License-Identifier: AGPL-3.0-or-later
// The representative song: one of each kind of track and every piece of Phazite's data a project keeps. The golden project
// test saves it (tests/GoldenProjectTests.cpp, where it is made: addRepresentativeContent()); the whole window's tests show,
// walk and paint it (AppShellTests.cpp, PaintTests.cpp), so that every kind of row, clip, strip, lane and device is there.
// Its plug-ins are the tests' stand-ins, which must be known before it is made: keep a TestPlugin and a SandboxedPlugins
// alive meanwhile.
#pragma once

#include <juce_core/juce_core.h>

namespace phazite
{
class Project;
class ProjectContext;
} // namespace phazite

namespace phazite::test
{
// Into a project open in a context (its song the context's), with the files it makes (sounds, takes) in scratch. It begins
// with the project's first track, which it renames: a project without one gets one first.
void addRepresentativeSong (Project&, ProjectContext&, const juce::File& scratch);
} // namespace phazite::test
