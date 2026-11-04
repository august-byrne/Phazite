// SPDX-License-Identifier: AGPL-3.0-or-later
// A song made through the model's functions, by IDs, beats and plain values, as the UI makes one: what a test builds
// with it keeps working however the model holds the song. It is a project in a temporary folder, open in a context of
// its own as the app opens one, with the commands the app registers for the song.
#pragma once

#include "TestHelpers.h"
#include "model/automation/AutomationValues.h"
#include "model/commands/CommandRegistry.h"
#include "model/modulation/ModulationValues.h"
#include "model/project/ProjectContext.h"

#include <initializer_list>
#include <memory>
#include <vector>

namespace phazite
{
class Project;
struct DeviceType;
} // namespace phazite

namespace phazite::test
{
class TestSong
{
public:
    // How the project starts: empty but for the track every new song has (firstTrack), or as the First Beat example song,
    // a beat and an arpeggio (model/project/ExampleSongs.h), with the presets in resources/presets.
    enum class Start
    {
        empty,
        firstBeat
    };

    explicit TestSong (Start = Start::empty);
    ~TestSong();

    ProjectContext context { getEngineContext() };
    CommandRegistry commands; // the song's commands, as the app registers them (app/AppCommands.h)
    RenderHost renders;       // what runs the freezes and bounces they start
    TrackID firstTrack;

    Project& getProject() const;
    juce::File getSoundsFolder() const; // for the test's sound files, outside the project

    // Saves the project, closes it and opens it again from its file in the same context, as the app opens one.
    void reopen();

    // Runs a command on what is selected, as a menu or a key does; it must be there and enabled. And a switch's command
    // (one menus tick, such as transport.metronome), run if it isn't on or off as asked.
    void run (const juce::String& commandID);
    void setSwitch (const juce::String& commandID, bool on);

    //==============================================================================
    void setTempo (double bpm);
    void setTimeSignature (int numerator, int denominator);

    // Tracks: one added after the others, named; a bus, a folder named so, whose tracks' sound (these, moved into it in
    // their order) mixes through its own fader.
    TrackID addTrack (const juce::String& name);
    TrackID addBus (const juce::String& name, const std::vector<TrackID>& tracks);
    void rename (TrackID, const juce::String& name);

    //==============================================================================
    // Devices, by their types (phazite::devices::Synth::typeID): a track's instrument, in place of any it had; an effect
    // after a track's effects, or the master's with an empty TrackID; and a parameter's value in its own range.
    DeviceType findDeviceType (const char* typeID) const;
    DeviceID setInstrument (TrackID, const char* typeID);
    DeviceID addEffect (TrackID, const char* typeID);
    void setParameter (DeviceID, const juce::String& parameterID, float value);

    // The mixer: a track's volume in decibels and its pan from -1 to 1, and their parameters, for automation and modulation.
    void setVolume (TrackID, float decibels);
    void setPan (TrackID, float pan);
    ParameterRef getVolumeParameter (TrackID);
    ParameterRef getPanParameter (TrackID);

    // A return holding an effect, fed by a send after the fader of a track, at a level in decibels if given (else as the
    // mixer starts a send); and a send from another track to it.
    TrackID addReturn (TrackID sender, const char* effectTypeID);
    TrackID addReturn (TrackID sender, const char* effectTypeID, float sendDecibels);
    void addSend (TrackID from, TrackID returnTrack, float decibels);

    //==============================================================================
    // Clips, in song beats: a MIDI clip, and notes added to it at song beats; an audio clip of a file, which the project
    // copies into its Audio folder, and the clips the playlist's commands act on, selected alone.
    ClipID addMidiClip (TrackID, double startBeat, double lengthBeats);
    void addNote (ClipID, int pitch, double startBeat, double lengthBeats, int velocity = 100);
    void addChord (ClipID, std::initializer_list<int> pitches, double startBeat, double lengthBeats, int velocity = 100);
    ClipID addAudioClip (TrackID, const juce::File&, double startBeat);
    void select (ClipID);

    // A copy of a clip, or an alias that plays what it plays, moved to start at a song beat on the same track.
    ClipID copyClip (ClipID, double startBeat, bool asAlias);

    // Waits for the renders of clips that follow the tempo, or are pitched or reversed, which Phazite makes in the
    // background (model/arrangement/AudioClipModel.h); a clip is silent until its own is ready.
    void waitForClipRenders();

    //==============================================================================
    void addAutomation (const ParameterRef&, std::initializer_list<AutomationPoint>);

    ModulatorID addModulator (TrackID, ModulatorType);
    void setModulatorParameter (ModulatorID, const juce::String& parameterID, float value);
    void setMacroValue (ModulatorID, float value); // from 0 to 1
    void modulate (ModulatorID, const ParameterRef&, float amount);

private:
    TemporaryFolder temporary;

    // Where the project and the sounds go: a folder no earlier song of this run had, as Phazite keeps what it read of an
    // audio file by its path, its size and when it was written (getAudioFileInfo()), which a test's sound written again
    // within the same moment would keep.
    const juce::File folder;
    std::unique_ptr<Project> project;
};
} // namespace phazite::test
