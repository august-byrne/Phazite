// SPDX-License-Identifier: AGPL-3.0-or-later
// Freezing a track and bouncing it in place (docs/design.md, section 18; roadmap step 2.11), by IDs, for the UI and the
// commands. Both render the track's sound to a file on a thread of their own, which the app shows in a progress window
// with Cancel (TrackRenderHost):
// - Freeze plays a track from a file in the project's Cache folder instead of its clips and its devices up to its fader,
//   stopping before a device that must hear its sound live (a sidechain, a send before the fader), so that its devices
//   take no CPU. While it is frozen, those devices' controls are off, and changing what it froze (its clips or notes, or
//   a frozen device, by undo too) unfreezes it, so that what plays is always what is shown. Freezing and unfreezing
//   aren't undo steps (a track keeps one frozen file, which a later freeze replaces), as mute and solo aren't.
// - Bounce in Place renders a track through its instrument and devices, before its fader, onto a new audio track under it,
//   in the project's Audio folder, and mutes and collapses the original: one undo step, "Bounce in Place".
// Returns, buses and folders are neither frozen nor bounced.
#pragma once

#include "model/base/IDs.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>

namespace phazite
{
class CommandRegistry;
class ProjectContext;

// Whether a track plays its frozen file.
bool isFrozen (const ProjectContext&, TrackID);

// Whether a device's sound is in its track's frozen file: its controls are off, and its plug-in window shut, until the
// track is unfrozen.
bool isFrozenDevice (const ProjectContext&, DeviceID);

// Why a track can't be frozen, or bounced in place, now, as a sentence; empty if it can.
juce::String getWhyNotFreeze (const ProjectContext&, TrackID);
juce::String getWhyNotBounce (const ProjectContext&, TrackID);

//==============================================================================
// A freeze or a bounce in progress, rendering on a thread of its own while this lives. It stops playback first, and lets
// the project play again when it is done. It calls back on the message thread, once, after the render thread has
// finished, with what came of it; deleting it first cancels it, without a callback, leaving the project as it was.
// Delete it on the message thread; the project must outlive it.
class TrackRender
{
public:
    ~TrackRender();

    float getProgress() const;     // from 0 to 1
    juce::String getTitle() const; // what it does: "Freezing Bass", "Bouncing Bass"

    struct Impl; // the render, and what its thread shares with this (Freeze.cpp)
    explicit TrackRender (std::unique_ptr<Impl>);

private:
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (TrackRender)
};

// Starts freezing a track, which is frozen once its file is rendered, or bouncing it in place, whose undo step comes then.
// Returns nothing, without calling back, if it can't start (getWhyNotFreeze(), getWhyNotBounce()).
std::unique_ptr<TrackRender> startFreeze (ProjectContext&, TrackID, std::function<void (const juce::Result&)> onFinished);
std::unique_ptr<TrackRender> startBounceInPlace (ProjectContext&, TrackID, std::function<void (const juce::Result&)> onFinished);

// Plays a frozen track live again, from its clips and devices. Not an undo step.
void unfreezeTrack (ProjectContext&, TrackID);

//==============================================================================
// What runs a freeze or a bounce while it renders: the app's progress window, with Cancel, or a test's.
class TrackRenderHost
{
public:
    virtual ~TrackRenderHost() = default;

    // Takes a render that has started: it runs while the host keeps it, and deleting it cancels it.
    virtual void runRender (std::unique_ptr<TrackRender>) = 0;

    // The render it runs has finished, or one couldn't start: its title ("Freezing Bass"), and what came of it, with why
    // it failed. The host may delete the render here.
    virtual void renderFinished (const juce::String& title, const juce::Result&) = 0;
};

// The commands' functions, for the selected track (the first, if several are): Freeze Track, or Unfreeze Track on a frozen
// one, and Bounce in Place. Each render ends in the host, which the commands' perform hands it to.
void toggleFreeze (ProjectContext&, TrackRenderHost&);
bool canToggleFreeze (const ProjectContext&);
bool isSelectedTrackFrozen (const ProjectContext&);
juce::String getFreezeCommandName (const ProjectContext&);
void bounceSelectedTrack (ProjectContext&, TrackRenderHost&);
bool canBounceSelectedTrack (const ProjectContext&);

// The commands: track.freeze and track.bounceInPlace.
void addFreezeCommands (CommandRegistry&, ProjectContext&, TrackRenderHost&);
} // namespace phazite
