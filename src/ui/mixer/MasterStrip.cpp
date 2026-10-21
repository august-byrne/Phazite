// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MasterStrip.h"
#include "model/mixer/MixerModel.h"
#include "model/presets/PresetModel.h"
#include "ui/input/LibraryDrag.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
using namespace strip;

MasterStrip::MasterStrip (CommandBridge& bridge, ProjectContext& projectContext)
    : context (projectContext),
      effects (DeviceSlots::Kind::effects, projectContext, bridge)
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
    resetButton.onClick = [this] { resetLoudness (context); };

    deviceView.setViewedComponent (&effects, false);
    deviceView.setScrollBarsShown (true, false);
    deviceView.setScrollBarThickness (6);

    for (auto* c : std::initializer_list<juce::Component*> { &effectsHeading, &deviceView, &meter, &volume, &loudness, &resetButton })
        addAndMakeVisible (c);

    context.addListener (this);
    showProject();
    startTimerHz (5);
}

MasterStrip::~MasterStrip()
{
    context.removeListener (this);
}

void MasterStrip::showProject()
{
    effects.setMaster();
    volumeAttachment.setParameter (getMasterVolume (context));
    meter.setSource (getMasterLevel (context));
    showEffects();
    timerCallback();
}

void MasterStrip::showEffects()
{
    effects.setSize (effects.getWidth(), effects.getPreferredHeight());
}

void MasterStrip::songChanged (const SongChanges& changes)
{
    if (changes.devices)
    {
        effects.refresh();
        showEffects();
    }
}

void MasterStrip::timerCallback()
{
    if (! isShowing()) // docs/performance-audit.md, U5
        return;

    volumeAttachment.update();
    effects.updateValues();

    // LUFS to one decimal, or a dash until enough has been measured.
    juce::String text;

    if (const auto readings = getLoudness (context))
    {
        const auto format = [] (std::optional<float> value) { return value.has_value() ? juce::String (*value, 1) : juce::String ("-"); };
        text << "M  " << format (readings->momentary) << " LUFS\n"
             << "S  " << format (readings->shortTerm) << " LUFS\n"
             << "I  " << format (readings->integrated) << " LUFS\n"
             << "TP " << format (readings->truePeak) << " dB";
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
    if (! context.hasProject())
        return false;

    // An effect, or a preset of one or of a container.
    if (const auto preset = getDraggedPreset (details.description))
    {
        const auto info = readPresetInfo (*preset);
        return info.has_value() && info->kind != PresetKind::track && ! info->isInstrument;
    }

    const auto device = getDraggedDevice (details.description);
    return device.has_value() && ! device->isInstrument;
}

void MasterStrip::setDropping (bool isDropping)
{
    dropping = isDropping;
    repaint();
}

void MasterStrip::itemDropped (const SourceDetails& details)
{
    setDropping (false);
    auto placed = false;

    if (const auto device = getDraggedDevice (details.description))
        placed = addDeviceTo (context, {}, *device);
    else if (const auto preset = getDraggedPreset (details.description))
        placed = loadPreset (context, *preset, {});

    if (placed)
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
