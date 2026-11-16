// SPDX-License-Identifier: AGPL-3.0-or-later
// The window's one hold on its input (the model's InputHolder, model/project/ProjectContext.h), for whatever must
// keep the user from changing the project while it waits: a command waiting for the plug-ins' states (ProjectSession's
// gate), the work of a command that reads much of an audio file, and whatever else takes it (a ProjectContext's hold for
// work to come: each holdInput() is matched by a releaseInput()).
//
// What it stops: every mouse event and key on every window of Phazite. It is a modal component of its own that never
// shows (JUCE keeps input from every component under a modal one): an invisible speck in the window it is given, which
// takes the keys there unless a text field has them (so that a value half typed into a field isn't committed: the field
// keeps its edit, and gets the keys again after; its keys meanwhile, Space among them, do nothing).
//
// What it passes: the keys of the few commands that change nothing in the song, its history or its files, those that run
// while a mouse button is down too (play and stop, the tools, the editors' zoom: setWindow()'s passKey,
// ui::KeyDispatcher::keyPressedWhileHeld()), so that a recording or the song can be stopped while it holds; and every
// key's going down or up to keysChanged (ui::KeyDispatcher::keyStateChanged()), so that one of those keys let go and
// pressed again runs again. Escape is none of them: it is taken and does nothing until a progress window shows.
//
// Holds are counted: it is let go of at the releaseInput() that matches the last holdInput(), so a hold begun while
// another holds keeps it held across both. One begun only after another was released holds afresh: ProjectSession's wait
// for the plug-ins' states lets go before it runs its command, and an import that command starts holds again in the same
// call, nothing run between. A progress window that shows after a second (the render host's, TrackRenderHost) is modal
// above it and takes its own input (its Cancel). It says nothing while it holds: no sound or window brought forward when
// input is tried, nothing for a screen reader (the progress window speaks for the wait).
//
// A quit doesn't count it as a dialog to finish first (isHold(): Main.cpp, ProjectSession::isHoldingInput()). As it ends,
// every window hears that its keys changed, so that a key let go meanwhile isn't taken for one still held (a stuck note
// of the computer keyboard, a shortcut ignored at its next press); and the keyboard's focus goes back where it was, if
// nothing else took it meanwhile.
//
// For work of the model's own that holds it (a ProjectContext's hold for work to come, an audio file's work): the model
// reaches it as an InputHolder through the context (ProjectContext::setInputHolder(), which MainWindow gives this and which
// hands it to the audio work too); their progress windows go through MainWindow's render queue
// (TrackRenderHost::runRender()), so that two never show at once, one waiting for the other's to end; Escape before a
// progress window shows cancels the newest work that holds it (ProjectSession::cancelHeldWork(), from setWindow()'s
// passKey); and a quit cancels every kind at once (ProjectSession::cancelWork()), waiting for none of their reads.
#pragma once

#include "model/project/ProjectContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace phazite
{
class InputHold final : public InputHolder
{
public:
    InputHold();
    ~InputHold() override; // let go, if it holds

    // Held from the first holdInput() to the releaseInput() that matches the last.
    void holdInput() override;
    void releaseInput() override;
    bool isHolding() const noexcept { return holds > 0; }

    // Whether a modal component is a hold, which a quit and an open dialog's checks don't wait for.
    static bool isHold (const juce::Component*);

    // The window it holds, in which it takes the keys while it holds, what it lets through of them, and what hears their
    // going down and up (see above). Without one, every key is held.
    void setWindow (juce::Component*, std::function<bool (const juce::KeyPress&)> passKey,
                    std::function<void (bool isKeyDown)> keysChanged = nullptr);

private:
    struct Holder;
    std::unique_ptr<Holder> holder;
    int holds = 0;
    juce::Component::SafePointer<juce::Component> window, focusBefore;
    std::function<bool (const juce::KeyPress&)> passKey;
    std::function<void (bool)> keysChanged;

    JUCE_DECLARE_NON_COPYABLE (InputHold)
};
} // namespace phazite
