// SPDX-License-Identifier: AGPL-3.0-or-later
#include "internal/Devices.h"
#include "devices/delay/Delay.h"
#include "devices/drums/DrumSampler.h"
#include "devices/dynamics/Compressor.h"
#include "devices/dynamics/Limiter.h"
#include "devices/dynamics/Multiband.h"
#include "devices/eq/Equalizer.h"
#include "devices/filter/FilterEffect.h"
#include "devices/fm/FmSynth.h"
#include "devices/motion/Chorus.h"
#include "devices/motion/Flanger.h"
#include "devices/motion/Phaser.h"
#include "devices/reverb/Reverb.h"
#include "devices/sampler/Sampler.h"
#include "devices/saturation/Exciter.h"
#include "devices/saturation/Saturator.h"
#include "devices/stereo/StereoExpander.h"
#include "devices/synth/Synth.h"
#include "devices/vinyl/Vinyl.h"
#include "devices/vocoder/Vocoder.h"
#include "devices/wavetable/WavetableSynth.h"

#include <vector>

namespace phazite
{
std::span<const devices::DeviceInfo* const> getOwnDevices()
{
    // In the order menus list them: the instruments, then the effects by what they do (tone, dynamics, colour, space,
    // motion, the voice, and the stereo expander last).
    static const devices::DeviceInfo* const own[] = {
        &devices::Synth::info,       &devices::WavetableSynth::info, &devices::FmSynth::info,      &devices::Sampler::info,
        &devices::DrumSampler::info, &devices::Equalizer::info,      &devices::FilterEffect::info, &devices::Compressor::info,
        &devices::Multiband::info,   &devices::Limiter::info,        &devices::Saturator::info,    &devices::Exciter::info,
        &devices::Vinyl::info,       &devices::Delay::info,          &devices::Reverb::info,       &devices::Phaser::info,
        &devices::Flanger::info,     &devices::Chorus::info,         &devices::Vocoder::info,      &devices::StereoExpander::info,
    };
    return own;
}

namespace
{
    template <typename Device>
    std::unique_ptr<devices::Processor> make()
    {
        return std::make_unique<Device>();
    }

    struct OwnDevice
    {
        const devices::DeviceInfo* info;
        std::unique_ptr<devices::Processor> (*create)();
    };

    const OwnDevice ownDevices[] = {
        { &devices::Synth::info, make<devices::Synth> },
        { &devices::WavetableSynth::info, make<devices::WavetableSynth> },
        { &devices::FmSynth::info, make<devices::FmSynth> },
        { &devices::Sampler::info, make<devices::Sampler> },
        { &devices::DrumSampler::info, make<devices::DrumSampler> },
        { &devices::Equalizer::info, make<devices::Equalizer> },
        { &devices::FilterEffect::info, make<devices::FilterEffect> },
        { &devices::Compressor::info, make<devices::Compressor> },
        { &devices::Multiband::info, make<devices::Multiband> },
        { &devices::Limiter::info, make<devices::Limiter> },
        { &devices::Saturator::info, make<devices::Saturator> },
        { &devices::Exciter::info, make<devices::Exciter> },
        { &devices::Vinyl::info, make<devices::Vinyl> },
        { &devices::Delay::info, make<devices::Delay> },
        { &devices::Reverb::info, make<devices::Reverb> },
        { &devices::Phaser::info, make<devices::Phaser> },
        { &devices::Flanger::info, make<devices::Flanger> },
        { &devices::Chorus::info, make<devices::Chorus> },
        { &devices::Vocoder::info, make<devices::Vocoder> },
        { &devices::StereoExpander::info, make<devices::StereoExpander> },
    };

    std::vector<OwnDevice>& getRegistered() // message thread
    {
        static std::vector<OwnDevice> registered;
        return registered;
    }

    const OwnDevice* findOwn (std::string_view typeID)
    {
        for (const auto& device : ownDevices)
            if (device.info->typeID == typeID)
                return &device;

        for (const auto& device : getRegistered())
            if (device.info->typeID == typeID)
                return &device;

        return nullptr;
    }
} // namespace

const devices::DeviceInfo* findOwnDevice (std::string_view typeID)
{
    const auto* device = findOwn (typeID);
    return device != nullptr ? device->info : nullptr;
}

std::unique_ptr<devices::Processor> createOwnDevice (std::string_view typeID)
{
    const auto* device = findOwn (typeID);
    return device != nullptr ? device->create() : nullptr;
}

void registerDeviceType (const devices::DeviceInfo& info, DeviceFactory create)
{
    if (findOwn (info.typeID) == nullptr)
        getRegistered().push_back ({ &info, create });
}
} // namespace phazite
