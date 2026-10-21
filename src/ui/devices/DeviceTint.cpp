// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeviceTint.h"
#include "devices/delay/Delay.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Maximizer.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Phaser.h"
#include "devices/reverb/Reverb.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/vocoder/Vocoder.h"
#include "ui/theme/LookAndFeel.h"

namespace phazite::ui
{
DeviceFamily getDeviceFamily (std::string_view typeID)
{
    struct Entry
    {
        std::string_view type;
        DeviceFamily family;
    };

    static const Entry entries[] = {
        { devices::Compressor::typeID, DeviceFamily::dynamics }, { devices::Limiter::typeID, DeviceFamily::dynamics },
        { devices::Maximizer::typeID, DeviceFamily::dynamics },  { devices::Multiband::typeID, DeviceFamily::dynamics },
        { devices::Equalizer::typeID, DeviceFamily::tone },      { devices::FilterEffect::typeID, DeviceFamily::tone },
        { devices::Saturator::typeID, DeviceFamily::drive },     { devices::Delay::typeID, DeviceFamily::space },
        { devices::Reverb::typeID, DeviceFamily::space },        { devices::Phaser::typeID, DeviceFamily::motion },
        { devices::Flanger::typeID, DeviceFamily::motion },      { devices::StereoExpander::typeID, DeviceFamily::stereo },
        { devices::Vocoder::typeID, DeviceFamily::voice },
    };

    for (const auto& entry : entries)
        if (entry.type == typeID)
            return entry.family;

    return DeviceFamily::instrument;
}

void applyDeviceTint (juce::Component& editor, const DeviceHandle& device)
{
    const auto* info = device.getInfo();
    const auto* lookAndFeel = dynamic_cast<const LookAndFeel*> (&editor.getLookAndFeel());
    const auto theme = lookAndFeel != nullptr ? lookAndFeel->getTheme() : ThemeName::dark;

    if (info == nullptr)
        return;

    if (const auto colour = getFamilyColour (getDeviceFamily (info->typeID), theme))
        editor.setColour (juce::Slider::rotarySliderFillColourId, *colour);
}

juce::Colour getDeviceTint (const juce::Component& component)
{
    for (auto* each = &component; each != nullptr; each = each->getParentComponent())
        if (each->isColourSpecified (juce::Slider::rotarySliderFillColourId))
            return each->findColour (juce::Slider::rotarySliderFillColourId);

    return getPalette (component).accent;
}
} // namespace phazite::ui
