// SPDX-License-Identifier: AGPL-3.0-or-later
// The audio devices as Phazite chooses and describes them (src/model/engine/AudioDevices.h): the buffer a low-latency setup
// takes, and which outputs are Bluetooth.
#include "TestHelpers.h"
#include "model/engine/AudioDevices.h"

TEST_CASE ("A low-latency setup takes the largest buffer from 128 to 256 samples that a device offers")
{
    CHECK (phazite::chooseLowLatencyBufferSize ({ 64, 128, 192, 256, 384, 480 }) == 256);
    CHECK (phazite::chooseLowLatencyBufferSize ({ 96, 144, 192, 240, 288 }) == 240);
    CHECK (phazite::chooseLowLatencyBufferSize ({ 64, 128 }) == 128);
    CHECK_FALSE (phazite::chooseLowLatencyBufferSize ({ 480, 960 }).has_value()); // Windows' shared mode at 48 kHz
    CHECK_FALSE (phazite::chooseLowLatencyBufferSize ({ 32, 64 }).has_value());
    CHECK_FALSE (phazite::chooseLowLatencyBufferSize ({}).has_value());
}

TEST_CASE ("Bluetooth outputs are told by their device, or else by their name")
{
    CHECK (phazite::isBluetoothName ("Headset (Wireless Headphones Hands-Free AG Audio)"));
    CHECK (phazite::isBluetoothName ("bluez_output.00_00_00_00_00_00.a2dp-sink"));
    CHECK (phazite::isBluetoothName ("Bluetooth Speaker"));
    CHECK_FALSE (phazite::isBluetoothName ("Speakers (Realtek(R) Audio)"));
    CHECK_FALSE (phazite::isBluetoothName ("Focusrite USB ASIO"));

    // A device that Windows doesn't list by that name goes by the name, as ASIO drivers always do.
    CHECK_FALSE (phazite::isBluetoothOutput ("Windows Audio", "No Such Output 1234"));
    CHECK (phazite::isBluetoothOutput ("Windows Audio", "No Such Bluetooth Output"));
    CHECK_FALSE (phazite::isBluetoothOutput ("ASIO", "Realtek ASIO"));
}
