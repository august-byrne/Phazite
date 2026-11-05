// SPDX-License-Identifier: AGPL-3.0-or-later
// The project context's insides (model/project/ProjectContext.h), for the model, the app and tests: the open song, what
// is selected in it, which the model's commands act on, the time range selected across tracks, what plays it, and what
// shows its devices' windows. Not for the UI.
#pragma once

#include "model/arrangement/internal/Selection.h"
#include "model/base/IDs.h"
#include "model/base/Song.h"
#include "model/base/internal/Selection.h"

namespace phazite
{
class AutomationWatcher;
class ClipRenders;
class EngineContext;
class PlaybackSession;
class ProjectContext;
class VersionHistory;
struct Clipboard;

Song* findSong (const ProjectContext&); // the open project's song, or nullptr while none is open
Selection& getSelection (ProjectContext&);
const Selection& getSelection (const ProjectContext&);
RangeSelection& getRangeSelection (ProjectContext&);

// The engine it plays on (the app's, model/engine/internal/EngineContext.h), and the open song's session there
// (model/playback/internal/PlaybackSession.h): nullptr while no project is open.
EngineContext& getEngineContext (const ProjectContext&);
PlaybackSession* getSession (const ProjectContext&);

// What remembers the open song's parameter touched last (model/automation/internal/Automation.h): nullptr while no
// project is open.
AutomationWatcher* getAutomationWatcher (const ProjectContext&);

// What renders the open song's audio clips stretched, pitched or reversed (model/arrangement/internal/ClipRenders.h):
// nullptr while no project is open.
ClipRenders* getClipRenders (const ProjectContext&);

// What Cut and Copy keep for Paste (model/arrangement/internal/Clipboard.h), of the open song: empty after another opens.
Clipboard& getClipboard (ProjectContext&);

// The open song's versions (model/versions/internal/VersionHistory.h): nullptr while no project is open.
VersionHistory* getVersionHistory (const ProjectContext&);

// The song open now, or none: ProjectSession's (and tests'), which its project owns, and which outlives its being open
// here. Nothing of the previous song stays selected, and the clipboard is empty. Then tells the listeners.
void setSong (ProjectContext&, Song*);

// What shows the devices' windows: a plug-in's own editor, or Phazite's editor of one of its own devices, each in a window
// of its own. The app's (src/app/PluginWindow.h), which it sets as it starts; without one (as in most tests), no device
// shows a window.
struct DeviceWindowHost
{
    virtual ~DeviceWindowHost() = default;
    virtual void show (DeviceID) = 0;
    virtual bool isOpen (DeviceID) const = 0;
    virtual void close (DeviceID) = 0;
};

void setDeviceWindows (ProjectContext&, DeviceWindowHost*);
DeviceWindowHost* getDeviceWindows (const ProjectContext&);
} // namespace phazite
