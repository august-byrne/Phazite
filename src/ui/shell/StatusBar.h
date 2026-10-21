// SPDX-License-Identifier: AGPL-3.0-or-later
// The strip along the bottom of the window (docs/design.md, section 4), as VS Code's status bar: on the left, whether the
// project is saved; in the middle, the hint bar (HintBar.h: what the control under the pointer does, or the guide's next
// step, and alerts such as no audio output); on the right, the audio output (its sample rate and delay) and how busy the
// engine is. Each item says more under the pointer, and a click on one does what it is about: saves, or opens the audio
// settings.
#pragma once

#include "HintBar.h"
#include "model/engine/EngineContext.h"
#include "ui/input/CommandBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
class StatusBar : public juce::Component,
                  private juce::Timer
{
public:
    StatusBar (EngineContext&, CommandBridge&);
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

    // What the items show now, for tests.
    juce::String getSaveText() const;
    juce::String getAudioText() const;
    juce::String getCpuText() const;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Item;

    EngineContext& engine;
    CommandBridge& commands;
    HintBar hintBar;
    std::unique_ptr<Item> saveItem, audioItem, cpuItem;
    int shownCpu = -1; // the percentage shown, to repaint only on changes

    void timerCallback() override; // the CPU, a few times a second
};
} // namespace phazite::ui
