// SPDX-License-Identifier: AGPL-3.0-or-later
// The strip along the bottom of the window (src/ui/shell/StatusBar.h), in the window's content as the app builds it: whether
// the project is saved, what is selected, where MIDI goes, the hints, how far clips being stretched have come, the delay
// compensation, the audio output and the CPU, each item doing what it is about when clicked.
#include "AppScreen.h"
#include "GoldenFiles.h"
#include "HostedDevice.h"
#include "TestClips.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/arrangement/internal/Clips.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/engine/internal/EngineContext.h"
#include "model/mixer/MixerModel.h"
#include "model/notes/internal/Notes.h"
#include "model/playback/internal/PlaybackHost.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"
#include "ui/shell/StatusBar.h"
#include "ui/theme/DesignTokens.h"

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
    phazite::test::dispatchPendingMessages();
    auto& status = screen.shell.getStatusBar();

    // What the main window tells it, as the project changes.
    status.showSaveState (SaveState::none);
    CHECK (status.getSaveText().isEmpty());
    status.showSaveState (SaveState::saved);
    CHECK (status.getSaveText() == juce::String ("Saved"));

    // With how long ago its changes were autosaved, if they were, in whole minutes.
    const auto now = juce::Time::getCurrentTime();
    status.showSaveState (SaveState::unsaved);
    CHECK (status.getSaveText() == juce::String ("Unsaved changes"));
    status.showSaveState (SaveState::unsaved, now);
    CHECK (status.getSaveText() == juce::String::fromUTF8 ("Unsaved changes \xc2\xb7 autosaved just now"));
    status.showSaveState (SaveState::unsaved, now - juce::RelativeTime::minutes (3));
    CHECK (status.getSaveText() == juce::String::fromUTF8 ("Unsaved changes \xc2\xb7 autosaved 3 min ago"));
    status.showSaveState (SaveState::neverSaved, now - juce::RelativeTime::hours (2));
    CHECK (status.getSaveText() == juce::String::fromUTF8 ("Not saved yet \xc2\xb7 autosaved over an hour ago"));
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

TEST_CASE ("The status bar says what is selected, and where what is played on a keyboard goes")
{
    Screen screen;
    screen.session.openAtStartup();
    REQUIRE (screen.registry.invoke ("help.example.exodus")); // tracks with clips, as Help > Example Songs opens it
    phazite::test::dispatchPendingMessages();
    phazite::test::dispatchPendingMessages();
    auto& status = screen.shell.getStatusBar();
    auto& context = screen.session.getContext();
    auto& selection = context.getSongSelection();
    const auto tracks = phazite::getAllPlaylistTracks (context);
    REQUIRE (tracks.size() >= 2);

    // Nothing selected: nothing said.
    selection.clear();
    context.getSongRange().clear();
    phazite::test::dispatchPendingMessages();
    CHECK (status.getSelectionText().isEmpty());

    // A track by its name; two, counted.
    selection.selectOnly (tracks[0]);
    phazite::test::dispatchPendingMessages();
    const auto first = phazite::getChannel (context, tracks[0]);
    REQUIRE (first.has_value());
    CHECK (status.getSelectionText() == first->name);
    selection.add (tracks[1]);
    phazite::test::dispatchPendingMessages();
    CHECK (status.getSelectionText() == juce::String ("2 tracks"));

    // Clips, counted; a time range, by its bars (from 1, as the bar numbers show them).
    std::vector<phazite::ClipID> clips;

    for (const auto track : tracks)
        for (const auto& clip : phazite::getClips (context, track))
            clips.push_back (clip.id);

    REQUIRE (clips.size() >= 2);
    selection.selectOnly (clips[0]);
    selection.add (clips[1]);
    phazite::test::dispatchPendingMessages();
    CHECK (status.getSelectionText() == juce::String ("2 clips"));
    context.getSongRange().set ({ phazite::BeatPosition::fromBeats (4.0), phazite::BeatPosition::fromBeats (16.0) }, { tracks[0] });
    phazite::test::dispatchPendingMessages();
    CHECK (status.getSelectionText() == juce::String ("Bars 2 to 4"));
    context.getSongRange().clear();
    phazite::test::dispatchPendingMessages();

    // Where MIDI goes: the track live MIDI plays, by its name, or no track (as before Phazite has found the MIDI inputs,
    // which on a computer without them, as in CI, it may not have yet).
    status.refreshNow();
    const auto arrow = juce::String::fromUTF8 ("MIDI \xe2\x86\x92 ");

    if (const auto live = phazite::getLiveMidiTrack (context); live.isValid())
    {
        const auto liveChannel = phazite::getChannel (context, live);
        REQUIRE (liveChannel.has_value());
        CHECK (status.getMidiText() == arrow + liveChannel->name);
    }
    else
    {
        CHECK (status.getMidiText() == arrow + "no track");
    }

    phazite::test::checkChildrenApart (status);

    // In a narrow window the hints keep their room: what is selected goes first, and nothing overlaps.
    screen.shell.setSize (700, screen.shell.getHeight());
    CHECK (status.getHintBar().getWidth() >= phazite::ui::Shell::statusHintsMinimumWidth);
    phazite::test::checkChildrenApart (status);
}

TEST_CASE ("The hint bar says what a command made that its name doesn't: Duplicate's copies that didn't fit")
{
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& context = screen.session.getContext();
    auto* song = phazite::findSong (context);
    REQUIRE (song != nullptr);

    // A clip from beat 0 to 4 that the next keeps from growing, with notes from beat 2 to its end: their copies, from beat 4,
    // don't fit.
    const auto track = phazite::addTrackAtEnd (*song, false, nullptr);
    REQUIRE (track.isValid());
    const auto clip = phazite::insertMidiClip (*song, track, { phazite::test::at (0.0), phazite::test::at (4.0) }, nullptr);
    REQUIRE (phazite::insertMidiClip (*song, track, { phazite::test::at (4.0), phazite::test::at (8.0) }, nullptr).isValid());
    phazite::addNote (*song, clip, 60, phazite::test::at (2.0), phazite::test::lasting (0.5), 100);
    phazite::addNote (*song, clip, 62, phazite::test::at (3.0), phazite::test::lasting (1.0), 100);
    auto& selection = context.getSongSelection();
    selection.setNoteClips ({ clip });
    selection.selectNotes (phazite::getRefs (phazite::getClipNotes (*song, clip)));
    phazite::test::dispatchPendingMessages();

    auto& hints = screen.shell.getStatusBar().getHintBar();
    REQUIRE (screen.registry.invoke ("edit.duplicate"));
    CHECK (hints.getHint().startsWith ("2 notes of 2 weren't made"));
    CHECK (phazite::getClipNotes (*song, clip).size() == 2);

    // A command that made all it says says nothing more, and the outcome stays for a while, whatever else is under the
    // pointer.
    REQUIRE (screen.registry.invoke ("edit.selectAll"));
    CHECK (hints.getHint().startsWith ("2 notes of 2 weren't made"));
}

TEST_CASE ("The status bar shows the song's delay compensation, while it has some")
{
    // Played through a device, as the app has it: devices know their latency once playback has prepared them.
    phazite::test::HostedDevice device;
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    auto& status = screen.shell.getStatusBar();
    auto& context = screen.session.getContext();
    phazite::getPlaybackHost (phazite::test::getEngineContext()).setLowLatency (false);

    // A limiter on the master looks ahead.
    phazite::DeviceType limiter;

    for (const auto& type : phazite::getDeviceTypes (context))
        if (type.description.fileOrIdentifier == juce::String ("phaziteLimiter"))
            limiter = type;

    phazite::addEffect (context, {}, limiter);
    phazite::test::dispatchPendingMessages(); // the song handed to the engine for its next block
    device.process (20);
    status.refreshNow();
    CHECK (status.getDelayText().startsWith ("Delay comp. "));
    CHECK (status.getDelayText().endsWith (" ms"));

    // Low latency turns it off.
    phazite::getPlaybackHost (phazite::test::getEngineContext()).setLowLatency (true);
    status.refreshNow();
    CHECK (status.getDelayText().isEmpty());
    phazite::getPlaybackHost (phazite::test::getEngineContext()).setLowLatency (false);
}

TEST_CASE ("While clips are stretched in the background after a tempo change, the status bar shows how far, as a bar")
{
    Screen screen;
    screen.session.openAtStartup();
    phazite::test::dispatchPendingMessages();
    phazite::test::dispatchPendingMessages();
    auto& status = screen.shell.getStatusBar();
    auto& context = screen.session.getContext();
    status.refreshNow();
    CHECK (status.getRenderText().isEmpty());

    // A loop long enough to take a while, at its own tempo, then the song a little slower.
    const auto track = phazite::addTrackAtEnd (context, false);
    REQUIRE (! phazite::importFiles (context,
                                     { phazite::test::writeClicks (screen.temp.folder.getChildFile ("Loop"), 64, 120.0).getFullPathName() },
                                     track, phazite::BeatPosition::fromBeats (64.0))
                   .empty());
    phazite::getTempoHandle (context).setValue (100.0);

    REQUIRE (phazite::test::dispatchMessagesUntil (
        [&status]
        {
            status.refreshNow();
            return status.getRenderText().isNotEmpty();
        }));
    CHECK (status.getRenderText().startsWith ("Stretching "));
    CHECK (status.getRenderProgress() >= 0.0);
    CHECK (status.getRenderProgress() <= 1.0);
    phazite::test::checkChildrenApart (status);

    // Once it is made, the bar goes.
    CHECK (phazite::test::dispatchMessagesUntil (
        [&status]
        {
            status.refreshNow();
            return status.getRenderText().isEmpty();
        }));
}
