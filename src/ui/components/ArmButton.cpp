// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ArmButton.h"
#include "model/transport/RecordingModel.h"
#include "ui/theme/LookAndFeel.h"

#include <algorithm>

namespace phazite::ui
{
ArmButton::ArmButton (ProjectContext& projectContext)
    : juce::Button ("Arm"),
      DisplayTicker::Display (static_cast<juce::Component&> (*this)),
      context (projectContext)
{
    setTitle ("Arm");
    setMouseClickGrabsKeyboardFocus (false);
}

void ArmButton::setTrack (TrackID id)
{
    track = id;
    update();
}

void ArmButton::update()
{
    const auto wasShown = shown;
    shown = track.isValid() && canRecordAudio (context, track);
    const auto armed = shown && isArmed (context, track);
    const auto input = shown ? getTrackInput (context, track) : juce::String();
    const auto changed = armed != getToggleState() || input != shownInput || shown != wasShown;
    shownInput = input;
    setToggleState (armed, juce::dontSendNotification);
    setTooltip (input.isEmpty()
                    ? juce::String ("Arm: choose the input this track records from (a microphone, a guitar...); it records it when "
                                    "Record is pressed")
                    : "Arm: records " + describeInput (input)
                          + " onto this track when Record is pressed. Right-click for "
                            "the input, and when you hear it");

    if (changed)
    {
        repaint();

        if (onChange != nullptr)
            onChange();
    }
}

void ArmButton::tick()
{
    // Cheap, and only while it shows: arming elsewhere shows here.
    const auto armed = shown && isArmed (context, track);

    if (armed != getToggleState() || (shown && getTrackInput (context, track) != shownInput))
        update();
}

void ArmButton::clicked (const juce::ModifierKeys& mods)
{
    if (! shown)
        return;

    if (mods.isPopupMenu() || getTrackInput (context, track).isEmpty())
    {
        showMenu();
        return;
    }

    setArmed (context, track, ! isArmed (context, track));
    update();
}

void ArmButton::showMenu()
{
    if (! shown)
        return;

    // Its items act on the track by ID (a menu that outlives the button still works), and show it after if it is still there.
    auto& projectContext = context;
    const auto id = track;
    const auto done = [safe = juce::Component::SafePointer<ArmButton> (this)]
    {
        if (safe != nullptr)
            safe->update();
    };

    const auto current = getTrackInput (context, track);
    const auto last = getLastInput (context);
    auto inputs = getAudioInputs (context);
    std::stable_partition (inputs.begin(), inputs.end(), [&last] (const AudioInput& input) { return input.id == last; });

    juce::PopupMenu menu;
    menu.addSectionHeader ("Input");

    if (inputs.empty())
        menu.addItem ("No audio inputs: choose an input device and its channels in Options > Audio Settings", false, false, nullptr);

    // Choosing an input arms the track, as choosing one in FL Studio's mixer does.
    for (const auto& input : inputs)
        menu.addItem (input.name, true, input.id == current,
                      [&projectContext, id, inputID = input.id, done]
                      {
                          setTrackInput (projectContext, id, inputID);
                          setArmed (projectContext, id, true);
                          done();
                      });

    menu.addItem ("No input", true, current.isEmpty(),
                  [&projectContext, id, done]
                  {
                      setTrackInput (projectContext, id, {});
                      done();
                  });

    menu.addSectionHeader ("Hear the input");
    const auto monitoring = getInputMonitoring (context, track);

    for (const auto& [name, choice] :
         { std::pair { "While armed", InputMonitoring::whileArmed }, std::pair { "Always", InputMonitoring::always },
           std::pair { "Never (the audio interface plays it to you)", InputMonitoring::never } })
        menu.addItem (name, true, monitoring == choice,
                      [&projectContext, id, done, choice]
                      {
                          setInputMonitoring (projectContext, id, choice);
                          done();
                      });

    menu.addSeparator();
    menu.addItem ("Arm Only This Track", current.isNotEmpty(), false,
                  [&projectContext, id, done]
                  {
                      armOnly (projectContext, id);
                      done();
                  });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}

void ArmButton::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    const auto& p = getPalette (*this);
    const auto size = (float) std::min (getWidth(), getHeight()) * ArmTint::dot;
    const auto dot = getLocalBounds().toFloat().withSizeKeepingCentre (size, size);

    if (isHighlighted || isDown)
    {
        g.setColour (isDown ? p.background : p.surface);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), cornerRadius);
    }

    // Lit: a red dot, as record buttons are; off: its outline.
    if (getToggleState())
    {
        g.setColour (p.danger);
        g.fillEllipse (dot);
    }
    else
    {
        g.setColour (isHighlighted ? p.text : p.textMuted);
        g.drawEllipse (dot.reduced (ArmTint::outline / 2.0f), ArmTint::outline);
    }
}

juce::String describeArmedInput (const ProjectContext& context, TrackID id)
{
    return isArmed (context, id) ? describeInput (getTrackInput (context, id)) : juce::String();
}
} // namespace phazite::ui
