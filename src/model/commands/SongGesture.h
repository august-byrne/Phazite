// SPDX-License-Identifier: AGPL-3.0-or-later
// A change of the open project's song that spans several events, such as a drag of the editors' tools: one named undo
// step from its start to its end, which the model's edits made meanwhile join (Undo.h's Gesture), without Tracktion's
// types (architecture phase A3). Message thread only.
#pragma once

#include <juce_core/juce_core.h>

#include <memory>

namespace phazite
{
class ProjectContext;

class SongGesture
{
public:
    // Opens the step for the project open now; with none open, it is closed from the start.
    SongGesture (ProjectContext&, const juce::String& name);
    ~SongGesture(); // keeps what it changed

    void commit();   // keeps what it changed, as one step
    void rollBack(); // takes it all back, leaving nothing to redo (Escape during a drag)
    bool isOpen() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (SongGesture)
};
} // namespace phazite
