// SPDX-License-Identifier: AGPL-3.0-or-later
#include "TestSong.h"
#include "UiTestHelpers.h"
#include "app/AppCommands.h"
#include "model/arrangement/AudioClipModel.h"
#include "model/arrangement/PlaylistModel.h"
#include "model/automation/AutomationModel.h"
#include "model/mixer/MixerModel.h"
#include "model/modulation/ModulationModel.h"
#include "model/notes/NotesModel.h"
#include "model/plugins/internal/PluginInstances.h"
#include "model/project/internal/ExampleSongs.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/transport/TransportModel.h"
#include "model/versions/VersionsModel.h"
#include "model/versions/internal/VersionHistory.h"

namespace phazite::test
{
namespace
{
    BeatPosition at (double beat)
    {
        return BeatPosition::fromBeats (beat);
    }

    int songsMade = 0; // in this run
} // namespace

TestSong::TestSong (Start start)
    : folder (temporary.folder.getChildFile ("Song " + juce::String (++songsMade)))
{
    juce::String error;
    REQUIRE (folder.createDirectory().wasOk());
    project = Project::create (folder, "Song", error,
                               start == Start::exodus ? getExampleSongContent (getEngineContext(), "exodus", getFactoryPresetsFolder())
                                                      : nullptr);
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    setSong (context, &project->getSong());
    addSongCommands (commands, context, renders);

    // As the app's (ProjectSession::addCommands()), but for its progress window: a command that reads the plug-ins' states
    // runs once those that said they changed have given them.
    context.setPluginStatesGate (
        [this] (std::function<void (bool)> run)
        { whenPluginStatesIn (project->getSong(), SandboxedInstance::Ask::changedAndAnswering, [run] (bool) { run (true); }); });
    commands.setPluginStatesGate ([this] (std::function<void (bool)> run) { context.withPluginStates (std::move (run)); });

    // As the app's save (ProjectSession::saveThen()): its plug-ins asked for their states first.
    save = [this] (std::function<void (bool)> then)
    { whenPluginStatesIn (project->getSong(), true, [this, then] (bool) { then (project != nullptr && project->save().wasOk()); }); };

    // Its history read, and its initial commit made, as a project's are as it starts.
    REQUIRE (waitForVersionHistory (context));

    const auto tracks = getAllPlaylistTracks (context);
    REQUIRE (! tracks.empty());
    firstTrack = tracks.front();
}

TestSong::~TestSong()
{
    renders.render = nullptr; // a freeze or a bounce still running, which needs the project
}

TestSong::SongDetacher::~SongDetacher()
{
    setSong (context, nullptr);

    // What its history still writes (its initial commit, say), which a close leaves to finish on its own, done before the
    // project's folder goes with the test, so that no test's writes run on into the next.
    waitForVersionsFinished();
}

bool waitForVersionHistory (ProjectContext& context)
{
    return dispatchMessagesUntil (
        [&context]
        {
            const auto* history = getVersionHistory (context);
            return history == nullptr || (! history->isReading() && ! history->isBusy());
        },
        versionHistoryTimeout);
}

Project& TestSong::getProject() const
{
    return *project;
}

juce::File TestSong::getSoundsFolder() const
{
    return folder.getChildFile ("Sounds");
}

void TestSong::saveWithPluginStates()
{
    // As the app saves: every plug-in asked for its state first, which a save doesn't wait for
    // (ProjectSession::saveThen(); model/plugins/internal/PluginInstances.h).
    auto asked = false;
    whenPluginStatesIn (project->getSong(), true, [&asked] (bool) { asked = true; });
    REQUIRE (dispatchMessagesUntil ([&asked] { return asked; }));
    REQUIRE (project->save().wasOk());
}

void TestSong::reopen (bool waitForPlugins)
{
    saveWithPluginStates();
    const auto file = project->getFile();
    setSong (context, nullptr);
    project = nullptr;

    juce::String error;
    project = Project::open (file, false, error);
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    setSong (context, &project->getSong());

    // Its plug-ins open in the background as it opens (model/plugins/internal/PluginInstances.h).
    if (waitForPlugins)
        REQUIRE (dispatchMessagesUntil ([this] { return getPluginsLoading (project->getSong()).isEmpty(); }, 60000));
}

bool TestSong::reopenAs (Reopening how)
{
    const auto file = project->getFile();
    const auto autosaveFile = Project::getAutosaveFile (file);
    juce::MemoryBlock autosave;

    if (how != Reopening::withoutSaving)
    {
        REQUIRE (project->autosave().wasOk());
        REQUIRE (autosaveFile.loadFileAsData (autosave));
    }

    setSong (context, nullptr);
    project = nullptr; // which deletes the autosave, as a close does; a crash leaves it

    if (how != Reopening::withoutSaving)
        REQUIRE (autosaveFile.replaceWithData (autosave.getData(), autosave.getSize()));

    juce::String error;
    project = Project::open (file, how == Reopening::afterCrashRestoring, error);

    if (project == nullptr)
        return false;

    setSong (context, &project->getSong());
    return true;
}

void TestSong::openSong (const juce::ValueTree& song)
{
    const auto file = project->getFile();
    setSong (context, nullptr);
    project = nullptr;

    const auto xml = song.createXml();
    REQUIRE ((xml != nullptr && xml->writeTo (file)));

    juce::String error;
    project = Project::open (file, false, error);
    REQUIRE_MESSAGE (project != nullptr, error.toStdString());
    setSong (context, &project->getSong());
    REQUIRE (dispatchMessagesUntil ([this] { return getPluginsLoading (project->getSong()).isEmpty(); }, 60000));
}

void TestSong::run (const juce::String& commandID)
{
    REQUIRE_MESSAGE (commands.invoke (commandID), (commandID + " is unknown or greyed out").toStdString());
}

void TestSong::setSwitch (const juce::String& commandID, bool on)
{
    const auto* command = commands.find (commandID);
    REQUIRE_MESSAGE ((command != nullptr && command->isTicked != nullptr), (commandID + " is no switch").toStdString());

    if (command->isTicked() != on)
        run (commandID);

    REQUIRE (command->isTicked() == on);
}

//==============================================================================
void TestSong::setTempo (double bpm)
{
    auto tempo = getTempoHandle (context);
    REQUIRE (tempo.isValid());
    tempo.setValue (bpm);
}

void TestSong::setTimeSignature (int numerator, int denominator)
{
    phazite::setTimeSignature (context, { numerator, denominator });
}

TrackID TestSong::addTrack (const juce::String& name)
{
    const auto track = addTrackAtEnd (context, false);
    REQUIRE (track.isValid());
    renameTrack (context, track, name);
    return track;
}

TrackID TestSong::addBus (const juce::String& name, const std::vector<TrackID>& tracks)
{
    const auto bus = addTrackAtEnd (context, true);
    REQUIRE (bus.isValid());
    renameTrack (context, bus, name);

    for (const auto track : tracks)
        setOutput (context, track, bus);

    setMixedAsBus (context, bus, true);
    return bus;
}

void TestSong::rename (TrackID track, const juce::String& name)
{
    renameTrack (context, track, name);
}

//==============================================================================
DeviceType TestSong::findDeviceType (const char* typeID) const
{
    for (const auto& type : getDeviceTypes (context))
        if (type.description.fileOrIdentifier == juce::String (typeID))
            return type;

    FAIL ("Phazite offers no device of type " << typeID);
    return {};
}

DeviceID TestSong::setInstrument (TrackID track, const char* typeID)
{
    chooseInstrument (context, track, findDeviceType (typeID));
    const auto instrument = getInstrument (context, track);
    REQUIRE (instrument.has_value());
    return instrument->id;
}

DeviceID TestSong::addEffect (TrackID track, const char* typeID)
{
    phazite::addEffect (context, track, findDeviceType (typeID));
    const auto effects = getEffects (context, track);
    REQUIRE (! effects.empty());
    return effects.back().id;
}

void TestSong::setParameter (DeviceID device, const juce::String& parameterID, float value)
{
    auto parameter = getParameter (context, { device, parameterID });
    REQUIRE_MESSAGE (parameter.exists(), ("no parameter " + parameterID).toStdString());
    parameter.setValue (value);
}

void TestSong::setVolume (TrackID track, float decibels)
{
    auto volume = getVolume (context, track);
    REQUIRE (volume.exists());
    volume.setValue (decibelsToFaderPosition (decibels));
}

void TestSong::setPan (TrackID track, float pan)
{
    auto parameter = getPan (context, track);
    REQUIRE (parameter.exists());
    parameter.setValue (pan);
}

ParameterRef TestSong::getVolumeParameter (TrackID track)
{
    return getVolume (context, track).getRef();
}

ParameterRef TestSong::getPanParameter (TrackID track)
{
    return getPan (context, track).getRef();
}

TrackID TestSong::addReturn (TrackID sender, const char* effectTypeID)
{
    const auto added = addReturnWith (context, sender, findDeviceType (effectTypeID));
    REQUIRE (added.isValid());
    return added;
}

TrackID TestSong::addReturn (TrackID sender, const char* effectTypeID, float sendDecibels)
{
    const auto added = addReturn (sender, effectTypeID);
    addSend (sender, added, sendDecibels);
    return added;
}

void TestSong::addSend (TrackID from, TrackID returnTrack, float decibels)
{
    auto level = getSendLevelTo (context, from, returnTrack);
    REQUIRE (level.isValid());
    level.setValue (decibelsToFaderPosition (decibels));
}

//==============================================================================
ClipID TestSong::addMidiClip (TrackID track, double startBeat, double lengthBeats)
{
    const auto clip = addMidiClipAt (context, track, at (startBeat));
    REQUIRE (clip.isValid());
    trimClipEnd (context, clip, at (startBeat + lengthBeats));
    return clip;
}

void TestSong::addNote (ClipID clip, int pitch, double startBeat, double lengthBeats, int velocity)
{
    REQUIRE (phazite::addNote (context, clip, pitch, at (startBeat), BeatDuration::fromBeats (lengthBeats), velocity).isValid());
}

void TestSong::addChord (ClipID clip, std::initializer_list<int> pitches, double startBeat, double lengthBeats, int velocity)
{
    for (const auto pitch : pitches)
        addNote (clip, pitch, startBeat, lengthBeats, velocity);
}

ClipID TestSong::addAudioClip (TrackID track, const juce::File& file, double startBeat)
{
    const auto added = importFiles (context, juce::StringArray (file.getFullPathName()), track, at (startBeat));
    REQUIRE (added.size() == 1);
    return added.front();
}

void TestSong::select (ClipID clip)
{
    context.getSongSelection().selectOnly (clip);
}

ClipID TestSong::copyClip (ClipID clip, double startBeat, bool asAlias)
{
    const auto info = getClip (context, clip);
    REQUIRE (info.has_value());
    const auto copies = copyClips (context, { clip }, asAlias);
    REQUIRE (copies.size() == 1);
    moveClip (context, copies.front(), at (startBeat), info->track);
    return copies.front();
}

void TestSong::waitForClipRenders()
{
    const auto anyPreparing = [this]
    {
        for (const auto track : getAllPlaylistTracks (context))
            for (const auto& clip : getClips (context, track))
                if (const auto audio = getAudioClip (context, clip.id); audio.has_value() && audio->state == AudioState::preparing)
                    return true;

        return false;
    };

    // Seen to take over 30 s while six other test programs ran: a limit only a hang reaches.
    REQUIRE (dispatchMessagesUntil ([&anyPreparing] { return ! anyPreparing(); }, hangTimeout));
}

//==============================================================================
void TestSong::addAutomation (const ParameterRef& parameter, std::initializer_list<AutomationPoint> points)
{
    for (const auto& point : points)
        addAutomationPoint (context, parameter, point);

    REQUIRE (getAutomationPoints (context, parameter).size() >= points.size());
}

ModulatorID TestSong::addModulator (TrackID track, ModulatorType type)
{
    const auto modulator = phazite::addModulator (context, track, type);
    REQUIRE (modulator.isValid());
    return modulator;
}

void TestSong::setModulatorParameter (ModulatorID modulator, const juce::String& parameterID, float value)
{
    auto parameter = getModulatorParameter (context, modulator, parameterID);
    REQUIRE_MESSAGE (parameter.exists(), ("no modulator parameter " + parameterID).toStdString());
    parameter.setValue (value);
}

void TestSong::setMacroValue (ModulatorID macro, float value)
{
    const auto parameters = getModulatorParameters (context, macro);
    REQUIRE (parameters.size() == 1); // a macro's one knob
    setModulatorParameter (macro, parameters.front().id, value);
}

void TestSong::modulate (ModulatorID modulator, const ParameterRef& parameter, float amount)
{
    phazite::modulate (context, modulator, parameter, amount);
    REQUIRE (! getModulations (context, parameter).empty());
}
} // namespace phazite::test
