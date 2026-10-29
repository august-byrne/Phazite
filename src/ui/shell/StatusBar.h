// SPDX-License-Identifier: AGPL-3.0-or-later
// The strip along the bottom of the window (docs/design.md, section 4), as VS Code's status bar: on the left, whether the
// project is saved, what is selected and where MIDI goes; in the middle, the hint bar (HintBar.h: what the control under
// the pointer does, or the guide's next step, and alerts such as no audio output); on the right, while clips are being
// stretched in the background, how far that has come, as a bar; the song's delay compensation, when it has some; the audio
// output (its sample rate and delay); and how busy the engine is. Each item says more under the pointer, and a click on
// one does what it is about: saves, or opens the audio or MIDI settings. Items that don't fit leave the hints room, the
// least needed first.
#pragma once

#include "HintBar.h"
#include "model/engine/EngineContext.h"
#include "model/project/ProjectContext.h"
#include "ui/input/CommandBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class StatusBar : public juce::Component,
                  private ProjectContext::Listener,
                  private juce::ChangeListener,
                  private juce::Timer
{
public:
    StatusBar (EngineContext&, CommandBridge&, ProjectContext&);
    ~StatusBar() override;

    HintBar& getHintBar() noexcept { return hintBar; }

    // Whether the open project is saved (the app knows): "Saved", "Unsaved changes", or "Not saved yet" for one that never
    // was; nothing while none is open.
    enum class SaveState
    {
        none,
        saved,
        unsaved,
        neverSaved
    };

    void showSaveState (SaveState);
    void showAudioOutput (const EngineContext::AudioOutput&); // when the audio devices change

    // What the items show now (empty for one that doesn't show), for tests.
    juce::String getSaveText() const;
    juce::String getSelectionText() const;
    juce::String getMidiText() const;
    juce::String getRenderText() const;
    double getRenderProgress() const; // the bar's, from 0 to 1
    juce::String getDelayText() const;
    juce::String getAudioText() const;
    juce::String getCpuText() const;

    // Brings what the timer shows up to date now: the CPU, the renders, where MIDI goes and the delay compensation. For tests.
    void refreshNow() { timerCallback(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Item;

    EngineContext& engine;
    CommandBridge& commands;
    ProjectContext& context;
    HintBar hintBar;
    std::unique_ptr<Item> saveItem, selectionItem, midiItem, renderItem, delayItem, audioItem, cpuItem;
    int shownCpu = -1; // the percentage shown, to repaint only on changes

    void showSelection();
    void showMidi();
    void showRenders();
    void showDelay();

    void projectChanged() override;
    void songChanged (const SongChanges&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection, or the range selected
    void timerCallback() override;                                   // a few times a second
};
} // namespace phazite::ui
