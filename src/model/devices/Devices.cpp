// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Devices.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/eq/Equalizer.h"
#include "devices/reverb/Reverb.h"
#include "devices/synth/Synth.h"
#include "internal/DevicePlugin.h"
#include "internal/DrumSamplerPlugin.h"

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
    plugins.createBuiltInType<DrumSamplerPlugin>();
}

std::span<const devices::DeviceInfo* const> getOwnDevices()
{
    static const devices::DeviceInfo* const own[] = { &devices::Synth::info,      &devices::DrumSampler::info, &devices::Equalizer::info,
                                                      &devices::Compressor::info, &devices::Limiter::info,     &devices::Reverb::info };
    return own;
}

bool isInstrument (te::Plugin& device)
{
    return device.isSynth() && dynamic_cast<te::RackInstance*> (&device) == nullptr;
}
} // namespace phazite
