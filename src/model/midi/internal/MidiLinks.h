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
// SongValues.h), without a read, which shares what it holds; a song that keeps everything in its tree (one that holds no
// values: Song::getValues()) has them read from its MIDILINKS element.
MidiLinksValue getMidiLinksValue (const Song&);

// The parameter a link names, as the song names it elsewhere (ParameterRef.h): a macro's link holds the word
// schema::midiLinkMacroValue as its parameter, which stays as a merge or a copy gives the macro a new ID. Of a link of the
// song's value, and of a link's element (read as the value reads it: the loader's, makeConsistent()).
ParameterRef readMidiLinkParameter (const Song&, const MidiLinkEntry&);
ParameterRef readMidiLinkParameter (const Song&, const juce::ValueTree& link);

// The song's links, by channel, then number; a parameter's controller; a controller's parameter.
std::vector<MidiLink> getMidiLinks (const Song&);
std::optional<MidiController> findMidiLink (const Song&, const ParameterRef&);
std::optional<ParameterRef> findLinkedParameter (const Song&, MidiController);

// The writers of the song's links, each in the caller's step (or outside the history while the song is written outside it),
// each reading the links' value, working the next one out and setting it once (SongValues.h: setMidiLinks(), which writes
// nothing for the links the song holds already). Each of a song that holds its values (model/project/internal/Project.h:
// makeSong()): a song without them is written nothing.
//
// Links a controller to a parameter: the controller's link, if it had one, and the parameter's own, if it had one, go, and
// the link is put after the others; the song's MIDILINKS, its place, added first where it has none (an action of the tree's,
// before the value's: so Undo takes the value back first, then the element), as it was always added with the first link.
// The parameter the controller turned before, if it was another.
std::optional<ParameterRef> linkMidiController (Song&, MidiController, const ParameterRef&);
void unlinkMidiController (Song&, MidiController);

// The links of what is gone: those whose device, chain, send, fader, modulator or macro isn't among the song's items (by
// their IDs, as forgetWhatNamesTheGone() finds them, model/mixer/internal/Mixer.h); and of the links that stay one of each
// controller and one of each parameter (MidiLinksValue.h: keepOneLinkEach()), as after a merge or an applied song, which can
// take one side's link of a parameter beside the other's.
void forgetMidiLinksOfTheGone (Song&, const std::set<std::uint64_t>& items);

// One link of each controller and one of each parameter in the MIDILINKS element of a song that keeps its links in its tree
// (the loader's, makeConsistent(), before the song holds its values; a version's song read in part): of two, the first in the
// element's order stays, the other goes, as MidiLinksValue.h's keepOneLinkEach() keeps them, outside any history. Nothing for
// a song that holds its values, whose MIDILINKS is empty.
void keepOneMidiLinkEach (Song&);

// The song's links made those of a MIDILINKS element (another song's: a version's, a merge's), read as the song holds links
// (MidiLinksValue.h: readHeldMidiLinks()), or none for an invalid tree, with its MIDILINKS as the other has one: added first
// where it comes, taken away last where it goes (the value never says more than the element), in the mode of the moment.
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
// taken out (SongValues::Listener::modulationChanged(), by its ID), and a send's level the same (sendsChanged(), by the send's
// ID); any other's (a device's, a chain's, a fader's) as the song's tree tells of a child taken out, its source looked
// for again (locateParameter()). Which of them a learned parameter is, is its source (ParameterSource), found as learning
// starts: so that the tree's callback asks only of what the tree still holds (isHeardFromTree()), and reads no area of the
// song's values.
class MidiLinker final : private juce::Timer,
                         private juce::ValueTree::Listener,
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
    juce::ValueTree state;
    std::optional<ParameterRef> learning;
    ParameterSource learningSource = ParameterSource::device; // what learning's parameter is (setLearning())
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

    // Whether the song's tree holds a source's parameters, so that the tree's callback looks for one of them again: every
    // source's but a modulator's and a macro's, which the song's values tell (modulationChanged()), and a send's
    // (sendsChanged()). A source whose area gets a listener of its own (roadmap step S5.20) leaves this list for it, with an
    // override of its own below.
    static bool isHeardFromTree (ParameterSource);

    void timerCallback() override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void modulationChanged (const std::vector<SongValues::ModulationChange>&, const std::vector<ModulatorPlace>&) override;
    void sendsChanged (const std::vector<SongValues::SendChange>&, const std::vector<TrackID>&) override;

    JUCE_DECLARE_NON_COPYABLE (MidiLinker)
};

// The linker of a song a project context holds, or nullptr.
MidiLinker* findMidiLinker (const Song&);
} // namespace phazite
