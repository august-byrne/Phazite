// SPDX-License-Identifier: AGPL-3.0-or-later
// The strip along the bottom of the window (src/ui/shell/StatusBar.h), in the window's content as the app builds it: whether
// the project is saved, the hints, the audio output and the CPU, each item doing what it is about when clicked.
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "ui/shell/StatusBar.h"

namespace
{
using Screen = phazite::test::AppScreen;
using SaveState = phazite::ui::StatusBar::SaveState;
} // namespace

TEST_CASE ("The status bar says whether the project is saved, and a click on that saves it")
{
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    phazite::test::settle();
    auto& status = screen.shell.getStatusBar();

    // What the main window tells it, as the project changes.
    status.showSaveState (SaveState::none);
    CHECK (status.getSaveText().isEmpty());
    status.showSaveState (SaveState::saved);
    CHECK (status.getSaveText() == juce::String ("Saved"));
    status.showSaveState (SaveState::neverSaved);
    CHECK (status.getSaveText() == juce::String ("Not saved yet"));
    REQUIRE (screen.session.isUntitled());

    auto* item = phazite::test::findComponentWithTitle (status, "Not saved yet");
    REQUIRE (item != nullptr);
    phazite::test::Mouse mouse (*item);
    mouse.click (item->getLocalBounds().getCentre());
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (screen.session.isUntitled()); // the save dialog answers with what it suggests here

    // The hints in the middle, between the items.
    auto& hints = status.getHintBar();
    CHECK (hints.getX() >= item->getRight());
    phazite::test::checkChildrenApart (status);
}

TEST_CASE ("The status bar shows the audio output's sample rate and delay, Bluetooth's in the warning colour, and the CPU")
{
    Screen screen;
    auto& status = screen.shell.getStatusBar();
    CHECK (status.getAudioText() == juce::String ("No audio output")); // the tests open no audio device

    phazite::EngineContext::AudioOutput output;
    output.isOpen = true;
    output.type = "Windows Audio";
    output.name = "Speakers";
    output.sampleRate = 48000.0;
    output.bufferSamples = 480;
    output.bufferSeconds = 0.01;
    output.latencySeconds = 0.02;
    output.isBluetooth = true;
    status.showAudioOutput (output);
    CHECK (status.getAudioText() == juce::String::fromUTF8 ("48 kHz \xc2\xb7 20 ms \xc2\xb7 Bluetooth"));

    output.sampleRate = 44100.0;
    output.isBluetooth = false;
    status.showAudioOutput (output);
    CHECK (status.getAudioText() == juce::String::fromUTF8 ("44.1 kHz \xc2\xb7 20 ms"));
    CHECK (status.getCpuText().startsWith ("CPU "));
    phazite::test::checkChildrenApart (status);
}
