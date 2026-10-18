// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Panels.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/project/ProjectContext.h"
#include "ui/audioclip/AudioClipEditor.h"
#include "ui/pianoroll/PianoRoll.h"

namespace phazite::ui
{
namespace
{
    constexpr int tabBarDepth = 32;
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

EditorDock::EditorDock (ProjectContext& projectContext, PianoRoll& notes, AudioClipEditor& audio, juce::Component& devicesPanel)
    : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop),
      context (projectContext),
      pianoRoll (notes),
      audioClipEditor (audio),
      clipTab (std::make_unique<ClipTab> (notes, audio))
{
    setTitle ("Editor");
    setTabBarDepth (tabBarDepth);
    setOutline (0);

    const auto background = juce::Colours::transparentBlack; // each panel paints its own

    // Automation has no tab: it is edited in its lanes, under its tracks in the playlist (docs/design.md, section 10).
    addTab ("Piano roll", background, clipTab.get(), false);
    addTab ("Devices", background, &devicesPanel, false);
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
}

void EditorDock::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (const auto type = getClipTypeToEdit (context))
        showClipEditor (*type);
}
} // namespace phazite::ui
