// SPDX-License-Identifier: AGPL-3.0-or-later
// The master's strip in the mixer (docs/design.md, section 8): its effects, fader and meter, and the loudness meter.
#pragma once

#include "DeviceSlots.h"
#include "StripParts.h"
#include "ui/components/Controls.h"

namespace phazite::ui
{
class MasterStrip : public juce::Component,
                    public juce::DragAndDropTarget,
                    private juce::Timer
{
public:
    static constexpr int width = 156;
    static int getMinimumHeight(); // as ChannelStrip's

    MasterStrip();

    void setEdit (te::Edit*);

    void paint (juce::Graphics&) override;
    void resized() override;

    // An effect dragged from the browser goes onto the master.
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override { setDropping (true); }
    void itemDragExit (const SourceDetails&) override { setDropping (false); }
    void itemDropped (const SourceDetails&) override;

private:
    te::SafeSelectable<te::Edit> edit;
    bool dropping = false;

    void setDropping (bool);
    StripHeading effectsHeading { "Effects" };
    DeviceSlots effects { DeviceSlots::Kind::effects };
    juce::Viewport deviceView;
    LevelMeter meter { "Master level" };
    Fader volume { Fader::Direction::vertical, "Master volume", te::decibelsToVolumeFaderPosition (0.0f) };
    ParameterAttachment volumeAttachment { volume, "Change Master Volume" };
    juce::Label loudness;
    juce::TextButton resetButton { "Reset" };

    void timerCallback() override; // the loudness readout, the meter and the fader
};

} // namespace phazite::ui
