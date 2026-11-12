// SPDX-License-Identifier: AGPL-3.0-or-later
// The computer keyboard as a MIDI keyboard.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h> // MidiMessage
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <vector>

namespace phazite::ui
{
// Plays notes from the computer keyboard, laid out as in Ableton Live and Logic Pro: the middle row
// of letters plays the white keys from C (A S D F G H J K L ;) and the row above the black keys
// (W E T Y U O P); Z and X change the octave, C and V the velocity. Keys pressed with Shift, Ctrl,
// Alt or Cmd are shortcuts: as in Ableton Live, Shift and a letter run that letter's single-key
// shortcut while this plays notes (ui::WindowKeys::keyPressedAfterListeners()). Keys typed into a text
// field play nothing, however quickly they are typed.
class ComputerKeyboard : public juce::KeyListener
{
public:
    // sendMessage gets the note-on and note-off messages, on the message thread. isKeyDown says whether a key was down
    // as the key message being handled was sent: the real keyboard's, unless a test gives one.
    explicit ComputerKeyboard (std::function<void (const juce::MidiMessage&)> sendMessage,
                               std::function<bool (int keyCode)> isKeyDown = {});
    ~ComputerKeyboard() override;

    void setEnabled (bool);
    bool isEnabled() const noexcept { return enabled; }

    // The note that A plays: middle C (C4, MIDI note 60) to start with. Z and X move it by octaves.
    int getLowestNote() const noexcept { return lowestNote; }

    // The velocity of new notes, from 1 to 127: 100 to start with. C and V change it by 20.
    int getVelocity() const noexcept { return velocity; }

    // Ends the notes that are held, for example when the window loses the keyboard focus.
    void releaseAll();

    // Starts and ends notes, and changes the octave or velocity, from which keys are down. A key acts
    // when it goes down, unless a shortcut modifier is held then. Returns true if a note started or
    // ended, or the octave or velocity changed. keyStateChanged() calls it with the real keyboard.
    //
    // Without `keyWentDown` (a key was let go, or a text field has the keys), keys that went down since
    // went somewhere else, into the text field: they act neither now nor later while they stay down, and
    // the notes of keys let go still end.
    bool update (const std::function<bool (int keyCode)>& keyIsDown, juce::ModifierKeys, bool keyWentDown = true);

    // Whether a key is one it plays notes with or changes the octave or velocity with, while it is enabled: a layout
    // key without Shift, Ctrl, Alt or Cmd.
    bool takesKey (const juce::KeyPress&) const;
    std::vector<juce::KeyPress> getKeys() const; // those keys, without modifiers: none while it is off

    std::function<void()> onChange;     // isEnabled(), the octave or the velocity changed
    std::function<void()> onNotePlayed; // a note started, after its note-on was sent

    //==============================================================================
    bool keyPressed (const juce::KeyPress&, juce::Component*) override;
    bool keyStateChanged (bool isKeyDown, juce::Component*) override;

private:
    static constexpr const char* noteKeys = "awsedftgyhujkolp;"; // semitones 0 to 16 above the lowest note
    static constexpr int numNoteKeys = 17;
    static constexpr std::array<char, 4> controlKeys { 'z', 'x', 'c', 'v' };
    static constexpr int midiChannel = 1;

    std::function<void (const juce::MidiMessage&)> sendMessage;
    std::function<bool (int keyCode)> isKeyDown;
    bool enabled = true;
    int lowestNote = 60, velocity = 100;
    std::array<int, numNoteKeys> heldNotes;        // the note each key is playing, or -1
    std::array<bool, numNoteKeys> noteKeysDown {}; // as of the last update(), to see when a key goes down
    std::array<bool, 4> controlKeysDown {};

    static bool isShortcut (juce::ModifierKeys);
    static bool isTyping(); // a text field has the keys
    void changed();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ComputerKeyboard)
};
} // namespace phazite::ui
