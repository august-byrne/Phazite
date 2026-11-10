// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio devices plugged in while Phazite runs (src/model/engine/AudioDeviceWatch.h), each offered in a notice in the
// window's corner (src/ui/shell/DeviceNotices.h; the owner, 2026-11-09), which switches to it as the audio settings would,
// closes, or goes by itself: on a device manager of the tests' own, whose driver's devices a test plugs in and pulls out.
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/engine/AudioDeviceWatch.h"
#include "ui/shell/DeviceNotices.h"
#include "ui/theme/DesignTokens.h"

#include <utility>

namespace
{
using phazite::AudioDeviceName;

// A device of the driver below, named by its output, or its input where it has none; it opens, and plays nothing.
class NamedDevice final : public juce::AudioIODevice
{
public:
    NamedDevice (const juce::String& outputName, const juce::String& inputName, const juce::String& typeName)
        : juce::AudioIODevice (outputName.isNotEmpty() ? outputName : inputName, typeName)
    {
    }

    ~NamedDevice() override { close(); }

    juce::StringArray getOutputChannelNames() override { return { "Out 1", "Out 2" }; }
    juce::StringArray getInputChannelNames() override { return { "In 1", "In 2" }; }
    juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 256 }; }
    int getDefaultBufferSize() override { return 256; }

    juce::String open (const juce::BigInteger& inputs, const juce::BigInteger& outputs, double, int) override
    {
        activeInputs = inputs;
        activeOutputs = outputs;
        opened = true;
        return {};
    }

    void close() override
    {
        stop();
        opened = false;
    }

    bool isOpen() override { return opened; }

    void start (juce::AudioIODeviceCallback* newCallback) override
    {
        if (newCallback != nullptr)
            newCallback->audioDeviceAboutToStart (this);

        callback = newCallback;
    }

    void stop() override
    {
        if (auto* stopped = std::exchange (callback, nullptr))
            stopped->audioDeviceStopped();
    }

    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return 256; }
    double getCurrentSampleRate() override { return 48000.0; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
    juce::BigInteger getActiveInputChannels() const override { return activeInputs; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

private:
    bool opened = false;
    juce::BigInteger activeInputs, activeOutputs;
    juce::AudioIODeviceCallback* callback = nullptr;
};

// A driver with inputs and outputs of their own, which a test plugs in and pulls out by their names, telling the device
// manager as Windows' driver does.
class PluggableDriver final : public juce::AudioIODeviceType
{
public:
    explicit PluggableDriver (const juce::String& name)
        : juce::AudioIODeviceType (name)
    {
    }

    void plug (const juce::String& name, bool isInput)
    {
        (isInput ? inputs : outputs).addIfNotAlreadyThere (name);
        callDeviceChangeListeners();
    }

    void pull (const juce::String& name, bool isInput)
    {
        (isInput ? inputs : outputs).removeString (name);
        callDeviceChangeListeners();
    }

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool wantInputs) const override { return wantInputs ? inputs : outputs; }
    int getDefaultDeviceIndex (bool) const override { return 0; }

    int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override
    {
        return device != nullptr ? (asInput ? inputs : outputs).indexOf (device->getName()) : -1;
    }

    bool hasSeparateInputsAndOutputs() const override { return true; }

    juce::AudioIODevice* createDevice (const juce::String& outputName, const juce::String& inputName) override
    {
        if ((outputName.isNotEmpty() && ! outputs.contains (outputName)) || (inputName.isNotEmpty() && ! inputs.contains (inputName)))
            return nullptr;

        return new NamedDevice (outputName, inputName, getTypeName());
    }

private:
    juce::StringArray outputs, inputs;
};

// A device manager with two such drivers, playing through the first's speakers and recording from nothing.
struct PluggableDevices
{
    static constexpr const char* driverName = "Phazite Test Driver";
    static constexpr const char* otherDriverName = "Phazite Other Test Driver";

    PluggableDevices()
    {
        auto first = std::make_unique<PluggableDriver> (driverName);
        auto second = std::make_unique<PluggableDriver> (otherDriverName);
        driver = first.get();
        otherDriver = second.get();
        driver->plug ("Speakers", false);
        otherDriver->plug ("Other Speakers", false);
        devices.addAudioDeviceType (std::move (first));
        devices.addAudioDeviceType (std::move (second));
        devices.setCurrentAudioDeviceType (driverName, true);

        auto setup = devices.getAudioDeviceSetup();
        setup.outputDeviceName = "Speakers";
        setup.inputDeviceName = {};
        setup.useDefaultOutputChannels = true;
        REQUIRE (devices.setAudioDeviceSetup (setup, true).isEmpty());
        REQUIRE (devices.getCurrentAudioDevice() != nullptr);
        phazite::test::dispatchPendingMessages();
    }

    juce::AudioDeviceManager devices;
    PluggableDriver* driver = nullptr;
    PluggableDriver* otherDriver = nullptr;
};

AudioDeviceName output (const char* name)
{
    return { PluggableDevices::driverName, name, false };
}

AudioDeviceName input (const char* name)
{
    return { PluggableDevices::driverName, name, true };
}
} // namespace

TEST_CASE ("A device plugged into the audio driver in use is offered, unless it plays already, and one pulled out goes")
{
    PluggableDevices rig;
    phazite::AudioDeviceWatch watch (rig.devices);
    std::vector<AudioDeviceName> plugged, gone;
    watch.onChange = [&plugged, &gone] (const auto& newlyPlugged, const auto& newlyGone)
    {
        plugged.insert (plugged.end(), newlyPlugged.begin(), newlyPlugged.end());
        gone.insert (gone.end(), newlyGone.begin(), newlyGone.end());
    };

    // What the driver listed as the watch began is there already; an output plugged in, then an input, each once.
    rig.driver->plug ("Headphones", false);
    phazite::test::dispatchPendingMessages();
    CHECK (plugged == std::vector { output ("Headphones") });
    rig.driver->plug ("Microphone", true);
    phazite::test::dispatchPendingMessages();
    CHECK (plugged == std::vector { output ("Headphones"), input ("Microphone") });
    CHECK (gone.empty());
    CHECK (phazite::isAudioDeviceInUse (rig.devices, output ("Speakers")));
    CHECK_FALSE (phazite::isAudioDeviceInUse (rig.devices, output ("Headphones")));
    CHECK_FALSE (phazite::isBluetoothDevice (output ("Headphones")));

    // Switched to, as the audio settings would: the output, its input as it was; then the input, its output as it was.
    CHECK (phazite::switchToAudioDevice (rig.devices, output ("Headphones")).isEmpty());
    CHECK (rig.devices.getAudioDeviceSetup().outputDeviceName == "Headphones");
    CHECK (rig.devices.getAudioDeviceSetup().inputDeviceName.isEmpty());
    CHECK (phazite::isAudioDeviceInUse (rig.devices, output ("Headphones")));
    CHECK (phazite::switchToAudioDevice (rig.devices, input ("Microphone")).isEmpty());
    CHECK (rig.devices.getAudioDeviceSetup().outputDeviceName == "Headphones");
    CHECK (rig.devices.getAudioDeviceSetup().inputDeviceName == "Microphone");
    phazite::test::dispatchPendingMessages();
    CHECK (plugged.size() == 2); // switching plugs nothing in

    // Pulled out, it goes; plugged in again while the settings still choose it, it isn't offered: it is the one chosen.
    rig.driver->pull ("Microphone", true);
    phazite::test::dispatchPendingMessages();
    CHECK (gone == std::vector { input ("Microphone") });
    rig.driver->plug ("Microphone", true);
    phazite::test::dispatchPendingMessages();
    CHECK (plugged.size() == 2);

    // One that isn't, pulled out and plugged in again, is offered again: Speakers, while Headphones play.
    plugged.clear();
    rig.driver->pull ("Speakers", false);
    phazite::test::dispatchPendingMessages();
    rig.driver->plug ("Speakers", false);
    phazite::test::dispatchPendingMessages();
    CHECK (plugged == std::vector { output ("Speakers") });

    // Another driver chosen: the first's devices go, and the other's are there already.
    plugged.clear();
    gone.clear();
    rig.devices.setCurrentAudioDeviceType (PluggableDevices::otherDriverName, true);
    phazite::test::dispatchPendingMessages();
    CHECK (plugged.empty());
    CHECK (gone.size() == 3);                                                                  // Headphones and Speakers, and Microphone
    CHECK_FALSE (phazite::switchToAudioDevice (rig.devices, output ("Headphones")).isEmpty()); // not this driver's
    rig.otherDriver->plug ("Other Headphones", false);
    phazite::test::dispatchPendingMessages();
    REQUIRE (plugged.size() == 1);
    CHECK (plugged[0].type == juce::String (PluggableDevices::otherDriverName));
}

TEST_CASE ("A notice offers a device plugged in, switches to it, closes, goes with the device or by itself, and lets clicks through")
{
    PluggableDevices rig;
    phazite::ui::DeviceNotices notices (rig.devices);
    phazite::test::TestWindow window (notices, 500, 600);
    auto changes = 0;
    notices.onChange = [&changes] { ++changes; };
    CHECK (notices.getNumNotices() == 0);
    CHECK (notices.getHeightFor (400) == 0);

    // Headphones plugged in: a notice that asks, its Switch and its X.
    rig.driver->plug ("Headphones", false);
    phazite::test::dispatchPendingMessages();
    REQUIRE (notices.getNumNotices() == 1);
    CHECK (notices.isVisible());
    CHECK (changes == 1);
    CHECK (notices.getText (0) == "Headphones is connected. Play through it?");
    CHECK (notices.getHeightFor (400) > 0);

    // A microphone too: a notice under it, the latest at the bottom, apart from the first, which moves up.
    rig.driver->plug ("Microphone", true);
    phazite::test::dispatchPendingMessages();
    REQUIRE (notices.getNumNotices() == 2);
    CHECK (notices.getText (1) == "Microphone is connected. Record from it?");
    auto& first = notices.getSwitchButton (0);
    auto& second = notices.getSwitchButton (1);
    CHECK (notices.getLocalPoint (&first, juce::Point<int>()).y < notices.getLocalPoint (&second, juce::Point<int>()).y);

    // Between the cards, and around them, clicks go to what is under them.
    CHECK_FALSE (notices.hitTest (2, 2));
    CHECK (notices.hitTest (notices.getLocalPoint (&second, second.getLocalBounds().getCentre()).x,
                            notices.getLocalPoint (&second, second.getLocalBounds().getCentre()).y));

    // Switch: Phazite records from the microphone, its output as it was, and that notice closes.
    phazite::test::Mouse (second).click (second.getLocalBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (rig.devices.getAudioDeviceSetup().inputDeviceName == "Microphone");
    CHECK (rig.devices.getAudioDeviceSetup().outputDeviceName == "Speakers");
    REQUIRE (notices.getNumNotices() == 1);
    CHECK (notices.getText (0).startsWith ("Headphones"));

    // The headphones pulled out: their notice goes with them, and the column hides.
    rig.driver->pull ("Headphones", false);
    phazite::test::dispatchPendingMessages();
    CHECK (notices.getNumNotices() == 0);
    CHECK_FALSE (notices.isVisible());

    // Plugged in again: offered again; its X closes it, and Phazite plays through the speakers still.
    rig.driver->plug ("Headphones", false);
    phazite::test::dispatchPendingMessages();
    REQUIRE (notices.getNumNotices() == 1);
    auto& close = notices.getCloseButton (0);
    phazite::test::Mouse (close).click (close.getLocalBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK (notices.getNumNotices() == 0);
    CHECK (rig.devices.getAudioDeviceSetup().outputDeviceName == "Speakers");

    // Left alone, a notice goes by itself once it has shown long enough; not while the pointer is over it.
    rig.driver->pull ("Headphones", false);
    phazite::test::dispatchPendingMessages();
    rig.driver->plug ("Headphones", false);
    phazite::test::dispatchPendingMessages();
    REQUIRE (notices.getNumNotices() == 1);
    phazite::test::Mouse mouse (notices);
    auto& hovered = notices.getSwitchButton (0);
    mouse.moveTo (notices.getLocalPoint (&hovered, hovered.getLocalBounds().getCentre()));
    notices.countShown (phazite::ui::NoticeTint::shownSeconds + 1.0);
    CHECK (notices.getNumNotices() == 1);
    mouse.moveTo ({ 1, 1 });
    notices.countShown (phazite::ui::NoticeTint::shownSeconds / 2.0);
    CHECK (notices.getNumNotices() == 1);
    notices.countShown (phazite::ui::NoticeTint::shownSeconds / 2.0);
    CHECK (notices.getNumNotices() == 0);

    // At most three show: a fourth device's pushes the oldest out.
    for (const auto* name : { "One", "Two", "Three", "Four" })
        rig.driver->plug (name, false);

    phazite::test::dispatchPendingMessages();
    REQUIRE (notices.getNumNotices() == phazite::ui::NoticeTint::mostShown);
    CHECK (notices.getText (0).startsWith ("Two"));
    CHECK (notices.getText (2).startsWith ("Four"));

    for (auto* card : notices.getChildren())
        phazite::test::checkChildrenApart (*card);
}
