// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "TempoStretchPanel.h"
#include "model/mixer/MixerModel.h"
#include "model/project/ProjectContext.h"
#include "model/transport/TransportModel.h"
#include "model/versions/VersionsModel.h"
#include "ui/components/Controls.h"
#include "ui/input/CommandBridge.h"
#include "ui/input/ComputerKeyboard.h"
#include "ui/theme/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace phazite::ui
{
// The transport bar, under the title bar (TitleBar.h): play, loop, the metronome, low latency, the tempo and time
// signature, where the playhead is, the master level and volume, and the computer keyboard. (A key is a track's, so the
// piano roll shows it; how busy the engine is, the status bar.)
class TopBar : public juce::Component,
               private ProjectContext::Listener,
               private juce::Timer
{
public:
    TopBar (CommandBridge&, ComputerKeyboard&, ProjectContext&);
    ~TopBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override; // the record button is red while recording

    // Puts the current shortcuts into the tooltips; call after adding commands or changing keymap.
    void refreshTooltips();

private:
    CommandBridge& commands;
    ComputerKeyboard& computerKeyboard;
    ProjectContext& projectContext;

    // A transport button whose right-click opens its options, rather than clicking it.
    struct OptionsButton : public IconButton
    {
        using IconButton::clicked;
        using IconButton::IconButton;
        std::function<void (bool isMenuClick)> onClicked;

        void clicked (const juce::ModifierKeys& mods) override
        {
            if (onClicked != nullptr)
                onClicked (mods.isPopupMenu());
        }
    };

    IconButton playButton { "Play", Icon::play }, captureButton { "Capture", Icon::capture }, loopButton { "Loop", Icon::loop },
        lowLatencyButton { "Low latency", Icon::lowLatency }, keyboardButton { "Computer keyboard", Icon::keyboard };
    OptionsButton recordButton { "Record", Icon::record }, metronomeButton { "Metronome", Icon::metronome };

    // Record's and the metronome's options, which a right-click also opens: an arrow joined to each, as a split button's
    // (the owner's UX audit, 2026-11-12: nothing showed they were there), as Ableton Live has its metronome's pull-down
    // switch beside it.
    IconButton recordMenuButton { "Record options", Icon::expanded }, metronomeMenuButton { "Metronome options", Icon::expanded };
    NumberField tempoField;               // dragged, scrolled or typed, a drag being one undo step
    TempoHandle tempo;                    // the open project's, which owns those steps
    TempoStretchPanel tempoStretch;       // what a tempo change stretches, under the field while the tempo changes
    juce::uint32 tempoStretchShownAt = 0; // when it was shown, or last still in use

    // While that shows, a press anywhere but on it or the tempo field hides it, as a popup goes (the field may keep the keys).
    struct OutsideClick;
    std::unique_ptr<OutsideClick> outsideClick;
    bool hearsOutsideClicks = false;
    juce::TextButton timeSignatureButton; // a menu of time signatures

    // Where the playhead is (the owner's UX audit, 2026-11-12: it showed nowhere): in bars, beats and sixteenths from 1
    // ("17.3.2"), as Ableton Live's Arrangement Position counts, and in minutes and seconds from the song's start ("1:23.4"), as
    // Logic Pro's display and Bitwig's show both. Each is dragged, scrolled or clicked and typed in, as the tempo is, and moves
    // the playhead there; a drag silences the song meanwhile, as dragging the playhead along a ruler does. While the song
    // plays they follow the playhead at each frame of the display, each repainting only when what it says changes.
    NumberField positionField, timeField;
    juce::VBlankAttachment positionFrames;
    bool pausedForPositionDrag = false;
    BeatPosition playheadAtDrag, playStartAtDrag; // as a drag of the position or the time began, which Escape puts back
    bool draggedWhileRecording = false;           // a drag begun while a take records, which moves the playhead as it ends
    double fieldValueAtDrag = 0.0;
    void updatePosition (std::optional<double> frameMilliseconds = {});
    bool isEditing (const NumberField&) const; // dragged or typed in: what it shows is what is being set
    LevelMeter masterMeter { "Master level" };
    Fader masterVolume { Fader::Direction::horizontal, "Master volume", decibelsToFaderPosition (0.0f) };
    ParameterAttachment masterVolumeAttachment { masterVolume };
    int shownKeyboardState = -1;          // what the keyboard button shows, to update it only on changes
    juce::uint32 shownKeyboardLayout = 0; // and the layout its tooltip names the keys of (KeyboardLayout::getFingerprint())

    // A/B (version.compare): a press hears the last commit's bounce in place of the song, at the same point and as loud, and
    // another the song again; lit while it is heard, or bounced to be. And beside it what is heard ("Hearing: Verse 2"), or
    // bounced to be, while that isn't the song: nothing hidden.
    juce::TextButton compareButton { "A/B" };
    juce::Label hearing;
    void updateCompare();

    void projectChanged() override;  // the tempo, the master volume and the meter follow the open project
    void versionsChanged() override; // which version is heard
    void timerCallback() override;   // updates the readouts a few times a second; only changes repaint
    void updateKeyboardButton();
    void updateTempo();
    void showTempoStretch(); // what the tempo stretches, as it starts to change
    void hideTempoStretch();
    // Record's menu: the recording commands, and a new track for one of the audio device's inputs, armed (as FL Studio's record
    // button asks what to record): what a click on Record opens while nothing would record. Its tooltip says what it records
    // onto (model/transport/RecordingModel.h: describeRecordTargets()), as the tracks' arm switches show it.
    void showRecordMenu();
    juce::String recordTargets;
    void refreshRecordTooltip();
    void showMetronomeMenu();
    void showTimeSignatureMenu();
    bool isCommandTicked (const juce::String& commandID) const;
    bool isCommandEnabled (const juce::String& commandID) const;
};
} // namespace phazite::ui
