// SPDX-License-Identifier: AGPL-3.0-or-later
// Freezing a track and bouncing it in place (roadmap step 2.11; src/model/mixer/FreezeModel.h): rendered in the
// background up to the fader (stopping before a sidechained device or a send before the fader), played from a file in the
// project's Cache folder, kept frozen while only its mixer changes and unfrozen when what it froze changes, opened frozen,
// and cancelled without a trace; and a bounce onto a new audio track under the original, as one undo step.
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "devices/utility/Utility.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/DevicePlugin.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/project/ProjectContext.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "ui/input/CommandBridge.h"
#include "ui/mixer/DevicesView.h"
#include "ui/playlist/ClipComponent.h"
#include "ui/playlist/Playlist.h"
#include "ui/playlist/TrackHeader.h"

#include <algorithm>

namespace
{
// A saved project with Bass, a sine from the start to 1.5 s through a utility that turns it up 6 dB, its fader at -6 dB
// and a send after it to the return Reverb; Bass is selected.
struct Song
{
    Song()
    {
        juce::String error;
        project = phazite::Project::create (phazite::test::getEngine(), temp.folder, "Song", error);
        REQUIRE (project != nullptr);
        auto& edit = project->getEdit();
        edit.ensureNumberOfAudioTracks (1);
        bass = te::getAudioTracks (edit)[0];
        bass->setName ("Bass");

        const auto sound = phazite::test::writeSound (temp.folder.getChildFile ("Sine.wav"), 2.0);
        clip = bass->insertWaveClip ("Sine", sound, { te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (1.5)) }, false);
        REQUIRE (clip != nullptr);

        utility = phazite::insertDevice (*bass, edit.getPluginCache().createNewPlugin (phazite::devices::Utility::typeID, {}));
        REQUIRE (utility != nullptr);
        setParameter (*utility, "gain", 6.0f);
        bass->getVolumePlugin()->setVolumeDb (-6.0f);

        reverb = phazite::addReturnTrack (edit, "Reverb", nullptr);
        REQUIRE (reverb != nullptr);
        REQUIRE (phazite::addSend (*bass, phazite::getReturnBus (*reverb), false) != nullptr);

        // Tracktion puts clips added in code in order a moment later, as an undo step of its own.
        phazite::test::dispatchPendingMessages();
        REQUIRE (project->save().wasOk());
        edit.getUndoManager().clearUndoHistory();
        open();
    }

    ~Song() { close(); }

    void open()
    {
        setEdit (context, &project->getEdit());
        getSelection (context).selectOnly (bass);
    }

    void close()
    {
        host.render = nullptr;
        clip = nullptr; // the clip goes with its Edit
        setEdit (context, nullptr);
    }

    // Closes the project, and opens it again from its file.
    void reopen()
    {
        close();
        const auto file = project->getFile();
        project = nullptr;
        juce::String error;
        project = phazite::Project::open (phazite::test::getEngine(), file, false, error);
        REQUIRE (project != nullptr);
        bass = te::getAudioTracks (project->getEdit())[0];
        utility = phazite::getDevices (*bass)[0];
        clip = nullptr;
        open();
    }

    static void setParameter (te::Plugin& plugin, std::string_view id, float value)
    {
        auto* parameter = dynamic_cast<phazite::DevicePlugin&> (plugin).findParameter (id);
        REQUIRE (parameter != nullptr);
        parameter->setParameter (value, juce::sendNotificationSync);
    }

    phazite::TrackID id() const { return phazite::getTrackID (*bass); }
    te::Edit& edit() const { return project->getEdit(); }
    juce::File frozenFile() const
    {
        return project->getCacheFolder().getChildFile ("trackFreeze_0_" + bass->itemID.toString() + ".freeze");
    }

    // Freezes Bass with its command's function, as the app does, and waits for it.
    void freeze()
    {
        host.finished.clear();
        phazite::toggleFreeze (context, host);
        REQUIRE (host.render != nullptr);
        REQUIRE (phazite::test::dispatchMessagesUntil ([this] { return ! host.finished.isEmpty(); }, 60000));
        REQUIRE (host.finished == juce::StringArray { "Freezing Bass: ok" });
        REQUIRE (bass->isFrozen (te::Track::individualFreeze));
    }

    bool hasFreezePoint() const { return bass->pluginList.findFirstPluginOfType<te::FreezePointPlugin>() != nullptr; }

    phazite::test::TemporaryFolder temp;
    std::unique_ptr<phazite::Project> project;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::test::RenderHost host;
    te::AudioTrack *bass = nullptr, *reverb = nullptr;
    te::WaveAudioClip::Ptr clip;
    te::Plugin* utility = nullptr;
};

// The RMS level of a stretch of a file's left channel, in seconds.
double getLevel (const juce::File& file, double start, double end)
{
    const std::unique_ptr<juce::AudioFormatReader> reader (
        phazite::test::getEngine().getAudioFileFormatManager().readFormatManager.createReaderFor (file));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
    const auto first = juce::roundToInt (start * reader->sampleRate);
    return audio.getRMSLevel (0, first, juce::roundToInt (end * reader->sampleRate) - first);
}
} // namespace

TEST_CASE ("Freezing renders a track up to its fader in the background, and plays that file until it is unfrozen")
{
    Song song;
    CHECK (phazite::getWhyNotFreeze (song.context, song.id()).isEmpty());
    CHECK (phazite::getFreezePointIndex (*song.bass) == song.bass->pluginList.indexOf (song.bass->getVolumePlugin()));
    CHECK (phazite::getFreezeCommandName (song.context) == "Freeze Track");
    CHECK (phazite::canToggleFreeze (song.context));

    song.freeze();
    CHECK (phazite::isFrozen (song.context, song.id()));
    CHECK (phazite::isSelectedTrackFrozen (song.context));
    CHECK (phazite::getFreezeCommandName (song.context) == "Unfreeze Track");
    CHECK (phazite::getWhyNotFreeze (song.context, song.id()) == "Bass is frozen already.");

    // The utility is in the file, and stops playing; the fader and the send after it stay live.
    CHECK (phazite::isFrozenDevice (song.context, phazite::getDeviceID (*song.utility)));
    CHECK (song.utility->isFrozen());
    CHECK_FALSE (song.bass->getVolumePlugin()->isFrozen());
    CHECK_FALSE (phazite::getSends (*song.bass)[0]->isFrozen());
    CHECK (song.hasFreezePoint());
    CHECK (phazite::getDevices (*song.bass).size() == 1); // the freeze point isn't a device

    // The file is in the project's Cache folder: the clip and its tail.
    REQUIRE (song.frozenFile().existsAsFile());
    CHECK (te::AudioFile (song.edit().engine, song.frozenFile()).getLength() >= 1.5);

    // Not an undo step, but a change to save; the render's switches are as they were.
    CHECK_FALSE (song.edit().getUndoManager().canUndo());
    CHECK (song.project->hasUnsavedChanges());
    CHECK (song.utility->isEnabled());
    CHECK (song.bass->getVolumePlugin()->isEnabled());
    CHECK_FALSE (song.bass->isMuted (false));

    // Unfreezing plays it live again, and leaves the file until the project closes.
    phazite::toggleFreeze (song.context, song.host);
    CHECK_FALSE (phazite::isFrozen (song.context, song.id()));
    CHECK_FALSE (song.utility->isFrozen());
    CHECK_FALSE (song.hasFreezePoint());
    CHECK_FALSE (song.edit().getUndoManager().canUndo());
    const auto frozenFile = song.frozenFile();
    CHECK (frozenFile.existsAsFile());

    song.close();
    song.project = nullptr; // closing purges the files nothing uses
    CHECK_FALSE (frozenFile.existsAsFile());
}

TEST_CASE ("A frozen track stays frozen while only its mixer changes, and unfreezes when what it froze does")
{
    Song song;
    song.freeze();

    // Its fader, name, mute switch and send: still frozen.
    song.bass->getVolumePlugin()->setVolumeDb (-3.0f);
    phazite::renameTrack (*song.bass, "Bass Line");
    song.bass->setMute (true);
    song.bass->setMute (false);
    phazite::getSends (*song.bass)[0]->setGainDb (-12.0f);
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::isFrozen (song.context, song.id()));

    // Its clip: unfrozen, in the clip's own undo step.
    {
        const phazite::UndoStep step (song.edit(), "Move Clip");
        song.clip->setStart (te::TimePosition::fromSeconds (0.5), true, false);
    }

    CHECK_FALSE (phazite::isFrozen (song.context, song.id()));
    CHECK_FALSE (song.hasFreezePoint());
    CHECK_FALSE (song.utility->isFrozen());
    CHECK (phazite::describeUndo (&song.edit()) == juce::String ("Undo Move Clip"));

    // Undo puts the clip back, and leaves it unfrozen: the file may not hold what plays.
    phazite::undo (song.edit());
    CHECK_FALSE (phazite::isFrozen (song.context, song.id()));

    // A frozen device: unfrozen too.
    song.bass->setName ("Bass"); // the render's title
    song.freeze();
    Song::setParameter (*song.utility, "gain", 3.0f);
    phazite::test::dispatchPendingMessages(); // the value reaches the device's state
    CHECK_FALSE (phazite::isFrozen (song.context, song.id()));

    // Undo of a change made before the freeze: unfrozen.
    {
        const phazite::UndoStep step (song.edit(), "Clip Gain");
        song.clip->setGainDB (-3.0f);
    }

    song.freeze();
    phazite::undo (song.edit());
    CHECK_FALSE (phazite::isFrozen (song.context, song.id()));
}

TEST_CASE ("A frozen track opens frozen from the project's Cache folder, and unfrozen without its file")
{
    Song song;
    song.freeze();
    REQUIRE (song.project->save().wasOk());

    song.reopen();
    CHECK (phazite::isFrozen (song.context, song.id()));
    CHECK (song.hasFreezePoint());
    CHECK (song.utility->isFrozen());
    CHECK_FALSE (song.project->hasUnsavedChanges());
    CHECK (song.frozenFile().existsAsFile());
    CHECK (phazite::getFreezeCommandName (song.context) == "Unfreeze Track");

    // Without the Cache folder, it opens unfrozen, as it would play.
    song.close();
    song.project = nullptr;
    REQUIRE (phazite::Project::getCacheFolder (song.temp.folder.getChildFile ("Song").getChildFile ("Song.phazite")).deleteRecursively());

    juce::String error;
    song.project = phazite::Project::open (phazite::test::getEngine(), song.temp.folder.getChildFile ("Song").getChildFile ("Song.phazite"),
                                           false, error);
    REQUIRE (song.project != nullptr);
    song.bass = te::getAudioTracks (song.project->getEdit())[0];
    song.utility = phazite::getDevices (*song.bass)[0];
    song.open();
    CHECK_FALSE (phazite::isFrozen (song.context, song.id()));
    CHECK_FALSE (song.hasFreezePoint());
    CHECK_FALSE (song.utility->isFrozen());
}

TEST_CASE ("Freezing stops before a device that listens to a sidechain or a send before the fader")
{
    Song song;
    auto& edit = song.edit();
    const auto kick = edit.insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (edit), nullptr);
    REQUIRE (kick != nullptr);

    auto* compressor = phazite::insertDevice (*song.bass, edit.getPluginCache().createNewPlugin (phazite::devices::Compressor::typeID, {}));
    REQUIRE (compressor != nullptr);
    compressor->setSidechainSourceID (kick->itemID);
    CHECK (phazite::getFreezePointIndex (*song.bass) == song.bass->pluginList.indexOf (compressor));

    compressor->setSidechainSourceID ({});
    auto* preFader = phazite::addSend (*song.bass, phazite::getReturnBus (*song.reverb), true);
    REQUIRE (preFader != nullptr);
    CHECK (phazite::getFreezePointIndex (*song.bass) == song.bass->pluginList.indexOf (preFader));

    // Frozen up to the send, which goes on sending what the file plays.
    song.freeze();
    CHECK (song.utility->isFrozen());
    CHECK (compressor->isFrozen());
    CHECK_FALSE (preFader->isFrozen());
}

TEST_CASE ("Returns, buses and empty tracks aren't frozen or bounced, and say why")
{
    Song song;
    CHECK (phazite::getWhyNotFreeze (song.context, phazite::getTrackID (*song.reverb))
           == "Returns can't be frozen: their sound comes from the tracks that send to them.");
    CHECK (phazite::getWhyNotBounce (song.context, phazite::getTrackID (*song.reverb))
           == "Returns can't be bounced: their sound comes from the tracks that send to them.");

    auto* bus = phazite::groupIntoFolder ({ song.bass }, nullptr);
    REQUIRE (bus != nullptr);
    CHECK (phazite::getWhyNotFreeze (song.context, phazite::getTrackID (*bus))
           == "Folders and buses can't be frozen: their sound is their tracks'.");

    auto* empty = song.edit().insertNewAudioTrack (te::TrackInsertPoint::getEndOfTracks (song.edit()), nullptr).get();
    REQUIRE (empty != nullptr);
    empty->setName ("Empty");
    CHECK (phazite::getWhyNotBounce (song.context, phazite::getTrackID (*empty)) == "Empty has nothing to bounce: it has no clips.");

    // The command tells the host why.
    getSelection (song.context).selectOnly (empty);
    CHECK_FALSE (phazite::canToggleFreeze (song.context));
    phazite::toggleFreeze (song.context, song.host);
    CHECK (song.host.finished == juce::StringArray { "Freezing Empty: Empty has nothing to freeze: it has no clips." });
    CHECK (song.host.render == nullptr);
}

TEST_CASE ("A freeze cancelled leaves the track as it was")
{
    Song song;
    song.bass->setMute (true);
    phazite::toggleFreeze (song.context, song.host);
    REQUIRE (song.host.render != nullptr);
    CHECK_FALSE (song.bass->isMuted (false)); // rendering
    CHECK_FALSE (song.bass->getVolumePlugin()->isEnabled());

    song.host.render = nullptr;
    phazite::test::dispatchPendingMessages();
    CHECK (song.host.finished.isEmpty());
    CHECK_FALSE (phazite::isFrozen (song.context, song.id()));
    CHECK_FALSE (song.hasFreezePoint());
    CHECK (song.bass->isMuted (false));
    CHECK (song.bass->getVolumePlugin()->isEnabled());
    CHECK (song.utility->isEnabled());

    for (const auto& file : song.project->getCacheFolder().findChildFiles (juce::File::findFiles, false))
        CHECK_FALSE (file.getFileName().startsWith ("trackFreeze"));
}

TEST_CASE ("Bounce in Place renders a track before its fader onto a new audio track under it, and mutes the original")
{
    Song song;
    CHECK (phazite::canBounceSelectedTrack (song.context));
    phazite::bounceSelectedTrack (song.context, song.host);
    REQUIRE (song.host.render != nullptr);
    CHECK (song.host.render->getTitle() == "Bouncing Bass");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.host.finished.isEmpty(); }, 60000));
    REQUIRE (song.host.finished == juce::StringArray { "Bouncing Bass: ok" });

    // A new track right under Bass, playing the file from the song's start, selected.
    const auto tracks = te::getAudioTracks (song.edit());
    REQUIRE (tracks.size() == 3);
    auto* bounced = tracks[1];
    CHECK (bounced->getName() == "Bass (Bounce)");
    CHECK (getSelection (song.context).isSelected (bounced));
    REQUIRE (bounced->getClips().size() == 1);
    auto* wave = dynamic_cast<te::WaveAudioClip*> (bounced->getClips()[0]);
    REQUIRE (wave != nullptr);
    CHECK (wave->getPosition().getStart() == te::TimePosition());
    const auto file = wave->getSourceFileReference().getFile();
    CHECK (file.getParentDirectory() == song.project->getAudioFolder());
    CHECK (file.getFileName() == "Bass (Bounce).wav");

    // The sound before the fader: the sine at a quarter of full scale in each channel, with the utility's 6 dB, without the
    // fader's -6 dB (or its pan law). The fader and the send come along, on the new track.
    const auto sineDecibels = juce::Decibels::gainToDecibels (0.25 / std::sqrt (2.0));
    CHECK (juce::Decibels::gainToDecibels (getLevel (file, 0.1, 1.4)) == doctest::Approx (sineDecibels + 6.0).epsilon (0.05));
    CHECK (bounced->getVolumePlugin()->getVolumeDb() == doctest::Approx (-6.0f).epsilon (0.01));
    REQUIRE (phazite::getSends (*bounced).size() == 1);
    CHECK (phazite::getSends (*bounced)[0]->getBusNumber() == phazite::getReturnBus (*song.reverb));
    CHECK (phazite::getDevices (*bounced).isEmpty());

    // The original, muted and collapsed; one undo step, which takes it all back.
    CHECK (song.bass->isMuted (false));
    CHECK (phazite::isCollapsed (*song.bass));
    CHECK (phazite::describeUndo (&song.edit()) == juce::String ("Undo Bounce in Place"));
    phazite::undo (song.edit());
    CHECK (te::getAudioTracks (song.edit()).size() == 2);
    CHECK_FALSE (song.bass->isMuted (false));
}

namespace
{
template <typename ComponentType>
juce::Array<ComponentType*> findAll (juce::Component& parent)
{
    juce::Array<ComponentType*> found;

    for (auto* child : parent.getChildren())
    {
        if (auto* wanted = dynamic_cast<ComponentType*> (child))
            found.add (wanted);

        found.addArray (findAll<ComponentType> (*child));
    }

    return found;
}
} // namespace

TEST_CASE ("A frozen track shows a snowflake that unfreezes it, frosted clips, and frozen devices with their controls off")
{
    Song song;
    phazite::CommandRegistry registry;
    phazite::ui::CommandBridge commands { registry };
    phazite::ui::Playlist playlist { commands, song.context };
    phazite::test::TestWindow window { playlist, 1000, 600 };
    phazite::ui::DevicesView devices { commands, song.context };
    phazite::test::TestWindow devicesWindow { devices, 1000, 400 };
    phazite::test::dispatchPendingMessages();

    const auto findHeader = [&playlist, &song]() -> phazite::ui::TrackHeader*
    {
        for (auto* header : findAll<phazite::ui::TrackHeader> (playlist))
            if (header->getTrack() == song.id())
                return header;

        return nullptr;
    };

    auto* header = findHeader();
    REQUIRE (header != nullptr);
    CHECK_FALSE (header->getFrozenButton().isVisible());

    const auto clipsAreFrosted = [&playlist]
    {
        const auto clips = findAll<phazite::ui::ClipComponent> (playlist);
        return ! clips.isEmpty() && std::all_of (clips.begin(), clips.end(), [] (auto* clip) { return clip->isFrozen(); });
    };

    CHECK_FALSE (clipsAreFrosted());

    song.freeze();
    phazite::test::dispatchPendingMessages();
    header = findHeader();
    REQUIRE (header != nullptr);
    CHECK (header->getFrozenButton().isVisible());
    CHECK (header->getFrozenButton().getTooltip().startsWith ("Bass is frozen"));
    CHECK (clipsAreFrosted());

    // The utility's controls are off, in the Devices view.
    devices.showDevice (phazite::getDeviceID (*song.utility));
    phazite::test::dispatchPendingMessages();
    auto* panel = devices.findPanel (phazite::getDeviceID (*song.utility));
    REQUIRE (panel != nullptr);
    auto* power = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*panel, "On"));
    REQUIRE (power != nullptr);
    CHECK_FALSE (power->isEnabled());

    // The snowflake plays it live again (the views show it anew).
    {
        phazite::test::Mouse mouse (header->getFrozenButton());
        mouse.click (header->getFrozenButton().getLocalBounds().getCentre());
    }

    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (phazite::isFrozen (song.context, song.id()));
    CHECK_FALSE (clipsAreFrosted());
    panel = devices.findPanel (phazite::getDeviceID (*song.utility));
    REQUIRE (panel != nullptr);
    power = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*panel, "On"));
    REQUIRE (power != nullptr);
    CHECK (power->isEnabled());
}
