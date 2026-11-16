// SPDX-License-Identifier: AGPL-3.0-or-later
// The computer keyboard as a MIDI keyboard.
#pragma once

#include "CommandBridge.h"
#include "KeyboardLayout.h"

#include <juce_audio_basics/juce_audio_basics.h> // MidiMessage
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <vector>

namespace phazite::ui
{
// Plays notes from the computer keyboard, laid out as in Ableton Live and Logic Pro: the middle row
// of letters plays the white keys from C (A S D F G H J K L ; on the US layout) and the row above the black keys
// (W E T Y U O P); Z and X change the octave, C and V the velocity. The keys are taken by their places, whatever the
// layout types there (KeyboardLayout.h): on AZERTY the white keys are Q S D F G H J K L, on QWERTZ the octave goes
// down with Y. Keys pressed with Shift, Ctrl, Alt or Cmd are shortcuts: as in Ableton Live, Shift and a letter run that
// letter's single-key shortcut while this plays notes (ui::WindowKeys::keyPressedAfterListeners()). The key that turns it
// off (input.computerKeyboard's) is that command's even where a note's place types it (M, right of L on AZERTY): that
// place plays no note. Keys typed into a text field play nothing, however quickly they are typed.
class ComputerKeyboard : public juce::KeyListener
{
public:
    // sendMessage gets the note-on and note-off messages, on the message thread. The layout says what each place types
    // and whether its key was down as the key message being handled was sent: getSystemLayout() in the app, the US layout
    // or one of their own in tests.
    ComputerKeyboard (std::function<void (const juce::MidiMessage&)> sendMessage, const KeyboardLayout& layout);
    ~ComputerKeyboard() override;

    // The commands whose key that turns it off (input.computerKeyboard's) it leaves to them, even at a note's place. The
    // bridge outlives it.
    void leaveKeysOf (const CommandBridge*);

    // The places' keys as the layout types them, for the help that names them: "A S D F G H J K L ;" for the white keys
    // on the US layout, without a place whose key turns it off.
    juce::String getWhiteKeysText() const { return describe ("asdfghjkl;"); }
    juce::String getBlackKeysText() const { return describe ("wetyuop"); }
    juce::String getOctaveKeysText() const { return describe ("zx").replace (" ", " and "); }
    juce::String getVelocityKeysText() const { return describe ("cv").replace (" ", " and "); }

    void setEnabled (bool);
    bool isEnabled() const noexcept { return enabled; }

    const KeyboardLayout& getLayout() const noexcept { return layout; }

    // The note that A plays: middle C (C4, MIDI note 60) to start with. Z and X move it by octaves.
    int getLowestNote() const noexcept { return lowestNote; }

    // The velocity of new notes, from 1 to 127: 100 to start with. C and V change it by 20.
    int getVelocity() const noexcept { return velocity; }

    // Ends the notes that are held, for example when the window loses the keyboard focus.
    void releaseAll();

    // Starts and ends notes, and changes the octave or velocity, from which keys are down, by their places (named as the
    // US layout types them: 'a' for the first white key). A key acts when it goes down, unless a shortcut modifier is held
    // then. Returns true if a note started or ended, or the octave or velocity changed. keyStateChanged() calls it with
    // the layout's keys.
    //
    // Without `keyWentDown` (a key was let go, or a text field has the keys), keys that went down since
    // went somewhere else, into the text field: they act neither now nor later while they stay down, and
    // the notes of keys let go still end.
    bool update (const std::function<bool (char place)>& placeIsDown, juce::ModifierKeys, bool keyWentDown = true);

    // Whether a key is one it plays notes with or changes the octave or velocity with, while it is enabled: one of its
    // places' keys as the layout types it, without Shift, Ctrl, Alt or Cmd.
    bool takesKey (const juce::KeyPress&) const;
    bool usesKey (const juce::KeyPress&) const;  // the same, on or off: the keys it takes while it is on
    std::vector<juce::KeyPress> getKeys() const; // those keys, without modifiers: none while it is off

    std::function<void()> onChange;     // isEnabled(), the octave or the velocity changed
    std::function<void()> onNotePlayed; // a note started, after its note-on was sent

    //==============================================================================
    bool keyPressed (const juce::KeyPress&, juce::Component*) override;
    bool keyStateChanged (bool isKeyDown, juce::Component*) override;

private:
    static constexpr const char* noteKeys = "awsedftgyhujkolp;"; // the places of semitones 0 to 16 above the lowest note
    static constexpr int numNoteKeys = 17;
    static constexpr std::array<char, 4> controlKeys { 'z', 'x', 'c', 'v' }; // places too
    static constexpr int midiChannel = 1;

    std::function<void (const juce::MidiMessage&)> sendMessage;
    const KeyboardLayout& layout;
    const CommandBridge* commands = nullptr;
    bool enabled = true;
    int lowestNote = 60, velocity = 100;
    std::array<int, numNoteKeys> heldNotes;        // the note each key is playing, or -1
    std::array<bool, numNoteKeys> noteKeysDown {}; // as of the last update(), to see when a key goes down
    std::array<bool, 4> controlKeysDown {};

    bool isLeft (char place) const; // its key turns the computer keyboard off
    bool isLeft (const juce::KeyPress&) const;
    juce::String describe (const char* places) const;
    static bool isShortcut (juce::ModifierKeys);
    static bool isTyping(); // a text field has the keys
    void changed();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ComputerKeyboard)
};
} // namespace phazite::ui
