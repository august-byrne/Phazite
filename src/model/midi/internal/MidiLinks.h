// SPDX-License-Identifier: AGPL-3.0-or-later
// The song's MIDI links (MidiLinkModel.h; Schema.h: MIDILINKS), for the model, the app and tests: reading and changing them in
// the caller's undo step, and what learns and turns them for the open song (MidiLinker), which the project context holds.
// Message thread only.
#pragma once

#include "model/base/Song.h"
#include "model/base/internal/MidiLinksValue.h"
#include "model/base/internal/ModulationValue.h"
#include "model/base/internal/SongValues.h"
#include "model/devices/ParameterRef.h"
#include "model/devices/internal/Parameters.h"
#include "model/midi/MidiLinkValues.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace phazite
{
class Trial;

struct MidiLink
{
    MidiController controller;
    ParameterRef parameter;
};

// The song's links as a value (model/base/internal/MidiLinksValue.h), in the song's order: its area's (model/base/internal/
// SongValues.h), without a read, which shares what it holds.
MidiLinksValue getMidiLinksValue (const Song&);

// The parameter a link names, as the song names it elsewhere (ParameterRef.h): a macro's link holds the word
// schema::midiLinkMacroValue as its parameter, which stays as a merge or a copy gives the macro a new ID. Of a link of the
// song's value; and of a link's element in a file's song's tree, the macro found by its element there (the loader's,
// makeConsistent()), by the same rule.
ParameterRef readMidiLinkParameter (const Song&, const MidiLinkEntry&);
ParameterRef readMidiLinkParameterIn (const juce::ValueTree& song, const juce::ValueTree& link);

// The song's links, by channel, then number; a parameter's controller; a controller's parameter.
std::vector<MidiLink> getMidiLinks (const Song&);
std::optional<MidiController> findMidiLink (const Song&, const ParameterRef&);
std::optional<ParameterRef> findLinkedParameter (const Song&, MidiController);

// The writers of the song's links, each in the caller's step (or outside the history while the song is written outside it),
// each reading the links' value, working the next one out and setting it once (SongValues.h: setMidiLinks(), which writes
// nothing for the links the song holds already).
//
// Links a controller to a parameter: the controller's link, if it had one, and the parameter's own, if it had one, go, and
// the link is put after the others. The parameter the controller turned before, if it was another.
std::optional<ParameterRef> linkMidiController (Song&, MidiController, const ParameterRef&);
void unlinkMidiController (Song&, MidiController);

// The links of what is gone: those whose device, chain, send, fader, modulator or macro isn't among the song's items (by
// their IDs, as forgetWhatNamesTheGone() finds them, model/mixer/internal/Mixer.h); and of the links that stay one of each
// controller and one of each parameter (MidiLinksValue.h: keepOneLinkEach()), as after a merge or an applied song, which can
// take one side's link of a parameter beside the other's.
void forgetMidiLinksOfTheGone (Song&, const std::set<std::uint64_t>& items);

// One link of each controller and one of each parameter in a file's song's MIDILINKS element (the loader's,
// makeConsistent(); a version's song read in part): of two, the first in the element's order stays, the other goes, as
// MidiLinksValue.h's keepOneLinkEach() keeps them. Nothing for an invalid tree.
void keepOneMidiLinkEach (juce::ValueTree midiLinksElement);

// The song's links made those of a MIDILINKS element (another song's: a version's, a merge's), read as the song holds links
// (MidiLinksValue.h: readHeldMidiLinks()), or none for an invalid tree (an empty list is none), in the mode of the moment.
// What the versions' links pass gives the song (model/versions/ApplySong.cpp). Whether it wrote anything.
bool makeMidiLinksAs (Song&, const juce::ValueTree& midiLinksElement);

// The value a controller's position (0 to 127) sets a parameter to: along its control's travel (getControlTravel()), as an
// automation point's position is, 127 all the way; a parameter that steps (a switch, a choice) in steps that share the travel
// evenly, a switch on from 64. None for a parameter the song hasn't.
std::optional<float> getValueAtControllerPosition (const Song&, const ParameterRef&, int position);

// What learns and turns the open song's links (the project context holds one for the song it holds, and it goes with it).
// A controller's values come from the MIDI inputs (model/engine/internal/MidiTap.h) through the song's session, which
// diverts the song's links' controllers and, while learning, any controller (PlaybackSession::setMidiLearning()).
//
// A burst of a knob's moves is one undo step, named as a control's change is ("Change Cutoff"), from its first value until
// the controllers have rested for restMilliseconds (devices::Clock), or until a step, a gesture, an undo or a redo begins
// meanwhile, which keeps it first (Trial); two knobs turned at once are one burst, named after the first. A burst that set
// every value as it was begins and keeps no step. A value that comes while a control is dragged joins the drag's step, and
// begins no burst. While the song plays and a parameter's track writes automation, its burst writes as a control's drag
// writes, punched in at its first value and out at the burst's end (one that changed nothing writes nothing: its hold is let
// go of, AutomationWriter::gestureLetGo()).
//
// Learning ends as its parameter goes, in the call that tells of it: a modulator's or a macro's as the song's values tell it
// taken out (SongValues::Listener::modulationChanged(), by its ID), a send's level the same (sendsChanged(), by the send's ID),
// and a device's, a plug-in's or a chain's as its device or chain is (devicesChanged(), by its ID); a fader's as its track is
// taken out, or given another fader or VCA (tracksChanged(), by the channel it remembers: learningChannel), the master's as its
// element is put in anew (a merge's choice of the whole MASTER: made) or given another fader. A bus made plain while its fader
// learns ends the learning. Which of them a learned parameter is, is its source (ParameterSource), found as learning starts. The
// linker listens to the song's values alone.
class MidiLinker final : private juce::Timer,
                         private SongValues::Listener
{
public:
    explicit MidiLinker (Song&);
    ~MidiLinker() override; // keeps what a burst changed

    Song& getSong() const noexcept { return *song; }

    void startLearning (const ParameterRef&);
    void cancelLearning();
    std::optional<ParameterRef> getLearning() const { return learning; }

    // A controller's value, 0 to 127: the controller learned, if a parameter learns, else its link's parameter set.
    void take (MidiController, int value);

    // Called as learning starts or ends, and as a link is learned (not as the linker goes).
    std::function<void()> onChange;

    // What the last link learned made, once (MidiLinkModel.h: takeMidiLinkMade()).
    std::optional<juce::String> takeLinkMade() { return std::exchange (linkMade, std::nullopt); }

    bool isInBurst() const;   // a burst open that changed something
    bool isBurstOpen() const; // one open, whether or not it changed anything (its timer runs)
    void endBurst();          // keeps it, as the controllers' rest does

    static constexpr double restMilliseconds = 500.0;

private:
    Song::Ptr song;
    std::optional<ParameterRef> learning;
    ParameterSource learningSource = ParameterSource::device; // what learning's parameter is (setLearning())
    ChannelRef learningChannel {};                            // and a fader's channel, as learning started (FoundParameter)
    std::optional<juce::String> linkMade;

    std::unique_ptr<Trial> burst;
    juce::String burstName;
    std::vector<ParameterRef> inBurst; // the parameters it turned, which a pass may write
    double lastMove = 0.0;             // when it moved last, in devices::Clock's milliseconds
    static constexpr int restCheckMilliseconds = 50;

    void setLearning (std::optional<ParameterRef>, ParameterSource = ParameterSource::device);
    void learn (MidiController);
    void turn (const ParameterRef&, float value);
    void finishWriting(); // the burst's passes told it ended, in its step

    // Where learning's parameter goes, each source by its area's listener: a modulator's and a macro's (modulationChanged()), a
    // send's (sendsChanged()), a device's, a plug-in's and a chain's (devicesChanged(), which looks again for a chain's
    // crossover, one only while its container splits into bands and it isn't the first, as something is taken out, in the
    // song's devices handed whole), and a fader's (tracksChanged()).
    void timerCallback() override;
    void tracksChanged (const std::vector<TrackChange>&, const std::vector<TrackID>&, const SongTracks& now) override;
    void modulationChanged (const std::vector<SongValues::ModulationChange>&, const std::vector<ModulatorPlace>&) override;
    void sendsChanged (const std::vector<SongValues::SendChange>&, const std::vector<TrackID>&, const SongSends& now) override;
    void devicesChanged (const std::vector<SongValues::DeviceChange>&, const std::vector<DevicePlace>&, const SongDevices& now) override;

    JUCE_DECLARE_NON_COPYABLE (MidiLinker)
};

// The linker of a song a project context holds, or nullptr.
MidiLinker* findMidiLinker (const Song&);
} // namespace phazite
