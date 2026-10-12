// SPDX-License-Identifier: AGPL-3.0-or-later
// The main window's panels. Most are placeholders until the roadmap step that builds them.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace phazite::ui
{
//==============================================================================
// A titled panel that says what will live there, and in which roadmap step.
class Panel : public juce::Component
{
public:
    Panel (const juce::String& title, const juce::String& placeholder);

    void paint (juce::Graphics&) override;

private:
    juce::String placeholder;
};

//==============================================================================
// The editor dock under the playlist: tabs for the piano roll, audio editor, automation and mixer.
class EditorDock : public juce::TabbedComponent
{
public:
    enum Tab
    {
        pianoRoll,
        audio,
        automation,
        mixer
    };

    EditorDock();

    void showTab (Tab tab) { setCurrentTabIndex (tab); }
};
} // namespace phazite::ui
