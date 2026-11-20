// SPDX-License-Identifier: AGPL-3.0-or-later
// The Settings' Audio group (src/ui/shell/SettingsPanel.h): what it says of delay, about the output, the song's delay
// compensation, low latency and Bluetooth (docs/design.md, section 20); and its rows, which open the device through the
// engine context (model/engine/EngineContext.h), on the tests' own device (TestAudioDevice.h), never the computer's.
#include "AppScreen.h"
#include "TestAudioDevice.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "model/project/ProjectContext.h"
#include "ui/shell/SettingsPanel.h"

namespace
{
phazite::EngineContext::AudioOutput getWiredOutput()
{
    phazite::EngineContext::AudioOutput output;
    output.isOpen = true;
    output.type = "Windows Audio";
    output.name = "Speakers (Realtek(R) Audio)";
    output.sampleRate = 48000.0;
    output.bufferSamples = 480;
    output.bufferSeconds = 0.01;
    output.latencySeconds = 0.0213;
    return output;
}

juce::String describe (const phazite::EngineContext::AudioOutput& output, const phazite::DelayCompensation& compensation = {},
                       bool lowLatency = false, bool hasAsioDrivers = false)
{
    return phazite::ui::describeDelay (output, compensation, lowLatency, hasAsioDrivers).lines.joinIntoString ("\n");
}
} // namespace

TEST_CASE ("The audio settings say how long a note takes to sound, and why")
{
    const auto output = getWiredOutput();
    auto text = describe (output);
    CHECK (text.startsWith ("A note you play sounds 21.3 ms to 31.3 ms later.\n"));
    CHECK (text.contains ("The buffer: 480 samples, 10.0 ms"));
    CHECK (text.contains ("The output: 21.3 ms"));
    CHECK (text.contains ("Delay compensation: none"));
    CHECK (phazite::ui::describeDelay (output, {}, false, false).warning.isEmpty());

    // A limiter on the master holds everything back, what is played too...
    const phazite::DelayCompensation compensation { 0.0021, { { "Limiter on Master", 0.0021 } } };
    text = describe (output, compensation);
    CHECK (text.startsWith ("A note you play sounds 23.4 ms to 33.4 ms later."));
    CHECK (text.contains ("Delay compensation: 2.1 ms, for Limiter on Master (2.1 ms)."));

    // ...unless low latency is on.
    text = describe (output, compensation, true);
    CHECK (text.startsWith ("A note you play sounds 21.3 ms to 31.3 ms later."));
    CHECK (text.contains ("Delay compensation: off, as Low Latency is on"));
}

TEST_CASE ("The audio settings suggest ASIO drivers, warn of Bluetooth, and say when no output is open")
{
    auto output = getWiredOutput();
    CHECK (describe (output, {}, false, true).contains ("choose ASIO above"));
    CHECK_FALSE (describe (output, {}, false, false).contains ("choose ASIO above")); // none installed

    output.type = "ASIO";
    CHECK_FALSE (describe (output, {}, false, true).contains ("choose ASIO above")); // chosen already

    output.isBluetooth = true;
    const auto bluetooth = phazite::ui::describeDelay (output, {}, false, false);
    CHECK (bluetooth.lines[0].endsWith ("later, and Bluetooth adds more."));
    CHECK (bluetooth.warning.contains ("150 to 250 ms"));

    CHECK (describe ({}) == juce::String ("No audio output is open, so nothing can be heard: choose one above."));
}

namespace
{
using phazite::test::TestAudioDeviceType;

// The tests' device open on the engine context's device manager, as the app's would be, and closed again after.
struct OpenDevice
{
    OpenDevice() { REQUIRE (TestAudioDeviceType::open (devices(), 48000.0, 480) != nullptr); }

    ~OpenDevice()
    {
        phazite::test::TestAudioDevice::refusedSampleRate = 0.0;
        TestAudioDeviceType::close (devices());
    }

    static juce::AudioDeviceManager& devices() { return phazite::test::getEngineContext().getAudioDeviceManager(); }
};

// The settings over the whole window's services and commands (AppScreen.h), as the main window shows them, as large as its
// card holds them in a window of 1280 by 800 (SettingsModalTests.cpp).
struct SettingsScreen
{
    phazite::test::AppScreen screen;
    phazite::ui::SettingsPanel panel { screen.services.engineContext, screen.commands,          screen.keyboard,
                                       screen.session.getContext(),   screen.services.settings, {} };
    phazite::test::TestWindow window { panel, 1022, 570 };

    phazite::ui::SettingRow& row (const juce::String& title)
    {
        auto* found = panel.findRow (title);
        INFO ("no row " << title);
        REQUIRE (found != nullptr);
        return *found;
    }

    template <typename Control>
    Control& control (const juce::String& title)
    {
        auto* found = dynamic_cast<Control*> (row (title).getControl());
        INFO ("the row " << title << " has no control of that kind");
        REQUIRE (found != nullptr);
        return *found;
    }

    // Picks an item of a row's list, as a click on it does.
    void pick (const juce::String& title, const juce::String& item)
    {
        auto& box = control<juce::ComboBox> (title);

        for (int i = 0; i < box.getNumItems(); ++i)
        {
            if (box.getItemText (i) == item)
            {
                box.setSelectedItemIndex (i, juce::sendNotificationSync);
                phazite::test::dispatchPendingMessages();
                return;
            }
        }

        FAIL ("the row " << title << " lists no " << item);
    }
};
} // namespace

TEST_CASE ("The engine context opens the audio device as the settings choose it, and opens what played again where it can't")
{
    const OpenDevice open;
    auto& engine = phazite::test::getEngineContext();

    auto choices = engine.getAudioChoices();
    CHECK (choices.drivers.contains (phazite::test::TestAudioDevice::typeName));
    CHECK (choices.driver == phazite::test::TestAudioDevice::typeName);
    CHECK (choices.outputs == juce::StringArray ("Test"));
    CHECK (choices.output == "Test");
    CHECK_FALSE (choices.inputsApart); // its device is an output and an input at once, as an ASIO driver's
    CHECK (choices.isOpen);
    CHECK (choices.sampleRates == juce::Array<double> { 44100.0, 48000.0 });
    CHECK (choices.sampleRate == doctest::Approx (48000.0));
    CHECK (choices.bufferSizes == juce::Array<int> { 128, 256, 480, 512 });
    CHECK (choices.bufferSize == 480);
    CHECK (choices.outputPairs == juce::StringArray ("Out 1 + 2"));
    CHECK (choices.inputPairs == juce::StringArray ("In 1 + 2"));
    CHECK (choices.outputPairsOn == juce::Array<bool> { true });
    CHECK (choices.inputPairsOn == juce::Array<bool> { false });
    CHECK_FALSE (choices.hasControlPanel);

    // Each choice opens the device so.
    CHECK (engine.chooseSampleRate (44100.0).isEmpty());
    CHECK (engine.getAudioOutput().sampleRate == doctest::Approx (44100.0));
    CHECK (engine.chooseBufferSize (256).isEmpty());
    CHECK (engine.getAudioOutput().bufferSamples == 256);
    CHECK (engine.setChannelPairOn (true, 0, true).isEmpty());
    CHECK (engine.getAudioChoices().inputPairsOn == juce::Array<bool> { true });
    CHECK (engine.setChannelPairOn (true, 0, false).isEmpty());
    CHECK (engine.getAudioChoices().inputPairsOn == juce::Array<bool> { false });
    CHECK (engine.chooseAudioOutput ("Test").isEmpty());
    CHECK (engine.chooseAudioDriver (phazite::test::TestAudioDevice::typeName).isEmpty()); // the one in use: nothing changes

    // The last output pair stays on: something must play.
    CHECK (engine.setChannelPairOn (false, 0, false).isNotEmpty());
    CHECK (engine.getAudioChoices().outputPairsOn == juce::Array<bool> { true });

    // A rate the driver refuses: why, and the device open again as it played.
    phazite::test::TestAudioDevice::refusedSampleRate = 48000.0;
    CHECK (engine.chooseSampleRate (48000.0) == juce::String ("The driver refused 48000 Hz."));
    CHECK (engine.getAudioOutput().isOpen);
    CHECK (engine.getAudioOutput().sampleRate == doctest::Approx (44100.0));
    CHECK (engine.getAudioOutput().bufferSamples == 256);
}

TEST_CASE ("The settings' Audio rows show the device and set it through the engine context, saying why where it doesn't open")
{
    const OpenDevice open;
    SettingsScreen s;
    auto& engine = s.screen.services.engineContext;

    CHECK (s.control<juce::ComboBox> ("Driver").getText() == phazite::test::TestAudioDevice::typeName);
    CHECK (s.control<juce::ComboBox> ("Output Device").getText() == "Test");
    CHECK_FALSE (s.row ("Input Device").isVisible()); // the output names the device
    CHECK (s.row ("Output Channels").isVisible());
    CHECK (s.row ("Input Channels").isVisible());
    CHECK (s.control<juce::ComboBox> ("Sample Rate").getText() == "48000 Hz");
    CHECK (s.control<juce::ComboBox> ("Buffer Size").getText() == "480 samples (10.0 ms)");
    CHECK (s.row ("Test Sound").isVisible());
    CHECK_FALSE (s.row ("Driver Control Panel").isVisible()); // the device has none
    CHECK (s.panel.getDelayText().startsWith ("A note you play sounds"));
    CHECK (s.panel.getWarning().isEmpty());

    // Each row sets the device.
    s.pick ("Sample Rate", "44100 Hz");
    CHECK (engine.getAudioOutput().sampleRate == doctest::Approx (44100.0));
    CHECK (s.control<juce::ComboBox> ("Sample Rate").getText() == "44100 Hz");
    s.pick ("Buffer Size", "256 samples (5.8 ms)");
    CHECK (engine.getAudioOutput().bufferSamples == 256);
    CHECK (s.panel.getDelayText().contains ("The buffer: 256 samples"));

    auto* inputPair = dynamic_cast<juce::ToggleButton*> (s.row ("Input Channels").getControl()->getChildComponent (0));
    REQUIRE (inputPair != nullptr);
    CHECK (inputPair->getTitle() == "In 1 + 2");
    inputPair->setToggleState (true, juce::sendNotificationSync);
    CHECK (engine.getAudioChoices().inputPairsOn == juce::Array<bool> { true });

    // A rate the driver refuses: its row says why, in the warning colour, and the list shows what plays.
    phazite::test::TestAudioDevice::refusedSampleRate = 48000.0;
    s.pick ("Sample Rate", "48000 Hz");
    CHECK (s.row ("Sample Rate").getNote() == "The driver refused 48000 Hz.");
    CHECK (s.control<juce::ComboBox> ("Sample Rate").getText() == "44100 Hz");
    CHECK (engine.getAudioOutput().sampleRate == doctest::Approx (44100.0));

    // A choice that opens takes the note away.
    s.pick ("Buffer Size", "512 samples (11.6 ms)");
    CHECK (s.row ("Sample Rate").getNote().isEmpty());
    CHECK (engine.getAudioOutput().bufferSamples == 512);

    // The rows and the groups' titles one under another, the delay's apart from the device's.
    phazite::test::checkChildrenApart (*s.panel.getList().getViewedComponent());
}

TEST_CASE ("A device that doesn't open, chosen while none is open, leaves the device chosen last as the one kept")
{
    // Going back to "no device" as a choice would be saved, and Phazite would start without sound from then on.
    auto& engine = phazite::test::getEngineContext();
    juce::String kept;

    {
        const OpenDevice open;
        REQUIRE (engine.chooseBufferSize (256).isEmpty()); // a choice, which the manager keeps
        const auto state = OpenDevice::devices().createStateXml();
        REQUIRE (state != nullptr);
        kept = state->toString();
    }

    REQUIRE_FALSE (engine.getAudioOutput().isOpen);
    CHECK (engine.chooseAudioOutput ("No such device").isNotEmpty());
    CHECK_FALSE (engine.getAudioOutput().isOpen);
    const auto state = OpenDevice::devices().createStateXml();
    REQUIRE (state != nullptr);
    CHECK (state->toString() == kept);
}

TEST_CASE ("Without a device open, the settings say so, and offer only the driver and the device")
{
    TestAudioDeviceType::close (OpenDevice::devices());
    SettingsScreen s;
    CHECK (s.panel.getDelayText().startsWith ("No audio output is open"));
    CHECK (s.panel.getWarning().isEmpty());

    for (const auto* title : { "Sample Rate", "Buffer Size", "Output Channels", "Input Channels", "Test Sound" })
    {
        INFO (title);
        CHECK_FALSE (s.row (title).isVisible());
    }

    CHECK (s.row ("Delay").isVisible());
    phazite::test::checkChildrenApart (*s.panel.getList().getViewedComponent());
}
