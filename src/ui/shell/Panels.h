// SPDX-License-Identifier: AGPL-3.0-or-later
// The window's two tabbed areas (docs/design.md, section 4): the whole song in the centre, and the selection, in detail,
// in the editor dock under it.
#pragma once

#include "model/arrangement/PlaylistModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace phazite::ui
{
class AudioClipEditor;
class PianoRoll;
class TimelineEditor;

//==============================================================================
// The centre: the whole song, as the playlist (the clips over time) or the mixer (every channel side by side), chosen by
// the tabs at its top, so that no key needs remembering (F5 and F9 do the same, as in FL Studio). The views belong to the
// window; this only shows them.
class SongViews : public juce::TabbedComponent
{
public:
    enum Tab
    {
        playlist,
        mixer
    };

    SongViews (juce::Component& playlistPanel, juce::Component& mixerPanel);

    void showTab (Tab tab) { setCurrentTabIndex (tab); }
    Tab getTab() const { return getCurrentTabIndex() == mixer ? mixer : playlist; }
};

//==============================================================================
// The editor dock under the centre: the selection, in detail. Its first tab is the editor of the selected clip, named for
// what it shows: the piano roll for MIDI clips, the audio clip editor for audio clips, as the song's selection asks
// (model/arrangement/AudioClipModel.h: getClipTypeToEdit()); its second, the selected track's devices (DevicesView.h). The
// views belong to the window; the dock only shows them.
class EditorDock : public juce::TabbedComponent,
                   private juce::ChangeListener
{
public:
    enum Tab
    {
        clipEditor,
        devices
    };

    EditorDock (ProjectContext&, PianoRoll&, AudioClipEditor&, juce::Component& devicesPanel);
    ~EditorDock() override;

    void showTab (Tab tab) { setCurrentTabIndex (tab); }

    // Opens a clip in the first tab: a MIDI clip in the piano roll, an audio clip in the audio clip editor.
    void openClip (ClipID);

    // The editor the first tab shows, and whether that is the audio clip editor.
    TimelineEditor& getClipEditor() const noexcept;
    bool showsAudioClip() const noexcept { return showingAudio; }

private:
    struct ClipTab; // the first tab: holds both editors, and shows one

    ProjectContext& context;
    PianoRoll& pianoRoll;
    AudioClipEditor& audioClipEditor;
    std::unique_ptr<ClipTab> clipTab;
    bool showingAudio = false;

    void showClipEditor (ClipType);
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // the selection changed: what it asks the first tab to show
};
} // namespace phazite::ui
