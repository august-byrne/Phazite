// SPDX-License-Identifier: AGPL-3.0-or-later
// The automation of what is deleted goes with it, in its step: what takes a track, a device, a container, a
// chain, a modulator, a macro's container, a send, a fader or a VCA out of the song erases the curves of its parameters from
// the song's curves (model/mixer/internal/Mixer.h: forgetWhatNamesTheGone()), so that the song passes its check, Undo brings
// every curve back as it was (its points, their keys, its place in the curves' order), Redo takes them again, and a save
// right after opens as the same song, with no unsaved change, a second save writing the same text. Each remover: Delete
// Track, a device removed, a container ungrouped (its devices keep theirs), a chain removed, a modulator removed, a send
// removed, a folder ungrouped (a bus's fader and devices; a plain folder's VCA), an instrument replaced, a version's line of
// a track added since, and a merge's choice of a whole track.
#include "PerformanceCounts.h"
#include "SongCheck.h"
#include "TestClips.h"
#include "TestDeviceValues.h"
#include "TestHelpers.h"
#include "TestSong.h"
#include "TestTracks.h"
#include "UndoCheck.h"
#include "devices/delay/Delay.h"
#include "devices/eq/Equalizer.h"
#include "devices/reverb/Reverb.h"
#include "devices/synth/Synth.h"
#include "model/arrangement/internal/Tracks.h"
#include "model/automation/AutomationModel.h"
#include "model/automation/internal/Automation.h"
#include "model/base/internal/CurveValues.h"
#include "model/base/internal/Schema.h"
#include "model/commands/internal/Undo.h"
#include "model/midi/internal/MidiLinks.h"
#include "model/mixer/ContainerModel.h"
#include "model/mixer/MixerModel.h"
#include "model/mixer/internal/Buses.h"
#include "model/mixer/internal/Containers.h"
#include "model/mixer/internal/Mixer.h"
#include "model/modulation/ModulationModel.h"
#include "model/modulation/internal/Modulation.h"
#include "model/project/internal/Project.h"
#include "model/project/internal/ProjectContext.h"
#include "model/versions/internal/ApplySong.h"
#include "model/versions/internal/AreaClashes.h"
#include "model/versions/internal/SongMerge.h"

#include <algorithm>
#include <functional>
#include <string_view>
#include <vector>

using phazite::DeviceID;
using phazite::ModulatorID;
using phazite::ParameterRef;
using phazite::TrackID;
using phazite::test::at;

namespace
{
namespace dev = phazite::devices;

juce::String toText (std::string_view text)
{
    return juce::String (text.data(), text.size());
}

ParameterRef ofModulator (ModulatorID modulator, const juce::String& parameter)
{
    return { DeviceID::fromRaw (modulator.getRaw()), parameter };
}

// A song whose every kind of curve owner is automated, each curve two points, the second bent:
//   - other: a track that stays, its volume automated first, so that the curves of what goes sit among others;
//   - lead: a synth (its cutoff), an EQ (its output), a container with one chain (its volume) holding a delay (its mix) and
//     its first macro (its value, where asked), an LFO (its rate, where asked), its fader (volume and pan) and a send (its
//     level) to reverb;
//   - reverb: a return, its volume;
//   - bus: a bus holding inBus, its fader (volume and pan) and an EQ (its output);
//   - folder: a plain folder holding inFolder, its VCA (its volume).
// A macro's and a modulator's curves only where asked: the UI automates neither, but a song holds such a curve while its
// owner is in the song, as it does any other (Automation.h: forgetCurvesOfGoneDevices()).
struct ModulationCurves
{
    bool ofLfo = false, ofMacro = false;
};

struct Automated
{
    phazite::test::TestSong s;
    TrackID lead, other, reverb, inBus, bus, inFolder, folder;
    DeviceID synth, eq, container, chain, delay, send, busEq;
    ModulatorID lfo, macro;
    ParameterRef otherVolume, leadVolume, leadPan, cutoff, eqOutput, chainVolume, delayMix, macroValue, lfoRate, sendLevel, reverbVolume,
        busVolume, busPan, busEqOutput, folderVolume;

    ModulationCurves modulationCurves;

    phazite::Song& getSong() const { return *phazite::findSong (s.context); }

    explicit Automated (ModulationCurves curvesOfModulation = {})
        : modulationCurves (curvesOfModulation)
    {
        auto& song = getSong();
        lead = s.firstTrack;
        s.rename (lead, "Lead");
        other = s.addTrack ("Other");
        synth = s.setInstrument (lead, dev::Synth::typeID);
        eq = s.addEffect (lead, dev::Equalizer::typeID);
        container = phazite::addContainer (s.context, lead);
        REQUIRE (container.isValid());
        chain = phazite::getChains (song, container).front();

        {
            const phazite::UndoStep step (song, "Add Delay");
            delay = phazite::test::addOwnDeviceToChain (song, chain, dev::Delay::typeID);
        }

        macro = phazite::getContainerMacros (song, container).front();
        lfo = s.addModulator (lead, phazite::ModulatorType::lfo);
        reverb = s.addReturn (lead, dev::Reverb::typeID, -6.0f);
        REQUIRE (phazite::getSends (s.context, lead).size() == 1);
        send = phazite::getSends (s.context, lead).front().id;
        inBus = s.addTrack ("In Bus");
        bus = s.addBus ("Bus", { inBus });
        busEq = s.addEffect (bus, dev::Equalizer::typeID);
        inFolder = s.addTrack ("In Folder");
        folder = s.addBus ("Folder", { inFolder });
        phazite::setMixedAsBus (s.context, folder, false);
        REQUIRE (phazite::isBus (song, bus));
        REQUIRE_FALSE (phazite::isBus (song, folder));

        const auto output = toText (dev::Equalizer::info.parameters[(size_t) dev::Equalizer::outputParameter].id);
        otherVolume = s.getVolumeParameter (other);
        leadVolume = s.getVolumeParameter (lead);
        leadPan = s.getPanParameter (lead);
        cutoff = { synth, "cutoff" };
        eqOutput = { eq, output };
        chainVolume = { chain, phazite::schema::chainVolumeParameter };
        delayMix = { delay, "mix" };
        macroValue = ofModulator (macro, juce::String (macro.getRaw())); // a macro's one parameter
        lfoRate = ofModulator (lfo, "rate");
        sendLevel = { send, phazite::schema::sendLevelParameter };
        reverbVolume = s.getVolumeParameter (reverb);
        busVolume = s.getVolumeParameter (bus);
        busPan = s.getPanParameter (bus);
        busEqOutput = { busEq, output };
        folderVolume = s.getVolumeParameter (folder);

        for (const auto& parameter : getAll())
        {
            INFO (phazite::getCurveKeyText (parameter).toStdString());
            REQUIRE (phazite::addAutomationPoint (s.context, parameter, { at (0.0), 0.25f }) >= 0);
            REQUIRE (phazite::addAutomationPoint (s.context, parameter, { at (4.0), 0.75f, 0.3f }) >= 0);
        }

        REQUIRE (phazite::getCurveKeys (song) == getAll());
        phazite::test::checkSong (song);
        song.getUndoManager().clearUndoHistory();
    }

    // Every parameter automated, in the order their curves were made.
    std::vector<ParameterRef> getAll() const
    {
        return withoutModulation ({ otherVolume, leadVolume, leadPan, cutoff, eqOutput, chainVolume, delayMix, macroValue, lfoRate,
                                    sendLevel, reverbVolume, busVolume, busPan, busEqOutput, folderVolume });
    }

    // Every parameter of lead's automated, whose curves go with it.
    std::vector<ParameterRef> getLead() const
    {
        return withoutModulation ({ leadVolume, leadPan, cutoff, eqOutput, chainVolume, delayMix, macroValue, lfoRate, sendLevel });
    }

    // The parameters, less the macro's and the LFO's where they aren't automated.
    std::vector<ParameterRef> withoutModulation (std::vector<ParameterRef> parameters) const
    {
        std::erase_if (
            parameters, [this] (const ParameterRef& parameter)
            { return (parameter == macroValue && ! modulationCurves.ofMacro) || (parameter == lfoRate && ! modulationCurves.ofLfo); });
        return parameters;
    }

    // A remover as the UI runs it (a change of its own, its step's name given), or wrapped in a step of that name where it
    // joins its caller's: afterwards, the curves of the parameters gone are no more, the others are there in their order,
    // the song passes its check with nothing excused, and the step is one, of that name; Undo brings the curves back as they
    // were (the same value: the points, their keys, the order), Redo takes them again. Then saved and opened again: the same
    // song, nothing unsaved, and a second save writes the same text.
    void checkRemoval (const juce::String& name, const std::function<void()>& remove, const std::vector<ParameterRef>& gone)
    {
        auto& song = getSong();
        const auto before = phazite::getCurvesValue (song);
        std::vector<ParameterRef> left;

        for (const auto& parameter : before.order)
            if (std::find (gone.begin(), gone.end(), parameter) == gone.end())
                left.push_back (parameter);

        for (const auto& parameter : gone)
            REQUIRE (phazite::findCurve (song, parameter) != nullptr);

        const auto checkGone = [this, &gone, &left]
        {
            for (const auto& parameter : gone)
            {
                INFO (phazite::getCurveKeyText (parameter).toStdString());
                CHECK (phazite::findCurve (getSong(), parameter) == nullptr);
            }

            CHECK (phazite::getCurveKeys (getSong()) == left);
            phazite::test::checkSong (getSong());
        };

        {
            phazite::test::UndoCheck undo (s.context);
            undo.check (name, remove);
            CHECK (undo.getNumSteps() == 1);
            CHECK (song.getUndoManager().getUndoDescription() == name);
            checkGone();

            undo.stepBackOrForth ("Undo", [&song] { phazite::undo (song); });
            CHECK (phazite::getCurvesValue (song).isSameAs (before));
            CHECK (phazite::getCurvesValue (song) == before);
            phazite::test::checkSong (song);

            undo.stepBackOrForth ("Redo", [&song] { phazite::redo (song); });
            checkGone();
        }

        // Saved and opened again: what was saved is the song as it opens.
        const auto asEdited = phazite::test::writeSong (song, phazite::test::SongPart::undoable);
        s.reopen();
        const auto opened = phazite::test::writeSong (getSong(), phazite::test::SongPart::undoable);
        INFO (phazite::test::describeSongDifferences (asEdited, opened).joinIntoString ("\n"));
        CHECK (opened == asEdited);
        CHECK (phazite::getCurveKeys (getSong()) == left);
        CHECK_FALSE (s.getProject().hasUnsavedChanges());

        const auto saved = s.getProject().getFile().loadFileAsString();
        REQUIRE (s.getProject().save().wasOk());
        CHECK (s.getProject().getFile().loadFileAsString() == saved);
    }

    // A change made in a step and undone, the song whole after it kept: a version of it, as the versions hold one.
    juce::ValueTree versionAfter (const std::function<void()>& change)
    {
        auto& song = getSong();

        {
            const phazite::UndoStep step (song, "For the Version");
            change();
        }

        const auto version = phazite::test::copyWholeSong (song);
        phazite::undo (song);
        song.getUndoManager().clearUndoHistory();
        return version;
    }

    // A function that joins its caller's step, run in a step of its own of that name.
    std::function<void()> inStep (const juce::String& name, const std::function<void()>& change)
    {
        return [this, name, change]
        {
            const phazite::UndoStep step (getSong(), name);
            change();
        };
    }
};

// An element's place from the song down, as a merge's clash names it ("TRACKS:TRACKS", "TRACK:12").
juce::StringArray pathOf (const juce::ValueTree& element)
{
    juce::StringArray path;

    for (auto each = element; each.isValid() && each.getParent().isValid(); each = each.getParent())
        path.insert (0, each.getType().toString() + ":" + phazite::schema::getElementKey (each));

    return path;
}
} // namespace

TEST_CASE ("Automation of the gone: a track deleted takes the curves of its fader, devices, chain, macro, modulator and send along")
{
    Automated a ({ .ofLfo = true, .ofMacro = true });
    a.checkRemoval ("Delete Track", a.inStep ("Delete Track", [&a] { phazite::deleteTracks (a.getSong(), { a.lead }, nullptr); }),
                    a.getLead());
}

TEST_CASE ("Automation of the gone: a device removed takes its curves along, and the others stay")
{
    Automated a;
    const auto name = "Remove " + phazite::getDeviceName (a.getSong(), a.eq);
    a.checkRemoval (name, [&a] { phazite::removeDevice (a.s.context, a.eq); }, { a.eqOutput });
}

TEST_CASE ("Automation of the gone: a container ungrouped takes its macro's and its chain's curves along, its devices keep theirs")
{
    Automated a ({ .ofMacro = true });
    a.checkRemoval ("Ungroup Container", [&a] { phazite::ungroupContainer (a.s.context, a.container); }, { a.chainVolume, a.macroValue });
    CHECK (phazite::findCurve (a.getSong(), a.delayMix) != nullptr);
}

TEST_CASE ("Automation of the gone: a chain removed takes its curves and its devices' along, its container staying")
{
    Automated a;
    REQUIRE (phazite::addChain (a.s.context, a.container).isValid());
    a.getSong().getUndoManager().clearUndoHistory();
    a.checkRemoval ("Remove Chain", [&a] { phazite::removeChain (a.s.context, a.chain); }, { a.chainVolume, a.delayMix });
    CHECK (phazite::getChains (a.getSong(), a.container).size() == 1);
}

TEST_CASE ("Automation of the gone: a modulator removed takes its curve along")
{
    Automated a ({ .ofLfo = true });
    const auto name = "Remove " + phazite::getModulatorName (a.getSong(), a.lfo);
    a.checkRemoval (name, [&a] { phazite::removeModulator (a.s.context, a.lfo); }, { a.lfoRate });
}

TEST_CASE ("Automation of the gone: a modulator's and a macro's curves stay while they are in the song, saved and opened again")
{
    // One notion of what a curve names (Automation.cpp: isCurveOwnerThere(), by the items forgetWhatNamesTheGone() finds): a
    // modulator and a macro own their curves while they are in the song, so the loader keeps them, and the song as it would
    // open is the song that opens.
    using phazite::test::SongPart;
    Automated a ({ .ofLfo = true, .ofMacro = true });
    const auto asSaved = phazite::test::writeSong (a.getSong(), SongPart::undoable);
    const auto asOpened = phazite::test::copyAsOpened (a.getSong());
    phazite::test::checkSong (*asOpened);
    const auto wholeAsOpened = phazite::test::writeSong (*asOpened, SongPart::whole);

    a.s.reopen();
    CHECK (phazite::getCurveKeys (a.getSong()) == a.getAll());

    for (const auto& parameter : { a.lfoRate, a.macroValue })
    {
        INFO (phazite::getCurveKeyText (parameter).toStdString());
        const auto* curve = phazite::findCurve (a.getSong(), parameter);
        REQUIRE (curve != nullptr);
        CHECK (curve->points.size() == 2);
    }

    const auto opened = phazite::test::writeSong (a.getSong(), SongPart::undoable);
    INFO (phazite::test::describeSongDifferences (asSaved, opened).joinIntoString ("\n"));
    CHECK (opened == asSaved);
    CHECK (phazite::test::writeSong (a.getSong(), SongPart::whole) == wholeAsOpened);
    CHECK_FALSE (a.s.getProject().hasUnsavedChanges());
    phazite::test::checkSong (a.getSong());

    // Then the modulator removed: its curve goes with it, in its step, which Undo brings back; saved and opened, gone. The
    // macro's stays.
    const auto name = "Remove " + phazite::getModulatorName (a.getSong(), a.lfo);
    a.checkRemoval (name, [&a] { phazite::removeModulator (a.s.context, a.lfo); }, { a.lfoRate });
    CHECK (phazite::findCurve (a.getSong(), a.macroValue) != nullptr);
}

TEST_CASE ("Automation of the gone: a bus folder ungrouped takes the modulations and the MIDI links of its fader and its devices")
{
    // An LFO on another track modulates the bus's volume and its EQ's output, and a MIDI link turns each: ungrouped, the
    // bus's fader and EQ go, and what named them with them, in the one step (Tracks.cpp: ungroupFolder(), which calls
    // forgetWhatNamesTheGone()); the LFO stays.
    Automated a;
    auto& song = a.getSong();
    const auto lfo = a.s.addModulator (a.other, phazite::ModulatorType::lfo);
    const phazite::MidiController forEq { .channel = 1, .number = 20 }, forVolume { .channel = 1, .number = 21 };

    {
        const phazite::UndoStep step (song, "Modulate and Link");
        phazite::setModulation (song, lfo, a.busVolume, 0.5f);
        phazite::setModulation (song, lfo, a.busEqOutput, -0.25f);
        phazite::linkMidiController (song, forEq, a.busEqOutput);
        phazite::linkMidiController (song, forVolume, a.busVolume);
    }

    const auto checkThere = [&a, forEq, forVolume]
    {
        auto& now = a.getSong();
        CHECK (phazite::findModulationsOf (now, a.busVolume).size() == 1);
        CHECK (phazite::findModulationsOf (now, a.busEqOutput).size() == 1);
        CHECK (phazite::findMidiLink (now, a.busEqOutput) == forEq);
        CHECK (phazite::findMidiLink (now, a.busVolume) == forVolume);
        phazite::test::checkSong (now);
    };

    const auto checkGone = [&a, lfo]
    {
        auto& now = a.getSong();
        CHECK (phazite::findModulationsOf (now, a.busVolume).empty());
        CHECK (phazite::findModulationsOf (now, a.busEqOutput).empty());
        CHECK_FALSE (phazite::findMidiLink (now, a.busEqOutput).has_value());
        CHECK_FALSE (phazite::findMidiLink (now, a.busVolume).has_value());
        CHECK (phazite::getModulatorValue (now, lfo).has_value());
        CHECK (phazite::getMidiLinks (now).empty());
        phazite::test::checkSong (now);
    };

    checkThere();
    song.getUndoManager().clearUndoHistory();

    phazite::test::UndoCheck undo (a.s.context);
    undo.check ("Ungroup Folder", a.inStep ("Ungroup Folder", [&a] { phazite::ungroupFolder (a.getSong(), a.bus); }));
    CHECK (undo.getNumSteps() == 1);
    CHECK (song.getUndoManager().getUndoDescription() == "Ungroup Folder");
    checkGone();

    undo.stepBackOrForth ("Undo", [&song] { phazite::undo (song); });
    checkThere();

    undo.stepBackOrForth ("Redo", [&song] { phazite::redo (song); });
    checkGone();
}

TEST_CASE ("Automation of the gone: a send removed takes its level's curve along")
{
    Automated a;
    a.checkRemoval ("Remove Send", [&a] { phazite::removeSend (a.s.context, a.send); }, { a.sendLevel });
}

TEST_CASE ("Automation of the gone: a folder ungrouped takes its fader's and its devices' curves along, a plain folder its VCA's")
{
    Automated a;

    SUBCASE ("A bus")
    {
        a.checkRemoval ("Ungroup Folder", a.inStep ("Ungroup Folder", [&a] { phazite::ungroupFolder (a.getSong(), a.bus); }),
                        { a.busVolume, a.busPan, a.busEqOutput });
    }

    SUBCASE ("A plain folder")
    {
        a.checkRemoval ("Ungroup Folder", a.inStep ("Ungroup Folder", [&a] { phazite::ungroupFolder (a.getSong(), a.folder); }),
                        { a.folderVolume });
    }
}

TEST_CASE ("Automation of the gone: an instrument replaced takes the old one's curves along")
{
    Automated a;
    const auto type = a.s.findDeviceType (dev::Synth::typeID);
    a.checkRemoval ("Replace Instrument with " + type.name, [&a, &type] { phazite::chooseInstrument (a.s.context, a.lead, type); },
                    { a.cutoff });
}

TEST_CASE ("Automation of the gone: a version's line of a track added since takes the track's curves away with it")
{
    Automated a ({ .ofLfo = true, .ofMacro = true });
    const auto version = a.versionAfter ([&a] { phazite::deleteTracks (a.getSong(), { a.lead }, nullptr); });
    a.checkRemoval ("Restore Track",
                    a.inStep ("Restore Track",
                              [&a, &version]
                              {
                                  REQUIRE (phazite::makeTrackLike (a.getSong(), a.lead, version));
                                  phazite::removeBrokenReferences (a.getSong());
                              }),
                    a.getLead());
}

TEST_CASE ("Automation of the gone: a merge's choice of a whole track without a device takes the device's curves away")
{
    // The other line's lead, whole, without its EQ, chosen as a merge's clash is chosen (VersionsModel.cpp: chooseClash()):
    // the side's track, then what names what the song no longer holds (removeBrokenReferences()), in the choice's step.
    Automated a;
    const auto version = a.versionAfter ([&a] { phazite::removeDevice (a.getSong(), a.eq); });
    const auto then = phazite::test::findTrackIn (version, a.lead);
    REQUIRE (then.isValid());
    const auto now = phazite::test::copyWholeSong (a.getSong());

    phazite::MergeClash clash;
    clash.line = "Lead";
    clash.trackID = a.lead.getRaw();
    clash.path = pathOf (then);
    clash.baseElement = phazite::test::findTrackIn (now, a.lead);
    clash.oursElement = clash.baseElement;
    clash.theirsElement = then;
    const phazite::MergeSides sides { now, now, version };
    REQUIRE (phazite::findByPath (a.getSong().getState(), clash.path).isValid());

    a.checkRemoval ("Take the Other Branch's",
                    a.inStep ("Take the Other Branch's",
                              [&a, &clash, &sides]
                              {
                                  REQUIRE (phazite::chooseTrackClash (a.getSong(), clash, 2, sides) == phazite::AreaChoice::chosen);
                                  phazite::removeBrokenReferences (a.getSong());
                              }),
                    { a.eqOutput });
}

TEST_CASE ("Automation of the gone: a removal that erases curves gives the engine one plan, as one that erases none")
{
    // What plays hears the curves' change with the removal's (SongValues::Listener::curvesChanged()): the plan the removal
    // makes holds no curve of what went, and no plan more is made for the curves.
    Automated a;
    auto& song = a.getSong();
    phazite::test::PlanCounter plans (a.s.context);
    const auto plain = a.s.addEffect (a.other, dev::Delay::typeID); // no curve of its own
    CHECK (plans.take() == 1);

    phazite::removeDevice (a.s.context, plain);
    CHECK (plans.take() == 1);
    phazite::removeDevice (a.s.context, a.eq);
    CHECK (phazite::findCurve (song, a.eqOutput) == nullptr);
    CHECK (plans.take() == 1);
    phazite::undo (song);
    CHECK (phazite::findCurve (song, a.eqOutput) != nullptr);
    CHECK (plans.take() == 1);
    phazite::redo (song);
    CHECK (plans.take() == 1);

    phazite::deleteTracks (song, { a.lead }, nullptr);
    CHECK (plans.take() == 1);
}

TEST_CASE ("Automation of the gone: what names nothing taken away again finds no curve to take: a second pass writes nothing")
{
    // A version applied (its curves made the song's by the curves pass, ApplySong.cpp: makeCurvesLike()), then what names
    // what the song no longer holds taken away (removeBrokenReferences(), which every apply runs, forgetting the curves of
    // what is gone with the rest), and again: the second changes nothing. The version holds a curve of a device it hasn't
    // (as one an earlier build saved could): the pass brings it in, and the forgetting takes it away, in the apply's step.
    Automated a;
    auto& song = a.getSong();
    auto version = a.versionAfter ([&a] { phazite::deleteTracks (a.getSong(), { a.lead }, nullptr); });
    auto automation = version.getChildWithName (phazite::schema::AUTOMATION);
    REQUIRE (automation.getNumChildren() > 0);
    auto stray = automation.getChild (0).createCopy();
    stray.setProperty (phazite::schema::curveDevice, "987654", nullptr);
    automation.appendChild (stray, nullptr);
    const auto strayParameter = phazite::readCurveParameter (stray);

    {
        const phazite::UndoStep step (song, "Bring Back");
        phazite::makeSongLike (song, version);
        phazite::removeBrokenReferences (song);
    }

    const auto after = phazite::getCurvesValue (song);
    CHECK (after.find (strayParameter) == nullptr);

    for (const auto& parameter : a.getLead())
        CHECK (after.find (parameter) == nullptr);

    {
        const phazite::UndoStep step (song, "Again");
        phazite::removeBrokenReferences (song);
    }

    CHECK (phazite::getCurvesValue (song).isSameAs (after));
    CHECK (song.getUndoManager().getUndoDescription() == "Bring Back"); // no step of its own: it changed nothing
    phazite::test::checkSong (song);
}
