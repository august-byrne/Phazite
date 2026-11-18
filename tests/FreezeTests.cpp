// SPDX-License-Identifier: AGPL-3.0-or-later
// Freezing a track and bouncing it in place (roadmap step 2.11; src/model/mixer/FreezeModel.h): rendered in the
// background up to the fader (stopping before a sidechained device), played from a file in the project's Cache folder,
// kept frozen while only its mixer changes and unfrozen when what it froze changes, opened frozen, and cancelled without
// a trace; and a bounce onto a new audio track under the original, as one undo step.
#include "TestClips.h"
#include "TestHelpers.h"
#include "UiTestHelpers.h"
#include "devices/dynamics/Compressor.h"
#include "devices/stereo/StereoExpander.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/devices/internal/Parameters.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/FreezeModel.h"
#include "model/mixer/internal/Freeze.h"
#include "model/mixer/internal/Mixer.h"
#include "model/mixer/internal/Sidechain.h"
#include "model/modulation/internal/Modulation.h"
#include "model/playback/internal/SongPlayer.h"
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
using phazite::DeviceID;
using phazite::TrackID;

// A saved project with Bass, a sine from the start to 1.5 s through a stereo expander that turns it up 6 dB, its fader at -6 dB
// and a send after it to the return Reverb; Bass is selected.
struct Song
{
    Song()
    {
        juce::String error;
        project = phazite::Project::create (temp.folder, "Song", error);
        REQUIRE (project != nullptr);
        auto& song = getSong();
        bass = phazite::ensureTracks (song, 1)[0];
        phazite::setTrackName (song, bass, "Bass");

        const auto sound = phazite::test::writeSound (temp.folder.getChildFile ("Sine.wav"), 2.0);
        clip = phazite::test::addAudioClip (song, bass, "Sine", sound, 0.0, 1.5);
        REQUIRE (clip.isValid());

        expander = phazite::insertDeviceState (
            song, bass, phazite::makeOwnDeviceState (song, phazite::devices::StereoExpander::typeID, { { "gain", 6.0f } }));
        REQUIRE (expander.isValid());
        phazite::setChannelVolume (song, bass, -6.0f);

        reverb = phazite::addReturnTrack (song, "Reverb", nullptr);
        REQUIRE (reverb.isValid());
        REQUIRE (phazite::addSend (song, bass, reverb, false).isValid());

        REQUIRE (project->save().wasOk());
        song.getUndoManager().clearUndoHistory();
        open();
    }

    ~Song() { close(); }

    void open()
    {
        setSong (context, &getSong());
        getSelection (context).selectOnly (bass);
    }

    void close()
    {
        host.render = nullptr;
        setSong (context, nullptr);
    }

    // Closes the project, and opens it again from its file: its tracks, clips and devices keep their IDs.
    void reopen()
    {
        close();
        const auto file = project->getFile();
        project = nullptr;
        juce::String error;
        project = phazite::Project::open (file, false, error);
        REQUIRE (project != nullptr);
        open();
    }

    phazite::Song& getSong() const { return project->getSong(); }
    juce::File frozenFile() const { return phazite::getFrozenFile (getSong(), bass); }

    // Freezes Bass with its command's function, as the app does, and waits for it.
    void freeze()
    {
        host.finished.clear();
        phazite::toggleFreeze (context, host);
        REQUIRE (host.render != nullptr);
        REQUIRE (phazite::test::dispatchMessagesUntil ([this] { return ! host.finished.isEmpty(); }));
        REQUIRE (host.finished == juce::StringArray { "Freezing Bass: ok" });
        REQUIRE (phazite::isFrozen (context, bass));
    }

    // The expander's gain, set by hand as its control sets it.
    void setExpanderGain (float decibels)
    {
        const phazite::UndoStep step (getSong(), "Change Gain");
        phazite::setValueSetByHand (getSong(), phazite::ParameterRef { expander, "gain" }, decibels);
    }

    phazite::test::TemporaryFolder temp;
    std::unique_ptr<phazite::Project> project;
    phazite::ProjectContext context { phazite::test::getEngineContext() };
    phazite::test::RenderHost host;
    TrackID bass, reverb;
    phazite::ClipID clip;
    DeviceID expander;
};

// A file's audio, which must be one.
juce::AudioBuffer<float> readAudio (const juce::File& file, double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    REQUIRE (reader != nullptr);
    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true));
    sampleRate = reader->sampleRate;
    return audio;
}

// How long a file lasts, in seconds.
double getLength (const juce::File& file)
{
    double sampleRate = 0.0;
    const auto audio = readAudio (file, sampleRate);
    return audio.getNumSamples() / sampleRate;
}

// The RMS level of a stretch of a file's left channel, in seconds.
double getLevel (const juce::File& file, double start, double end)
{
    double sampleRate = 0.0;
    const auto audio = readAudio (file, sampleRate);
    const auto first = juce::roundToInt (start * sampleRate);
    return audio.getRMSLevel (0, first, juce::roundToInt (end * sampleRate) - first);
}
} // namespace

TEST_CASE ("Freezing renders a track up to its fader in the background, and plays that file until it is unfrozen")
{
    Song song;
    auto& model = song.getSong();
    CHECK (phazite::getWhyNotFreeze (song.context, song.bass).isEmpty());
    CHECK (phazite::getFrozenDevices (model, song.bass) == std::vector { song.expander });
    CHECK (phazite::getFreezeCommandName (song.context) == "Freeze Track");
    CHECK (phazite::canToggleFreeze (song.context));

    song.freeze();
    CHECK (phazite::isFrozen (song.context, song.bass));
    CHECK (phazite::isSelectedTrackFrozen (song.context));
    CHECK (phazite::getFreezeCommandName (song.context) == "Unfreeze Track");
    CHECK (phazite::getWhyNotFreeze (song.context, song.bass) == "Bass is frozen already.");
    CHECK (phazite::getFrozenTracks (model) == std::vector { song.bass });

    // The expander is in the file, and stops playing; the fader and the send after it stay live.
    CHECK (phazite::isFrozenDevice (song.context, song.expander));
    CHECK (phazite::isFrozenDevice (model, song.expander));
    CHECK_FALSE (phazite::isFrozenDevice (model, phazite::getFader (model, song.bass)));
    CHECK_FALSE (phazite::isFrozenDevice (model, phazite::getSends (model, song.bass)[0]));
    CHECK (phazite::getDevices (model, song.bass).size() == 1);

    // The file is in the project's Cache folder: the clip and its tail.
    REQUIRE (song.frozenFile().existsAsFile());
    CHECK (song.frozenFile().getParentDirectory() == song.project->getCacheFolder());
    CHECK (getLength (song.frozenFile()) >= 1.5);

    // Not an undo step, but a change to save; the render's switches are as they were.
    CHECK_FALSE (phazite::canUndo (model));
    CHECK (song.project->hasUnsavedChanges());
    CHECK (phazite::isDeviceOn (model, song.expander));
    CHECK_FALSE (phazite::isTrackMuted (model, song.bass, false));

    // Unfreezing plays it live again, and leaves the file until the project closes.
    phazite::toggleFreeze (song.context, song.host);
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (phazite::findTrackState (model, song.bass).hasProperty (phazite::schema::trackFrozen));
    CHECK_FALSE (phazite::isFrozenDevice (model, song.expander));
    CHECK_FALSE (phazite::canUndo (model));
    const auto frozenFile = song.frozenFile();
    CHECK (frozenFile.existsAsFile());

    song.close();
    song.project = nullptr; // closing purges the files nothing uses
    CHECK_FALSE (frozenFile.existsAsFile());
}

TEST_CASE ("A frozen track stays frozen while only its mixer changes, and unfreezes when what it froze does")
{
    Song song;
    auto& model = song.getSong();
    song.freeze();

    // Its fader, name, mute switch and send: still frozen.
    phazite::setChannelVolume (model, song.bass, -3.0f);
    phazite::renameTrack (model, song.bass, "Bass Line");
    phazite::setTrackMuted (model, song.bass, true);
    phazite::setTrackMuted (model, song.bass, false);
    phazite::setSendLevel (model, phazite::getSends (model, song.bass)[0], -12.0f);
    phazite::test::dispatchPendingMessages();
    CHECK (phazite::isFrozen (song.context, song.bass));

    // Its clip: unfrozen, in the clip's own undo step.
    {
        const phazite::UndoStep step (model, "Move Clip");
        const auto halfASecond = phazite::BeatPosition::fromBeats (1.0); // at 120 BPM
        phazite::trimClipStart (model, song.clip, halfASecond);
    }

    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (phazite::isFrozenDevice (model, song.expander));
    CHECK (phazite::describeUndo (&model) == juce::String ("Undo Move Clip"));

    // Undo puts the clip back, and leaves it unfrozen: the file may not hold what plays.
    phazite::undo (model);
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));

    // A frozen device: unfrozen too.
    phazite::setTrackName (model, song.bass, "Bass"); // the render's title
    song.freeze();
    song.setExpanderGain (3.0f);
    phazite::test::dispatchPendingMessages();
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));

    // Undo of a change made before the freeze: unfrozen.
    {
        const phazite::UndoStep step (model, "Clip Gain");
        phazite::setClipGain (model, song.clip, -3.0f);
    }

    song.freeze();
    phazite::undo (model);
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
}

TEST_CASE ("A frozen track opens frozen from the project's Cache folder, and unfrozen without its file")
{
    Song song;
    song.freeze();
    REQUIRE (song.project->save().wasOk());

    // Frozen in the song (Schema.h: trackFrozen): a song made from its state alone plays the file too.
    CHECK ((bool) phazite::findTrackState (song.getSong(), song.bass)[phazite::schema::trackFrozen]);

    {
        const auto copy =
            phazite::makeSong (song.getSong().getState().createCopy(), song.project->getFile(), song.project->getCacheFolder());
        REQUIRE (copy != nullptr);
        CHECK (phazite::getFrozenTracks (*copy) == std::vector { song.bass });
        CHECK (phazite::isFrozenDevice (*copy, song.expander));
        CHECK (phazite::getFrozenFile (*copy, song.bass) == song.frozenFile());
    }

    song.reopen();
    CHECK (phazite::isFrozen (song.context, song.bass));
    CHECK (phazite::isFrozenDevice (song.getSong(), song.expander));
    CHECK_FALSE (song.project->hasUnsavedChanges());
    CHECK (song.frozenFile().existsAsFile());
    CHECK (phazite::getFreezeCommandName (song.context) == "Unfreeze Track");

    // Without the Cache folder, it opens unfrozen, as it would play.
    song.close();
    song.project = nullptr;
    const auto file = song.temp.folder.getChildFile ("Song").getChildFile ("Song.phazite");
    REQUIRE (phazite::Project::getCacheFolder (file).deleteRecursively());

    juce::String error;
    song.project = phazite::Project::open (file, false, error);
    REQUIRE (song.project != nullptr);
    song.open();
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (phazite::isFrozenDevice (song.getSong(), song.expander));
    CHECK_FALSE (phazite::findTrackState (song.getSong(), song.bass).hasProperty (phazite::schema::trackFrozen));
}

TEST_CASE ("Freezing stops before a device that listens to a sidechain; a send before the fader sends what the file plays")
{
    Song song;
    auto& model = song.getSong();
    const auto kick = phazite::insertPlainTrack (model, TrackID());
    REQUIRE (kick.isValid());

    const auto compressor =
        phazite::insertDeviceState (model, song.bass, phazite::makeOwnDeviceState (model, phazite::devices::Compressor::typeID));
    REQUIRE (compressor.isValid());
    phazite::setSidechainSource (model, compressor, kick);
    CHECK (phazite::getFrozenDevices (model, song.bass) == std::vector { song.expander });

    // A send before the fader doesn't stop it: frozen up to the fader, the send goes on sending what the file plays.
    phazite::setSidechainSource (model, compressor, TrackID());
    const auto preFader = phazite::addSend (model, song.bass, song.reverb, true);
    REQUIRE (preFader.isValid());
    CHECK (phazite::getFrozenDevices (model, song.bass) == std::vector { song.expander, compressor });

    song.freeze();
    CHECK (phazite::isFrozenDevice (model, song.expander));
    CHECK (phazite::isFrozenDevice (model, compressor));
    CHECK_FALSE (phazite::isFrozenDevice (model, preFader));
}

TEST_CASE ("Returns, buses and empty tracks aren't frozen or bounced, and say why")
{
    Song song;
    auto& model = song.getSong();
    CHECK (phazite::getWhyNotFreeze (song.context, song.reverb)
           == "Returns can't be frozen: their sound comes from the tracks that send to them.");
    CHECK (phazite::getWhyNotBounce (song.context, song.reverb)
           == "Returns can't be bounced: their sound comes from the tracks that send to them.");

    const auto bus = phazite::groupIntoFolder (model, { song.bass }, nullptr);
    REQUIRE (bus.isValid());
    CHECK (phazite::getWhyNotFreeze (song.context, bus) == "Folders and buses can't be frozen: their sound is their tracks'.");

    const auto empty = phazite::insertPlainTrack (model, TrackID());
    REQUIRE (empty.isValid());
    phazite::setTrackName (model, empty, "Empty");
    CHECK (phazite::getWhyNotBounce (song.context, empty) == "Empty has nothing to bounce: it has no clips.");

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
    auto& model = song.getSong();
    phazite::setTrackMuted (model, song.bass, true);
    phazite::toggleFreeze (song.context, song.host);
    REQUIRE (song.host.render != nullptr);
    CHECK (phazite::isTrackMuted (model, song.bass, false)); // nothing of the song changes while Phazite's engine renders it

    song.host.render = nullptr;
    phazite::test::dispatchPendingMessages();
    CHECK (song.host.finished.isEmpty());
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (phazite::isFrozenDevice (model, song.expander));
    CHECK (phazite::isTrackMuted (model, song.bass, false));
    CHECK (phazite::isDeviceOn (model, song.expander));
    CHECK_FALSE (song.frozenFile().existsAsFile());

    for (const auto& file : song.project->getCacheFolder().findChildFiles (juce::File::findFiles, false))
        CHECK_FALSE (file.getFileName().startsWith ("Frozen"));
}

TEST_CASE ("A track frozen while it is muted, or while another track is soloed, freezes its sound, not silence")
{
    // A render before the fader hears the track whatever its mute switch or a solo elsewhere says: muted, it rendered its
    // first block alone; Bounce in Place mutes the original, so a second bounce of it was silent.
    for (const auto soloElsewhere : { false, true })
    {
        CAPTURE (soloElsewhere);
        Song song;
        auto& model = song.getSong();

        if (soloElsewhere)
            phazite::setTrackSoloed (model, song.reverb, true);
        else
            phazite::setTrackMuted (model, song.bass, true);

        song.freeze();
        CHECK (getLevel (song.frozenFile(), 0.5, 1.0) > 0.05);
    }
}

TEST_CASE ("Bounce in Place renders a track before its fader onto a new audio track under it, and mutes and freezes the original")
{
    Song song;
    auto& model = song.getSong();
    CHECK (phazite::canBounceSelectedTrack (song.context));
    phazite::bounceSelectedTrack (song.context, song.host);
    REQUIRE (song.host.render != nullptr);
    CHECK (song.host.render->getTitle() == "Bouncing Bass");
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.host.finished.isEmpty(); }));
    REQUIRE (song.host.finished == juce::StringArray { "Bouncing Bass: ok" });

    // A new track right under Bass, playing the file from the song's start, selected.
    const auto tracks = phazite::getTrackIDs (model);
    REQUIRE (tracks.size() == 3);
    const auto bounced = tracks[1];
    CHECK (phazite::getTrackName (model, bounced) == "Bass (Bounce)");
    CHECK (getSelection (song.context).isSelected (bounced));
    const auto clips = phazite::getClips (model, bounced);
    REQUIRE (clips.size() == 1);
    const auto clipState = phazite::findClipState (model, clips[0]);
    REQUIRE (phazite::isAudioState (clipState));
    CHECK (phazite::getClipRange (model, clipState).getStart() == phazite::BeatPosition());
    const auto file = phazite::getClipFile (model, clipState);
    CHECK (file.getParentDirectory() == song.project->getAudioFolder());
    CHECK (file.getFileName() == "Bass (Bounce).wav");

    // The sound before the fader: the sine at a quarter of full scale in each channel, with the expander's 6 dB, without the
    // fader's -6 dB (or its pan law). The fader comes along, on the new track, and the send moves there (known bug 194).
    const auto sineDecibels = juce::Decibels::gainToDecibels (0.25 / std::sqrt (2.0));
    CHECK (juce::Decibels::gainToDecibels (getLevel (file, 0.1, 1.4)) == doctest::Approx (sineDecibels + 6.0).epsilon (0.05));
    CHECK (phazite::getChannelVolume (model, bounced) == doctest::Approx (-6.0f).epsilon (0.01));
    const auto sends = phazite::getSends (model, bounced);
    REQUIRE (sends.size() == 1);
    CHECK (phazite::getSendTarget (model, sends[0]) == song.reverb);
    CHECK (phazite::getSends (model, song.bass).empty());
    CHECK (phazite::getDevices (model, bounced).empty());

    // The original muted, and shown as it was: collapsing it was view state, outside the step, which Undo left collapsed
    // (known bug 138). One undo step, which takes it all back, mute included, though a mute alone is no undo step; Redo
    // brings it back.
    CHECK (phazite::isTrackMuted (model, song.bass, false));
    CHECK_FALSE (phazite::isCollapsed (model, song.bass));
    CHECK (phazite::describeUndo (&model) == juce::String ("Undo Bounce in Place"));

    // And frozen in the same step, the bounce its frozen file: a muted track plays on underneath, and so would cost its
    // devices for nothing (the review of known bug 78). The frozen file is the bounce, sample for sample.
    CHECK (phazite::isFrozen (song.context, song.bass));
    REQUIRE (song.frozenFile().existsAsFile());
    CHECK (getLevel (song.frozenFile(), 0.1, 1.4) == doctest::Approx (getLevel (file, 0.1, 1.4)));
    phazite::test::dispatchPendingMessages(); // what keeps frozen tracks honest has heard the step: it stays frozen
    CHECK (phazite::isFrozen (song.context, song.bass));

    phazite::undo (model);
    CHECK (phazite::getTrackIDs (model).size() == 2);
    CHECK_FALSE (phazite::isTrackMuted (model, song.bass, false));
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (phazite::isCollapsed (model, song.bass));
    CHECK (phazite::getSends (model, song.bass).size() == 1);
    REQUIRE (phazite::canRedo (model));
    phazite::redo (model);
    CHECK (phazite::getTrackIDs (model).size() == 3);
    CHECK (phazite::isTrackMuted (model, song.bass, false));
    CHECK (phazite::isFrozen (song.context, song.bass));
}

namespace
{
// Bounces Bass in place with its command's function, as the app does, and waits for it.
void bounce (Song& song)
{
    song.host.finished.clear();
    phazite::bounceSelectedTrack (song.context, song.host);
    REQUIRE (song.host.render != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.host.finished.isEmpty(); }));
    REQUIRE (song.host.finished == juce::StringArray { "Bouncing Bass: ok" });
    phazite::test::dispatchPendingMessages();
}

// What the engine plays of a track: whether it plays a file (its frozen file, in place of its clips) and how many devices.
std::pair<bool, size_t> getPlayed (phazite::Song& song, TrackID track)
{
    phazite::SongPlayer player (song);
    const auto plan = player.compile (48000.0, 256, phazite::SongPlayer::Live {});

    for (const auto& strip : plan->strips)
        if (strip.id == (std::int64_t) track.getRaw())
            return { ! strip.audio.empty() && strip.audio.front().file != nullptr, strip.devices.size() };

    return { false, 0 };
}
} // namespace

TEST_CASE ("Bounce in Place's freeze is kept across a save, holds a track's containers, and isn't brought back without its file")
{
    // The review of known bug 78 (E): what the freeze that Bounce in Place leaves the original with does where it was not
    // looked at.
    SUBCASE ("Saved and opened again: still frozen, playing its file")
    {
        Song song;
        bounce (song);
        REQUIRE (song.project->save().wasOk());
        song.reopen();
        CHECK (phazite::isFrozen (song.context, song.bass));
        CHECK (song.frozenFile().existsAsFile());
        CHECK (getPlayed (song.getSong(), song.bass) == std::pair<bool, size_t> { true, 0 });
    }

    SUBCASE ("A track holding a container: frozen too, the container in its file")
    {
        Song song;
        REQUIRE (phazite::groupIntoContainer (song.context, { song.expander }).isValid());
        bounce (song);
        CHECK (phazite::isFrozen (song.context, song.bass));
        CHECK (getPlayed (song.getSong(), song.bass) == std::pair<bool, size_t> { true, 0 });
    }

    SUBCASE ("Undone, its file let go of, and redone: not frozen without the file, which would play nothing")
    {
        Song song;
        auto& model = song.getSong();
        bounce (song);
        phazite::undo (model);
        phazite::purgeCache (model); // as a project closing does: the file of a track that isn't frozen goes
        REQUIRE_FALSE (song.frozenFile().existsAsFile());
        phazite::redo (model);
        phazite::test::dispatchPendingMessages();
        CHECK (phazite::isTrackMuted (model, song.bass, false));
        CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
        CHECK (getPlayed (model, song.bass).second == 1); // its devices play live, for when it is unmuted
    }
}

TEST_CASE ("Bounce in Place takes the automation and the modulation of the track's fader and sends along to the new track")
{
    // The render is the track before its fader: a volume fade, a pan move or a send throw is no part of it, and stayed with
    // the muted original, so the bounce played without it.
    Song song;
    auto& model = song.getSong();
    const auto fader = phazite::getFader (model, song.bass);
    const auto send = phazite::getSends (model, song.bass).front();
    const phazite::ParameterRef volume { fader, phazite::getVolumeParameterID (model, song.bass) };
    const phazite::ParameterRef pan { fader, phazite::getPanParameterID (model, song.bass) };
    const phazite::ParameterRef level { send, phazite::schema::sendLevelParameter };
    const phazite::ParameterRef gain { song.expander, "gain" };
    const auto at = [] (double beat) { return phazite::BeatPosition::fromBeats (beat); };

    phazite::addPoint (model, volume, { at (0.0), 0.7f, 0.0f });
    phazite::addPoint (model, volume, { at (4.0), 0.2f, 0.5f });
    phazite::addPoint (model, level, { at (2.0), 0.6f, 0.0f });
    phazite::addPoint (model, gain, { at (0.0), 0.9f, 0.0f }); // a device's: rendered, not copied

    const auto lfo = phazite::addModulator (model, song.bass, phazite::ModulatorType::lfo);
    const auto macro = phazite::addModulator (model, song.bass, phazite::ModulatorType::macro);
    const auto other = phazite::addModulator (model, song.bass, phazite::ModulatorType::random);
    REQUIRE (lfo.isValid());
    REQUIRE (macro.isValid());
    REQUIRE (other.isValid());
    phazite::setModulation (model, lfo, pan, 0.25f);
    phazite::setModulation (model, lfo, gain, 0.5f);
    phazite::setModulation (model, macro, level, -0.5f);
    phazite::setModulation (model, other, gain, 0.5f); // modulates the devices alone: stays behind
    model.getUndoManager().clearUndoHistory();
    const auto levelPoints = phazite::getPoints (model, level);

    phazite::bounceSelectedTrack (song.context, song.host);
    REQUIRE (song.host.render != nullptr);
    REQUIRE (phazite::test::dispatchMessagesUntil ([&song] { return ! song.host.finished.isEmpty(); }));
    REQUIRE (song.host.finished == juce::StringArray { "Bouncing Bass: ok" });

    const auto tracks = phazite::getTrackIDs (model);
    REQUIRE (tracks.size() == 3);
    const auto bounced = tracks[1];
    const auto newFader = phazite::getFader (model, bounced);
    const auto newSends = phazite::getSends (model, bounced);
    REQUIRE (newSends.size() == 1);
    REQUIRE (newFader != fader);
    const phazite::ParameterRef newVolume { newFader, volume.parameter }, newPan { newFader, pan.parameter };
    const phazite::ParameterRef newLevel { newSends[0], level.parameter };

    // The fader's and the send's curves, point for point; the original fader keeps its own, and the send moved (known bug
    // 194: a copy left with the muted original sent before its fader too).
    const auto samePoints = [] (const auto& first, const auto& second)
    {
        return first.size() == second.size()
               && std::equal (first.begin(), first.end(), second.begin(),
                              [] (const auto& x, const auto& y)
                              {
                                  return x.beat == y.beat && juce::approximatelyEqual (x.position, y.position)
                                         && juce::approximatelyEqual (x.curve, y.curve);
                              });
    };

    CHECK (phazite::getPoints (model, newVolume).size() == 2);
    CHECK (samePoints (phazite::getPoints (model, newVolume), phazite::getPoints (model, volume)));
    CHECK (phazite::getPoints (model, newLevel).size() == 1);
    CHECK (samePoints (phazite::getPoints (model, newLevel), levelPoints));
    CHECK (phazite::getPoints (model, newPan).empty());
    CHECK (phazite::getSends (model, song.bass).empty());

    // The modulators that moved the fader and the send, copied onto the new track with those modulations alone; the
    // original's macro modulated the send alone, which has gone, and modulates nothing now.
    const auto modulators = phazite::getModulators (model, bounced);
    REQUIRE (modulators.size() == 2);
    CHECK (phazite::getModulatorType (model, modulators[0]) == phazite::ModulatorType::lfo);
    CHECK (phazite::getModulatorType (model, modulators[1]) == phazite::ModulatorType::macro);
    const auto ofLfo = phazite::getModulations (model, modulators[0]), ofMacro = phazite::getModulations (model, modulators[1]);
    REQUIRE (ofLfo.size() == 1);
    CHECK (ofLfo[0].parameter == newPan);
    CHECK (ofLfo[0].amount == doctest::Approx (0.25f));
    REQUIRE (ofMacro.size() == 1);
    CHECK (ofMacro[0].parameter == newLevel);
    CHECK (ofMacro[0].amount == doctest::Approx (-0.5f));
    CHECK (phazite::getModulators (model, song.bass).size() == 3);
    CHECK (phazite::getModulations (model, lfo).size() == 2);
    CHECK (phazite::getModulations (model, macro).empty());

    // All of it in the bounce's one undo step, the send and its modulation back on the original.
    phazite::undo (model);
    CHECK (phazite::getTrackIDs (model).size() == 2);
    CHECK (phazite::getPoints (model, newVolume).empty());
    CHECK (phazite::getPoints (model, newLevel).empty());
    CHECK (phazite::getSends (model, song.bass) == std::vector { send });
    CHECK (samePoints (phazite::getPoints (model, level), levelPoints));
    CHECK (phazite::getModulations (model, macro).size() == 1);
    CHECK_FALSE (phazite::canUndo (model));
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
            if (header->getTrack() == song.bass)
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

    // The expander's controls are off, in the Devices view.
    devices.showDevice (song.expander);
    phazite::test::dispatchPendingMessages();
    auto* panel = devices.findPanel (song.expander);
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
    CHECK_FALSE (phazite::isFrozen (song.context, song.bass));
    CHECK_FALSE (clipsAreFrosted());
    panel = devices.findPanel (song.expander);
    REQUIRE (panel != nullptr);
    power = dynamic_cast<juce::Button*> (phazite::test::findComponentWithTitle (*panel, "On"));
    REQUIRE (power != nullptr);
    CHECK (power->isEnabled());
}
