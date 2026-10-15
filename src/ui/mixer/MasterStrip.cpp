// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MasterStrip.h"
#include "model/engine/LoudnessMeterPlugin.h"
#include "model/mixer/Mixer.h"
#include "services/Library.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
using namespace strip;

MasterStrip::MasterStrip()
{
    setTitle ("Master");
    showAsDecibels (volume);
    volume.setTooltip ("The volume of everything Phazite plays. Double-click for 0 dB");

    loudness.setFont (LookAndFeel::getNumberFont (TextSize::small));
    loudness.setJustificationType (juce::Justification::topLeft);
    loudness.setTitle ("Loudness");
    loudness.setTooltip ("Loudness, measured as streaming services do (EBU R128): momentary, over the last 3 seconds, and integrated since "
                         "the reset, with the true peak. It measures the master's effects, before the master volume");

    resetButton.setTooltip ("Starts the integrated loudness and the true peak again");
    resetButton.onClick = [this]
    {
        if (auto* e2 = edit.get())
            if (auto* meterPlugin = findLoudnessMeter (*e2))
                meterPlugin->resetReadings();
    };

    deviceView.setViewedComponent (&effects, false);
    deviceView.setScrollBarsShown (true, false);
    deviceView.setScrollBarThickness (6);

    for (auto* c : std::initializer_list<juce::Component*> { &effectsHeading, &deviceView, &meter, &volume, &loudness, &resetButton })
        addAndMakeVisible (c);

    startTimerHz (5);
}

void MasterStrip::setEdit (te::Edit* newEdit)
{
    edit = newEdit;
    effects.setChannel (nullptr, newEdit);

    const auto volumePlugin = newEdit != nullptr ? newEdit->getMasterVolumePlugin() : nullptr;
    volumeAttachment.setParameter (volumePlugin.get(), volumePlugin != nullptr ? volumePlugin->volParam->paramID : juce::String());
    timerCallback();
}

void MasterStrip::timerCallback()
{
    auto* e = edit.get();
    volumeAttachment.update();
    effects.update();
    effects.setSize (effects.getWidth(), effects.getPreferredHeight());

    auto* context = e != nullptr ? e->getCurrentPlaybackContext() : nullptr;
    meter.setSource (context != nullptr ? &context->masterLevels : nullptr);

    // LUFS to one decimal, or a dash until enough has been measured.
    juce::String text;

    if (auto* meterPlugin = e != nullptr ? findLoudnessMeter (*e) : nullptr)
    {
        const auto readings = meterPlugin->getReadings();
        const auto format = [] (bool valid, float value) { return valid ? juce::String (value, 1) : juce::String ("-"); };
        text << "M  " << format (readings.momentaryValid, readings.momentaryLufs) << " LUFS\n"
             << "S  " << format (readings.shortTermValid, readings.shortTermLufs) << " LUFS\n"
             << "I  " << format (readings.integratedValid, readings.integratedLufs) << " LUFS\n"
             << "TP " << format (readings.truePeakDb > te::LoudnessMeter::silenceFloorDb, readings.truePeakDb) << " dB";
    }

    loudness.setText (text, juce::dontSendNotification);
}

void MasterStrip::paint (juce::Graphics& g)
{
    const auto& p = getPalette (*this);
    g.fillAll (p.surface);
    g.setColour (p.border);
    g.fillRect (0, 0, 1, getHeight()); // between the tracks' strips and this
    g.setColour (p.text);
    g.setFont (juce::FontOptions (TextSize::body, juce::Font::bold));
    g.drawText ("Master", getLocalBounds().reduced (Space::s).removeFromTop (controlHeight), juce::Justification::centred);

    // While an effect from the browser is dragged over it.
    if (dropping)
    {
        g.setColour (p.accent);
        g.drawRect (getLocalBounds(), 2);
    }
}

bool MasterStrip::isInterestedInDragSource (const SourceDetails& details)
{
    const auto device = getDraggedDevice (details.description);
    return edit.get() != nullptr && device.has_value() && ! device->isInstrument;
}

void MasterStrip::setDropping (bool isDropping)
{
    dropping = isDropping;
    repaint();
}

void MasterStrip::itemDropped (const SourceDetails& details)
{
    setDropping (false);

    if (auto* e = edit.get())
        if (const auto device = getDraggedDevice (details.description))
            if (addDeviceTo (*e, nullptr, *device, nullptr) != nullptr)
                markPlaced (details.description);
}

void MasterStrip::resized()
{
    auto area = getLocalBounds().reduced (Space::s);
    area.removeFromTop (controlHeight);

    auto readout = area.removeFromBottom (loudnessHeight);
    resetButton.setBounds (readout.removeFromBottom (buttonHeight).removeFromLeft (64));
    loudness.setBounds (readout);
    area.removeFromBottom (Space::s);

    // The fader, and above it the effects, which scroll. (The mixer keeps the strip at least getMinimumHeight() tall.)
    const auto faderHeight = getFaderHeight (area.getHeight(), Space::xs + minSlotsHeight);
    auto faderArea = area.removeFromBottom (faderHeight).withSizeKeepingCentre (std::min (area.getWidth(), 56), faderHeight);
    meter.setBounds (faderArea.removeFromRight (meterWidth));
    volume.setBounds (faderArea);
    area.removeFromBottom (Space::xs);

    effectsHeading.setBounds (area.removeFromTop (headingHeight));
    deviceView.setBounds (area);
    effects.setSize (std::max (1, area.getWidth() - deviceView.getScrollBarThickness()), effects.getPreferredHeight());
}

int MasterStrip::getMinimumHeight()
{
    // As resized() lays it out: the name, a heading and a button, the shortest fader and the loudness readout.
    return 2 * Space::s + controlHeight + minSlotsHeight + Space::xs + minFaderHeight + Space::s + loudnessHeight;
}

} // namespace phazite::ui
