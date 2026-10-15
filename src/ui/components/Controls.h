// SPDX-License-Identifier: AGPL-3.0-or-later
// Phazite's base controls: knob, fader and level meter, and the link between a slider and a Tracktion
// parameter. ui::LookAndFeel draws them. They work from the keyboard (Tab moves the focus, the arrow
// keys change the value) and tell screen readers their name and value.
#pragma once

#include "model/commands/CommandRegistry.h"
#include "model/devices/ParameterHandle.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include <array>

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

// Shows and reads a slider's value, a Tracktion volume fader position from 0 to 1, in decibels.
void showAsDecibels (juce::Slider&);

// Shows and reads a slider's value, from -1 (left) to 1 (right), as a pan position ("30% left").
void showAsPan (juce::Slider&);

// Shows and reads a slider's value as a value of one of Phazite's devices' parameters, by its unit ("1.20 kHz").
void showAsParameter (juce::Slider&, const devices::ParameterInfo&);

//==============================================================================
// Links a slider to a parameter of a plug-in, such as a track's volume. Moving the slider sets the
// parameter, as one named undo step per drag or key press, which the model's ParameterHandle owns;
// update() shows changes made elsewhere, such as undo or automation. The plug-in may be deleted at any
// time (when a project closes, for example): the slider then turns off.
class ParameterAttachment
{
public:
    ParameterAttachment (juce::Slider&, juce::String undoName);

    // The parameter to control, by plug-in and parameter ID (take the ID from the parameter, such as
    // a VolumeAndPanPlugin's volParam->paramID: the master's differs from a track's); nullptr for none.
    void setParameter (te::Plugin*, const juce::String& parameterID);

    // The parameter to control, as the model hands it out. Its undo steps keep the name the handle has.
    void setParameter (ParameterHandle);

    // Shows the parameter's value. Call it regularly, from a timer.
    void update();

private:
    juce::Slider& slider;
    const juce::String undoName;
    ParameterHandle handle; // owns the undo steps: the slider only reports its drags and values

    void showRange (const ParameterHandle&);
};

//==============================================================================
// A level meter: each channel's peak level from -60 to +6 dB, the highest peak of the last second as
// a line, and red above 0 dB, where the sound clips. It reads a Tracktion LevelMeasurer 30 times a
// second on the message thread, never on the audio thread, and repaints only when the levels change.
class LevelMeter : public juce::Component,
                   private juce::Timer
{
public:
    explicit LevelMeter (const juce::String& title);
    ~LevelMeter() override;

    // The measurer to show, or nullptr. The meter notices when a measurer is deleted.
    void setSource (te::LevelMeasurer*);

    void paint (juce::Graphics&) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    static constexpr int numChannels = 2;
    static constexpr float minDb = -60.0f, maxDb = 6.0f;

    juce::WeakReference<te::LevelMeasurer> source;
    te::LevelMeasurer::Client client;
    std::array<float, numChannels> levels, peaks;
    std::array<juce::uint32, numChannels> peakTimes {};

    juce::String getLevelText() const; // what screen readers read
    void detach();
    void timerCallback() override;
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
    MuteSoloButtons();
    ~MuteSoloButtons() override;

    void setTrack (te::Track*);
    void update(); // shows the track's mute and solo as they are now

    void resized() override;

private:
    struct Button;

    te::SafeSelectable<te::Track> track;
    std::unique_ptr<Button> mute, solo;
};
} // namespace phazite::ui
