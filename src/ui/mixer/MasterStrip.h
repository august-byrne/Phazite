// SPDX-License-Identifier: AGPL-3.0-or-later
// The master's strip in the mixer (docs/design.md, section 8): its effects, fader and meter, and the loudness meter.
#pragma once

#include "DeviceSlots.h"
#include "StripParts.h"
#include "model/project/ProjectContext.h"
#include "ui/components/Controls.h"

namespace phazite::ui
{
class MasterStrip : public juce::Component,
                    public juce::DragAndDropTarget,
                    private ProjectContext::Listener,
                    private juce::Timer
{
public:
    static constexpr int width = 156;
    static int getMinimumHeight(); // as ChannelStrip's

    // Shows the open project's master, and nothing while none is open.
    MasterStrip (CommandBridge&, ProjectContext&); // the commands, for the effects' menus
    ~MasterStrip() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // An effect, or a preset of one or of a container, dragged from the browser goes onto the master.
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override { setDropping (true); }
    void itemDragExit (const SourceDetails&) override { setDropping (false); }
    void itemDropped (const SourceDetails&) override;

private:
    ProjectContext& context;
    bool dropping = false;

    void setDropping (bool);
    StripHeading effectsHeading { "Effects" };
    DeviceSlots effects;
    juce::Viewport deviceView;
    LevelMeter meter { "Master level" };
    Fader volume { Fader::Direction::vertical, "Master volume", decibelsToFaderPosition (0.0f) };
    ParameterAttachment volumeAttachment { volume };
    juce::Label loudness;
    juce::TextButton resetButton { "Reset" };

    void showProject(); // the open project's master
    void showEffects();
    void projectChanged() override { showProject(); }
    void songChanged (const SongChanges&) override;
    void timerCallback() override; // the loudness readout and the controls' values
};

} // namespace phazite::ui
