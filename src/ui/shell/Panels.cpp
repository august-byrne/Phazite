// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Panels.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/project/ProjectContext.h"
#include "ui/audioclip/AudioClipEditor.h"
#include "ui/input/CommandUI.h"
#include "ui/mixer/DevicesView.h"
#include "ui/pianoroll/PianoRoll.h"
#include "ui/theme/DesignTokens.h"

namespace phazite::ui
{
namespace
{
    constexpr int tabBarDepth = 32;

    // A panel's tabs end before its buttons (PanelLayout.h), in the bar's top right corner; where they don't all fit, the
    // tab bar shows the rest in its menu.
    void leaveCorner (juce::TabbedComponent& tabs, int buttons)
    {
        auto& bar = tabs.getTabbedButtonBar();
        bar.setBounds (bar.getBounds().withTrimmedRight (Shell::getPanelCornerWidth (buttons) + Space::xs));
    }
} // namespace

SongViews::SongViews (juce::Component& playlistPanel, juce::Component& mixerPanel)
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop)
{
    setTitle ("Song");
    setTabBarDepth (tabBarDepth);
    setOutline (0);

    const auto background = juce::Colours::transparentBlack; // each view paints its own
    addTab ("Playlist", background, &playlistPanel, false);
    addTab ("Mixer", background, &mixerPanel, false);
}

void SongViews::resized()
{
    juce::TabbedComponent::resized();
    leaveCorner (*this, 2); // maximize and hide
}

void SongViews::refreshTooltips (const CommandBridge& commands)
{
    auto& bar = getTabbedButtonBar();
    bar.getTabButton (playlist)->setTooltip (
        withShortcut (commands, "Playlist: the song's tracks and their clips, over time", "view.playlist"));
    bar.getTabButton (mixer)->setTooltip (
        withShortcut (commands, "Mixer: a strip for each track, with its devices, sends, pan and fader", "view.mixer"));
}

//==============================================================================
struct EditorDock::ClipTab : public juce::Component
{
    ClipTab (juce::Component& shown, juce::Component& hidden)
    {
        setTitle ("Clip editor");
        addAndMakeVisible (shown);
        addChildComponent (hidden);
    }

    void resized() override
    {
        for (auto* editor : getChildren())
            editor->setBounds (getLocalBounds());
    }
};

EditorDock::EditorDock (ProjectContext& projectContext, PianoRoll& notes, AudioClipEditor& audio, DevicesView& devices)
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop),
      context (projectContext),
      pianoRoll (notes),
      audioClipEditor (audio),
      devicesView (devices),
      clipTab (std::make_unique<ClipTab> (notes, audio))
{
    setTitle ("Editor");
    setTabBarDepth (tabBarDepth);
    setOutline (0);

    const auto background = juce::Colours::transparentBlack; // each panel paints its own

    // Automation has no tab: it is edited in its lanes, under its tracks in the playlist (docs/design.md, section 10).
    addTab ("Piano roll", background, clipTab.get(), false);
    addTab ("Instrument", background, &devicesView, false); // the same view in both: the tab chosen picks its page
    addTab ("Effects", background, &devicesView, false);
    context.getSongSelection().addChangeListener (this);
}

EditorDock::~EditorDock()
{
    context.getSongSelection().removeChangeListener (this);
}

void EditorDock::openClip (ClipID clip)
{
    if (const auto info = getClip (context, clip))
    {
        if (info->kind == ClipType::audio)
            audioClipEditor.showClip (clip);
        else
            pianoRoll.showClip (clip);

        showClipEditor (info->kind);
    }
}

TimelineEditor& EditorDock::getClipEditor() const noexcept
{
    if (showingAudio)
        return audioClipEditor;

    return pianoRoll;
}

void EditorDock::showClipEditor (ClipType type)
{
    showingAudio = type == ClipType::audio;
    pianoRoll.setVisible (! showingAudio);
    audioClipEditor.setVisible (showingAudio);
    setTabName (clipEditor, showingAudio ? "Audio clip" : "Piano roll");
    refreshClipTabTooltip();
}

void EditorDock::refreshTooltips (const CommandBridge& commands)
{
    shortcuts = &commands;
    auto& bar = getTabbedButtonBar();
    bar.getTabButton (instrument)
        ->setTooltip (
            withShortcut (commands, "Instrument: the selected track's instrument, with its modulators and macros", "view.instrument"));
    bar.getTabButton (effects)->setTooltip (
        withShortcut (commands, "Effects: the selected track's effects, as cards, one open at a time", "view.effects"));
    refreshClipTabTooltip();
}

void EditorDock::refreshClipTabTooltip()
{
    if (shortcuts == nullptr)
        return;

    getTabbedButtonBar()
        .getTabButton (clipEditor)
        ->setTooltip (showingAudio ? juce::String ("Audio clip: the selected audio clip's edges, fades, gain, pitch and tempo")
                                   : withShortcut (*shortcuts, "Piano roll: the notes of the selected clips", "view.pianoRoll"));
}

void EditorDock::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (const auto type = getClipTypeToEdit (context))
        showClipEditor (*type);
}

void EditorDock::resized()
{
    juce::TabbedComponent::resized();
    leaveCorner (*this, 3); // maximize, its own window and hide
}

void EditorDock::currentTabChanged (int index, const juce::String&)
{
    if (index == instrument || index == effects)
        devicesView.showPage (index == instrument ? DevicesView::Page::instrument : DevicesView::Page::effects);
}
} // namespace phazite::ui
