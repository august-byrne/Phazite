// SPDX-License-Identifier: AGPL-3.0-or-later
// The transport and the song's tempo as the top bar shows them (docs/design.md, section 4), by plain values
// (architecture phase A3: no Tracktion here). Each function takes the open project's context, and gives nothing, or
// does nothing, while no project is open. The changes are undo steps of their own, as Tempo.h names them.
#pragma once

#include "TempoValues.h"
#include "model/base/IDs.h"

#include <memory>
#include <optional>

namespace phazite
{
class ProjectContext;

bool isPlaying (const ProjectContext&);
bool isLoopOn (const ProjectContext&); // the loop button's switch (Playback.h)

std::optional<TimeSignature> getTimeSignature (const ProjectContext&);
void setTimeSignature (ProjectContext&, TimeSignature);

// The track that what is played on a MIDI keyboard or the computer keyboard goes to, and records onto
// (model/engine/internal/LiveInput.h); an empty ID for none.
TrackID getLiveMidiTrack (const ProjectContext&);

//==============================================================================
// The tempo as a field changes it: a drag is one undo step, "Change Tempo", from its first move to its last (a Gesture,
// model/commands/internal/Undo.h), and a change outside a drag, such as a typed tempo, is one of its own. It forgets a project
// that closes.
class TempoHandle
{
public:
    TempoHandle(); // of nothing
    TempoHandle (TempoHandle&&) noexcept;
    TempoHandle& operator= (TempoHandle&&) noexcept;
    ~TempoHandle(); // ends a drag in progress, keeping its change

    bool isValid() const;
    double getValue() const; // in beats per minute; 120 without a project

    void beginGesture();
    void setValue (double bpm); // within the limits (TempoValues.h), to a hundredth
    void endGesture();
    bool isInGesture() const;

    struct Impl; // the project's Edit
    explicit TempoHandle (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;
};

// The open project's tempo, or a handle of nothing while none is open.
TempoHandle getTempoHandle (ProjectContext&);
} // namespace phazite
