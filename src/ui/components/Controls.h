// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's base controls: knob, fader, level meter and a meter of a device's value, and the links between controls
// and a device's parameters. ui::LookAndFeel draws them. They work from the keyboard (Tab moves the focus, the arrow
// keys change the value) and tell screen readers their name and value.
#pragma once

#include "model/base/IDs.h"
#include "model/devices/ParameterHandle.h"
#include "model/mixer/LevelSource.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <optional>

namespace phazite
{
class ProjectContext;
}

namespace phazite::ui
{
//==============================================================================
// What Phazite's sliders do: double-clicking returns to the default value, dragging with Shift held
// moves in fine steps, the value shows while dragging, and Tab and the arrow keys reach and change it.
class ValueSlider : public juce::Slider
{
public:
    // The title is what screen readers say ("Volume"); textFromValueFunction sets what they read
    // as the value, and what shows while dragging.
    ValueSlider (SliderStyle, const juce::String& title, double defaultValue);

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
    void resized() override;

private:
    bool fineDrag = false; // Shift was held when the drag started
};

// A rotary control, dragged up and down or left and right.
class Knob : public ValueSlider
{
public:
    Knob (const juce::String& title, double defaultValue);
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

// A number that is dragged up and down, scrolled, stepped with the arrow keys, or clicked and typed, such as the top
// bar's tempo: its text is its face. Dragging (a fifth as far with Shift), the wheel and the keys move it in steps;
// typing takes any value in its range, to a hundredth of a step. textFromValueFunction and valueFromTextFunction give
// it units ("120 BPM"), which screen readers read too.
class NumberField : public ValueSlider
{
public:
    NumberField (const juce::String& title, double minimum, double maximum, double step, double defaultValue);

    double getStep() const noexcept { return step; }

    double snapValue (double attemptedValue, DragMode) override; // a drag moves in whole steps
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override; // up and right step up, down and left down
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    static constexpr int pixelsPerStep = 2;
    static constexpr double wheelNotch = 60.0 / 256.0; // one notch of a mouse wheel, as JUCE reports it on Windows
    const double step;
    double wheelNotches = 0.0; // a trackpad's movements, added up until they make a step
};

// Shows and reads a slider's value, a volume fader's position from 0 to 1 (model/mixer/MixerModel.h), in decibels.
void showAsDecibels (juce::Slider&);

// Shows and reads a slider's value, from -1 (left) to 1 (right), as a pan position ("30% left").
void showAsPan (juce::Slider&);

// Shows and reads a slider's value as a value of one of Phazite's devices' parameters, by its unit ("1.20 kHz").
void showAsParameter (juce::Slider&, const devices::ParameterInfo&);

// Shows and reads a slider's value as an integrated loudness ("-14.0 LUFS"), or as a true peak ("-1.0 dBTP").
void showAsLoudness (juce::Slider&);
void showAsTruePeak (juce::Slider&);

// Shows a button as its window's main one, in the accent colour, or as an ordinary one. Call it again when the theme
// changes (lookAndFeelChanged()).
void setPrimary (juce::TextButton&, bool primary);

//==============================================================================
// Links a slider to a parameter of a device, such as a track's volume. Moving the slider sets the parameter, as one
// named undo step per drag or key press, which the model's ParameterHandle owns; update() shows changes made
// elsewhere, such as undo or automation. The device may be deleted at any time (when a project closes, for example):
// the slider then turns off.
class ParameterAttachment
{
public:
    explicit ParameterAttachment (juce::Slider&);

    // The parameter to control, as the model hands it out (an empty handle for none). Its undo steps have the name the
    // handle gives them. A handle of the parameter it has already keeps that one, and a drag in progress with it.
    void setParameter (ParameterHandle);

    // The slider shows values as the device writes them ("-6.0 dB"), for plug-ins that aren't Phazite's own.
    void showDeviceText();

    // Shows the parameter's value. Call it regularly, from a timer.
    void update();

private:
    juce::Slider& slider;
    ParameterHandle handle; // owns the undo steps: the slider only reports its drags and values

    void showRange (const ParameterHandle&);
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
// a line, and red above 0 dB, where the sound clips. It reads its source (model/mixer/LevelSource.h) 30 times a
// second on the message thread, never on the audio thread, and repaints only when the levels change.
class LevelMeter : public juce::Component,
                   private juce::Timer
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
    std::array<float, numChannels> levels, peaks;
    std::array<juce::uint32, numChannels> peakTimes {};

    juce::String getLevelText() const; // what screen readers read
    void timerCallback() override;
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

//==============================================================================
// A track's mute (M) and solo (S) buttons, side by side. Clicking M mutes or unmutes the track; right-clicking it
// solos the track alone, as in FL Studio, and again unsolos it. Clicking S solos the track along with others,
// and Ctrl+click solos it alone. Soloing never changes the mute switches (FL Studio's solo does, a long-standing
// complaint): a track silent because others are soloed shows its M half lit. Neither is an undo step, as in
// many DAWs.
class MuteSoloButtons : public juce::Component
{
public:
    explicit MuteSoloButtons (ProjectContext&);
    ~MuteSoloButtons() override;

    void setTrack (TrackID); // an empty ID for none
    void update();           // shows the track's mute and solo as they are now

    void resized() override;

private:
    struct Button;

    ProjectContext& context;
    TrackID track;
    std::unique_ptr<Button> mute, solo;
};
} // namespace phazite::ui
