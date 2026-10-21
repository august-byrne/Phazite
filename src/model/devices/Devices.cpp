// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Devices.h"
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
#include "devices/synth/Synth.h"
#include "internal/DevicePlugin.h"
#include "internal/DrumSamplerPlugin.h"
#include "internal/WavetablePlugin.h"

namespace phazite
{
void registerDevices (te::Engine& engine)
{
    auto& plugins = engine.getPluginManager();
    plugins.createBuiltInType<DevicePluginOf<devices::Equalizer>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Compressor>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Limiter>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Reverb>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Synth>>();
    plugins.createBuiltInType<DevicePluginOf<devices::FilterEffect>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Multiband>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Maximizer>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Saturator>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Delay>>();
    plugins.createBuiltInType<DevicePluginOf<devices::StereoExpander>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Phaser>>();
    plugins.createBuiltInType<DevicePluginOf<devices::Flanger>>();
    plugins.createBuiltInType<DrumSamplerPlugin>();
    plugins.createBuiltInType<WavetablePlugin>();
}

std::span<const devices::DeviceInfo* const> getOwnDevices()
{
    // In the order menus list them: the instruments, then the effects by what they do (tone, dynamics, colour, space,
    // motion, and the stereo expander last).
    static const devices::DeviceInfo* const own[] = {
        &devices::Synth::info,        &devices::WavetableSynth::info, &devices::DrumSampler::info,    &devices::Equalizer::info,
        &devices::FilterEffect::info, &devices::Compressor::info,     &devices::Multiband::info,      &devices::Limiter::info,
        &devices::Maximizer::info,    &devices::Saturator::info,      &devices::Delay::info,          &devices::Reverb::info,
        &devices::Phaser::info,       &devices::Flanger::info,        &devices::StereoExpander::info,
    };
    return own;
}

bool isInstrument (te::Plugin& device)
{
    return device.isSynth() && dynamic_cast<te::RackInstance*> (&device) == nullptr;
}
} // namespace phazite
