// SPDX-License-Identifier: AGPL-3.0-or-later
// The Audio settings' delay section (src/ui/shell/SettingsPanel.h): what it says about the output, the song's delay
// compensation, low latency and Bluetooth (docs/design.md, section 20).
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

TEST_CASE ("The Audio tab shows the device settings, then the delay, apart")
{
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::ui::AudioSettings settings (phazite::test::getEngineContext(), context);
    phazite::test::TestWindow window { settings, 640, 560 };

    // The tests open no device.
    CHECK (settings.getDelayText().startsWith ("No audio output is open"));
    CHECK (settings.getWarning().isEmpty());
    phazite::test::checkChildrenApart (settings);
}
