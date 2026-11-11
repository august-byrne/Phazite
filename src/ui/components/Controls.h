// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's base controls: knob, fader, level meter and a meter of a device's value, and the links between controls
// and a device's parameters. ui::LookAndFeel draws them. They work from the keyboard (Tab moves the focus, the arrow
// keys change the value) and tell screen readers their name and value.
#pragma once

#include "DisplayTicker.h"
#include "model/base/IDs.h"
#include "model/devices/ParameterHandle.h"
#include "model/mixer/LevelSource.h"
#include "model/modulation/ModulationModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <optional>

namespace phazite::ui
{
//==============================================================================
// What Phazite's sliders do: double-clicking returns to the default value, dragging with Shift held
// moves in fine steps, the value shows while dragging, and Tab and the arrow keys reach and change it.
//
// And a value is typed, exactly (the owner, 2026-11-09: "a tiny bit off is totally wrong" for a pitch), as Ableton Live and
// FL Studio take one: while it has the keys (a click gives them), a number typed opens a field over it with what was typed;
// a click on its readout and Type a Value... in its menu open one with its value, all of it selected. Enter, or a click
// elsewhere, sets what the field says, read by valueFromTextFunction (in its units, "1.2k", "C4"), as a drag's start and
// end, which ParameterAttachment makes one undo step; Esc leaves it as it was, and so does its own value set again. Not a
// slider with two or three values.
class ValueSlider : public juce::Slider
{
public:
    // The title is what screen readers say ("Volume"); textFromValueFunction sets what they read
    // as the value, and what shows while dragging.
    ValueSlider (SliderStyle, const juce::String& title, double defaultValue);
    ~ValueSlider() override;

    // What a right-click shows, such as its parameter's menu (ParameterAttachment sets it); none by default. A right-click
    // that shows it doesn't move the value.
    std::function<void()> onContextMenu;

    // Opens the field a value is typed in, with this text (as typed, the caret after it) or, with none, the value's text, all
    // of it selected; nothing while it is off or hidden. A slider with a text box of its own (a NumberField) types there. The
    // field while it is open, else nullptr.
    void showValueField (const juce::String& typed = {});
    juce::TextEditor* getValueField() const noexcept;

    // The label that shows its value beside it, which its parent lays out (a device's knob's value under it), or none: a
    // click on it opens the field, over it, and a right-click shows the slider's menu; the pointer is a text cursor over it.
    void setValueReadout (juce::Component*);
    juce::Component* getValueReadout() const noexcept { return readout.getComponent(); }

    // While it is dragged without Shift, it moves in steps of this much from its minimum (a pitch in whole semitones); 0, the
    // default, for none. A drag with Shift, a value typed and automation fall between them.
    void setDragStep (double);

    double snapValue (double attemptedValue, DragMode) override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    void resized() override;

private:
    struct ValueField;
    struct ReadoutClicks;

    bool fineDrag = false;   // Shift was held when the drag started
    bool showedMenu = false; // the press was a right-click that showed the menu
    double dragStep = 0.0;
    std::unique_ptr<ValueField> valueField; // while it is open, and until a message after it closed
    juce::Component::SafePointer<juce::Component> readout;
    std::unique_ptr<ReadoutClicks> readoutClicks;

    bool canTypeValue() const;
    void typeInTextBox (const juce::String& typed);                          // a NumberField's own
    void applyTypedText (const juce::String&);                               // what the field said
    juce::Rectangle<int> getValueFieldTarget (const juce::Component&) const; // what it covers, in that component's coordinates
};

// A rotary control, dragged up and down or left and right. A ring around it shows what modulation does to its value
// (model/modulation/ModulationValues.h): the range it moves it over, and where it is now. Dragging the ring changes the
// modulation's amount, and a modulator dropped on it modulates it (ParameterAttachment does both).
class Knob : public ValueSlider,
             public juce::DragAndDropTarget
{
public:
    Knob (const juce::String& title, double defaultValue);

    // The ring, or none.
    void setModulation (std::optional<ModulationRange>);
    const std::optional<ModulationRange>& getModulation() const noexcept { return modulation; }

    // Dragging the ring: whether a modulation can be dragged as the drag starts, then the change of its amount (a part of
    // the range: up is more, a fifth as much with Shift), and its end.
    std::function<bool()> onModulationDragStart;
    std::function<void (float change)> onModulationDrag;
    std::function<void()> onModulationDragEnd;

    // A modulator dropped on it (ModulatorDrag.h), if it can modulate it.
    std::function<bool (ModulatorID)> canAcceptModulator;
    std::function<void (ModulatorID)> onModulatorDropped;

    // While a modulator is dragged, from the moment the drag starts, every knob it can modulate shows a faint ring all the way
    // round, in every window, so that where it can go shows before the pointer gets there (the owner, 2026-11-01); the one
    // under the pointer, a full one. Called by what a modulator is dragged in (its drag container) as the drag starts, and
    // with nothing as it ends.
    static void showModulatorTargets (std::optional<ModulatorID>);
    bool isModulatorTarget() const noexcept { return target; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    std::optional<ModulationRange> modulation;
    bool draggingRing = false, dropping = false;
    bool target = false; // a modulator dragged now could modulate it

    bool isOnRing (juce::Point<float>) const; // the outer edge, where the ring is
};

// A fader. Dragging moves the value from where it is, wherever the fader was clicked.
class Fader : public ValueSlider
{
public:
    enum class Direction
    {
        vertical,
        horizontal
    };

    Fader (Direction, const juce::String& title, double defaultValue);
};

// Left of a vertical volume fader (a fader's position from 0 to 1, model/mixer/MixerModel.h), the decibels along its travel,
// where the fader puts them (FaderScaleTint). Only for the eye: it takes no clicks, and screen readers read the fader's value.
class FaderScale : public juce::Component
{
public:
    explicit FaderScale (const Fader&); // a fader that outlives it, which its parent lays it out beside

    void paint (juce::Graphics&) override;

private:
    const Fader& fader;
};

// One notch of a mouse wheel, as JUCE reports it (juce::MouseWheelDetails), which a trackpad's smaller movements add up to:
// what scrolls a step. JUCE has no unit for a notch: it reports one as 60/256 on Windows and 50/256 on Linux, where taken
// for Windows' a notch scrolled, zoomed and stepped five sixths as far. JUCE workaround W22 (docs/juce-workarounds.md).
#if JUCE_LINUX
inline constexpr double wheelNotch = 50.0 / 256.0;
#else
inline constexpr double wheelNotch = 60.0 / 256.0;
#endif

// A number that is dragged up and down, scrolled, stepped with the arrow keys, or clicked and typed, such as the top
// bar's tempo: its text is its face. Dragging (a fifth as far with Shift), the wheel and the keys move it in steps;
// typing (in its own text box, which a number typed while it has the keys opens too, as on a knob) takes any value in its
// range, to a hundredth of a step. textFromValueFunction and valueFromTextFunction give it units ("120 BPM"), which screen
// readers read too.
class NumberField : public ValueSlider
{
public:
    NumberField (const juce::String& title, double minimum, double maximum, double step, double defaultValue);

    double getStep() const noexcept { return step; }

    // Another step, which a drag, the wheel and the keys move it by, and a value typed is taken to a hundredth of; its value
    // as it was (a value between the old steps' hundredths kept to the new's).
    void setStep (double newStep);

    double snapValue (double attemptedValue, DragMode) override; // a drag moves in whole steps
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override; // up and right step up, down and left down
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    static constexpr int pixelsPerStep = 2;
    double step;
    double wheelNotches = 0.0; // a trackpad's movements, added up until they make a step
};

// Shows and reads a slider's value, a volume fader's position from 0 to 1 (model/mixer/MixerModel.h), in decibels, and gives
// the slider that range.
void showAsDecibels (juce::Slider&);

// Shows and reads a slider's value, from -1 (left) to 1 (right), as a pan position ("30% left").
void showAsPan (juce::Slider&);

// Shows and reads a slider's value as a value of one of Phazite's devices' parameters, by its unit ("1.20 kHz"). One of
// Phazite's sliders with a pitch in semitones is dragged in whole semitones (ValueSlider::setDragStep()), with Shift
// between them.
void showAsParameter (juce::Slider&, const devices::ParameterInfo&);

// Shows and reads a slider's value as an integrated loudness ("-14.0 LUFS"), or as a true peak ("-1.0 dBTP").
void showAsLoudness (juce::Slider&);
void showAsTruePeak (juce::Slider&);

// Shows a button as its window's main one, in the accent colour, or as an ordinary one, in each theme switched to after
// (setThemeColour()).
void setPrimary (juce::TextButton&, bool primary);

//==============================================================================
// Links a slider to a parameter of a device, such as a track's volume. Moving the slider sets the parameter, as one
// named undo step per drag, key press or value typed, which the model's ParameterHandle owns; update() shows changes made
// elsewhere, such as undo or automation. Right-clicking one of Phazite's sliders offers to type a value, and to automate
// the parameter: its lane shows under its track in the playlist. The device may be deleted at any time (when a project
// closes, for example): the slider then turns off.
class ParameterAttachment
{
public:
    explicit ParameterAttachment (juce::Slider&);

    // The parameter to control, as the model hands it out (an empty handle for none). Its undo steps have the name the
    // handle gives them. A handle of the parameter it has already keeps that one, and a drag in progress with it.
    void setParameter (ParameterHandle);

    // The slider shows values as the parameter writes them, and reads values typed so: one with a description (a
    // modulator's, a container's chain's crossover) by its unit, as showAsParameter() does ("1.20 kHz"); a plug-in's as
    // the plug-in writes and reads them ("-6.0 dB"); any other as the model writes and reads it (ParameterHandle::getText()).
    void showDeviceText();

    // Shows the parameter's value, and a knob's modulation. Call it regularly, from a timer.
    void update();

    // What a right-click on one of Phazite's sliders shows: Type a Value..., Automate, Delete Automation and Modulate, or for a
    // parameter that the first change makes (a send to a return), Type a Value... alone. Empty without a parameter.
    juce::PopupMenu getMenu();

private:
    juce::Slider& slider;
    ParameterHandle handle;                         // owns the undo steps: the slider only reports its drags and values
    std::unique_ptr<ModulationDrag> modulationDrag; // while a knob's ring is dragged

    void showRange (const ParameterHandle&);
    void showMenu(); // a right-click's: Automate, Delete Automation, and Modulate
};

// Links a combo box to a parameter of one of Phazite's devices whose value picks one of its choices: the box lists
// them, and picking one is an undo step that the handle owns. update() shows changes made elsewhere.
class ChoiceAttachment
{
public:
    explicit ChoiceAttachment (juce::ComboBox&);

    void setParameter (ParameterHandle);
    void update();

private:
    juce::ComboBox& box;
    ParameterHandle handle;
    float minimum = 0.0f;
};

// Links a toggle button to a parameter that is on or off, in the same way.
class ToggleAttachment
{
public:
    explicit ToggleAttachment (juce::Button&);

    void setParameter (ParameterHandle);
    void update();

private:
    juce::Button& button;
    ParameterHandle handle;
};

//==============================================================================
// A level meter: each channel's peak level from -60 to +6 dB, the highest peak of the last second as
// a line, and red above 0 dB, where the sound clips. It reads its source (model/mixer/LevelSource.h) at each tick of the
// displays' ticker (DisplayTicker.h), on the message thread, never on the audio thread, and repaints only when what it
// draws moves: a silent meter repaints nothing.
class LevelMeter : public juce::Component,
                   private DisplayTicker::Display
{
public:
    explicit LevelMeter (const juce::String& title);
    ~LevelMeter() override;

    // What to show, as the model hands it out (an empty source for nothing). It follows what it measures.
    void setSource (LevelSource);

    void paint (juce::Graphics&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    static constexpr int numChannels = 2;
    static constexpr float minDb = -60.0f, maxDb = 6.0f;

    LevelSource source;
    std::array<float, numChannels> levels, peaks;           // as they fall
    std::array<float, numChannels> shownLevels, shownPeaks; // as drawn
    std::array<juce::uint32, numChannels> peakTimes {};

    juce::String getLevelText() const; // what screen readers read
    void tick() override;
};

//==============================================================================
// A meter of one value that a device's editor reads from its DSP, in decibels: a level, filled from the bottom, or how
// far a device turns the sound down, filled from the top. A line can mark a value, such as a limiter's ceiling.
// Screen readers read its value.
class ValueMeter : public juce::Component
{
public:
    enum class Fill
    {
        fromBottom, // a level, from lowest at the bottom to highest at the top
        fromTop     // a reduction, from lowest at the top down to highest at the bottom
    };

    ValueMeter (const juce::String& title, float lowest, float highest, Fill);

    // Jumps up to what was reached since the last update, or falls back smoothly: call it from a timer.
    void update (float reached);
    float getValue() const noexcept { return value; }

    void setMark (std::optional<float>); // none by default

    // What screen readers read: the value in decibels, a reduction as a negative number.
    juce::String getValueText() const;

    void paint (juce::Graphics&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    static constexpr float fallPerUpdate = 0.5f; // dB: 15 dB a second, at 30 updates a second

    const float lowest, highest;
    const Fill fill;
    float value;
    std::optional<float> mark;
};

//==============================================================================
// A one-line search field over a list: Up and Down go to onNavigationKey, for the list, where a TextEditor would move
// its caret. The command palette, the plug-in picker and the browser have one.
class SearchField : public juce::TextEditor
{
public:
    std::function<bool (const juce::KeyPress&)> onNavigationKey;

    bool keyPressed (const juce::KeyPress& key) override
    {
        return (onNavigationKey != nullptr && onNavigationKey (key)) || juce::TextEditor::keyPressed (key);
    }
};
} // namespace phazite::ui
